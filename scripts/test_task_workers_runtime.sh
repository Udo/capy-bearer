#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != "--inside" ]]; then
    exec timeout --signal=TERM --kill-after=5s 240s unshare --mount --fork --kill-child=TERM "$0" --inside
fi

root=$(mktemp -d /tmp/bearer-task-workers.XXXXXX)
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
mkdir -p "$root"/{bin,uploads,sessions,tasks,jobs,run}
cp etc/bearer/settings.cfg "$settings"
cat >>"$settings" <<CFG
BIN_DIRECTORY=$root/bin
TMP_UPLOAD_PATH=$root/uploads
SESSION_PATH=$root/sessions
TASK_DIRECTORY=$root/tasks
FCGI_SOCKET_PATH=$root/run/fastcgi.sock
FCGI_PORT=
FCGI_BIND_ADDRESS=
CLI_SOCKET_PATH=$root/run/cli.sock
HTTP_SOCKET_PATH=$root/run/http.sock
HTTP_PORT=
HTTP_BIND_ADDRESS=
WS_BROKER_SOCKET_PATH=$root/run/ws.sock
WORKER_COUNT=2
PROACTIVE_COMPILE_ENABLED=0
TASK_WORKERS=1
TASK_QUEUE_CAPACITY=1024
TASK_EXECUTION_TIMEOUT_MS=30000
CFG
mount --bind "$settings" /etc/bearer/settings.cfg
export BEARER_CLI_SOCKET="$root/run/cli.sock"
export BEARER_JOB_ROOT="$root/jobs"

set_value() {
    python3 - "$settings" "$1" "$2" <<'PY'
import sys
path, key, value = sys.argv[1:]
lines = open(path, encoding="utf-8").read().splitlines()
lines = [f"{key}={value}" if line.startswith(key + "=") else line for line in lines]
with open(path, "w", encoding="utf-8") as output:
    output.write("\n".join(lines) + "\n")
PY
}
stop_server() {
    [[ -n "$server_pid" ]] || return 0
    kill -TERM "$server_pid" 2>/dev/null || true
    for _ in $(seq 1 200); do kill -0 "$server_pid" 2>/dev/null || break; sleep 0.05; done
    if kill -0 "$server_pid" 2>/dev/null; then kill -KILL "$server_pid" 2>/dev/null || true; fi
    wait "$server_pid" 2>/dev/null || true
    server_pid=""
}
start_server() {
    stop_server
    rm -f "$root/run"/*.sock "$root/tasks/admission_healthy"
    bin/bearer_fastcgi.linux.bin >"$root/server.log" 2>&1 &
    server_pid=$!
    for _ in $(seq 1 600); do
        [[ -S "$BEARER_CLI_SOCKET" && -f "$root/tasks/admission_healthy" ]] && return 0
        kill -0 "$server_pid" 2>/dev/null || break
        sleep 0.05
    done
    cat "$root/server.log" >&2
    return 1
}
expect() {
    local name=$1 expected=$2 actual=$3
    if [[ "$actual" != "$expected" ]]; then
        printf '%s mismatch\nexpected: %q\nactual:   %q\n' "$name" "$expected" "$actual" >&2
        exit 1
    fi
}

scripts/test_task_workers.sh
start_server
expect "one task worker serializes work" "succeeded|succeeded|serial" "$(timeout 25 scripts/bearer-cli /tests/capy-task-concurrency.capy)"
expect "running cancellation and worker recovery" "canceled|stopped|failed:handler_trap|failed:missing_handler|succeeded:none|succeeded:before" "$(timeout 30 scripts/bearer-cli /tests/capy-task-runtime.capy)"
set_value TASK_WORKERS 2
start_server
expect "configured task worker parallelism" "succeeded|succeeded|parallel" "$(timeout 20 scripts/bearer-cli /tests/capy-task-concurrency.capy)"
set_value TASK_WORKERS 1
set_value TASK_EXECUTION_TIMEOUT_MS 200
start_server
expect "shared task deadline" "failed:execution_timeout" "$(timeout 15 scripts/bearer-cli /tests/capy-task-timeout.capy)"
set_value TASK_EXECUTION_TIMEOUT_MS 30000
set_value TASK_QUEUE_CAPACITY 1
start_server
set +e
overflow=$(timeout 15 scripts/bearer-cli /tests/capy-task-overflow.capy 2>&1)
overflow_status=$?
set -e
if [[ $overflow_status -eq 0 || "$overflow" == *must-not-run* ]] || ! grep -Eq '(^|[^[:alnum:]_])queue_full([^[:alnum:]_]|$)' <<<"$overflow"; then
    printf 'bounded task queue must fail with queue_full: status=%s output=%q\n' "$overflow_status" "$overflow" >&2
    exit 1
fi
stop_server
echo "isolated task worker runtime passed"
