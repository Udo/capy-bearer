#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
bin="${TMPDIR:-/tmp}/test_dvalue_none_native.$$"
trap 'rm -f "$bin"' EXIT
compiler=$(command -v clang++ || command -v g++)
timeout --signal=TERM --kill-after=2s 30s "$compiler" -std=c++20 -fpermissive -I"$repo" "$repo/scripts/test_dvalue_none_native.cpp" -lpcre2-8 -o "$bin"
timeout --signal=TERM --kill-after=2s 15s "$bin"
