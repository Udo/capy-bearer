#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != "--inside" ]]; then
	exec timeout --signal=TERM --kill-after=5s 150s unshare --mount --fork --kill-child=TERM "$0" --inside
fi

name="parallel-proactive-test-$$"
root="/tmp/$name"
site="$root/site"
work="$root/work"
settings="$root/settings.cfg"
log="$root/service.log"
shim_log="$root/compile.tsv"
server_pid=""

cleanup() {
	if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
		kill -TERM "$server_pid" 2>/dev/null || true
		deadline=$((SECONDS + 10))
		while kill -0 "$server_pid" 2>/dev/null && (( SECONDS < deadline )); do sleep 0.05; done
		if kill -0 "$server_pid" 2>/dev/null; then kill -KILL "$server_pid" 2>/dev/null || true; fi
		wait "$server_pid" 2>/dev/null || true
	fi
	rm -rf "$root"
}
trap cleanup EXIT
mkdir -p "$site" "$work" "$root/run" "$root/session" "$root/upload"
cp /etc/bearer/settings.cfg "$settings"
cat >>"$settings" <<CFG
BIN_DIRECTORY=$work
PRECOMPILE_FILES_IN=$site
SITE_DIRECTORY=$site
FCGI_SOCKET_PATH=$root/run/fastcgi.sock
FCGI_PORT=
CLI_SOCKET_PATH=$root/run/cli.sock
WS_BROKER_SOCKET_PATH=$root/run/ws.sock
HTTP_PORT=
HTTP_DOCUMENT_ROOT=$site
SESSION_PATH=$root/session
TMP_UPLOAD_PATH=$root/upload
WASM_CORE_PATH=$(pwd)/bin/wasm/core.wasm
WASM_COMPILE_SCRIPT=$root/compile
WORKER_COUNT=1
PROACTIVE_COMPILE_ENABLED=1
PROACTIVE_COMPILE_JOBS=2
PROACTIVE_COMPILE_CHECK_INTERVAL=1
COMPILE_FAILURE_RETRY_SECONDS=2
SERVE_LAST_KNOWN_GOOD=0
CFG
mount --bind "$settings" /etc/bearer/settings.cfg

cat >"$root/compile" <<SHIM
#!/usr/bin/env bash
set -uo pipefail
root='$root'
real='$(pwd)/scripts/compile_wasm_unit'
source_file="\$3"
exec 9>>"\$root/counter.lock"
flock 9
active=0
if [[ -r "\$root/active" ]]; then read -r active <"\$root/active"; fi
active=\$((active + 1))
printf '%s\n' "\$active" >"\$root/active"
maximum=0
if [[ -r "\$root/maximum" ]]; then read -r maximum <"\$root/maximum"; fi
if (( active > maximum )); then printf '%s\n' "\$active" >"\$root/maximum"; fi
nice_value=\$(timeout 2s ps -o ni= -p "\$PPID" | tr -d ' ')
owner_pid=\$PPID
for _ in 1 2 3 4; do
	owner_command=\$(timeout 2s ps -o comm= -p "\$owner_pid" | tr -d ' ')
	[[ "\$owner_command" == bearer_fastcgi.* ]] && break
	owner_pid=\$(timeout 2s ps -o ppid= -p "\$owner_pid" | tr -d ' ')
	[[ -n "\$owner_pid" && "\$owner_pid" != "1" ]] || break
done
printf '%s\t%s\t%s\t%s\n' "\$(date +%s%N)" "\$nice_value" "\$source_file" "\$owner_pid" >>"\$root/compile.tsv"
flock -u 9
if [[ -r "\$root/hold-paths" ]] && grep -Fxq "\$source_file" "\$root/hold-paths"; then
		: >"\$root/held-\$owner_pid"
		deadline=\$((SECONDS + 20))
		while [[ ! -e "\$root/release" && \$SECONDS -lt \$deadline ]]; do sleep 0.05; done
		[[ -e "\$root/release" ]] || exit 98
fi
sleep 0.75
"\$real" "\$@"
rc=\$?
flock 9
active=1
if [[ -r "\$root/active" ]]; then read -r active <"\$root/active"; fi
printf '%s\n' "\$((active - 1))" >"\$root/active"
flock -u 9
exit "\$rc"
SHIM
chmod +x "$root/compile"

printf '%s\n' 'String common_marker() { return("common-a"); }' >"$site/common.uce"
for unit in 0 1 2 3; do
	printf '%s\n' '#load "common.uce"' "CLI(Request& context) { print(common_marker(), \"-$unit\"); }" >"$site/unit-$unit.uce"
