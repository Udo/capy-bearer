#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != --inside ]]; then
	exec timeout --signal=TERM --kill-after=5s 90s unshare --mount --fork --kill-child=TERM "$0" --inside
fi

root="/tmp/capy-compile-timeout-$$"
site="$root/site"
work="$root/work"
settings="$root/settings.cfg"
log="$root/service.log"
server_pid=""
request_timeout=5

cleanup() {
	status=$?
	if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
		kill -TERM "$server_pid" 2>/dev/null || true
		for _ in $(seq 1 100); do kill -0 "$server_pid" 2>/dev/null || break; sleep 0.02; done
		kill -KILL "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
	if (( status != 0 )) && [[ -r "$log" ]]; then cat "$log" >&2; fi
	rm -rf "$root"
	return "$status"
}
trap cleanup EXIT

mkdir -p "$site" "$work" "$root/run" "$root/session" "$root/upload"
sed -E '/^[[:space:]]*(BIN_DIRECTORY|PRECOMPILE_FILES_IN|SITE_DIRECTORY|FCGI_SOCKET_PATH|FCGI_PORT|CLI_SOCKET_PATH|WS_BROKER_SOCKET_PATH|HTTP_SOCKET_PATH|HTTP_SOCKET_MODE|HTTP_PORT|HTTP_BIND_ADDRESS|HTTP_DOCUMENT_ROOT|SESSION_PATH|TMP_UPLOAD_PATH|WASM_CORE_PATH|WASM_COMPILE_SCRIPT|WASM_INVOCATION_TIMEOUT_MS|WASM_EPOCH_PERIOD_MS|PROACTIVE_COMPILE_ENABLED|WORKER_COUNT|COMPILE_FAILURE_RETRY_SECONDS|COMPILER_GRAPH_MAX_DEPTH)[[:space:]]*=/d' \
	/etc/bearer/settings.cfg >"$settings"
cat >>"$settings" <<CFG
BIN_DIRECTORY=$work
PRECOMPILE_FILES_IN=$site
SITE_DIRECTORY=$site
FCGI_SOCKET_PATH=$root/run/fastcgi.sock
FCGI_PORT=
CLI_SOCKET_PATH=$root/run/cli.sock
WS_BROKER_SOCKET_PATH=$root/run/ws.sock
HTTP_SOCKET_PATH=
HTTP_PORT=
HTTP_DOCUMENT_ROOT=$site
SESSION_PATH=$root/session
TMP_UPLOAD_PATH=$root/upload
WASM_CORE_PATH=$(pwd)/bin/wasm/core.wasm
WASM_INVOCATION_TIMEOUT_MS=100
WASM_EPOCH_PERIOD_MS=20
PROACTIVE_COMPILE_ENABLED=0
WORKER_COUNT=1
COMPILE_FAILURE_RETRY_SECONDS=60
COMPILER_GRAPH_MAX_DEPTH=1
CFG
mount --bind "$settings" /etc/bearer/settings.cfg

cat >"$site/driver.capy" <<'CAPY'
function CLI(request : dval) {
    if string(request.query.health, "") == "1" {
        print("health")
        return
    }
    if string(request.query.graph, "") == "1" {
        if unit_compile("graph-parent.capy") { print("graph-unlimited") } else { print("graph-limited") }
        return
    }
    if unit_compile("slow.capy") { print("compiled") } else { print("failed") }
}
CAPY
printf '%s\n' 'function CLI(request : dval) { print("old") }' >"$site/slow.capy"

timeout 40s env BEARER_PRECOMPILE_FILES_IN="$site" BEARER_PRECOMPILE_BIN_DIRECTORY="$work" BEARER_PRECOMPILE_JOBS=1 bin/bearer_fastcgi.linux.bin --precompile >"$root/precompile.log" 2>&1 || { cat "$root/precompile.log" >&2; exit 1; }
cat >"$site/graph-child.capy" <<'CAPY'
#exports GraphChild
struct GraphChild { value : string }
CAPY
cat >"$site/graph-parent.capy" <<'CAPY'
#import "graph-child.capy" as child
type ImportedChild = child.GraphChild
function CLI(request : dval) { print("graph") }
CAPY
cache="$(scripts/unit_cache_directory "$work")$(realpath "$site")"
wasm="$cache/slow.capy.wasm"
[[ -s "$wasm" ]]
before=$(sha256sum "$wasm")

