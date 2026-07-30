#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
settings=/etc/bearer/settings.cfg
backup=$(mktemp)
cp "$settings" "$backup"
restore() {
	cp "$backup" "$settings"
	rm -f "$backup"
	systemctl restart bearer.service >/dev/null 2>&1 || true
	timeout 30 systemctl is-active --quiet bearer.service >/dev/null 2>&1 || true
	sleep 2
}
trap restore EXIT

set_value() {
	local key=$1 value=$2
	sed -i "s|^${key}=.*|${key}=${value}|" "$settings"
}
restart() {
	systemctl restart bearer.service
	timeout 30 systemctl is-active --quiet bearer.service
	sleep 2
}
expect() {
	local name=$1 expected=$2 actual=$3
	if [[ "$actual" != "$expected" ]]; then
		printf '%s mismatch\nexpected: %q\nactual:   %q\n' "$name" "$expected" "$actual" >&2
		exit 1
	fi
}

set_value TASK_WORKERS 1
set_value TASK_QUEUE_CAPACITY 1024
set_value TASK_EXECUTION_TIMEOUT_MS 30000
restart
expect "one task worker serializes work" "succeeded|succeeded|serial" "$(timeout 25 scripts/bearer-cli /tests/capy-task-concurrency.capy)"
expect "running cancellation, failure isolation, and worker recovery" "canceled|stopped|failed:handler_trap|failed:missing_handler|succeeded:recovered" "$(timeout 30 scripts/bearer-cli /tests/capy-task-runtime.capy)"

set_value TASK_WORKERS 2
restart
expect "configured task worker parallelism" "succeeded|succeeded|parallel" "$(timeout 20 scripts/bearer-cli /tests/capy-task-concurrency.capy)"

set_value TASK_WORKERS 1
set_value TASK_EXECUTION_TIMEOUT_MS 200
restart
expect "shared task compile/invocation deadline" "failed:execution_timeout" "$(timeout 15 scripts/bearer-cli /tests/capy-task-timeout.capy)"

set_value TASK_EXECUTION_TIMEOUT_MS 30000
set_value TASK_QUEUE_CAPACITY 1
restart
set +e
overflow=$(timeout 15 scripts/bearer-cli /tests/capy-task-overflow.capy 2>&1)
overflow_status=$?
set -e
if [[ $overflow_status -eq 0 || "$overflow" == *must-not-run* ]]; then
	printf 'bounded task queue did not fail closed: status=%s output=%q\n' "$overflow_status" "$overflow" >&2
	exit 1
fi

echo "dedicated task worker runtime passed"
