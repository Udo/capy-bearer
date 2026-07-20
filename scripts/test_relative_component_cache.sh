#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="relative-component-cache-test-$$"
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
mkdir -p "$source_dir/a" "$source_dir/b"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"

printf '%s\n' \
	'CLI(Request& context) {' \
	'  print(component("a/parent", context));' \
	'  print("/");' \
	'  print(component("b/parent", context));' \
	'}' >"$source_dir/entry.uce"
printf '%s\n' 'COMPONENT(Request& context) { print(component("child", context)); }' >"$source_dir/a/parent.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("relative-a"); }' >"$source_dir/a/child.uce"
printf '%s\n' 'COMPONENT(Request& context) { print(component("child", context)); }' >"$source_dir/b/parent.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("relative-b"); }' >"$source_dir/b/child.uce"

output=$(scripts/bearer-cli "/$test_name/entry.uce")
if [[ "$output" != "relative-a/relative-b" ]]; then
	echo "relative component cache crossed caller boundaries: $output" >&2
	exit 1
fi

echo "relative component cache passed"
