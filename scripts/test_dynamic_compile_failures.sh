#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != "--inside" ]]; then
	exec timeout --signal=TERM --kill-after=5s 240s unshare --mount --net --fork --kill-child=TERM "$0" --inside
fi
ip link set lo up
root="/tmp/uce-dynamic-compile-failures-$$"
site="$root/site"
work="$root/work"
settings="$root/settings.cfg"
log="$root/service.log"
port=18080
server_pid=""
cleanup() {
	status=$?
	if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
		kill -TERM "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
	if (( status != 0 )) && [[ -r "$log" ]]; then cat "$log" >&2; fi
	while umount /etc/uce/settings.cfg 2>/dev/null; do :; done
	rm -rf "$root"
	return "$status"
}
trap cleanup EXIT
mkdir -p "$site" "$work" "$root/run" "$root/session" "$root/upload"
cat >"$site/driver.uce" <<'UCE'
RENDER(Request& context) {
  print("driver:");
  if(context.get["mode"] == "render") unit_render("broken-render", context);
  else if(context.get["mode"] == "call") unit_call("broken-call", "COMPONENT");
  else print(component("child", context));
}
UCE
printf '%s\n' 'COMPONENT(Request& context) { print("good-v1"); }' >"$site/child.uce"
printf '%s\n' 'RENDER(Request& context) { print("render-ok"); }' >"$site/broken-render.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("call-ok"); }' >"$site/broken-call.uce"

write_settings() {
	local show_errors="$1" serve_good="$2" proactive="$3"
	sed -E '/^[[:space:]]*(BIN_DIRECTORY|PRECOMPILE_FILES_IN|SITE_DIRECTORY|FCGI_SOCKET_PATH|FCGI_PORT|CLI_SOCKET_PATH|WS_BROKER_SOCKET_PATH|HTTP_PORT|HTTP_DOCUMENT_ROOT|SESSION_PATH|TMP_UPLOAD_PATH|WASM_CORE_PATH|PROACTIVE_COMPILE_ENABLED|PROACTIVE_COMPILE_CHECK_INTERVAL|PROACTIVE_COMPILE_JOBS|WORKER_COUNT|SHOW_DYNAMIC_COMPILE_ERRORS|SERVE_LAST_KNOWN_GOOD)[[:space:]]*=/d' \
		/etc/uce/settings.cfg >"$settings"
	cat >>"$settings" <<CFG
BIN_DIRECTORY=$work
PRECOMPILE_FILES_IN=$site
SITE_DIRECTORY=$site
FCGI_SOCKET_PATH=$root/run/fastcgi.sock
FCGI_PORT=
CLI_SOCKET_PATH=$root/run/cli.sock
WS_BROKER_SOCKET_PATH=$root/run/ws.sock
HTTP_PORT=$port
HTTP_DOCUMENT_ROOT=$site
SESSION_PATH=$root/session
TMP_UPLOAD_PATH=$root/upload
WASM_CORE_PATH=$(pwd)/bin/wasm/core.wasm
PROACTIVE_COMPILE_ENABLED=$proactive
PROACTIVE_COMPILE_CHECK_INTERVAL=1
PROACTIVE_COMPILE_JOBS=1
WORKER_COUNT=1
SHOW_DYNAMIC_COMPILE_ERRORS=$show_errors
SERVE_LAST_KNOWN_GOOD=$serve_good
CFG
	mount --bind "$settings" /etc/uce/settings.cfg
}
start_server() {
	: >"$log"
	bin/uce_fastcgi.linux.bin >"$log" 2>&1 &
	server_pid=$!
	for _ in $(seq 1 500); do
		curl -sS --max-time 1 "http://127.0.0.1:$port/driver.uce" >/dev/null 2>&1 && return
		kill -0 "$server_pid" 2>/dev/null || break
		sleep 0.02
	done
	cat "$log" >&2
	return 1
}
stop_server() {
	kill -TERM "$server_pid" 2>/dev/null || true
	wait "$server_pid" 2>/dev/null || true
	server_pid=""
}
request() {
	local method="${1:-GET}" query="${2:-}"
	if [[ "$method" == POST ]]; then
		curl -sS --max-time 30 -w $'\n%{http_code}' -X POST --data '' "http://127.0.0.1:$port/driver.uce?$query"
	else
		curl -sS --max-time 30 -w $'\n%{http_code}' "http://127.0.0.1:$port/driver.uce?$query"
	fi
}
body_of() { printf '%s' "${1%$'\n'*}"; }
status_of() { printf '%s' "${1##*$'\n'}"; }

