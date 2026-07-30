#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

root=$(mktemp -d /tmp/bearer-task-signatures.XXXXXX)
trap 'rm -rf "$root"' EXIT
mkdir -p "$root/out"

compile() {
	local source=$1 wasm=$2
	cp "$root/$source" "$root/out/$source"
	scripts/compile_wasm_unit "$root" "$root/out" "$source" "$source" "$wasm" "$root/out"
}

printf '%s\n' '#include "bearer_lib.h"' 'TASK(Request& request) {}' >"$root/default.cpp"
printf '%s\n' '#include "bearer_lib.h"' 'BEARER_NAMED_TASK(__bearer_task_NAME, (Request& request)) {}' >"$root/named.cpp"
printf '%s\n' '#include "bearer_lib.h"' 'CLI() {}' >"$root/ordinary.cpp"

compile default.cpp default.wasm
cp "$root/out/default.wasm" "$root/default-first.wasm"
cp "$root/out/default.wasm.source-map" "$root/default-first.map"
compile default.cpp default.wasm
cmp "$root/default-first.wasm" "$root/out/default.wasm"
cmp "$root/default-first.map" "$root/out/default.wasm.source-map"
compile named.cpp named.wasm
cp "$root/out/named.wasm" "$root/named-first.wasm"
cp "$root/out/named.wasm.source-map" "$root/named-first.map"
compile named.cpp named.wasm
cmp "$root/named-first.wasm" "$root/out/named.wasm"
cmp "$root/named-first.map" "$root/out/named.wasm.source-map"
grep -Fq "$root/out/named.cpp" "$root/out/named.wasm.source-map"
grep -q '^L' "$root/out/named.wasm.source-map"
compile ordinary.cpp ordinary.wasm

python3 - "$root/out/default.wasm" __bearer_task "$root/out/named.wasm" __bearer_task_NAME <<'PY'
import sys

def u32(data, at):
    value = shift = 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 0x7f) << shift
        if not byte & 0x80:
            return value, at
        shift += 7

def section(data, wanted):
    at = 8
    while at < len(data):
        ident, at = u32(data, at)
        size, at = u32(data, at)
        end = at + size
        if ident == wanted:
            return data[at:end]
        at = end
    raise AssertionError(f"missing section {wanted}")

def task_signature(path, wanted):
    data = open(path, 'rb').read()
    assert data[:4] == b'\0asm'
    types = []
    payload = section(data, 1)
    count, at = u32(payload, 0)
    for _ in range(count):
        assert payload[at] == 0x60
        at += 1
        argc, at = u32(payload, at)
        params = tuple(payload[at:at + argc])
        at += argc
        resultc, at = u32(payload, at)
        results = tuple(payload[at:at + resultc])
        at += resultc
        types.append((params, results))
    function_types = []
    payload = section(data, 3)
    count, at = u32(payload, 0)
    for _ in range(count):
        item, at = u32(payload, at)
        function_types.append(item)
    payload = section(data, 7)
    count, at = u32(payload, 0)
    for _ in range(count):
        length, at = u32(payload, at)
        name = payload[at:at + length].decode()
        at += length
        kind = payload[at]
        at += 1
        index, at = u32(payload, at)
        if name == wanted:
            assert kind == 0
            assert types[function_types[index]] == ((0x7f,), ())
            return
    raise AssertionError(f"missing {wanted}")

for wasm, export in zip(sys.argv[1::2], sys.argv[2::2]):
    task_signature(wasm, export)
PY

for kind in default named; do
	for case in zero extra value pointer const nonvoid; do
		case "$case" in
			zero) parameters='()'; body='{}' ;;
			extra) parameters='(Request& request, Request& extra)'; body='{}' ;;
			value) parameters='(Request request)'; body='{}' ;;
			pointer) parameters='(Request* request)'; body='{}' ;;
			const) parameters='(const Request& request)'; body='{}' ;;
			nonvoid) parameters='(Request& request)'; body='{ return 1; }' ;;
		esac
		if [[ "$kind" == default ]]; then
			printf '%s\n' '#include "bearer_lib.h"' "TASK$parameters $body" >"$root/invalid-$kind-$case.cpp"
		else
			printf '%s\n' '#include "bearer_lib.h"' "BEARER_NAMED_TASK(__bearer_task_NAME, $parameters) $body" >"$root/invalid-$kind-$case.cpp"
		fi
		if compile "invalid-$kind-$case.cpp" "invalid-$kind-$case.wasm" >"$root/$kind-$case.log" 2>&1; then
			echo "invalid $kind TASK $case signature compiled" >&2
			exit 1
		fi
		if [[ "$case" != nonvoid && "$case" != extra ]]; then
			grep -Fq 'TASK must have exact signature void(Request&)' "$root/$kind-$case.log"
		fi
	done
done

echo "TASK compiler signature checks passed"
