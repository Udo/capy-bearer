#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

capyc=${CAPYC:-bin/capyc}
runs=${RUNS:-3}
ratio_limit=${RATIO_LIMIT:-4.0}
absolute_limit_ms=${ABSOLUTE_LIMIT_MS:-1000}
subprocess_timeout_seconds=${SUBPROCESS_TIMEOUT_SECONDS:-2}
root=$(mktemp -d /tmp/capy-compiler-scaling.XXXXXX)
trap 'rm -rf "$root"' EXIT

[[ "$runs" =~ ^[1-9][0-9]*$ ]] || { echo "RUNS must be a positive integer" >&2; exit 2; }
[[ -x "$capyc" ]] || { echo "Capy compiler is not executable: $capyc" >&2; exit 2; }

python3 - "$root" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])

def depth_source(depth):
    expression = "value"
    for _ in range(depth):
        expression = f"({expression} + value)"
    return f"function nested(value : s64) s64 {{ -> {expression} }}\nfunction CLI(request : dval) {{ print(nested(1)) }}\n"

def marker_source(statements):
    body = "\n".join("print([1][1])" for _ in range(statements))
    return f"function CLI(request : dval) {{\n{body}\n}}\n"

for depth in (18, 22):
    (root / f"depth-{depth}.capy").write_text(depth_source(depth))
for statements in (500, 2000):
    (root / f"markers-{statements}.capy").write_text(marker_source(statements))
PY

printf 'commit: %s\n' "$(git rev-parse HEAD)"
printf 'capyc: %s\n' "$(sha256sum "$capyc" | awk '{print $1}')"
printf 'runs: %s\n' "$runs"

python3 - "$capyc" "$root" "$runs" "$ratio_limit" "$absolute_limit_ms" "$subprocess_timeout_seconds" <<'PY'
import math
import statistics
import subprocess
import sys
import time

capyc, root, runs, ratio_limit, absolute_limit_ms, subprocess_timeout_seconds = sys.argv[1:]
runs = int(runs)
ratio_limit = float(ratio_limit)
absolute_limit_ns = int(absolute_limit_ms) * 1_000_000
subprocess_timeout_seconds = float(subprocess_timeout_seconds)
denominator_floor_ns = 1_000_000
if not math.isfinite(ratio_limit) or ratio_limit <= 0:
    raise SystemExit("RATIO_LIMIT must be a finite positive number")
if absolute_limit_ns <= 0:
    raise SystemExit("ABSOLUTE_LIMIT_MS must be a positive integer")
if not math.isfinite(subprocess_timeout_seconds) or subprocess_timeout_seconds <= 0:
    raise SystemExit("SUBPROCESS_TIMEOUT_SECONDS must be a finite positive number")

def run(path):
    try:
        subprocess.run([capyc, "--check", path], check=True, stdout=subprocess.DEVNULL, timeout=subprocess_timeout_seconds)
    except subprocess.TimeoutExpired:
        raise SystemExit(f"Capy compiler exceeded the {subprocess_timeout_seconds:.3f} second subprocess limit for {path}") from None

def check_shape(label, small_name, large_name):
    paths = {"small": f"{root}/{small_name}.capy", "large": f"{root}/{large_name}.capy"}
    run(paths["small"])
    run(paths["large"])
    samples = {"small": [], "large": []}
    for sample in range(runs):
        order = ("small", "large") if sample % 2 == 0 else ("large", "small")
        for size in order:
            started = time.perf_counter_ns()
            run(paths[size])
            samples[size].append(time.perf_counter_ns() - started)
    small = int(statistics.median(samples["small"]))
    large = int(statistics.median(samples["large"]))
    ratio = large / max(small, denominator_floor_ns)
    print(f"{label}: small {small / 1_000_000:.3f} ms, large {large / 1_000_000:.3f} ms, ratio {ratio:.3f}x")
    failures = []
    if ratio >= ratio_limit:
        failures.append(f"{label} ratio {ratio:.3f}x reached the {ratio_limit:.3f}x limit")
    if large >= absolute_limit_ns:
        failures.append(f"{label} large case took {large / 1_000_000:.3f} ms and reached the {absolute_limit_ns / 1_000_000:.3f} ms limit")
    return failures

failures = []
failures.extend(check_shape("inference depth 18 to 22", "depth-18", "depth-22"))
failures.extend(check_shape("source markers 500 to 2000", "markers-500", "markers-2000"))
if failures:
    raise SystemExit("\n".join(failures))
PY
