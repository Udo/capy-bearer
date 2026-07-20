#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="parallel-precompile-test-$$"
test_root="/tmp/$test_name"
source_dir="$test_root/site"
bin_directory="$test_root/work"
absolute_source_dir=""
artifact_dir=""
generation_log=""
generation_pid=""
precompile_timeout="${UCE_TEST_PRECOMPILE_TIMEOUT:-120s}"

precompile() {
	timeout "$precompile_timeout" env UCE_PRECOMPILE_FILES_IN="$source_dir" UCE_PRECOMPILE_BIN_DIRECTORY="$bin_directory" "$@" bin/uce_fastcgi.linux.bin --precompile
}

cleanup() {
	if [[ -n "$generation_pid" ]] && kill -0 "$generation_pid" 2>/dev/null; then
		kill "$generation_pid" 2>/dev/null || true
		cleanup_deadline=$((SECONDS + 10))
		while kill -0 "$generation_pid" 2>/dev/null && (( SECONDS < cleanup_deadline )); do
			sleep 0.05
		done
		if kill -0 "$generation_pid" 2>/dev/null; then
			kill -KILL "$generation_pid" 2>/dev/null || true
		fi
		wait "$generation_pid" 2>/dev/null || true
	fi
	rm -rf "$test_root"
	if [[ -n "$generation_log" ]]; then
		rm -f "$generation_log"
	fi
}
trap cleanup EXIT
mkdir -p "$source_dir"
absolute_source_dir=$(realpath "$source_dir")
artifact_dir="$(scripts/unit_cache_directory "$bin_directory")$absolute_source_dir"

write_units() {
	local version="$1"
	for unit in 0 1 2 3; do
		printf 'CLI(Request& context) { print("parallel-precompile-%s-%s"); }\n' "$version" "$unit" >"$source_dir/unit-$unit.uce"
	done
}

printf 'CLI(Request& context) { print("parallel-precompile-warmup"); }\n' >"$source_dir/warmup.uce"
precompile UCE_PRECOMPILE_JOBS=1 UCE_WASM_PCH_DIR="$artifact_dir/pch-shared" >/dev/null
rm "$source_dir/warmup.uce"

write_units serial
serial_start=$(date +%s%N)
serial_output=$(precompile UCE_PRECOMPILE_JOBS=1 UCE_WASM_PCH_DIR="$artifact_dir/pch-shared")
serial_ns=$(( $(date +%s%N) - serial_start ))
if ! grep -Eq 'with 1 job: .* 4 compiled, 0 failed, worker status ok' <<<"$serial_output"; then
	echo "serial precompile did not compile the four controlled units" >&2
	echo "$serial_output" >&2
	exit 1
fi

write_units parallel
parallel_start=$(date +%s%N)
parallel_output=$(precompile UCE_PRECOMPILE_JOBS=2 UCE_WASM_PCH_DIR="$artifact_dir/pch-shared")
parallel_ns=$(( $(date +%s%N) - parallel_start ))
if [[ $(grep -Ec '^Precompile worker [12]/2:' <<<"$parallel_output") -ne 2 ]]; then
	echo "parallel precompile did not report both workers" >&2
	echo "$parallel_output" >&2
	exit 1
fi
if ! grep -Eq 'with 2 jobs: .* 4 compiled, 0 failed, worker status ok' <<<"$parallel_output"; then
	echo "parallel precompile did not compile the four controlled units" >&2
	echo "$parallel_output" >&2
	exit 1
fi
for unit in 0 1 2 3; do
	if [[ ! -s "$artifact_dir/unit-$unit.uce.wasm" || ! -s "$artifact_dir/unit-$unit.uce.cwasm" ]]; then
		echo "parallel precompile did not publish wasm and serialized artifacts for unit $unit" >&2
		exit 1
	fi
done

printf 'CLI(Request& context) { print("parallel-precompile-race-0"); }\n' >"$source_dir/race-0.uce"
printf 'CLI(Request& context) { print("parallel-precompile-race-1"); }\n' >"$source_dir/race-1.uce"
race_output=$(precompile UCE_PRECOMPILE_JOBS=2 UCE_WASM_PCH_DIR="$artifact_dir/pch-race")
if ! grep -Eq 'with 2 jobs: .* 2 compiled, 0 failed, worker status ok' <<<"$race_output" || \
	[[ $(find "$artifact_dir/pch-race" -maxdepth 1 -type f -name '*.pch' | wc -l) -ne 1 ]] || \
	find "$artifact_dir/pch-race" -maxdepth 1 -type f -name '*.tmp.*' -print -quit | grep -q .; then
	echo "parallel precompile did not publish exactly one clean shared PCH" >&2
	echo "$race_output" >&2
	exit 1
fi
rm "$source_dir/race-0.uce" "$source_dir/race-1.uce"

for unit in $(seq -w 0 9); do
	printf 'CLI(Request& context) { print("parallel-precompile-generation-%s"); }\n' "$unit" >"$source_dir/generation-$unit.uce"
