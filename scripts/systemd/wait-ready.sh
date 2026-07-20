#!/usr/bin/env bash
set -euo pipefail

socket_path="/run/uce/cli.sock"
if [[ -r /etc/uce/settings.cfg ]]; then
	configured_socket=$(awk -F= '/^[[:space:]]*CLI_SOCKET_PATH[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
	if [[ -n "${configured_socket:-}" ]]; then
		socket_path="$configured_socket"
	fi
fi

for ((attempt = 0; attempt < 200; attempt++)); do
	if [[ -S "$socket_path" ]] && [[ "$(curl -sS --max-time 0.2 --unix-socket "$socket_path" http://localhost/ping 2>/dev/null || true)" == "uce-cli: ok" ]]; then
		exit 0
	fi
	sleep 0.05
done

echo "UCE CLI socket did not become ready: $socket_path" >&2
exit 1