done
printf '%s\n' 'CLI(Request& context) { print("removed"); }' >"$site/removed.uce"
printf '%s\n' 'CLI(Request& context) { print("baseline"); }' >"$site/broken.uce"

timeout --signal=TERM --kill-after=5s 120s bin/bearer_fastcgi.linux.bin >"$log" 2>&1 &
server_pid=$!
deadline=$((SECONDS + 20))
while [[ ! -S "$root/run/cli.sock" ]] && (( SECONDS < deadline )); do sleep 0.05; done
[[ -S "$root/run/cli.sock" ]] || { echo "private BEARER CLI socket was not ready" >&2; cat "$log" >&2; exit 1; }

generation="$(scripts/unit_cache_directory "$work")"
artifacts="$generation$(realpath "$site")"
deadline=$((SECONDS + 60))
while (( SECONDS < deadline )); do
	ready=1
	for unit in 0 1 2 3; do
		[[ -s "$artifacts/unit-$unit.uce.wasm" && -s "$artifacts/unit-$unit.uce.cwasm" ]] || ready=0
	done
	(( ready == 1 )) && break
	sleep 0.1
done
(( ready == 1 )) || { echo "parallel scanners did not publish all valid units" >&2; cat "$log" >&2; exit 1; }
[[ "$(<"$root/maximum")" -ge 2 ]] || { echo "parallel scanners never overlapped" >&2; cat "$shim_log" >&2; exit 1; }
grep -q 'proactive compiler worker 1/2 ready' "$log"
grep -q 'proactive compiler worker 2/2 ready' "$log"
for unit in 0 1 2 3; do
	[[ "$(grep -c "$site/unit-$unit.uce" "$shim_log")" -eq 1 ]] || { echo "unit-$unit was not compiled exactly once" >&2; cat "$shim_log" >&2; exit 1; }
done
deadline=$((SECONDS + 15))
while { [[ ! -s "$artifacts/broken.uce.cwasm" || ! -s "$artifacts/removed.uce.cwasm" ]] || [[ "$(<"$root/active")" != "0" ]]; } && (( SECONDS < deadline )); do sleep 0.1; done
[[ -s "$artifacts/broken.uce.cwasm" && -s "$artifacts/removed.uce.cwasm" && "$(<"$root/active")" == "0" ]] || { echo "phase fixtures did not become idle" >&2; cat "$log" >&2; exit 1; }
exec 7>>"$root/counter.lock"
flock 7
printf '0\n' >"$root/maximum"
flock -u 7
mapfile -t scanner_pids < <(awk -F '\t' -v site="$site/" '$3 ~ ("^" site "unit-[0-9]+[.]uce$") { count[$4]++ } END { for(pid in count) if(count[pid] >= 2) print pid }' "$shim_log" | sort -n)
[[ "${#scanner_pids[@]}" -eq 2 ]] || { echo "controlled units were not split between both scanners" >&2; cat "$shim_log" >&2; exit 1; }
blockers=()
victims=()
for scanner_pid in "${scanner_pids[@]}"; do
	mapfile -t scanner_units < <(awk -F '\t' -v pid="$scanner_pid" -v site="$site/" '$4 == pid && $3 ~ ("^" site "unit-[0-9]+[.]uce$") { print $3 }' "$shim_log")
	blockers+=("${scanner_units[0]}")
	victims+=("${scanner_units[1]}")
done
printf '%s\n' "${blockers[@]}" >"$root/hold-paths"
rm -f "$root"/held-* "$root/release"
sed -i 's/common-a/common-b/' "$site/common.uce"
deadline=$((SECONDS + 20))
while { [[ ! -e "$root/held-${scanner_pids[0]}" ]] || [[ ! -e "$root/held-${scanner_pids[1]}" ]]; } && (( SECONDS < deadline )); do sleep 0.05; done
[[ -e "$root/held-${scanner_pids[0]}" && -e "$root/held-${scanner_pids[1]}" ]] || { echo "both scanners did not reach their controlled queue barriers" >&2; cat "$log" >&2; exit 1; }
for victim in "${victims[@]}"; do
	victim_marker=$(BEARER_CLI_SOCKET="$root/run/cli.sock" timeout 20s scripts/bearer-cli "/$(basename "$victim")")
	[[ "$victim_marker" == *"common-b-"* ]] || { echo "request worker did not publish queued victim: $victim_marker" >&2; exit 1; }
