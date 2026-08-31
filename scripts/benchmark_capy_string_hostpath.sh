#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

capyc=${CAPYC:-bin/capyc}
runs=${RUNS:-10}
abi=$(awk '/^#define BEARER_WASM_CORE_ABI_VERSION / {print $3; exit}' src/wasm/abi.h)
root=$(mktemp -d /tmp/capy-string-hostpath-bench.XXXXXX)
trap 'rm -rf "$root"' EXIT

cat >"$root/minimal.capy" <<'CAPY'
function CLI(request : dval) {
    print("ok")
}
CAPY

cat >"$root/ten-call.capy" <<'CAPY'
function CLI(request : dval) {
    var raw := "  Capy <x>  "
    var a := trim(raw)
    var b := upper(raw)
    var c := lower(raw)
    var d := contains(raw, "Capy")
    var e := str_starts_with(raw, "  ")
    var f := str_ends_with(raw, "  ")
    var g := strpos(raw, "Capy")
    var h := replace(raw, "Capy", "Bearer")
    var i := safe_name(raw)
    var j := html_escape(raw)
    print(a, b, c, d, e, f, g, h, i, j)
}
CAPY

measure() {
    local name=$1 source=$2
    local times="$root/$name.times"
    : >"$times"
    for n in $(seq 1 "$runs"); do
        local output="$root/$name-$n.wasm"
        /usr/bin/time -f '%e' -o "$root/time" \
            "$capyc" "$source" -o "$output" \
            --source-map "$output.source-map" --abi-version "$abi" >/dev/null
        cat "$root/time" >>"$times"
    done
    local size median minimum maximum
    size=$(stat -c '%s' "$root/$name-1.wasm")
    median=$(sort -n "$times" | awk -v count="$runs" 'NR == int((count + 1) / 2) { low = $1 } NR == int(count / 2 + 1) { high = $1 } END { if (count % 2) printf "%.4f", low; else printf "%.4f", (low + high) / 2 }')
    minimum=$(sort -n "$times" | head -1)
    maximum=$(sort -n "$times" | tail -1)
    printf '%s: %s bytes, median %ss, min %ss, max %ss\n' "$name" "$size" "$median" "$minimum" "$maximum"
}

printf 'commit: %s\n' "$(git rev-parse HEAD)"
printf 'capyc: %s\n' "$(sha256sum "$capyc" | awk '{print $1}')"
printf 'runs: %s\n' "$runs"
measure minimal "$root/minimal.capy"
measure ten-call "$root/ten-call.capy"
