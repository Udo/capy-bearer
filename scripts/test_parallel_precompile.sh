#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="parallel-precompile-test-$$"
test_root="/tmp/$test_name"
source_dir="$test_root/site"
bin_directory="$test_root/work"
precompile_timeout="${BEARER_TEST_PRECOMPILE_TIMEOUT:-120s}"
cleanup() { rm -rf "$test_root"; }
trap cleanup EXIT
mkdir -p "$source_dir"
artifact_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"
precompile() { timeout "$precompile_timeout" env BEARER_PRECOMPILE_FILES_IN="$source_dir" BEARER_PRECOMPILE_BIN_DIRECTORY="$bin_directory" "$@" bin/bearer_fastcgi.linux.bin --precompile; }
write_units() {
	local version=$1
	for unit in 0 1 2 3; do
		printf 'function CLI(request : dval) { print("parallel-precompile-%s-%s") }\n' "$version" "$unit" >"$source_dir/unit-$unit.capy"
	done
}
write_units serial
serial_output=$(precompile BEARER_PRECOMPILE_JOBS=1)
grep -Eq 'with 1 job: .* 4 compiled, 0 failed, worker status ok' <<<"$serial_output"
write_units parallel
parallel_output=$(precompile BEARER_PRECOMPILE_JOBS=2)
[[ $(grep -Ec '^Precompile worker [12]/2:' <<<"$parallel_output") -eq 2 ]]
grep -Eq 'with 2 jobs: .* 4 compiled, 0 failed, worker status ok' <<<"$parallel_output"
for unit in 0 1 2 3; do
	[[ -s "$artifact_dir/unit-$unit.capy.wasm" && -s "$artifact_dir/unit-$unit.capy.cwasm" ]]
done
printf '%s\n' 'function CLI(request : dval) { print(deliberate_parallel_precompile_failure) }' >"$source_dir/broken.capy"
set +e
failure_output=$(precompile BEARER_PRECOMPILE_JOBS=2 2>&1)
failure_rc=$?
set -e
[[ $failure_rc -ne 0 ]] && grep -Eq 'with 2 jobs: .* 1 failed, worker status failed' <<<"$failure_output"
rm "$source_dir/broken.capy"
for unit in $(seq -w 0 4); do
	printf 'function CLI(request : dval) { print("parallel-precompile-bounds-%s") }\n' "$unit" >"$source_dir/bounds-$unit.capy"
done
negative_jobs_output=$(precompile BEARER_PRECOMPILE_JOBS=-1)
malformed_jobs_output=$(precompile BEARER_PRECOMPILE_JOBS=invalid)
oversized_jobs_output=$(precompile BEARER_PRECOMPILE_JOBS=17)
grep -q 'with 1 job: 9 units,' <<<"$negative_jobs_output"
grep -q 'with 2 jobs: 9 units,' <<<"$malformed_jobs_output"
grep -q 'with 9 jobs: 9 units,' <<<"$oversized_jobs_output"
echo "parallel precompile passed"