done
generation_log="/tmp/$test_name-generation.log"
precompile UCE_PRECOMPILE_JOBS=2 UCE_WASM_PCH_DIR="$artifact_dir/pch-shared" >"$generation_log" 2>&1 &
generation_pid=$!
generation_deadline=$((SECONDS + 60))
while [[ ! -s "$artifact_dir/generation-0.uce.meta.txt" ]]; do
	if ! kill -0 "$generation_pid" 2>/dev/null; then
		echo "parallel precompile exited before the controlled source mutation" >&2
		wait "$generation_pid" || true
		cat "$generation_log" >&2
		exit 1
	fi
	if (( SECONDS >= generation_deadline )); then
		echo "timed out waiting for the controlled precompile mutation boundary" >&2
		exit 1
	fi
	sleep 0.05
done
printf 'CLI(Request& context) { print("parallel-precompile-generation-mutated"); }\n' >"$source_dir/generation-0.uce"
printf 'CLI(Request& context) { print("parallel-precompile-generation-added"); }\n' >"$source_dir/generation-new.uce"
if ! kill -0 "$generation_pid" 2>/dev/null; then
	echo "parallel precompile exited at the controlled source mutation boundary" >&2
	exit 1
fi
set +e
wait "$generation_pid"
generation_rc=$?
set -e
generation_pid=""
if [[ $generation_rc -eq 0 ]] || ! grep -q 'source generation changed during precompile; candidate generation rejected' "$generation_log"; then
	echo "parallel precompile accepted a source generation that changed during compilation" >&2
	cat "$generation_log" >&2
	exit 1
fi
if [[ -e "$artifact_dir/generation-new.uce.wasm" || -e "$artifact_dir/generation-new.uce.cwasm" ]]; then
	echo "parallel precompile published an added unit outside the rejected candidate source set" >&2
	exit 1
fi
generation_retry_output=$(precompile UCE_PRECOMPILE_JOBS=2 UCE_WASM_PCH_DIR="$artifact_dir/pch-shared")
if ! grep -Eq 'with 2 jobs: .* 2 compiled, 0 failed, worker status ok' <<<"$generation_retry_output"; then
	echo "parallel precompile did not reconcile the changed and added units on retry" >&2
	echo "$generation_retry_output" >&2
	exit 1
fi
rm -f "$generation_log"

printf 'CLI(Request& context) { print(deliberate_parallel_precompile_failure); }\n' >"$source_dir/broken.uce"
set +e
failure_output=$(precompile UCE_PRECOMPILE_JOBS=2 UCE_WASM_PCH_DIR="$artifact_dir/pch-shared" 2>&1)
failure_rc=$?
set -e
if [[ $failure_rc -eq 0 ]] || ! grep -Eq 'with 2 jobs: .* 1 failed, worker status failed' <<<"$failure_output"; then
	echo "parallel precompile did not aggregate a controlled worker failure" >&2
	echo "$failure_output" >&2
	exit 1
fi
set +e
repeat_failure_output=$(precompile UCE_PRECOMPILE_JOBS=2 UCE_WASM_PCH_DIR="$artifact_dir/pch-shared" 2>&1)
repeat_failure_rc=$?
set -e
if [[ $repeat_failure_rc -eq 0 ]] || ! grep -Eq 'with 2 jobs: .* 1 failed, worker status failed' <<<"$repeat_failure_output"; then
	echo "parallel precompile accepted a persisted current compile failure" >&2
	echo "$repeat_failure_output" >&2
	exit 1
fi
rm "$source_dir/broken.uce"

rm -f "$source_dir"/unit-*.uce
for unit in $(seq -w 0 4); do
	printf 'CLI(Request& context) { print("parallel-precompile-bounds-%s"); }\n' "$unit" >"$source_dir/bounds-$unit.uce"
done
negative_jobs_output=$(precompile UCE_PRECOMPILE_JOBS=-1)
malformed_jobs_output=$(precompile UCE_PRECOMPILE_JOBS=invalid)
oversized_jobs_output=$(precompile UCE_PRECOMPILE_JOBS=17)
if ! grep -q 'with 1 job: 16 units,' <<<"$negative_jobs_output" || ! grep -q 'with 2 jobs: 16 units,' <<<"$malformed_jobs_output" || ! grep -q 'with 16 jobs: 16 units,' <<<"$oversized_jobs_output"; then
	echo "precompile job bounds did not map negative/malformed/oversized input to 1/2/16" >&2
	exit 1
fi
rm -f "$source_dir"/bounds-*.uce "$source_dir"/generation-*.uce

printf 'parallel precompile passed: serial %.3fs, parallel %.3fs\n' \
	"$(awk -v ns="$serial_ns" 'BEGIN { print ns / 1000000000 }')" \
	"$(awk -v ns="$parallel_ns" 'BEGIN { print ns / 1000000000 }')"
