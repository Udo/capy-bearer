#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="wasm-source-location-test-$$"
site_directory="${UCE_TEST_SITE_DIRECTORY:-site}"
bin_directory="${UCE_TEST_BIN_DIRECTORY:-/tmp/uce/work}"
if [[ -r /etc/uce/settings.cfg ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*HTTP_DOCUMENT_ROOT[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
	configured_bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
	[[ -n "${UCE_TEST_SITE_DIRECTORY:-}" ]] || site_directory="${configured_site_directory:-$site_directory}"
	[[ -n "${UCE_TEST_BIN_DIRECTORY:-}" ]] || bin_directory="${configured_bin_directory:-$bin_directory}"
fi
source_dir="$site_directory/$test_name"
absolute_source_dir=""
artifact_dir=""

cleanup() {
	rm -rf "$source_dir"
	[[ -z "$artifact_dir" ]] || rm -rf "$artifact_dir"
}
trap cleanup EXIT
mkdir -p "$source_dir"
absolute_source_dir=$(realpath "$source_dir")
artifact_dir="$(scripts/unit_cache_directory "$bin_directory")$absolute_source_dir"
printf '%s\n' 'CLI(Request& context) { __builtin_trap(); }' >"$source_dir/entry.uce"

set +e
output=$(scripts/uce-cli "/$test_name/entry.uce" 2>&1)
status=$?
set -e
if [[ $status -eq 0 ]]; then
	echo "trapping wasm unit unexpectedly succeeded" >&2
	exit 1
fi
expected="$absolute_source_dir/entry.uce:1:25"
if [[ "$output" != *'wasm `unreachable` instruction executed'* || "$output" != *"source locations:"* || "$output" != *"$expected"* ]]; then
	echo "wasm trap omitted its source location:" >&2
	echo "$output" >&2
	exit 1
fi

rm "$artifact_dir/entry.uce.wasm.source-map"
set +e
without_map=$(scripts/uce-cli "/$test_name/entry.uce" 2>&1)
status=$?
set -e
if [[ $status -eq 0 || "$without_map" != *'wasm `unreachable` instruction executed'* || "$without_map" == *"source locations:"* ]]; then
	echo "missing-map trap fallback failed:" >&2
	echo "$without_map" >&2
	exit 1
fi

echo "wasm source locations passed"
