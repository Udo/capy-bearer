#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

CXX=${CXX:-clang++}
OUTPUT=${1:-bin/capyc}
MODE=${2:-release}
FLAGS=(-std=c++20 -Wall -Wextra -Werror -pedantic -Isrc/capy)
if [[ "$MODE" == debug ]]; then
	FLAGS+=(-O0 -g)
else
	FLAGS+=(-O2 -DNDEBUG)
fi

mkdir -p "$(dirname "$OUTPUT")"
TEMPORARY="$OUTPUT.tmp.$$"
trap 'rm -f "$TEMPORARY"' EXIT
"$CXX" "${FLAGS[@]}" \
	src/capy/main.cpp \
	src/capy/compiler.cpp \
	src/capy/frontend.cpp \
	src/capy/wasm.cpp \
	-o "$TEMPORARY"
chmod 0755 "$TEMPORARY"
mv "$TEMPORARY" "$OUTPUT"
