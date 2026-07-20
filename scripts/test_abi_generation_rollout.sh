#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="abi-generation-test-$$"
site_directory="${UCE_TEST_SITE_DIRECTORY:-site}"
if [[ -z "${UCE_TEST_SITE_DIRECTORY:-}" && -r /etc/uce/settings.cfg ]]; then
	configured_site_directory=$(awk -F= '/^[[:space:]]*SITE_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/uce/settings.cfg)
	[[ -z "${configured_site_directory:-}" ]] || site_directory="$configured_site_directory"
fi
source_dir="$site_directory/$test_name"
http_host="${UCE_TEST_HTTP_HOST:-uce.openfu.com}"
body_file="/tmp/uce-abi-generation-body-$$"
ready_file="/tmp/uce-abi-generation-ready-$$"

cleanup() {
	rm -rf "$source_dir"
	rm -f "$body_file" "$ready_file"
	[[ -z "${artifact_dir:-}" ]] || rm -rf "$artifact_dir"
}
trap cleanup EXIT
mkdir -p "$source_dir"

printf '%s\n' \
	'RENDER(Request& context) { print("entry-current"); }' \
	'CLI(Request& context) {' \
	'  if(context.get["artifact"] == "entry") print(unit_info(context.params["SCRIPT_FILENAME"])["wasm_name"].to_string());' \
	'  else print("entry-current");' \
	'}' >"$source_dir/entry.uce"
printf '%s\n' \
	'COMPONENT(Request& context) { print("component-current"); }' >"$source_dir/child.uce"
printf '%s\n' \
	'RENDER(Request& context) { DValue props; print(component("child", props, context)); }' \
	'CLI(Request& context) {' \
	'  DValue props; component("child", props, context);' \
	'  print(unit_info("child.uce")["wasm_name"].to_string());' \
	'}' >"$source_dir/component-parent.uce"

entry_url="http://127.0.0.1/$test_name/entry.uce"
component_url="http://127.0.0.1/$test_name/component-parent.uce"
[[ "$(curl -fsS -H "Host: $http_host" "$entry_url")" == "entry-current" ]]
entry_wasm=$(scripts/uce-cli --get "/$test_name/entry.uce" artifact=entry)
component_wasm=$(scripts/uce-cli "/$test_name/component-parent.uce")
artifact_dir=$(dirname "$entry_wasm")
generation_name=$(basename "$(scripts/unit_cache_directory /tmp/uce-generation-probe)")
if [[ "$entry_wasm" != *"/$generation_name/"* || "$component_wasm" != *"/$generation_name/"* ]]; then
	echo "unit artifacts are not isolated by compiler/core ABI generation: $entry_wasm $component_wasm" >&2
	exit 1
fi

publish_incompatible_artifact_while_locked() {
	local source_file="$1"
	local wasm_file="$2"
	local marker="$3"
	local destination
	destination=$(dirname "$wasm_file")
	(
		exec 8>"$wasm_file.lock"
		flock 8
		UCE_UNIT_ABI_VERSION=6 scripts/compile_wasm_unit \
			"$(dirname "$source_file")" "$destination" "$source_file" \
			"$(basename "$source_file").cpp" "$(basename "$wasm_file")"
		sed -i \
			-e 's/^unit_abi_version=.*/unit_abi_version=0/' \
			-e 's/^wasm_core_abi_version=.*/wasm_core_abi_version=0/' \
			"${wasm_file%.wasm}.meta.txt"
		rm -f "${wasm_file%.wasm}.cwasm"
		printf '%s\n' "$marker" >"$ready_file"
		sleep 2
	) &
	lock_pid=$!
	deadline=$((SECONDS + 20))
	while [[ ! -e "$ready_file" && $SECONDS -lt $deadline ]]; do sleep 0.05; done
	if [[ ! -e "$ready_file" || "$(cat "$ready_file")" != "$marker" ]]; then
		echo "incompatible artifact was not published under lock: $marker" >&2
		exit 1
	fi
}

publish_incompatible_artifact_while_locked "$source_dir/entry.uce" "$entry_wasm" entry
started_at=$(date +%s%N)
entry_status=$(curl -sS --max-time 30 -o "$body_file" -w '%{http_code}' -H "Host: $http_host" "$entry_url")
entry_elapsed_ms=$(( ($(date +%s%N) - started_at) / 1000000 ))
wait "$lock_pid"
if [[ "$entry_status" != "200" || "$(cat "$body_file")" != "entry-current" || $entry_elapsed_ms -lt 1500 ]]; then
	echo "ABI-incompatible entry artifact was served instead of joining its rebuild: status=$entry_status elapsed=${entry_elapsed_ms}ms body=$(cat "$body_file")" >&2
	exit 1
fi

rm -f "$ready_file"
publish_incompatible_artifact_while_locked "$source_dir/child.uce" "$component_wasm" component
started_at=$(date +%s%N)
component_status=$(curl -sS --max-time 30 -o "$body_file" -w '%{http_code}' -H "Host: $http_host" "$component_url")
component_elapsed_ms=$(( ($(date +%s%N) - started_at) / 1000000 ))
wait "$lock_pid"
if [[ "$component_status" != "200" || "$(cat "$body_file")" != "component-current" || $component_elapsed_ms -lt 1500 ]]; then
	echo "ABI-incompatible component artifact was served instead of joining its rebuild: status=$component_status elapsed=${component_elapsed_ms}ms body=$(cat "$body_file")" >&2
	exit 1
fi

echo "ABI generation rollout passed"
