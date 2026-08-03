#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != "--inside" ]]; then
	exec timeout --signal=TERM --kill-after=5s 180s unshare --mount --fork --kill-child=TERM "$0" --inside
fi

header=src/wasm/abi.h
running_abi=$(awk '/^#define BEARER_WASM_CORE_ABI_VERSION / {print $3; exit}' "$header")
compiler_abi=$(awk '/^#define BEARER_COMPILER_UNIT_ABI_VERSION / {print $3; exit}' "$header")
next_abi=$((running_abi + 1))
root=$(mktemp -d /tmp/bearer-running-abi-pin.XXXXXX)
settings="$root/settings.cfg"
source_file="$root/site/entry.uce"
artifact="$root/bin/units-c${compiler_abi}-w${running_abi}${source_file}.wasm"
server_pid=""
cleanup()
{
	if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
		kill -TERM "$server_pid" 2>/dev/null || true
		for _ in $(seq 1 200); do kill -0 "$server_pid" 2>/dev/null || break; sleep 0.05; done
		kill -KILL "$server_pid" 2>/dev/null || true
		wait "$server_pid" 2>/dev/null || true
	fi
	rm -rf "$root"
}
trap cleanup EXIT
mkdir -p "$root"/{bin,site,uploads,sessions,tasks,jobs,run}
cp etc/bearer/settings.cfg "$settings"
python3 - "$settings" "$root" <<'PY'
from pathlib import Path
import sys
path, root = sys.argv[1:]
values = {
    "BIN_DIRECTORY": f"{root}/bin",
    "TMP_UPLOAD_PATH": f"{root}/uploads",
    "SESSION_PATH": f"{root}/sessions",
    "TASK_DIRECTORY": f"{root}/tasks",
    "SITE_DIRECTORY": f"{root}/site",
    "FCGI_SOCKET_PATH": f"{root}/run/fastcgi.sock",
    "FCGI_PORT": "",
    "HTTP_SOCKET_PATH": f"{root}/run/http.sock",
    "HTTP_PORT": "",
    "CLI_SOCKET_PATH": f"{root}/run/cli.sock",
    "WS_BROKER_SOCKET_PATH": f"{root}/run/ws.sock",
    "WORKER_COUNT": "1",
    "PROACTIVE_COMPILE_ENABLED": "0",
    "TASK_WORKERS": "1",
}
lines = Path(path).read_text().splitlines()
seen = set()
for index, line in enumerate(lines):
    key = line.split("=", 1)[0].strip()
    if key in values:
        lines[index] = f"{key}={values[key]}"
        seen.add(key)
for key in values.keys() - seen:
    lines.append(f"{key}={values[key]}")
Path(path).write_text("\n".join(lines) + "\n")
PY
sed "s/^#define BEARER_WASM_CORE_ABI_VERSION ${running_abi}$/#define BEARER_WASM_CORE_ABI_VERSION ${next_abi}/" "$header" >"$root/abi.h"
grep -q "^#define BEARER_WASM_CORE_ABI_VERSION ${next_abi}$" "$root/abi.h"
mount --bind "$settings" /etc/bearer/settings.cfg
mount --bind "$root/abi.h" "$PWD/$header"
printf '%s\n' 'CLI(Request& request) { (void)request; print("abi-pin-ok"); }' >"$source_file"
export BEARER_CLI_SOCKET="$root/run/cli.sock"
export BEARER_JOB_ROOT="$root/jobs"
bin/bearer_fastcgi.linux.bin >"$root/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 600); do
	[[ -S "$BEARER_CLI_SOCKET" && -f "$root/tasks/admission_healthy" ]] && break
	kill -0 "$server_pid" 2>/dev/null || { cat "$root/server.log" >&2; exit 1; }
	sleep 0.05
done
[[ -S "$BEARER_CLI_SOCKET" && -f "$root/tasks/admission_healthy" ]] || { cat "$root/server.log" >&2; exit 1; }

[[ "$(scripts/bearer-cli /entry.uce)" == abi-pin-ok ]]
grep -aq "unit_abi_version=${running_abi}" "$artifact"
if grep -aq "unit_abi_version=${next_abi}" "$artifact"; then
	echo "The unit used the source-tree ABI instead of the running core ABI." >&2
	exit 1
fi

echo "Running core ABI pin passed"