write_settings 1 0 0
start_server
printf '%s\n' 'COMPONENT(Request& context) { UCE_DYNAMIC_COMPILE_MARKER }' >"$site/child.uce"
response=$(request GET)
body=$(body_of "$response")
[[ "$(status_of "$response")" == 500 ]]
[[ "$body" == *UCE_DYNAMIC_COMPILE_MARKER* ]]
[[ "$body" != *"component not found"* ]]
printf '%s\n' 'RENDER(Request& context) { UCE_RENDER_COMPILE_MARKER }' >"$site/broken-render.uce"
response=$(request GET mode=render)
[[ "$(status_of "$response")" == 500 && "$(body_of "$response")" == *UCE_RENDER_COMPILE_MARKER* ]]
printf '%s\n' 'COMPONENT(Request& context) { UCE_CALL_COMPILE_MARKER }' >"$site/broken-call.uce"
response=$(request GET mode=call)
[[ "$(body_of "$response")" == *UCE_CALL_COMPILE_MARKER* ]]
[[ "$(body_of "$response")" != *"function 'COMPONENT' not found"* ]]
printf '%s\n' 'COMPONENT(Request& context) { print("recovered"); }' >"$site/child.uce"
response=$(request GET)
[[ "$(status_of "$response")" == 200 && "$(body_of "$response")" == "driver:recovered" ]]
stop_server

write_settings 0 0 0
start_server
printf '%s\n' 'COMPONENT(Request& context) { UCE_HIDDEN_COMPILE_MARKER }' >"$site/child.uce"
response=$(request GET)
body=$(body_of "$response")
[[ "$(status_of "$response")" == 500 ]]
[[ "$body" == *"component not found: child"* ]]
[[ "$body" != *UCE_HIDDEN_COMPILE_MARKER* ]]
stop_server

printf '%s\n' 'COMPONENT(Request& context) { print("good-v1"); }' >"$site/child.uce"
write_settings 1 1 1
start_server
for _ in $(seq 1 100); do
	response=$(request GET)
	[[ "$(status_of "$response")" == 200 && "$(body_of "$response")" == "driver:good-v1" ]] && break
	sleep 0.1
done
[[ "$(status_of "$response")" == 200 && "$(body_of "$response")" == "driver:good-v1" ]]
cache="$(scripts/unit_cache_directory "$work")$(realpath "$site")"
for artifact in "$cache/child.uce.wasm" "$cache/child.uce.cwasm" "$cache/child.uce.wasm.source-map"; do [[ -s "$artifact" ]]; done
before_hashes=$(sha256sum "$cache/child.uce.wasm" "$cache/child.uce.cwasm" "$cache/child.uce.wasm.source-map")
printf '%s\n' 'COMPONENT(Request& context) { UCE_STALE_COMPILE_MARKER }' >"$site/child.uce"
for _ in $(seq 1 30); do
	response=$(request GET)
	[[ "$(status_of "$response")" == 200 && "$(body_of "$response")" == "driver:good-v1" ]]
	sleep 0.1
done
[[ -s "$cache/child.uce.compile.txt" ]]
[[ "$(sha256sum "$cache/child.uce.wasm" "$cache/child.uce.cwasm" "$cache/child.uce.wasm.source-map")" == "$before_hashes" ]]
stop_server
start_server
response=$(request GET)
[[ "$(status_of "$response")" == 200 && "$(body_of "$response")" == "driver:good-v1" ]] || { echo "last-known-good artifact did not survive worker restart: $response" >&2; exit 1; }
printf '%s\n' 'COMPONENT(Request& context) { print("permission-repair"); }' >"$site/child.uce"
chmod 000 "$site/child.uce"
for _ in $(seq 1 100); do
	grep -q "source file is not readable" "$cache/child.uce.compile.txt" 2>/dev/null && break
	sleep 0.05
done
[[ "$(sha256sum "$cache/child.uce.wasm" "$cache/child.uce.cwasm" "$cache/child.uce.wasm.source-map")" == "$before_hashes" ]]
response=$(request GET)
[[ "$(status_of "$response")" == 200 && "$(body_of "$response")" == "driver:good-v1" ]] || { echo "unreadable source removed last-known-good service: $response" >&2; exit 1; }
chmod 644 "$site/child.uce"
printf '%s\n' 'COMPONENT(Request& context) { print("good-v2"); }' >"$site/child.uce"
for _ in $(seq 1 200); do
	response=$(request GET)
	if [[ "$(status_of "$response")" == 200 && "$(body_of "$response")" == "driver:good-v2" ]]; then break; fi
	sleep 0.1
done
[[ "$(status_of "$response")" == 200 && "$(body_of "$response")" == "driver:good-v2" ]] || { echo "repaired unit was not published: $response" >&2; exit 1; }

echo "dynamic compile failure handling passed"
