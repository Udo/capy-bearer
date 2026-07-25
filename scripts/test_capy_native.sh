#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR=${TMPDIR:-/tmp}/capy-native-tests
mkdir -p "$BUILD_DIR"
COMMON=(-std=c++20 -Wall -Wextra -Werror -pedantic -Isrc/capy)
ABI_VERSION=$(awk '/BEARER_WASM_CORE_ABI_VERSION/ {print $3; exit}' src/wasm/abi.h)
PARITY_MANIFEST="$BUILD_DIR/capy-uce-parity.md"
needs_rebuild() {
	local output=$1
	shift
	[[ ! -x "$output" ]] || find "$@" -newer "$output" -print -quit | grep -q .
}
if needs_rebuild "$BUILD_DIR/capyc" src/capy scripts/build_capy.sh; then
	scripts/build_capy.sh "$BUILD_DIR/capyc" debug
else
	echo "Reusing $BUILD_DIR/capyc"
fi
"$BUILD_DIR/capyc" --check-stdlib src/capy/stdlib.capy src/capy/stdlib.embedded.h
python3 - <<'PY'
from pathlib import Path
import re
core = Path("src/wasm/core.cpp").read_text()
start = core.index("size_t bearer_dv_apply_brrb(")
end = core.index("// Copied BRRB transport", start)
assert all(int(operation) <= 21 for operation in re.findall(r"case (\d+):", core[start:end]))
stdlib = Path("src/capy/stdlib.capy").read_text()
assert not re.search(r"__bearer_dv_apply_brrb\((?:2[2-9]|[3-9]\d|1\d\d)", stdlib)
assert "function request_apply" not in stdlib
assert "case 152:" not in core[start:end]
PY
scripts/test_hardened_http_native.sh
scripts/test_crypto_operation_native.sh
scripts/test_capy_exact_handle_native.sh
"$BUILD_DIR/capyc" --parity-manifest "$PARITY_MANIFEST"
cmp "$PARITY_MANIFEST" docs/capy-uce-parity.md
[[ $(grep -c '^| `' "$PARITY_MANIFEST") -eq $(find site/doc/pages -maxdepth 1 -type f -name '*.txt' | wc -l) ]]

clang++ "${COMMON[@]}" src/capy/frontend.cpp scripts/test_capy_native_frontend.cpp \
	-o "$BUILD_DIR/frontend"
mapfile -t fixtures < <(git ls-files 'site/**/*.capy')
"$BUILD_DIR/frontend" "${fixtures[@]}"

clang++ "${COMMON[@]}" src/capy/wasm.cpp scripts/test_capy_native_wasm.cpp \
	-o "$BUILD_DIR/wasm"
"$BUILD_DIR/wasm"

clang++ "${COMMON[@]}" src/capy/frontend.cpp src/capy/wasm.cpp src/capy/compiler.cpp \
	scripts/test_capy_native_compiler.cpp -o "$BUILD_DIR/compiler"
"$BUILD_DIR/compiler"
wasm-validate /tmp/capy-native.wasm

! grep -Eq 'python3[[:space:]]+scripts/capy_(compiler|frontend|backend)\.py' scripts/compile_wasm_unit
"$BUILD_DIR/capyc" site/tests/capy-phase1.capy \
	-o "$BUILD_DIR/phase1.wasm" --source-map "$BUILD_DIR/phase1.wasm.source-map" --abi-version "$ABI_VERSION"
wasm-validate "$BUILD_DIR/phase1.wasm"
"$BUILD_DIR/capyc" --check-unit "$BUILD_DIR/phase1.wasm" "$ABI_VERSION"
mkdir -p "$BUILD_DIR/repeat"
"$BUILD_DIR/capyc" site/tests/capy-phase1.capy \
	-o "$BUILD_DIR/repeat/phase1.wasm" --source-map "$BUILD_DIR/repeat/phase1.wasm.source-map" --abi-version "$ABI_VERSION"
