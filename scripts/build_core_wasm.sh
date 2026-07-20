#!/bin/bash
# Build the production W1 BEARER WASM core from the production runtime carve-out.
# Run on k-bearer from any working directory.
set -euo pipefail
cd "$(dirname "$0")/.."

SDK=${WASI_SDK:-/opt/wasi-sdk}
OUT=${BEARER_WASM_OUT:-/tmp/bearer/wasm-w1}
mkdir -p "$OUT" bin/wasm

if [ ! -x "$SDK/bin/clang++" ]; then
	echo "wasi-sdk clang++ not found; set WASI_SDK" >&2
	exit 1
fi

"$SDK/bin/clang++" --target=wasm32-wasip1 -mexec-model=reactor \
	-O1 -g -std=c++20 -fno-exceptions -fno-rtti \
	-D__BEARER_WASM_CORE__ \
	-I. -Isrc/lib \
	src/wasm/core.cpp -o "$OUT/core.wasm" \
	-Wl,--export-all \
	-Wl,--export=__heap_base \
	-Wl,--export=__stack_pointer \
	-Wl,--export-table \
	-Wl,--growable-table \
	-Wl,-z,stack-size=8388608 \
	-Wl,--allow-undefined-file=src/wasm/core_hostcalls.syms \
	-Wl,--no-entry \
	$(sed "s/^/-Wl,--export-if-defined=/" src/wasm/core_libc_exports.syms | tr "\n" " ")

cp "$OUT/core.wasm" bin/wasm/core.wasm
ls -lh "$OUT/core.wasm" bin/wasm/core.wasm
