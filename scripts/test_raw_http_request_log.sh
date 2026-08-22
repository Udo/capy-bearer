#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

http_socket="${BEARER_RAW_HTTP_TEST_SOCKET:-}"
http_port="${BEARER_RAW_HTTP_TEST_PORT:-}"
if [[ -r /etc/bearer/settings.cfg ]]; then
	[[ -n "$http_socket" ]] || http_socket=$(awk -F= '/^[[:space:]]*HTTP_SOCKET_PATH[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
	[[ -n "$http_port" ]] || http_port=$(awk -F= '/^[[:space:]]*HTTP_PORT[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
fi
if [[ -n "$http_socket" ]]; then [[ -S "$http_socket" ]] || { echo "raw HTTP Unix socket is not ready: $http_socket" >&2; exit 1; }; transport=(--unix-socket "$http_socket" "http://localhost"); else transport=("http://127.0.0.1:${http_port:-8080}"); fi
request_path="${BEARER_RAW_HTTP_TEST_PATH:-/info/index.capy}"
marker="raw-http-log-$$-$(date +%s%N)"
separator='?'; [[ "$request_path" == *\?* ]] && separator='&'
started_at=$(date '+%Y-%m-%d %H:%M:%S')
base=${transport[-1]}; unset 'transport[-1]'
response=$(curl -fsS --max-time 15 "${transport[@]}" "${base}${request_path}${separator}__bearer_log_probe=${marker}")
[[ -n "$response" ]] || { echo "raw HTTP probe returned an empty response" >&2; exit 1; }
for _ in $(seq 1 30); do
	request_logs=$(journalctl -u bearer --since "$started_at" --no-pager | grep '(r)' | grep "$marker" || true)
	[[ $(grep -c '(r)' <<<"$request_logs" || true) -ge 2 ]] && break
	sleep 0.1
done
[[ $(grep -c '(r)' <<<"$request_logs" || true) -eq 2 ]] || { echo "expected exactly two raw HTTP request-stage records" >&2; printf '%s\n' "$request_logs" >&2; exit 1; }
[[ $(grep -c 'transport:http' <<<"$request_logs" || true) -eq 1 && $(grep -c 'transport:fastcgi' <<<"$request_logs" || true) -eq 1 ]] || { echo "raw HTTP request-stage records did not identify HTTP and FastCGI transports" >&2; exit 1; }
mapfile -t durations < <(sed -n 's/.*[[:space:]]\([0-9][0-9.]*\)s[[:space:]]*fps:.*/\1/p' <<<"$request_logs")
[[ ${#durations[@]} -eq 2 ]] || { echo "could not parse both raw HTTP request-stage durations" >&2; exit 1; }
for duration in "${durations[@]}"; do awk -v duration="$duration" 'BEGIN { exit !(duration > 0 && duration < 60) }'; done
echo "raw HTTP request log passed"
