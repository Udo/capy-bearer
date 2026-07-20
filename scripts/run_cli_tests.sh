#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

socket_path="${UCE_CLI_SOCKET:-/run/uce/cli.sock}"
curl_timeout="${UCE_CLI_TEST_TIMEOUT:-900}"
if [[ -z "${UCE_CLI_SOCKET:-}" && -r /etc/uce/settings.cfg ]]; then
	configured_socket=$(awk -F= '/^[[:space:]]*CLI_SOCKET_PATH[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
	if [[ -n "${configured_socket:-}" ]]; then
		socket_path="$configured_socket"
	fi
fi

include_kill=0
skip_local_service_pages=0
action="run"
while [[ $# -gt 0 ]]; do
	case "$1" in
		--include-wasm-kill|--include-kill)
			include_kill=1
			shift
			;;
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
Usage: scripts/run_cli_tests.sh [--include-wasm-kill] [--skip-local-service-pages] [--list]

Runs the UCE unit-based test suite through the runtime CLI socket.

Environment:
  UCE_CLI_TEST_TIMEOUT  Per CLI runner group curl timeout in seconds (default: 900).
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
	echo "UCE CLI socket not found: $socket_path" >&2
	exit 1
fi

base_url="http://localhost/tests/cli_runner.uce?action=${action}&include_kill=${include_kill}&skip_local_service_pages=${skip_local_service_pages}"
if [[ "$action" == "list" ]]; then
	curl -sS --max-time "$curl_timeout" --fail-with-body --unix-socket "$socket_path" "$base_url"
	exit 0
fi

if [[ "$action" == "run" ]]; then
	groups=(demo http site doc-gate-{1..30} security task-lifetime pool-isolation starter tcp)
	if [[ "$include_kill" == "1" ]]; then
		groups+=(wasm-kill)
	fi
	for group in "${groups[@]}"; do
		echo "== UCE CLI group: $group =="
		curl -sS --max-time "$curl_timeout" --fail-with-body --unix-socket "$socket_path" "${base_url}&group=${group}"
	done
	scripts/test_dependency_invalidation.sh
	scripts/test_abi_generation_rollout.sh
	scripts/test_parallel_precompile.sh
	timeout --signal=TERM --kill-after=5s 175s scripts/test_parallel_proactive_compile.sh
	scripts/test_cold_component_deadline.sh
	scripts/test_wasm_invocation_timeout.sh
	timeout --signal=TERM --kill-after=5s 130s scripts/test_wasm_compile_timeout.sh
	scripts/test_nested_component_props.sh
	scripts/test_component_once_prefetch.sh
	scripts/test_relative_component_cache.sh
	scripts/test_password_hashing.sh
	scripts/test_mysql_epoch_refresh.sh
	scripts/test_mysql_persistent_pool.sh
	scripts/test_log_timeliness.sh
	scripts/test_raw_http_request_log.sh
	scripts/test_component_resolution_ttl.sh
	timeout --signal=TERM --kill-after=5s 120s scripts/test_entry_freshness_ttl.sh
	scripts/test_unit_export_surface.sh
	timeout --signal=TERM --kill-after=5s 120s scripts/test_wasm_metadata_buffer.sh
	timeout --signal=TERM --kill-after=5s 150s scripts/test_wasm_metadata_deadline.sh
	timeout --signal=TERM --kill-after=5s 240s scripts/test_dynamic_compile_failures.sh
	scripts/test_wasm_source_locations.sh
	scripts/test_server_arguments.sh
	scripts/test_socket_activation.sh
fi
