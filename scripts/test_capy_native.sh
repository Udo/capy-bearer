#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR=${TMPDIR:-/tmp}/capy-native-tests
mkdir -p "$BUILD_DIR"
COMMON=(-std=c++20 -Wall -Wextra -Werror -pedantic -Isrc/capy)
ABI_VERSION=$(awk '/BEARER_WASM_CORE_ABI_VERSION/ {print $3; exit}' src/wasm/abi.h)
PARITY_MANIFEST="$BUILD_DIR/capy-uce-parity.md"
scripts/build_capy_parity_manifest.py "$PARITY_MANIFEST"
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

scripts/build_capy.sh "$BUILD_DIR/capyc" debug
! grep -Eq 'python3[[:space:]]+scripts/capy_(compiler|frontend|backend)\.py' scripts/compile_wasm_unit
"$BUILD_DIR/capyc" site/tests/capy-phase1.capy \
	-o "$BUILD_DIR/phase1.wasm" --source-map "$BUILD_DIR/phase1.wasm.source-map" --abi-version "$ABI_VERSION"
wasm-validate "$BUILD_DIR/phase1.wasm"
python3 scripts/check_unit_wasm.py "$BUILD_DIR/phase1.wasm" --abi-version "$ABI_VERSION"
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
	python3 scripts/check_unit_wasm.py "$BUILD_DIR/$fixture.wasm" --abi-version "$ABI_VERSION"
done

for source in "${fixtures[@]}"; do
	artifact="$BUILD_DIR/${source//\//_}.wasm"
	"$BUILD_DIR/capyc" "$source" -o "$artifact" --source-map "$artifact.source-map" --abi-version "$ABI_VERSION"
	wasm-validate "$artifact"
	python3 scripts/check_unit_wasm.py "$artifact" --abi-version "$ABI_VERSION"
done

wasm-objdump -x "$BUILD_DIR/phase1.wasm" >"$BUILD_DIR/phase1.objdump"
! grep -q 'bearer_request_context_brrb\|bearer_response_set_\|bearer_\(print\|format\)_s64\|bearer_\(print\|format\)_u64\|bearer_\(print\|format\)_f64\|bearer_time\|bearer_file_\|bearer_unit_info_brrb\|bearer_units_list_brrb\|bearer_unit_compile\|bearer_codec\|bearer_regex\|bearer_string_list\|bearer_string_nonblank' "$BUILD_DIR/phase1.objdump"
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
wasm-objdump -x "$BUILD_DIR/site_tests_capy-codecs.capy.wasm" >"$BUILD_DIR/codecs.objdump"
grep -q 'env.bearer_codec' "$BUILD_DIR/codecs.objdump"
grep -q 'env.bearer_dv_f64_to_brrb' "$BUILD_DIR/codecs.objdump"
grep -q 'env.bearer_dv_f64_brrb' "$BUILD_DIR/codecs.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-regex.capy.wasm" >"$BUILD_DIR/regex.objdump"
grep -q 'env.bearer_regex_match' "$BUILD_DIR/regex.objdump"
grep -q 'env.bearer_regex' "$BUILD_DIR/regex.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-string-lists.capy.wasm" >"$BUILD_DIR/string-lists.objdump"
grep -q 'env.bearer_string_list' "$BUILD_DIR/string-lists.objdump"
grep -q 'env.bearer_string_nonblank' "$BUILD_DIR/string-lists.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-first-empty.capy.wasm" >"$BUILD_DIR/first-empty.objdump"
! grep -q 'env.bearer_string_nonblank' "$BUILD_DIR/first-empty.objdump"
wasm-objdump -x "$BUILD_DIR/site_tests_capy-unit-admin.capy.wasm" >"$BUILD_DIR/unit-admin.objdump"
for import in unit_info_brrb units_list_brrb unit_compile; do
	grep -q "env.bearer_$import" "$BUILD_DIR/unit-admin.objdump"
done
wasm-objdump -x "$BUILD_DIR/site_tests_capy-request-context.capy.wasm" >"$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_request_context_brrb' "$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_request_context_for_brrb' "$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_request_value' "$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_request_body' "$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_response_set_status' "$BUILD_DIR/request-context.objdump"
grep -q 'env.bearer_response_set_header' "$BUILD_DIR/request-context.objdump"

echo "native Capy frontend, Wasm, compiler, CLI, and tracked fixture checks passed"
