#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR=${TMPDIR:-/tmp}/capy-native-tests
mkdir -p "$BUILD_DIR"
COMMON=(-std=c++20 -Wall -Wextra -Werror -pedantic -Isrc/capy)

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
	-o "$BUILD_DIR/phase1.wasm" --source-map "$BUILD_DIR/phase1.wasm.source-map" --abi-version 11
wasm-validate "$BUILD_DIR/phase1.wasm"
python3 scripts/check_unit_wasm.py "$BUILD_DIR/phase1.wasm" --abi-version 11
mkdir -p "$BUILD_DIR/repeat"
"$BUILD_DIR/capyc" site/tests/capy-phase1.capy \
	-o "$BUILD_DIR/repeat/phase1.wasm" --source-map "$BUILD_DIR/repeat/phase1.wasm.source-map" --abi-version 11
cmp "$BUILD_DIR/phase1.wasm" "$BUILD_DIR/repeat/phase1.wasm"
cmp "$BUILD_DIR/phase1.wasm.source-map" "$BUILD_DIR/repeat/phase1.wasm.source-map"
scripts/compile_wasm_unit . "$BUILD_DIR" site/tests/capy-phase1.capy unused.cpp wrapper.wasm "$BUILD_DIR"
wasm-validate "$BUILD_DIR/wrapper.wasm"

for fixture in capy-arc capy-loop-control capy-phase3 capy-closures capy-markup capy-dval-rich capy-cross; do
	"$BUILD_DIR/capyc" "site/tests/$fixture.capy" \
		-o "$BUILD_DIR/$fixture.wasm" --source-map "$BUILD_DIR/$fixture.wasm.source-map" --abi-version 11
	wasm-validate "$BUILD_DIR/$fixture.wasm"
	python3 scripts/check_unit_wasm.py "$BUILD_DIR/$fixture.wasm" --abi-version 11
done

for source in "${fixtures[@]}"; do
	artifact="$BUILD_DIR/${source//\//_}.wasm"
	"$BUILD_DIR/capyc" "$source" -o "$artifact" --source-map "$artifact.source-map" --abi-version 11
	wasm-validate "$artifact"
	python3 scripts/check_unit_wasm.py "$artifact" --abi-version 11
done

echo "native Capy frontend, Wasm, compiler, CLI, and tracked fixture checks passed"
