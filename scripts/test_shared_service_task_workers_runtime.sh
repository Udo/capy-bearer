#!/usr/bin/env bash
set -euo pipefail

if [[ "${BEARER_RUN_SHARED_SERVICE_TASK_WORKER_TESTS:-0}" != "1" ]]; then
	printf '%s\n' 'Set BEARER_RUN_SHARED_SERVICE_TASK_WORKER_TESTS=1 to modify /etc/bearer and restart bearer.service.' >&2
	exit 2
fi

cd "$(dirname "$0")/.."
settings=/etc/bearer/settings.cfg
backup=$(mktemp)
cp "$settings" "$backup"
restore() {
	local test_status=$? restore_status=0
	cp "$backup" "$settings" || restore_status=1
	rm -f "$backup"
	timeout 30 systemctl restart bearer.service >/dev/null 2>&1 || restore_status=1
	timeout 30 systemctl is-active --quiet bearer.service >/dev/null 2>&1 || restore_status=1
	sleep 2
	if [[ $restore_status -ne 0 ]]; then
		printf '%s\n' 'Failed to restore the shared Bearer service.' >&2
		exit 1
	fi
	exit "$test_status"
}
trap restore EXIT

set_value() {
	local key=$1 value=$2
	sed -i "s|^${key}=.*|${key}=${value}|" "$settings"
}
restart() {
	timeout 30 systemctl restart bearer.service
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
if [[ $overflow_status -eq 0 || "$overflow" == *must-not-run* ]] || ! grep -Eq '(^|[^[:alnum:]_])queue_full([^[:alnum:]_]|$)' <<<"$overflow"; then
	printf 'bounded task queue must fail with queue_full: status=%s output=%q\n' "$overflow_status" "$overflow" >&2
	exit 1
fi

echo "shared-service task worker runtime passed"
