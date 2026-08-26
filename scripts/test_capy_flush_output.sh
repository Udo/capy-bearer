#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

output=$(curl -fsS --max-time 30 -H 'Host: bearer.openfu.com' http://127.0.0.1/tests/capy-flush-output.capy)
[[ "$output" == "first|true|second" ]] || {
    printf 'flush_output ordering mismatch: %q\n' "$output" >&2
    exit 1
}
