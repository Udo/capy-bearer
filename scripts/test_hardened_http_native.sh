#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
bin="${TMPDIR:-/tmp}/test_hardened_http_native.$$"
trap 'rm -f "$bin"' EXIT
compiler=$(command -v clang++ || command -v g++)
"$compiler" -std=c++20 -fpermissive -I"$repo" "$repo/scripts/test_hardened_http_native.cpp" -lpcre2-8 -o "$bin"
timeout --signal=TERM --kill-after=2s 15s "$bin"
