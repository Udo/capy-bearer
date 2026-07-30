#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
bin="${TMPDIR:-/tmp}/test_task_queue_native.$$"
trap 'rm -f "$bin"' EXIT
compiler=$(command -v clang++ || command -v g++)
"$compiler" -std=c++20 -fpermissive -DTASK_QUEUE_TESTING -I"$repo" "$repo/scripts/test_task_queue_native.cpp" -o "$bin"
timeout --signal=TERM --kill-after=2s 20s "$bin"