cmp "$BUILD_DIR/phase1.wasm" "$BUILD_DIR/repeat/phase1.wasm"
cmp "$BUILD_DIR/phase1.wasm.source-map" "$BUILD_DIR/repeat/phase1.wasm.source-map"
scripts/compile_wasm_unit . "$BUILD_DIR" site/tests/capy-phase1.capy unused.cpp wrapper.wasm "$BUILD_DIR"
wasm-validate "$BUILD_DIR/wrapper.wasm"

for fixture in capy-arc capy-loop-control capy-phase3 capy-closures capy-markup capy-dval-rich capy-cross; do
	"$BUILD_DIR/capyc" "site/tests/$fixture.capy" \
		-o "$BUILD_DIR/$fixture.wasm" --source-map "$BUILD_DIR/$fixture.wasm.source-map" --abi-version "$ABI_VERSION"
	wasm-validate "$BUILD_DIR/$fixture.wasm"
	"$BUILD_DIR/capyc" --check-unit "$BUILD_DIR/$fixture.wasm" "$ABI_VERSION"
done

for source in "${fixtures[@]}"; do
	artifact="$BUILD_DIR/${source//\//_}.wasm"
	"$BUILD_DIR/capyc" "$source" -o "$artifact" --source-map "$artifact.source-map" --abi-version "$ABI_VERSION"
	wasm-validate "$artifact"
	"$BUILD_DIR/capyc" --check-unit "$artifact" "$ABI_VERSION"
done

wasm-objdump -x "$BUILD_DIR/phase1.wasm" >"$BUILD_DIR/phase1.objdump"
! grep -q 'bearer_request_context_brrb\|bearer_response_set_\|bearer_\(print\|format\)_s64\|bearer_\(print\|format\)_u64\|bearer_\(print\|format\)_f64\|bearer_time\|bearer_file_\|bearer_unit_info_brrb\|bearer_units_list_brrb\|bearer_unit_compile\|bearer_codec\|bearer_regex\\|bearer_string_nonblank\|bearer_dv_merge_brrb\|bearer_sqlite_' "$BUILD_DIR/phase1.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-wide-scalars.capy.wasm" >"$BUILD_DIR/wide-scalars.objdump"
grep -q 'env.bearer_print_s64' "$BUILD_DIR/wide-scalars.objdump"
grep -q 'env.bearer_print_u64' "$BUILD_DIR/wide-scalars.objdump"
grep -q 'env.bearer_print_f64' "$BUILD_DIR/wide-scalars.objdump"
grep -q 'env.bearer_time' "$BUILD_DIR/wide-scalars.objdump"
grep -q 'env.bearer_time_precise' "$BUILD_DIR/wide-scalars.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-markup.capy.wasm" >"$BUILD_DIR/markup.objdump"
grep -q 'env.bearer_format_s64' "$BUILD_DIR/markup.objdump"
grep -q 'env.bearer_format_u64' "$BUILD_DIR/markup.objdump"
grep -q 'env.bearer_format_f64' "$BUILD_DIR/markup.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-files.capy.wasm" >"$BUILD_DIR/files.objdump"
for import in file_open file_read file_write file_seek file_tell file_fsync file_close file_temp file_unlink; do
	grep -q "env.bearer_$import" "$BUILD_DIR/files.objdump"
