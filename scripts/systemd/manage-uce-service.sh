#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID:-0} -ne 0 ]]; then
	echo "This script must run as root." >&2
	exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
UNIT_NAME="uce.service"
UNIT_SOURCE="$SCRIPT_DIR/uce.service"
if [[ ( "$REPO_ROOT" == "/usr/lib/uce" || "$REPO_ROOT" == "/opt/uce" ) && -f "$REPO_ROOT/scripts/deb/uce.service" ]]; then
	UNIT_SOURCE="$REPO_ROOT/scripts/deb/uce.service"
fi
UNIT_DEST="/etc/systemd/system/$UNIT_NAME"
CONFIG_SOURCE="$REPO_ROOT/etc/uce/settings.cfg"
CONFIG_DEST="/etc/uce/settings.cfg"

action="${1:-setup}"
precompile_timeout="${UCE_PRECOMPILE_TIMEOUT:-900s}"

render_runtime_file() {
	local source="$1"
	local destination="$2"
	local mode="$3"
	local tmp
	tmp="$(mktemp)"
	trap 'rm -f "$tmp"' RETURN
	sed "s#/Code/uce.openfu.com/uce#$REPO_ROOT#g" "$source" > "$tmp"
	install -D -m "$mode" "$tmp" "$destination"
	rm -f "$tmp"
	trap - RETURN
}

install_unit() {
	render_runtime_file "$UNIT_SOURCE" "$UNIT_DEST" 0644
	if [[ ! -f "$CONFIG_DEST" ]]; then
		render_runtime_file "$CONFIG_SOURCE" "$CONFIG_DEST" 0644
	fi
	systemctl daemon-reload
}

case "$action" in
	install)
		install_unit
		;;
	setup)
		install_unit
		systemctl enable --now "$UNIT_NAME"
		;;
	enable)
		install_unit
		systemctl enable "$UNIT_NAME"
		;;
	restart)
		if [[ "$REPO_ROOT" != "/usr/lib/uce" && "$REPO_ROOT" != "/opt/uce" ]]; then
			"$REPO_ROOT/scripts/build_linux.sh"
		fi
		(
			cd "$REPO_ROOT"
			service_user=$(systemctl show "$UNIT_NAME" -p User --value)
			if [[ -n "$service_user" && "$(id -u)" == "0" ]]; then
				runuser -u "$service_user" -- timeout --signal=TERM --kill-after=10s "$precompile_timeout" nice -n 10 "$REPO_ROOT/bin/uce_fastcgi.linux.bin" --precompile
			else
				timeout --signal=TERM --kill-after=10s "$precompile_timeout" nice -n 10 "$REPO_ROOT/bin/uce_fastcgi.linux.bin" --precompile
			fi
		)
		systemctl restart "$UNIT_NAME"
		;;
	start|stop|status)
		systemctl "$action" "$UNIT_NAME"
		;;
	logs)
		lines="${2:-100}"
		journalctl -u "$UNIT_NAME" -n "$lines" --no-pager
		;;
	*)
		echo "Usage: $0 [install|setup|enable|start|stop|restart|status|logs [lines]]" >&2
		exit 1
		;;
esac
