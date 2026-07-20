#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="nested-component-props-test-$$"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
if [[ -z "${BEARER_TEST_SITE_DIRECTORY:-}" && -r /etc/bearer/settings.cfg ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
	if [[ -n "${configured_site_directory:-}" ]]; then
		site_directory="$configured_site_directory"
	fi
fi
source_dir="$site_directory/$test_name"
threshold_ms="${BEARER_NESTED_COMPONENT_PROPS_MAX_MS:-1500}"
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
	'  context.props["sentinel"] = "caller";' \
	'  String payload; for(u64 i = 0; i < 4096; ++i) payload += "x";' \
	'  DValue props; for(u64 i = 0; i < 300; ++i) { DValue item; item = payload; props["items"].push(item); }' \
	'  String output = component("outer", props, context);' \
	'  if(context.props["sentinel"].to_string() != "caller") { print("caller props not restored"); return; }' \
	'  print(output);' \
	'}' >"$source_dir/parent.uce"

printf '%s\n' \
	'COMPONENT(Request& context) {' \
	'  if(context.props["items"]._map.size() != 300) { print("outer props missing"); return; }' \
	'  for(u64 i = 0; i < 300; ++i) {' \
	'    DValue props; props["index"] = std::to_string(i); component("leaf", props, context);' \
	'    if(context.props["items"]._map.size() != 300) { print("outer props not restored"); return; }' \
	'  }' \
	'  print("nested-component-props-ok");' \
	'}' >"$source_dir/outer.uce"

printf '%s\n' \
	'COMPONENT(Request& context) {' \
	'  if(context.props["index"].to_string() == "") print("leaf props missing");' \
	'}' >"$source_dir/leaf.uce"

warm_output=$(scripts/bearer-cli "/$test_name/parent.uce")
if [[ "$warm_output" != "nested-component-props-ok" ]]; then
	echo "nested component props warmup failed: $warm_output" >&2
	exit 1
fi

start_ns=$(date +%s%N)
output=$(scripts/bearer-cli "/$test_name/parent.uce")
elapsed_ms=$(( ($(date +%s%N) - start_ns) / 1000000 ))
if [[ "$output" != "nested-component-props-ok" ]]; then
	echo "nested component props failed: $output" >&2
	exit 1
fi
if (( elapsed_ms > threshold_ms )); then
	echo "nested component props took ${elapsed_ms}ms (limit ${threshold_ms}ms)" >&2
	exit 1
fi

echo "nested component props passed in ${elapsed_ms}ms"