done
"$BUILD_DIR/capyc" site/tests/capy-final-parity.capy -o "$BUILD_DIR/final-parity.wasm" --source-map "$BUILD_DIR/final-parity.wasm.source-map" --abi-version "$ABI_VERSION"
wasm-validate "$BUILD_DIR/final-parity.wasm"
wasm-objdump -x "$BUILD_DIR/final-parity.wasm" >"$BUILD_DIR/final-parity.objdump"
for import in file_pread file_pwrite; do grep -q "env.bearer_$import" "$BUILD_DIR/final-parity.objdump"; done
grep -q 'env.bearer_text_parsing_brrb' "$BUILD_DIR/final-parity.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-strings.capy.wasm" >"$BUILD_DIR/strings.objdump"
grep -q 'env.bearer_string_substr' "$BUILD_DIR/strings.objdump"
grep -q 'env.bearer_string_strpos' "$BUILD_DIR/strings.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-codecs.capy.wasm" >"$BUILD_DIR/codecs.objdump"
grep -q 'env.bearer_codec' "$BUILD_DIR/codecs.objdump"
grep -q 'env.bearer_dv_f64_to_brrb' "$BUILD_DIR/codecs.objdump"
grep -q 'env.bearer_dv_f64_brrb' "$BUILD_DIR/codecs.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-regex.capy.wasm" >"$BUILD_DIR/regex.objdump"
grep -q 'env.bearer_regex_match' "$BUILD_DIR/regex.objdump"
grep -q 'env.bearer_regex_dval' "$BUILD_DIR/regex.objdump"
grep -q 'env.bearer_regex_text' "$BUILD_DIR/regex.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-coreutil.capy.wasm" >"$BUILD_DIR/coreutil.objdump"
for import in text_parsing_brrb route_path_brrb runtime_diagnostics_brrb; do
	grep -q "env.bearer_$import" "$BUILD_DIR/coreutil.objdump"
done
wasm-objdump -x "$BUILD_DIR/site_tests_capy-string-lists.capy.wasm" >"$BUILD_DIR/string-lists.objdump"
grep -q 'env.bearer_text_parsing_brrb' "$BUILD_DIR/string-lists.objdump"
grep -q 'env.bearer_string_nonblank' "$BUILD_DIR/string-lists.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-first-empty.capy.wasm" >"$BUILD_DIR/first-empty.objdump"
! grep -q 'env.bearer_string_nonblank' "$BUILD_DIR/first-empty.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-dval-merge.capy.wasm" >"$BUILD_DIR/dval-merge.objdump"
grep -q 'env.bearer_dv_merge_brrb' "$BUILD_DIR/dval-merge.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-sqlite.capy.wasm" >"$BUILD_DIR/sqlite.objdump"
for import in sqlite_connect sqlite_disconnect sqlite_error sqlite_query sqlite_insert_id sqlite_affected_rows; do
	grep -q "env.bearer_$import" "$BUILD_DIR/sqlite.objdump"
done
wasm-objdump -x "$BUILD_DIR/site_tests_capy-unit-admin.capy.wasm" >"$BUILD_DIR/unit-admin.objdump"
for import in unit_info_brrb units_list_brrb unit_compile; do
	grep -q "env.bearer_$import" "$BUILD_DIR/unit-admin.objdump"
done
"$BUILD_DIR/capyc" site/tests/capy-request-parity.capy -o "$BUILD_DIR/request-parity.wasm" --source-map "$BUILD_DIR/request-parity.wasm.source-map" --abi-version "$ABI_VERSION"
wasm-validate "$BUILD_DIR/request-parity.wasm"
"$BUILD_DIR/capyc" --check-unit "$BUILD_DIR/request-parity.wasm" "$ABI_VERSION"
wasm-objdump -x "$BUILD_DIR/request-parity.wasm" >"$BUILD_DIR/request-parity.objdump"
grep -q 'env.bearer_request_workspace_brrb' "$BUILD_DIR/request-parity.objdump"
! grep -q 'env.bearer_request_context_brrb\|env.bearer_redirect\|env.bearer_session_start' "$BUILD_DIR/request-parity.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-request-context.capy.wasm" >"$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_request_context_brrb' "$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_request_context_for_brrb' "$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_request_value' "$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_request_body' "$BUILD_DIR/request-context.objdump"
! grep -q 'env.bearer_response_set_status' "$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_response_set_header' "$BUILD_DIR/request-context.objdump"
CAPYC="$BUILD_DIR/capyc" scripts/test_capy_artifact_golden.sh

echo "native Capy frontend, Wasm, compiler, CLI, and tracked fixture checks passed"
