#!/usr/bin/env bash
set -euo pipefail

service_name="${UCE_TEST_SERVICE:-uce.service}"
socket_name="${UCE_TEST_SOCKET_SERVICE:-uce.socket}"
http_host="${UCE_TEST_HTTP_HOST:-uce.openfu.com}"
http_path="${UCE_TEST_HTTP_PATH:-/info/}"
requests="${UCE_TEST_RESTART_REQUESTS:-100}"
statuses=$(mktemp)
trap 'rm -f "$statuses"' EXIT

systemctl is-active --quiet "$socket_name"
systemctl is-active --quiet "$service_name"
socket_path=$(systemctl show -p Listen --value "$socket_name" | sed -n 's/ \+(Stream)$//p')
[[ -S "$socket_path" ]]
inode_before=$(stat -c %i "$socket_path")

(
	for ((i = 0; i < requests; i++)); do
		curl -sS --max-time 15 -o /dev/null -w '%{http_code}\n' \
			-H "Host: $http_host" "http://127.0.0.1${http_path}"
	done
) >"$statuses" &
load_pid=$!
sleep 0.05
systemctl restart "$service_name"
wait "$load_pid"

inode_after=$(stat -c %i "$socket_path")
[[ "$inode_after" = "$inode_before" ]]
[[ $(wc -l <"$statuses") -eq "$requests" ]]
if [[ $(grep -c '^200$' "$statuses") -ne "$requests" ]]; then
	sort "$statuses" | uniq -c >&2
	exit 1
fi
systemctl is-active --quiet "$socket_name"
systemctl is-active --quiet "$service_name"
echo "socket activation restart passed across $requests HTTP requests"
