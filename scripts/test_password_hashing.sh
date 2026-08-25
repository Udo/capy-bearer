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
    print(encoded, "\n", if encoded != second { -> "randomized" } else { -> "reused" }, "\n", if password_verify("correct horse battery staple", encoded) { -> "valid" } else { -> "invalid" }, "\n", if password_verify("wrong password", encoded) { -> "wrong-valid" } else { -> "wrong-rejected" })
}
EOF

output=$(scripts/bearer-cli "/$test_name/test.capy")
if [[ "$output" != *'$bearer$scrypt$65536$8$1$'* || "$output" != *$'\nrandomized\nvalid\nwrong-rejected' ]]; then
	echo "native password hashing failed: $output" >&2
	exit 1
fi

echo "password hashing passed"
