#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

http_port="${BEARER_RAW_HTTP_TEST_PORT:-}"
if [[ -z "$http_port" && -r /etc/bearer/settings.cfg ]]; then
	http_port=$(awk -F= '/^[[:space:]]*HTTP_PORT[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
fi
http_port="${http_port:-8080}"
request_path="${BEARER_RAW_HTTP_TEST_PATH:-/info/index.uce}"
marker="raw-http-log-$$-$(date +%s%N)"
separator="?"
if [[ "$request_path" == *\?* ]]; then
	separator="&"
fi
started_at=$(date '+%Y-%m-%d %H:%M:%S')

response=$(curl -fsS --max-time 15 "http://127.0.0.1:${http_port}${request_path}${separator}__bearer_log_probe=${marker}")
if [[ -z "$response" ]]; then
	echo "raw HTTP probe returned an empty response" >&2
	exit 1
fi

request_logs=""
for _ in $(seq 1 30); do
	request_logs=$(journalctl -u bearer --since "$started_at" --no-pager | grep '(r)' | grep "$marker" || true)
	if [[ $(printf '%s\n' "$request_logs" | grep -c '(r)' || true) -ge 2 ]]; then
		break
	fi
	sleep 0.1
done

if [[ $(printf '%s\n' "$request_logs" | grep -c '(r)' || true) -ne 2 ]]; then
	echo "expected exactly two raw HTTP request-stage records" >&2
	printf '%s\n' "$request_logs" >&2
	exit 1
fi
if [[ $(printf '%s\n' "$request_logs" | grep -c 'transport:http' || true) -ne 1 ||
	$(printf '%s\n' "$request_logs" | grep -c 'transport:fastcgi' || true) -ne 1 ]]; then
	echo "raw HTTP request-stage records did not identify HTTP and FastCGI transports" >&2
	printf '%s\n' "$request_logs" >&2
	exit 1
fi

mapfile -t durations < <(printf '%s\n' "$request_logs" | sed -n 's/.*[[:space:]]\([0-9][0-9.]*\)s[[:space:]]*fps:.*/\1/p')
if [[ ${#durations[@]} -ne 2 ]]; then
	echo "could not parse both raw HTTP request-stage durations" >&2
	printf '%s\n' "$request_logs" >&2
	exit 1
fi
for duration in "${durations[@]}"; do
	awk -v duration="$duration" 'BEGIN { exit !(duration > 0 && duration < 60) }' || {
		echo "raw HTTP request-stage duration is outside (0, 60) seconds: ${duration}s" >&2
		printf '%s\n' "$request_logs" >&2
		exit 1
	}
done

echo "raw HTTP request log passed"
