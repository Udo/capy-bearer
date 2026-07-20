#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

binary="$PWD/bin/bearer_fastcgi.linux.bin"
root=$(mktemp -d /tmp/bearer-server-arguments.XXXXXX)
listener_pid=""
cleanup() {
	[[ -n "$listener_pid" ]] && kill "$listener_pid" 2>/dev/null || true
	rm -rf "$root"
}
trap cleanup EXIT

cfg="$root/settings.cfg"
socket_path="$root/sentinel.sock"
cp /etc/bearer/settings.cfg "$cfg"
sed -E -i \
	-e "s|^[[:space:]]*FCGI_SOCKET_PATH[[:space:]]*=.*|FCGI_SOCKET_PATH=|" \
	-e "s|^[[:space:]]*FCGI_PORT[[:space:]]*=.*|FCGI_PORT=|" \
	-e "s|^[[:space:]]*CLI_SOCKET_PATH[[:space:]]*=.*|CLI_SOCKET_PATH=$socket_path|" \
	-e "s|^[[:space:]]*HTTP_PORT[[:space:]]*=.*|HTTP_PORT=|" \
	-e "s|^[[:space:]]*PROACTIVE_COMPILE_ENABLED[[:space:]]*=.*|PROACTIVE_COMPILE_ENABLED=false|" \
	-e "s|^[[:space:]]*WORKER_COUNT[[:space:]]*=.*|WORKER_COUNT=1|" \
	-e "s|^[[:space:]]*BIN_DIRECTORY[[:space:]]*=.*|BIN_DIRECTORY=$root/bin|" \
	-e "s|^[[:space:]]*TMP_UPLOAD_PATH[[:space:]]*=.*|TMP_UPLOAD_PATH=$root/upload|" \
	-e "s|^[[:space:]]*SESSION_PATH[[:space:]]*=.*|SESSION_PATH=$root/session|" \
	"$cfg"

python3 -c 'import socket,sys,time; s=socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.listen(); time.sleep(30)' "$socket_path" &
listener_pid=$!
for _ in $(seq 1 50); do
	[[ -S "$socket_path" ]] && break
	sleep 0.02
done
[[ -S "$socket_path" ]]
socket_inode=$(stat -c %i "$socket_path")

invoke() {
	local output="$1"
	shift
	timeout --signal=TERM --kill-after=1s 2s unshare --mount --fork \
		bash -c 'mount --bind "$1" /etc/bearer/settings.cfg; exec "$2" "${@:3}"' \
		_ "$cfg" "$binary" "$@" >"$output.stdout" 2>"$output.stderr"
}

for option in --help -h; do
	invoke "$root/help" "$option"
	grep -q '^Usage: bearer_fastcgi' "$root/help.stdout"
	grep -q -- '--precompile' "$root/help.stdout"
	[[ ! -s "$root/help.stderr" ]]
done

for arguments in '--unknown' '--precompile extra' '--help extra'; do
	read -r -a argv <<<"$arguments"
	set +e
	invoke "$root/invalid" "${argv[@]}"
	rc=$?
	set -e
	[[ $rc -eq 2 ]]
	grep -q '^invalid arguments$' "$root/invalid.stderr"
	grep -q '^Usage: bearer_fastcgi' "$root/invalid.stderr"
	[[ ! -s "$root/invalid.stdout" ]]
done

[[ -S "$socket_path" ]]
[[ "$(stat -c %i "$socket_path")" == "$socket_inode" ]]
kill -0 "$listener_pid"

echo 'server argument handling passed: help and invalid arguments exited before listener setup'
