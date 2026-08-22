#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

socket_path="${BEARER_CLI_SOCKET:-/run/bearer/cli.sock}"
curl_timeout="${BEARER_CLI_TEST_TIMEOUT:-900}"
if [[ -z "${BEARER_CLI_SOCKET:-}" && -r /etc/bearer/settings.cfg ]]; then
	configured_socket=$(awk -F= '/^[[:space:]]*CLI_SOCKET_PATH[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
	if [[ -n "${configured_socket:-}" ]]; then
		socket_path="$configured_socket"
	fi
fi

skip_local_service_pages=0
action="run"
while [[ $# -gt 0 ]]; do
	case "$1" in
		--skip-local-service-pages)
			skip_local_service_pages=1
			shift
			;;
		--list)
			action="list"
			shift
			;;
		-h|--help)
			cat <<'USAGE'
Usage: scripts/run_cli_tests.sh [--skip-local-service-pages] [--list]

Runs the BEARER unit-based test suite through the runtime CLI socket.

Environment:
  BEARER_CLI_TEST_TIMEOUT  Per CLI runner group curl timeout in seconds (default: 900).
USAGE
			exit 0
			;;
		*)
			echo "unknown option: $1" >&2
			exit 2
			;;
	esac
done

if [[ ! -S "$socket_path" ]]; then
	echo "BEARER CLI socket not found: $socket_path" >&2
	exit 1
fi

base_url="http://localhost/tests/cli_runner.capy?action=${action}&skip_local_service_pages=${skip_local_service_pages}"
if [[ "$action" == "list" ]]; then
	curl -sS --max-time "$curl_timeout" --fail-with-body --unix-socket "$socket_path" "$base_url"
	exit 0
fi

if [[ "$action" == "run" ]]; then
	scripts/test_session_state_native.sh
	scripts/test_split_utf8_native.sh
	scripts/test_fcgi_forward_native.sh
	python3 scripts/check_capy_doc_examples.py --self-test
	python3 scripts/check_capy_doc_examples.py
	echo "== BEARER CLI health =="
	curl -sS --max-time "$curl_timeout" --fail-with-body --unix-socket "$socket_path" "${base_url}&group=native"
	scripts/test_wasm_core_smoke.sh
	scripts/test_capy_phase1.sh
	timeout --signal=TERM --kill-after=5s 120s scripts/test_task_workers_runtime.sh
	scripts/test_capy_reference.sh
	scripts/test_server_arguments.sh
	scripts/test_socket_activation.sh
fi
