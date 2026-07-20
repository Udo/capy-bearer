#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
bin_directory="${BEARER_TEST_BIN_DIRECTORY:-/tmp/bearer/work}"
if [[ -r /etc/bearer/settings.cfg ]]; then
	configured_site=$(awk -F= '/^[[:space:]]*HTTP_DOCUMENT_ROOT[[:space:]]*=/ {sub(/^[^=]*=/, ""); print; exit}' /etc/bearer/settings.cfg)
	configured_bin=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {sub(/^[^=]*=/, ""); print; exit}' /etc/bearer/settings.cfg)
	[[ -n "${BEARER_TEST_SITE_DIRECTORY:-}" ]] || site_directory="${configured_site:-$site_directory}"
	[[ -n "${BEARER_TEST_BIN_DIRECTORY:-}" ]] || bin_directory="${configured_bin:-$bin_directory}"
fi
site_directory=$(realpath "$site_directory")

python3 scripts/test_capy_compiler.py >/dev/null

output=$(scripts/bearer-cli /tests/capy-phase1.capy)
[[ "$output" == "capy-direct-ok" ]] || { echo "Capy CLI output mismatch: $output" >&2; exit 1; }

cache="$(scripts/unit_cache_directory "$bin_directory")$site_directory/tests/capy-phase1.capy"
[[ -s "$cache.wasm" && -s "$cache.cwasm" && -s "$cache.wasm.source-map" && -s "$cache.meta.txt" ]]
python3 scripts/check_unit_wasm.py "$cache.wasm" --abi-version "$(awk '/BEARER_WASM_CORE_ABI_VERSION/ {print $3; exit}' src/wasm/abi.h)"
wasm-objdump -x "$cache.wasm" >"$cache.objdump"
grep -q 'env.bearer_print_bytes' "$cache.objdump"
grep -q '__bearer_cli' "$cache.objdump"
! grep -q 'wasi_snapshot_preview1' "$cache.objdump"
rm -f "$cache.objdump"

fixture="capy-compile-recovery-$$"
source_dir="$site_directory/$fixture"
artifact_dir="$(scripts/unit_cache_directory "$bin_directory")$source_dir"
compiler_backup=$(mktemp)
cp -p scripts/capy_compiler.py "$compiler_backup"
cleanup() {
	cp -p "$compiler_backup" scripts/capy_compiler.py
	rm -f "$compiler_backup"
	rm -rf "$source_dir" "$artifact_dir"
}
trap cleanup EXIT

before_build=$(awk -F= '/^build_token=/ {print $2}' "$cache.meta.txt")
printf '\n# frontend invalidation fixture\n' >>scripts/capy_compiler.py
[[ "$(scripts/bearer-cli /tests/capy-phase1.capy)" == "capy-direct-ok" ]]
after_build=$(awk -F= '/^build_token=/ {print $2}' "$cache.meta.txt")
[[ -n "$before_build" && -n "$after_build" && "$before_build" != "$after_build" ]] || {
	echo "Capy frontend content change did not invalidate its compiled unit" >&2
	exit 1
}
cp -p "$compiler_backup" scripts/capy_compiler.py
[[ "$(scripts/bearer-cli /tests/capy-phase1.capy)" == "capy-direct-ok" ]]
mkdir -p "$source_dir"
for size in 63 64 127 128; do
	payload=$(printf '%*s' "$size" '' | tr ' ' x)
	printf 'function CLI { print("%s") }\n' "$payload" >"$source_dir/entry.capy"
	[[ "$(scripts/bearer-cli "/$fixture/entry.capy")" == "$payload" ]] || {
		echo "Capy signed-LEB output boundary failed at $size bytes" >&2
		exit 1
	}
done
render_prefix=$(printf '%*s' 64 '' | tr ' ' r)
printf 'function RENDER { print("%s") }\nfunction CLI { print("offset-ok") }\n' "$render_prefix" >"$source_dir/entry.capy"
[[ "$(scripts/bearer-cli "/$fixture/entry.capy")" == "offset-ok" ]] || {
	echo "Capy signed-LEB data offset boundary failed" >&2
	exit 1
}

printf '%s\n' 'function CLI { print(not_a_constant) }' >"$source_dir/entry.capy"
set +e
failure=$(scripts/bearer-cli "/$fixture/entry.capy" 2>&1)
status=$?
set -e
[[ $status -ne 0 ]]
[[ "$failure" == *"entry.capy:1:"* && "$failure" == *"print arguments must be string or integer constants"* ]]
[[ ! -e "$artifact_dir/entry.capy.wasm" ]]

printf '%s\n' 'function CLI { print("capy-recovered") }' >"$source_dir/entry.capy"
[[ "$(scripts/bearer-cli "/$fixture/entry.capy")" == "capy-recovered" ]]
[[ -s "$artifact_dir/entry.capy.wasm" ]]

echo "Capy phase 1 parser/direct-Wasm/CLI smoke passed"