done
touch "$root/release"
deadline=$((SECONDS + 30))
while (( SECONDS < deadline )); do
	rebuilt=1
	for unit in 0 1 2 3; do
		[[ "$(grep -c "$site/unit-$unit.uce" "$shim_log" || true)" -ge 2 ]] || rebuilt=0
	done
	(( rebuilt == 1 )) && break
	sleep 0.1
done
(( rebuilt == 1 )) || { echo "common dependency edit did not rebuild all scanner shards" >&2; cat "$log" >&2; exit 1; }
deadline=$((SECONDS + 5))
while [[ "$(<"$root/active")" != "0" ]] && (( SECONDS < deadline )); do sleep 0.1; done
sleep 1.25
for unit in 0 1 2 3; do
	[[ "$(grep -c "$site/unit-$unit.uce" "$shim_log")" -eq 2 ]] || { echo "unit-$unit common-dependency rebuild was not exact" >&2; cat "$shim_log" >&2; exit 1; }
done
rm -f "$root/hold-paths" "$root"/held-* "$root/release"
[[ "$(<"$root/maximum")" -ge 2 ]] || { echo "common dependency fanout did not overlap across scanner owners" >&2; cat "$shim_log" >&2; exit 1; }
marker=$(BEARER_CLI_SOCKET="$root/run/cli.sock" timeout 20s scripts/bearer-cli /unit-0.uce)
[[ "$marker" == *"common-b-0"* ]] || { echo "rebuilt parent did not execute the changed dependency: $marker" >&2; exit 1; }
rm "$site/removed.uce"
registry="$generation/known-bearer-files.txt"
deadline=$((SECONDS + 5))
while grep -q "$site/removed.uce" "$registry" 2>/dev/null && (( SECONDS < deadline )); do sleep 0.1; done
if grep -q "$site/removed.uce" "$registry" 2>/dev/null; then echo "removed unit remained tracked" >&2; cat "$log" >&2; exit 1; fi

printf '%s\n' 'CLI(Request& context) { print(deliberate_parallel_scanner_failure); }' >"$site/broken.uce"
deadline=$((SECONDS + 30))
while [[ "$(grep -c "$site/broken.uce" "$shim_log" || true)" -lt 3 ]] && (( SECONDS < deadline )); do sleep 0.1; done
[[ "$(grep -c "$site/broken.uce" "$shim_log" || true)" -ge 3 ]] || { echo "persisted scanner failure was not retried" >&2; cat "$log" >&2; exit 1; }
mapfile -t broken_times < <(awk -F '\t' -v path="$site/broken.uce" '$3 == path { print $1 }' "$shim_log")
(( broken_times[2] - broken_times[1] >= 2000000000 )) || { echo "scanner failure retry ignored its backoff" >&2; cat "$shim_log" >&2; exit 1; }
[[ ! -e "$artifacts/broken.uce.wasm" ]] || { echo "failed scanner build published wasm" >&2; exit 1; }
printf '%s\n' 'CLI(Request& context) { print("recovered"); }' >"$site/broken.uce"
deadline=$((SECONDS + 8))
while [[ ! -s "$artifacts/broken.uce.cwasm" ]] && (( SECONDS < deadline )); do sleep 0.1; done
[[ -s "$artifacts/broken.uce.cwasm" ]] || { echo "changed failed source waited for the unchanged-failure backoff" >&2; cat "$log" >&2; exit 1; }

printf '%s\n' 'CLI(Request& context) { print("priority"); }' >"$site/priority.uce"
priority_file="$generation/proactive-priority.txt"
exec 8>"$priority_file.lock"
flock 8
printf '%s\n' "$(realpath "$site/priority.uce")" >>"$priority_file"
flock -u 8
deadline=$((SECONDS + 15))
while [[ ! -s "$artifacts/priority.uce.cwasm" ]] && (( SECONDS < deadline )); do sleep 0.1; done
[[ -s "$artifacts/priority.uce.cwasm" ]] || { echo "priority compiler did not publish requested unit" >&2; cat "$log" >&2; exit 1; }
priority_nice=$(awk -F '\t' -v path="$site/priority.uce" '$3 == path { print $2; exit }' "$shim_log")
[[ "$priority_nice" == "5" ]] || { echo "priority queue was not owned by the nice-5 compiler: $priority_nice" >&2; cat "$shim_log" >&2; exit 1; }

printf '%s\n' 'parallel proactive compile passed'
