#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
cd ..

BUILDMODE=${2:-"debug"}
OPT_FLAG="O0"
GF="bearer_fastcgi"

mkdir -p bin/tmp bin/assets bin/wasm work
exec 9>bin/.build.lock
flock 9
build_tmp_files=()
cleanup_build_tmp() {
	((${#build_tmp_files[@]} == 0)) || rm -f "${build_tmp_files[@]}"
}
trap cleanup_build_tmp EXIT

COMPILER="clang++"
# -rdynamic is a link-time flag; the -c compiles below do not need it.
FLAGS="-g -w -Wall -$OPT_FLAG -std=c++20 -fpermissive -ffast-math"

# Wasmtime C++ API — needed only by the wasm backend object (src/wasm).
WASMTIME_HOME=${WASMTIME_HOME:-/opt/wasmtime}
WASM_FLAGS="-I$WASMTIME_HOME/include"
WASM_LIBS="-L$WASMTIME_HOME/lib -Wl,-rpath,$WASMTIME_HOME/lib -lwasmtime"

LIBS="-ldl -lm -lpthread -lpcre2-8 -lcrypto `mysql_config --cflags --libs` $WASM_LIBS"
SRCFLAGS="-D EXEC_NAME=\"$GF\" -D PLATFORM_NAME=\"linux\""

# The runtime is split into separately-compiled objects so an edit to one
# module no longer recompiles the others (notably: editing the wasm backend
# does not recompile the rest, and vendored SQLite is built once):
#   bin/sqlite3.o  vendored SQLite amalgamation (depends only on its own source)
#   bin/wasm.o     wasm backend + worker + wasmtime.hh (src/wasm)
#   bin/main.o     linux_fastcgi.cpp + the bearer_lib core amalgamation
# All link into the single -rdynamic binary. Delete bin/*.o to force a clean
# rebuild.

# Rebuild $1 if it is missing or anything under the remaining find-args is newer.
needs_rebuild() {
	local obj="$1"; shift
	[ ! -f "$obj" ] && return 0
	[ -n "$(find "$@" -newer "$obj" -print -quit 2>/dev/null)" ] && return 0
	return 1
}

# core.wasm: guest runtime loaded by the native wasm backend.
if needs_rebuild bin/wasm/core.wasm src/wasm/core.cpp src/lib src/wasm/core_hostcalls.syms src/wasm/core_libc_exports.syms scripts/build_core_wasm.sh; then
	echo "Compiling wasm core..."
	bash scripts/build_core_wasm.sh || exit 1
else
	echo "Reusing bin/wasm/core.wasm"
fi

# SQLite: vendored C, depends only on its own source (not our headers).
if needs_rebuild bin/sqlite3.o src/3rdparty/sqlite/sqlite3.c src/3rdparty/sqlite/sqlite3.h; then
	echo "Compiling SQLite..."
	tmp="bin/sqlite3.o.tmp.$$"
	build_tmp_files+=("$tmp")
	clang -g -O2 -fPIC \
		-DSQLITE_THREADSAFE=1 \
		-DSQLITE_OMIT_LOAD_EXTENSION=1 \
		-DSQLITE_DQS=0 \
		-DSQLITE_DEFAULT_FOREIGN_KEYS=1 \
		-DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
		-c src/3rdparty/sqlite/sqlite3.c -o "$tmp" 2>&1
	mv "$tmp" bin/sqlite3.o
else
	echo "Reusing bin/sqlite3.o"
fi

# wasm backend object: the wasm sources plus the lib headers it includes for
# declarations (not the lib .cpp — those are compiled into main.o).
if needs_rebuild bin/wasm.o src/wasm src/lib/*.h; then
	echo "Compiling wasm backend..."
	tmp="bin/wasm.o.tmp.$$"
	build_tmp_files+=("$tmp")
	time -p $COMPILER -c src/wasm/wasm_module.cpp $SRCFLAGS $FLAGS $WASM_FLAGS -o "$tmp" 2>&1
	mv "$tmp" bin/wasm.o
else
	echo "Reusing bin/wasm.o"
fi

# main object: the FastCGI entrypoint + the bearer_lib core amalgamation. Depends
# on linux_fastcgi.cpp, the whole lib tree, fcgicc, and the wasm backend header
# (its only view of the wasm object) — but not the wasm .cpp sources.
if needs_rebuild bin/main.o src/linux_fastcgi.cpp src/lib src/fastcgi src/wasm/backend.h src/wasm/abi.h; then
	echo "Compiling main..."
	tmp="bin/main.o.tmp.$$"
	build_tmp_files+=("$tmp")
	time -p $COMPILER -c src/linux_fastcgi.cpp $SRCFLAGS $FLAGS -o "$tmp" 2>&1
	mv "$tmp" bin/main.o
else
	echo "Reusing bin/main.o"
fi

echo "Linking..."
binary_tmp="bin/$GF.linux.bin.tmp.$$"
build_tmp_files+=("$binary_tmp")
$COMPILER -rdynamic bin/main.o bin/wasm.o bin/sqlite3.o $FLAGS $LIBS -o "$binary_tmp" 2>&1
test -s "$binary_tmp"
chmod 0755 "$binary_tmp"
mv "$binary_tmp" "bin/$GF.linux.bin"
ls -lh "bin/$GF.linux.bin"