service_user=${BEARER_TEST_SERVICE_USER:-www-data}
service_uid=$(id -u "$service_user")
service_gid=$(id -g "$service_user")
find "$work" -type d -exec chown "$service_uid:$service_gid" {} +
chown -R "$service_uid:$service_gid" "$root/run" "$root/session" "$root/upload"
chown "$service_uid:$service_gid" "$root"

stop_server() {
	if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
		kill -TERM "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
	server_pid=""
}

start_server() {
	rm -f "$root/run/cli.sock"
	timeout --signal=TERM --kill-after=5s 60s setpriv --reuid="$service_uid" --regid="$service_gid" --clear-groups bin/bearer_fastcgi.linux.bin >"$log" 2>&1 &
	server_pid=$!
	for _ in $(seq 1 200); do [[ -S "$root/run/cli.sock" ]] && break; sleep 0.02; done
	[[ -S "$root/run/cli.sock" ]] || { cat "$log" >&2; exit 1; }
}

start_server

request() {
	local spec=$1 path=${1%%\?*}
	if [[ "$spec" == *\?* ]]; then
		timeout "${request_timeout}s" scripts/bearer-cli --get "$path" "${spec#*\?}"
	else
		timeout "${request_timeout}s" scripts/bearer-cli "$path"
	fi
}
for _ in $(seq 1 100); do
	if [[ "$(request '/driver.capy?health=1' 2>/dev/null || true)" == health ]]; then break; fi
	sleep 0.05
done
[[ "$(request '/driver.capy?health=1')" == health ]]
[[ "$(request '/driver.capy?graph=1')" == graph-limited ]] || { echo "COMPILER_GRAPH_MAX_DEPTH=1 did not limit nested imports" >&2; exit 1; }
stop_server
settings_text=$(</etc/bearer/settings.cfg)
settings_text=${settings_text/COMPILER_GRAPH_MAX_DEPTH=1/COMPILER_GRAPH_MAX_DEPTH=256}
printf '%s' "$settings_text" >/etc/bearer/settings.cfg
start_server
[[ "$(request '/driver.capy?graph=1')" == graph-unlimited ]] || { echo "compiler graph did not compile after the configured limit increased" >&2; exit 1; }

for i in $(seq 1 100000); do
	printf 'function compile_timeout_pad_%s() s64 { -> %s }\n' "$i" "$i"
done >"$site/slow.capy"
printf '%s\n' 'function CLI(request : dval) { print("new") }' >>"$site/slow.capy"
set +e
timeout_output=$(request /driver.capy 2>&1)
timeout_rc=$?
set -e
[[ $timeout_rc -ne 0 && "$timeout_output" == *BEARER_INVOCATION_TIMEOUT:* ]] || { echo "compile did not time out: rc=$timeout_rc output=$timeout_output" >&2; exit 1; }
[[ -s "$wasm" && "$(sha256sum "$wasm")" == "$before" ]] || { echo "compile timeout removed or changed the last known good Wasm artifact" >&2; exit 1; }
[[ ! -e "$cache/slow.capy.compile.txt" && ! -e "$cache/slow.capy.wasm-check.txt" ]] || { echo "compile timeout persisted a failure" >&2; exit 1; }

stop_server
settings_text=$(</etc/bearer/settings.cfg)
settings_text=${settings_text/WASM_INVOCATION_TIMEOUT_MS=100/WASM_INVOCATION_TIMEOUT_MS=30000}
printf '%s' "$settings_text" >/etc/bearer/settings.cfg
start_server
request_timeout=30
[[ "$(request '/driver.capy?health=1')" == health ]]
[[ "$(request /driver.capy)" == compiled ]] || { echo "request replayed a persisted compile failure" >&2; exit 1; }
[[ "$(request /slow.capy)" == new ]] || { echo "recovery did not publish the recompiled Capy unit" >&2; exit 1; }
[[ -s "$wasm" && "$(sha256sum "$wasm")" != "$before" ]] || { echo "recovery did not replace the old Wasm artifact" >&2; exit 1; }

echo "Capy compile timeout preserves the last known good artifact and retries"
