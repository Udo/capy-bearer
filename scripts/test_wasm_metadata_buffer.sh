#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

test_name="wasm-metadata-buffer-test-$$"
site_directory="${BEARER_TEST_SITE_DIRECTORY:-site}"
bin_directory="${BEARER_TEST_BIN_DIRECTORY:-/tmp/bearer/work}"
if [[ -r /etc/bearer/settings.cfg ]]; then
	if [[ -z "${BEARER_TEST_SITE_DIRECTORY:-}" ]]; then
		configured_site_directory=$(awk -F= '/^[[:space:]]*HTTP_DOCUMENT_ROOT[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
		site_directory="${configured_site_directory:-$site_directory}"
	fi
	if [[ -z "${BEARER_TEST_BIN_DIRECTORY:-}" ]]; then
		configured_bin_directory=$(awk -F= '/^[[:space:]]*BIN_DIRECTORY[[:space:]]*=/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit}' /etc/bearer/settings.cfg)
		bin_directory="${configured_bin_directory:-$bin_directory}"
	fi
fi
source_dir="$site_directory/$test_name"
artifact_dir=""

cleanup() {
	rm -rf "$source_dir"
	if [[ -n "$artifact_dir" ]]; then
		rm -rf "$artifact_dir"
	fi
}
trap cleanup EXIT
mkdir -p "$source_dir"
artifact_dir="$(scripts/unit_cache_directory "$bin_directory")$(realpath "$source_dir")"

printf '%s\n' \
	'CLI(Request& context) {' \
	'  String target = context.get["unit"];' \
	'  String rendered = component(target, context);' \
	'  DValue perf = request_perf();' \
	'  DValue selected;' \
	'  perf["unit_module_operations"].each([&](DValue operation, String key) {' \
	'    if(operation["kind"].to_string() == "component" && contains(operation["unit"].to_string(), target + ".uce")) selected = operation;' \
	'  });' \
	'  print(rendered, "\t", selected["source"].to_string(), "\t", selected["read_count"].to_string(), "\t", selected["read_bytes"].to_string(), "\t", selected["read_us"].to_string());' \
	'}' >"$source_dir/parent.uce"

payload=$(head -c 131072 /dev/zero | tr '\0' x)
for unit in $(seq 0 7); do
	printf 'String metadata_payload_%s() { return("%s"); }\nCOMPONENT(Request& context) { String payload = metadata_payload_%s(); u64 offset = std::atoi(context.get["offset"].c_str()) %% payload.size(); print(std::to_string(payload.size()), ":", payload.substr(offset, 1)); }\n' \
		"$unit" "$payload" "$unit" >"$source_dir/component-$unit.uce"
done
unset payload

declare -a read_times=()
for unit in $(seq 0 7); do
	found=0
	for attempt in $(seq 1 20); do
		output=$(timeout --signal=TERM --kill-after=2s 30s scripts/bearer-cli "/$test_name/parent.uce?unit=component-$unit&offset=$attempt")
		IFS=$'\t' read -r marker source read_count read_bytes read_us <<<"$output"
		if [[ "$marker" != "131072:x" ]]; then
			echo "metadata buffer component failed: unit=$unit output=$output" >&2
			exit 1
		fi
		if [[ "$source" != "serialized" ]]; then
			continue
		fi
		wasm_size=$(stat -c %s "$artifact_dir/component-$unit.uce.wasm")
		if ! awk -v count="$read_count" -v bytes="$read_bytes" -v size="$wasm_size" \
			'BEGIN { exit !(count > 0 && count <= 64 && bytes > 0 && bytes <= count * 4096 && bytes < size) }'; then
			echo "metadata buffer read was not bounded: unit=$unit reads=$read_count bytes=$read_bytes wasm=$wasm_size us=$read_us" >&2
			exit 1
		fi
		read_times+=("$read_us")
		found=1
		break
	done
	if (( found == 0 )); then
		echo "metadata buffer did not observe a serialized cold-worker load for component-$unit" >&2
		exit 1
	fi
done

mapfile -t sorted < <(printf '%s\n' "${read_times[@]}" | sort -n)
median=${sorted[$(( ${#sorted[@]} / 2 ))]}
maximum=${sorted[$(( ${#sorted[@]} - 1 ))]}
if ! awk -v median="$median" 'BEGIN { exit !(median < 3000) }'; then
	echo "metadata buffer median exceeded 3ms: median=${median}us maximum=${maximum}us samples=${read_times[*]}" >&2
	exit 1
fi

echo "wasm metadata buffer passed: ${#read_times[@]} cold serialized loads, median ${median}us, maximum ${maximum}us"
