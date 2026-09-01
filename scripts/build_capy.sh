#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

CXX=${CXX:-clang++}
OUTPUT=${1:-bin/capyc}
MODE=${2:-release}
FLAGS=(-std=c++20 -Wall -Wextra -Werror -pedantic -fuse-ld=lld -Isrc/capy)
if [[ "$MODE" == debug ]]; then
	FLAGS+=(-O0 -g)
else
	FLAGS+=(-O2 -DNDEBUG)
fi

mkdir -p "$(dirname "$OUTPUT")"
TEMPORARY="$OUTPUT.tmp.$$"
EMBEDDED=src/capy/stdlib.embedded.h
GENERATED="$EMBEDDED.tmp.$$"
BACKUP="$EMBEDDED.bak.$$"
EMBEDDED_REPLACED=0
cleanup() {
	status=$?
	rm -f "$TEMPORARY" "$GENERATED"
	if ((status != 0 && EMBEDDED_REPLACED)); then
		if [[ -f "$BACKUP" ]]; then
			mv "$BACKUP" "$EMBEDDED"
		else
			rm -f "$EMBEDDED"
		fi
	else
		rm -f "$BACKUP"
	fi
	exit "$status"
}
trap cleanup EXIT

build() {
	"$CXX" "${FLAGS[@]}" \
		src/capy/main.cpp \
		src/capy/compiler.cpp \
		src/capy/frontend.cpp \
		src/capy/wasm.cpp \
		src/capy/tools.cpp \
		-o "$1"
	chmod 0755 "$1"
}

if [[ -x "$OUTPUT" ]] && "$OUTPUT" --embed-stdlib src/capy/stdlib.capy "$GENERATED"; then
	:
else
	rm -f "$GENERATED"
	build "$TEMPORARY"
	"$TEMPORARY" --embed-stdlib src/capy/stdlib.capy "$GENERATED"
fi

if cmp -s "$GENERATED" "$EMBEDDED"; then
	rm -f "$GENERATED"
else
	[[ ! -f "$EMBEDDED" ]] || cp -p "$EMBEDDED" "$BACKUP"
	mv "$GENERATED" "$EMBEDDED"
	EMBEDDED_REPLACED=1
fi

build "$TEMPORARY"
mv "$TEMPORARY" "$OUTPUT"
