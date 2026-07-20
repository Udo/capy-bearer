#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="password-hash-test-$$"
site_directory="${UCE_TEST_SITE_DIRECTORY:-site}"
if [[ -z "${UCE_TEST_SITE_DIRECTORY:-}" && -r /etc/uce/settings.cfg ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
	if [[ -n "${configured_site_directory:-}" ]]; then
		site_directory="$configured_site_directory"
	fi
fi
source_dir="$site_directory/$test_name"
bin_directory="${BIN_DIRECTORY:-}"
if [[ -z "$bin_directory" && -r /etc/uce/settings.cfg ]]; then
	bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
fi
bin_directory="${bin_directory:-/tmp/uce/work}"
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

printf '%s\n' \
	'CLI(Request& context) {' \
	'  String encoded = password_hash("correct horse battery staple");' \
	'  String second = password_hash("correct horse battery staple");' \
	'  String weaker = "$uce$scrypt$16384$8$1$00112233445566778899aabbccddeeff$29fdfb3d991961e926a19c1136a07e252afa5fdb8d3a0fb74cdcfa5016956f34";' \
	'  String excessive = "$uce$scrypt$131072$8$1$00112233445566778899aabbccddeeff$0000000000000000000000000000000000000000000000000000000000000000";' \
	'  print(encoded, "\n", encoded != second ? "randomized" : "reused", "\n", password_verify("correct horse battery staple", encoded) ? "valid" : "invalid", "\n", password_verify("wrong password", encoded) ? "wrong-valid" : "wrong-rejected", "\n", password_needs_rehash(encoded) ? "rehash" : "current", "\n", password_verify("legacy password", weaker) && password_needs_rehash(weaker) ? "legacy-valid-rehash" : "legacy-failed", "\n", !password_verify("password", excessive) && password_needs_rehash(excessive) ? "excessive-rejected" : "excessive-accepted", "\n", !password_verify("password", "$uce$scrypt$65536$8$1$zz$00") && password_needs_rehash("malformed") ? "malformed-rejected" : "malformed-valid");' \
	'}' >"$source_dir/test.uce"

output=$(scripts/uce-cli "/$test_name/test.uce")
if [[ "$output" != *'$uce$scrypt$65536$8$1$'* || "$output" != *$'\nrandomized\nvalid\nwrong-rejected\ncurrent\nlegacy-valid-rehash\nexcessive-rejected\nmalformed-rejected'* ]]; then
	echo "native password hashing failed: $output" >&2
	exit 1
fi

echo "password hashing passed"
