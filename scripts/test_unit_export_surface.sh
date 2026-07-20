#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="unit-export-surface-test-$$"
site_directory="${UCE_TEST_SITE_DIRECTORY:-site}"
bin_directory="${UCE_TEST_BIN_DIRECTORY:-/tmp/uce/work}"
if [[ -r /etc/uce/settings.cfg ]]; then
	if [[ -z "${UCE_TEST_SITE_DIRECTORY:-}" ]]; then
		configured_site_directory=$(awk -F= '/^[[:space:]]*HTTP_DOCUMENT_ROOT[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
		site_directory="${configured_site_directory:-$site_directory}"
	fi
	if [[ -z "${UCE_TEST_BIN_DIRECTORY:-}" ]]; then
		configured_bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
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
	'String visibility_unused() { return("uce-private-unused-marker-8f61d2"); }' \
	'EXPORT String visibility_shared() { return("shared"); }' \
	'CLI(Request& context) { print(visibility_shared(), ":", visibility_used(), ":", component("named:NAMED", context)); }' \
	>"$source_dir/entry.uce"
printf '%s\n' \
	'String named_used() { return("private-named-used"); }' \
	'String named_unused() { return("uce-named-unused-marker-4ae973"); }' \
	'EXPORT String named_shared() { return("named-export"); }' \
	'COMPONENT:NAMED(Request& context) { print(named_shared(), ":", named_used()); }' \
	>"$source_dir/named.uce"

output=$(scripts/uce-cli "/$test_name/entry.uce")
if [[ "$output" != "shared:private-used:named-export:private-named-used" ]]; then
	echo "unit export surface runtime failed: $output" >&2
	exit 1
fi

entry_wasm="$artifact_dir/entry.uce.wasm"
named_wasm="$artifact_dir/named.uce.wasm"
python3 - "$entry_wasm" "$named_wasm" "$absolute_source_dir" <<'PY'
import sys
from pathlib import Path

from scripts.check_unit_wasm import collect

cases = [
    (Path(sys.argv[1]), {"__wasm_call_ctors", "__uce_set_current_request", "__uce_cli", "visibility_shared"}, b"uce-private-unused-marker-8f61d2"),
    (Path(sys.argv[2]), {"__wasm_call_ctors", "__uce_set_current_request", "__uce_component_NAMED", "named_shared"}, b"uce-named-unused-marker-4ae973"),
]
source_dir = Path(sys.argv[3])
for path, allowed, unused_marker in cases:
    data = path.read_bytes()
    customs, imports, exports = collect(path)
    names = {name for name, _ in exports}
    unexpected = names - allowed
    missing = allowed - names
    if unexpected or missing:
        raise SystemExit(f"{path}: unexpected exports={sorted(unexpected)} missing={sorted(missing)} all={sorted(names)}")
    if unused_marker in data:
        raise SystemExit(f"{path}: retained unused private code marker")
    if len(imports) >= 40:
        raise SystemExit(f"{path}: retained {len(imports)} imports (expected fewer than 40)")
    if path.stat().st_size >= 1024 * 1024:
        raise SystemExit(f"{path}: artifact is {path.stat().st_size} bytes (expected under 1 MiB)")
    source_map = Path(str(path) + ".source-map")
    if not source_map.is_file() or source_map.stat().st_size >= 256 * 1024:
        raise SystemExit(f"{path}: missing or oversized source map")
    module = customs["uce.module"][-1].decode()
    lines = source_map.read_text().splitlines()
    if not lines or lines[0] != f"UCE_SOURCE_MAP_V1\t{module}":
        raise SystemExit(f"{path}: source map does not match wasm module identity")
    expected_source = str(source_dir / path.name.removesuffix(".wasm"))
    if not any(line.startswith("F\t") and line.endswith("\t" + expected_source) for line in lines):
        raise SystemExit(f"{path}: source map does not identify {expected_source}")
    if not any(line.startswith("L\t") for line in lines):
        raise SystemExit(f"{path}: source map has no address rows")
PY

echo "unit export surface passed"
