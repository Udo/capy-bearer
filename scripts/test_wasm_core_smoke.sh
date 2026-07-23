#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

wasmtime_root=${WASMTIME_C_API_ROOT:-/opt/wasmtime-v45.0.1-x86_64-linux-c-api}
binary=$(mktemp /tmp/bearer-w1-smoke.XXXXXX)
cleanup() { rm -f "$binary"; }
trap cleanup EXIT

g++ -std=c++20 -O2 \
	-I"$wasmtime_root/include" \
	src/wasm/w1_smoke.cpp \
	-L"$wasmtime_root/lib" -Wl,-rpath,"$wasmtime_root/lib" -lwasmtime \
	-o "$binary"
"$binary" bin/wasm/core.wasm
