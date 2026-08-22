#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != "--inside" ]]; then
	exec timeout --signal=TERM --kill-after=5s 150s unshare --mount --fork --kill-child=TERM "$0" --inside
fi
root=$(mktemp -d /tmp/bearer-parallel-proactive.XXXXXX)
site="$root/site"
work="$root/work"
settings="$root/settings.cfg"
server_pid=""
cleanup() {
	if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
		kill -TERM "$server_pid" 2>/dev/null || true
		for _ in $(seq 1 200); do kill -0 "$server_pid" 2>/dev/null || break; sleep 0.05; done
		kill -KILL "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
	rm -rf "$root"
}
trap cleanup EXIT
mkdir -p "$site" "$work" "$root"/{run,session,upload,tasks}
cp etc/bearer/settings.cfg "$settings"
cat >>"$settings" <<CFG
BIN_DIRECTORY=$work
PRECOMPILE_FILES_IN=$site
SITE_DIRECTORY=$site
FCGI_SOCKET_PATH=$root/run/fastcgi.sock
FCGI_PORT=
CLI_SOCKET_PATH=$root/run/cli.sock
WS_BROKER_SOCKET_PATH=$root/run/ws.sock
HTTP_SOCKET_PATH=$root/run/http.sock
HTTP_PORT=
SESSION_PATH=$root/session
TMP_UPLOAD_PATH=$root/upload
WORKER_COUNT=1
PROACTIVE_COMPILE_ENABLED=1
PROACTIVE_COMPILE_JOBS=2
PROACTIVE_COMPILE_CHECK_INTERVAL=1
COMPILE_FAILURE_RETRY_SECONDS=2
SERVE_LAST_KNOWN_GOOD=0
CFG
mount --bind "$settings" /etc/bearer/settings.cfg
cat >"$site/common.capy" <<'EOF'
#exports Marker
struct Marker { value : s32 }
EOF
for unit in 0 1 2 3; do
	cat >"$site/unit-$unit.capy" <<EOF
#import"common.capy" as common
type ImportedMarker = common.Marker
function CLI(request : dval) { print("parallel-proactive-$unit") }
EOF
done
printf '%s\n' 'function CLI(request : dval) { print("removed") }' >"$site/removed.capy"
printf '%s\n' 'function CLI(request : dval) { print("baseline") }' >"$site/broken.capy"
export BEARER_CLI_SOCKET="$root/run/cli.sock"
bin/bearer_fastcgi.linux.bin >"$root/service.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 400); do [[ -S "$BEARER_CLI_SOCKET" ]] && break; sleep 0.05; done
[[ -S "$BEARER_CLI_SOCKET" ]] || { cat "$root/service.log" >&2; exit 1; }
generation="$(scripts/unit_cache_directory "$work")"
artifacts="$generation$(realpath "$site")"
for _ in $(seq 1 600); do
	ready=1
	for unit in 0 1 2 3; do [[ -s "$artifacts/unit-$unit.capy.wasm" && -s "$artifacts/unit-$unit.capy.cwasm" ]] || ready=0; done
	[[ $ready -eq 1 ]] && break
	sleep 0.1
done
[[ $ready -eq 1 ]] || { echo "parallel scanners did not publish all valid Capy units" >&2; cat "$root/service.log" >&2; exit 1; }
grep -q 'proactive compiler worker 1/2 ready' "$root/service.log"
grep -q 'proactive compiler worker 2/2 ready' "$root/service.log"
[[ "$(timeout 20 scripts/bearer-cli /unit-0.capy)" == parallel-proactive-0 ]]
old_meta=$(sha256sum "$artifacts/unit-0.capy.meta.txt" | awk '{print $1}')
printf '%s\n' '#exports Marker' 'struct Marker { value : s64 }' >"$site/common.capy"
for _ in $(seq 1 200); do
	new_meta=$(sha256sum "$artifacts/unit-0.capy.meta.txt" 2>/dev/null | awk '{print $1}')
	[[ "$new_meta" != "$old_meta" ]] && break
	sleep 0.1
done
[[ "$new_meta" != "$old_meta" ]] || { echo "common Capy dependency edit did not rebuild an importer" >&2; cat "$root/service.log" >&2; exit 1; }
for unit in 0 1 2 3; do [[ -s "$artifacts/unit-$unit.capy.cwasm" ]]; done
rm "$site/removed.capy"
for _ in $(seq 1 100); do ! grep -q "$site/removed.capy" "$generation/known-bearer-files.txt" 2>/dev/null && break; sleep 0.1; done
! grep -q "$site/removed.capy" "$generation/known-bearer-files.txt" 2>/dev/null || { echo "removed Capy unit remained tracked" >&2; exit 1; }
printf '%s\n' 'function CLI(request : dval) { print(deliberate_parallel_scanner_failure) }' >"$site/broken.capy"
for _ in $(seq 1 300); do [[ ! -e "$artifacts/broken.capy.wasm" ]] && grep -q 'deliberate_parallel_scanner_failure' "$root/service.log" && break; sleep 0.1; done
[[ ! -e "$artifacts/broken.capy.wasm" ]] || { echo "failed proactive Capy build published wasm" >&2; exit 1; }
printf '%s\n' 'function CLI(request : dval) { print("recovered") }' >"$site/broken.capy"
for _ in $(seq 1 150); do [[ -s "$artifacts/broken.capy.cwasm" ]] && break; sleep 0.1; done
[[ -s "$artifacts/broken.capy.cwasm" ]] || { echo "changed failed Capy source did not recover" >&2; cat "$root/service.log" >&2; exit 1; }
echo "parallel proactive compile passed"
