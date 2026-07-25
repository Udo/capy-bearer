#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="unit-export-surface-test-$$"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
bin_directory="${BEARER_TEST_BIN_DIRECTORY:-/tmp/bearer/work}"
if [[ -r /etc/bearer/settings.cfg ]]; then
	if [[ -z "${BEARER_TEST_SITE_DIRECTORY:-}" ]]; then
		configured_site_directory=$(awk -F= '/^[[:space:]]*HTTP_DOCUMENT_ROOT[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
		site_directory="${configured_site_directory:-$site_directory}"
	fi
	if [[ -z "${BEARER_TEST_BIN_DIRECTORY:-}" ]]; then
		configured_bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
		bin_directory="${configured_bin_directory:-$bin_directory}"
	fi
fi
source_dir="$site_directory/$test_name"
artifact_dir=""

cleanup() {
	rm -rf "$source_dir"
	if [[ -n "$artifact_dir" ]]; then
		rm -rf "$artifact_dir"
	fi
}
trap cleanup EXIT
mkdir -p "$source_dir"
absolute_source_dir=$(realpath "$source_dir")
artifact_dir="$(scripts/unit_cache_directory "$bin_directory")$absolute_source_dir"

printf '%s\n' \
	'String visibility_used() { return("private-used"); }' \
	'String visibility_unused() { return("bearer-private-unused-marker-8f61d2"); }' \
	'EXPORT String visibility_shared() { return("shared"); }' \
	'CLI(Request& context) { print(visibility_shared(), ":", visibility_used(), ":", component("named:NAMED", context)); }' \
	>"$source_dir/entry.uce"
printf '%s\n' \
	'String named_used() { return("private-named-used"); }' \
	'String named_unused() { return("bearer-named-unused-marker-4ae973"); }' \
	'EXPORT String named_shared() { return("named-export"); }' \
	'COMPONENT:NAMED(Request& context) { print(named_shared(), ":", named_used()); }' \
	>"$source_dir/named.uce"

output=$(scripts/bearer-cli "/$test_name/entry.uce")
if [[ "$output" != "shared:private-used:named-export:private-named-used" ]]; then
	echo "unit export surface runtime failed: $output" >&2
	exit 1
fi

entry_wasm="$artifact_dir/entry.uce.wasm"
named_wasm="$artifact_dir/named.uce.wasm"
for wasm in "$entry_wasm" "$named_wasm"; do
	wasm-objdump -x "$wasm" >"$wasm.objdump"
	case "$wasm" in
		"$entry_wasm") expected='__wasm_call_ctors __bearer_set_current_request __bearer_cli visibility_shared'; marker='bearer-private-unused-marker-8f61d2' ;;
		*) expected='__wasm_call_ctors __bearer_set_current_request __bearer_component_NAMED named_shared'; marker='bearer-named-unused-marker-4ae973' ;;
	esac
	grep -aFq "$marker" "$wasm" && { echo "$wasm retained unused private code marker" >&2; exit 1; }
	# Export surface must be exactly the expected set (no missing, no unexpected).
	actual_names=$(awk '/^Export\[/{f=1;next} f&&/^ - /{if(match($0,/"[^"]*"$/)) print substr($0,RSTART+1,RLENGTH-2); next} f{exit}' "$wasm.objdump" | sort -u)
	expected_names=$(tr ' ' '\n' <<<"$expected" | sort -u)
	[[ "$actual_names" == "$expected_names" ]] || { echo "$wasm export surface mismatch: got [$actual_names] want [$expected_names]" >&2; exit 1; }
	# Import count and artifact size stay bounded.
	imports=$(awk '/^Import\[/{f=1;next} f&&/^ - /{n++; next} f{exit} END{print n+0}' "$wasm.objdump")
	[[ $imports -lt 40 ]] || { echo "$wasm retained $imports imports (expected fewer than 40)" >&2; exit 1; }
	[[ $(stat -c%s "$wasm") -lt 1048576 ]]
	# Source map matches the module identity and identifies the unit source.
	map="$wasm.source-map"
	[[ -s "$map" && $(stat -c%s "$map") -lt 262144 ]]
	map_header=$(head -1 "$map")
	[[ "$map_header" == BEARER_SOURCE_MAP_V1$'\t'* ]] || { echo "$map: bad header" >&2; exit 1; }
	grep -aFq "${map_header#*$'\t'}" "$wasm" || { echo "$map: module identity not in wasm" >&2; exit 1; }
	grep -q '^L' "$map"
	grep '^F' "$map" | grep -q "$(realpath "$source_dir")/$(basename "${wasm%.wasm}")$"
	rm -f "$wasm.objdump"
done

echo "unit export surface passed"
