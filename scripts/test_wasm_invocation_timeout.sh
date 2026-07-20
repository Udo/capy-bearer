#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

raw_calls=$(grep -En -- '(->|\.)call\(' src/wasm/worker.cpp)
if [[ $(wc -l <<<"$raw_calls") -ne 1 || "$raw_calls" != *'return(func.call(context, args));'* ]]; then
	echo "raw Wasmtime call bypasses call_guest(): $raw_calls" >&2
	exit 1
fi
if [[ "${1:-}" == "--static-only" ]]; then
	echo "wasm invocation timeout static call gate passed"
	exit 0
fi

settings_file="${BEARER_SETTINGS_FILE:-/etc/bearer/settings.cfg}"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
socket_path="${BEARER_CLI_SOCKET:-/run/bearer/cli.sock}"
bin_directory="${BIN_DIRECTORY:-/tmp/bearer/work}"
if [[ -r "$settings_file" ]]; then
	[[ -n "${BEARER_TEST_SITE_DIRECTORY:-}" ]] || site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file")
	[[ -n "${BEARER_CLI_SOCKET:-}" ]] || socket_path=$(awk -F= '/^[[:space:]]*CLI_SOCKET_PATH[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file")
	[[ -n "${BIN_DIRECTORY:-}" ]] || bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file")
	invocation_ms=$(awk -F= '/^[[:space:]]*WASM_INVOCATION_TIMEOUT_MS[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' "$settings_file")
fi
site_directory="${site_directory:-site}"
socket_path="${socket_path:-/run/bearer/cli.sock}"
bin_directory="${bin_directory:-/tmp/bearer/work}"
invocation_ms="${invocation_ms:-30000}"
test_name="invocation-timeout-test-$$"
source_dir="$site_directory/$test_name"
pid_file="/tmp/bearer-$test_name-worker"
cache_dir=""
body_file="/tmp/bearer-$test_name-body"

cleanup() {
	rm -rf "$source_dir"
	[[ -z "$cache_dir" ]] || rm -rf "$cache_dir"
	rm -f "$pid_file" "$body_file"
}
trap cleanup EXIT
mkdir -p "$source_dir"
cache_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"

printf '%s\n' \
	'CLI(Request& context) {' \
	'  if(context.get["warm"] == "1") { print("warm"); return; }' \
	"  file_put_contents(\"$pid_file\", std::to_string(request_perf()[\"worker_pid\"].to_u64()));" \
	'  while(true) time_precise();' \
	'}' >"$source_dir/hostcall-loop.uce"
printf '%s\n' \
	'CLI(Request& context) {' \
	'  if(context.get["warm"] == "1") { print("warm"); return; }' \
	'  if(context.get["quick"] == "1") { print(shell_exec("printf quick")); return; }' \
	'  if(context.get["status"] == "1") { DValue spec; spec["cmd"] = "exit 7"; spec["timeout_ms"] = (f64)500; print(shell_exec(spec)["exit_code"].to_u64()); return; }' \
	'  if(context.get["zero"] == "1") { DValue spec; spec["cmd"] = "printf zero"; spec["timeout_ms"] = (f64)0; print(shell_exec(spec)["stdout"].to_string()); return; }' \
	'  if(context.get["job"] == "1") { DValue spec; spec["cmd"] = "sleep 2"; spec["timeout_ms"] = (f64)5000; u64 job = shell_spawn(spec); f64 started = time_precise(); job_await(job, 300); u64 elapsed = (u64)((time_precise() - started) * 1000); job_cancel(job); print(elapsed); return; }' \
	"  file_put_contents(\"$pid_file\", std::to_string(request_perf()[\"worker_pid\"].to_u64()));" \
	'  print(shell_exec("printf shell-start; sleep 60 & printf shell-end"));' \
	'}' >"$source_dir/legacy-shell.uce"
printf '%s\n' \
	'CLI(Request& context) {' \
	'  if(context.get["warm"] == "1") { print("warm"); return; }' \
	"  file_put_contents(\"$pid_file\", std::to_string(request_perf()[\"worker_pid\"].to_u64()));" \
	'  while(true) sleep(60);' \
	'  print("sleep-end");' \
	'}' >"$source_dir/sleep.uce"
printf '%s\n' \
	'CLI(Request& context) { print(request_perf()["worker_pid"].to_u64(), "|health"); }' >"$source_dir/health.uce"

max_seconds=$(( (invocation_ms + 15000) / 1000 ))
(( max_seconds >= 10 )) || max_seconds=10

request() {
	local unit="$1"
	curl -sS --max-time "$max_seconds" -o "$body_file" -w '%{http_code}' --unix-socket "$socket_path" "http://localhost/$test_name/$unit"
}

same_worker_health() {
	local expected_pid="$1"
	for _ in $(seq 1 32); do
		local health
		health=$(curl -sS --max-time 5 --fail-with-body --unix-socket "$socket_path" "http://localhost/$test_name/health.uce")
		[[ "$health" == "$expected_pid|health" ]] && return 0
	done
	return 1
}

curl -sS --max-time "$max_seconds" --fail-with-body --unix-socket "$socket_path" "http://localhost/$test_name/hostcall-loop.uce?warm=1" >/dev/null
curl -sS --max-time "$max_seconds" --fail-with-body --unix-socket "$socket_path" "http://localhost/$test_name/legacy-shell.uce?warm=1" >/dev/null
curl -sS --max-time "$max_seconds" --fail-with-body --unix-socket "$socket_path" "http://localhost/$test_name/sleep.uce?warm=1" >/dev/null
curl -sS --max-time "$max_seconds" --fail-with-body --unix-socket "$socket_path" "http://localhost/$test_name/health.uce" >/dev/null
quick=$(curl -sS --max-time 5 --fail-with-body --unix-socket "$socket_path" "http://localhost/$test_name/legacy-shell.uce?quick=1")
[[ "$quick" == quick ]] || { echo "quick legacy shell returned: $quick" >&2; exit 1; }
exit_status=$(curl -sS --max-time 5 --fail-with-body --unix-socket "$socket_path" "http://localhost/$test_name/legacy-shell.uce?status=1")
[[ "$exit_status" == 7 ]] || { echo "structured shell lost exit status: $exit_status" >&2; exit 1; }
zero_timeout=$(curl -sS --max-time 5 --fail-with-body --unix-socket "$socket_path" "http://localhost/$test_name/legacy-shell.uce?zero=1")
[[ "$zero_timeout" == zero ]] || { echo "structured shell zero-timeout default changed: $zero_timeout" >&2; exit 1; }
job_elapsed=$(curl -sS --max-time 5 --fail-with-body --unix-socket "$socket_path" "http://localhost/$test_name/legacy-shell.uce?job=1")
[[ "$job_elapsed" =~ ^[0-9]+$ && "$job_elapsed" -ge 200 && "$job_elapsed" -lt 500 ]] || { echo "job_await two-call duration was ${job_elapsed}ms" >&2; exit 1; }

for unit in hostcall-loop.uce legacy-shell.uce sleep.uce; do
	rm -f "$pid_file"
	started_ns=$(date +%s%N)
	status=$(request "$unit")
	elapsed_ms=$(( ($(date +%s%N) - started_ns) / 1000000 ))
	[[ "$status" == "500" ]] || { echo "$unit returned HTTP $status" >&2; exit 1; }
	if [[ "$unit" == legacy-shell.uce ]]; then
		if (( invocation_ms <= 5000 )); then
			grep -q 'BEARER_INVOCATION_TIMEOUT:' "$body_file" || { echo "$unit lacked invocation-timeout classification" >&2; exit 1; }
			expected_ms=$invocation_ms
		else
			grep -q 'BEARER_HOSTCALL_TIMEOUT:' "$body_file" || { echo "$unit lacked hostcall-timeout classification" >&2; exit 1; }
			expected_ms=5000
		fi
	else
		grep -q 'BEARER_INVOCATION_TIMEOUT:' "$body_file" || { echo "$unit lacked invocation-timeout classification: $(cat "$body_file")" >&2; exit 1; }
		expected_ms=$invocation_ms
	fi
	(( elapsed_ms >= expected_ms - 1000 && elapsed_ms <= expected_ms + 3000 )) || { echo "$unit completed in ${elapsed_ms}ms; expected about ${expected_ms}ms" >&2; exit 1; }
	grep -q "$unit" "$body_file" || { echo "$unit lacked a source-mapped trace" >&2; exit 1; }
	[[ -s "$pid_file" ]] || { echo "$unit did not record its worker" >&2; exit 1; }
	worker_pid=$(<"$pid_file")
	same_worker_health "$worker_pid" || { echo "worker $worker_pid did not survive $unit" >&2; exit 1; }
done

echo "wasm invocation timeout passed (absolute ${invocation_ms}ms; sleep and process hostcalls bounded)"
