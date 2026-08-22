#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="password-hash-test-$$"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
if [[ -z "${BEARER_TEST_SITE_DIRECTORY:-}" && -r /etc/bearer/settings.cfg ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
	if [[ -n "${configured_site_directory:-}" ]]; then
		site_directory="$configured_site_directory"
	fi
fi
source_dir="$site_directory/$test_name"
bin_directory="${BIN_DIRECTORY:-}"
if [[ -z "$bin_directory" && -r /etc/bearer/settings.cfg ]]; then
	bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
fi
bin_directory="${bin_directory:-/tmp/bearer/work}"
cache_dir=""

cleanup() {
	rm -rf "$source_dir"
	if [[ -n "$cache_dir" ]]; then
		rm -rf "$cache_dir"
	fi
}
trap cleanup EXIT
mkdir -p "$source_dir"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"

cat >"$source_dir/test.capy" <<'EOF'
function CLI(request : dval) {
    var encoded := password_hash("correct horse battery staple")
    var second := password_hash("correct horse battery staple")
    var weaker := "$bearer$scrypt$16384$8$1$00112233445566778899aabbccddeeff$29fdfb3d991961e926a19c1136a07e252afa5fdb8d3a0fb74cdcfa5016956f34"
    var excessive := "$bearer$scrypt$131072$8$1$00112233445566778899aabbccddeeff$0000000000000000000000000000000000000000000000000000000000000000"
    print(encoded, "\n", if encoded != second { -> "randomized" } else { -> "reused" }, "\n", if password_verify("correct horse battery staple", encoded) { -> "valid" } else { -> "invalid" }, "\n", if password_verify("wrong password", encoded) { -> "wrong-valid" } else { -> "wrong-rejected" }, "\n", if password_needs_rehash(encoded) { -> "rehash" } else { -> "current" }, "\n", if password_verify("legacy password", weaker) && password_needs_rehash(weaker) { -> "legacy-valid-rehash" } else { -> "legacy-failed" }, "\n", if !password_verify("password", excessive) && password_needs_rehash(excessive) { -> "excessive-rejected" } else { -> "excessive-accepted" }, "\n", if !password_verify("password", "$bearer$scrypt$65536$8$1$zz$00") && password_needs_rehash("malformed") { -> "malformed-rejected" } else { -> "malformed-valid" })
}
EOF

output=$(scripts/bearer-cli "/$test_name/test.capy")
if [[ "$output" != *'$bearer$scrypt$65536$8$1$'* || "$output" != *$'\nrandomized\nvalid\nwrong-rejected\ncurrent\nlegacy-valid-rehash\nexcessive-rejected\nmalformed-rejected'* ]]; then
	echo "native password hashing failed: $output" >&2
	exit 1
fi

echo "password hashing passed"
