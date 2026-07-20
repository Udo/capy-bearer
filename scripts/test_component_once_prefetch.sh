#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="component-once-prefetch-test-$$"
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

printf '%s\n' \
	'CLI(Request& context) {' \
	'  String first = component("child", context);' \
	'  String second = component("child", context);' \
	'  DValue perf = request_perf();' \
	'  print(first, ",", second, ",", perf["component_resolve_count"].to_u64(), ",", perf["component_loaded_reuse_count"].to_u64());' \
	'}' >"$source_dir/entry.uce"

printf '%s\n' \
	'ONCE(Request& context) { context.call["once"] = context.call["once"].to_u64() + 1; }' \
	'COMPONENT(Request& context) { print(context.call["once"].to_u64()); }' >"$source_dir/child.uce"

output=$(scripts/bearer-cli "/$test_name/entry.uce")
if [[ "$output" != "1,1,1,0" ]]; then
	echo "component ONCE slot was not returned with the primary handler: $output" >&2
	exit 1
fi

echo "component ONCE prefetch passed"
