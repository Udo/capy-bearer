#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="component-deadline-test-$$"
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
	'CLI(Request& context) { DValue props; print(component("child", props, context)); }' >"$source_dir/parent.uce"
for i in $(seq 1 800); do
	printf 'String cold_component_pad_%s() { return("%s"); }\n' "$i" "$i"
done >"$source_dir/child.uce"
printf '%s\n' \
	'COMPONENT(Request& context) { <><strong>cold-component-deadline-ok</strong></> }' >>"$source_dir/child.uce"
rm -rf "$cache_dir"

output=$(scripts/uce-cli "/$test_name/parent.uce")
if [[ "$output" != *cold-component-deadline-ok* ]]; then
	echo "cold component compilation consumed the guest epoch budget: $output" >&2
	exit 1
fi

echo "cold component deadline passed"
