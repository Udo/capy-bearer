#!/usr/bin/env bash
set -euo pipefail

samples="${1:-40}"
settings="${SETTINGS:-/etc/bearer/settings.cfg}"
url_base="${URL_BASE:-http://127.0.0.1}"
host="${HOST_HEADER:-bearer.openfu.com}"
service="${SERVICE:-bearer.service}"

[[ "$samples" =~ ^[1-9][0-9]*$ ]] || { echo "samples must be a positive integer" >&2; exit 2; }
[[ -r "$settings" && -w "$settings" ]] || { echo "cannot read and write $settings" >&2; exit 2; }

backup="$(mktemp)"
results="$(mktemp)"
cp -- "$settings" "$backup"
restore() {
	cp -- "$backup" "$settings"
	systemctl restart "$service"
	rm -f -- "$backup" "$results"
}
trap restore EXIT

python3 - "$settings" <<'PY'
import pathlib
import re
import sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
key = r'(?m)^WASM_BACKEND_VERBOSE=.*$'
if re.search(key, text):
    text = re.sub(key, 'WASM_BACKEND_VERBOSE=1', text)
else:
    text += '\nWASM_BACKEND_VERBOSE=1\n'
path.write_text(text)
PY
systemctl restart "$service"

while IFS=$'\t' read -r name path; do
	for ((sample = 1; sample <= samples; sample++)); do
		headers="$(curl --fail --silent --show-error --max-time 30 --dump-header - --output /dev/null -H "Host: $host" "$url_base$path")"
		python3 - "$name" "$headers" >> "$results" <<'PY'
import re
import sys
name, headers = sys.argv[1:]
fields = (
    'Entry-Wasmtime-Call-Us', 'Entry-Guest-Execution-Us',
    'Entry-Hostcall-Us', 'Entry-Hostcall-Count', 'Hostcall-Total-Us',
    'Hostcall-MySQL-Us', 'Hostcall-Memcache-Us', 'Output-Collect-Us',
    'Component-Resolve-Total-Us', 'Component-Path-Total-Us',
    'Component-Artifact-Total-Us', 'Component-Load-Total-Us',
    'Component-Link-Total-Us',
)
for field in fields:
    match = re.search(r'^X-BEARER-Wasm-' + re.escape(field) + r':\s*(\d+)\s*$', headers, re.I | re.M)
    if not match:
        raise SystemExit('missing X-BEARER-Wasm-' + field)
    print(name, field, match.group(1), sep='\t')
PY
	done
done <<'TARGETS'
doc-index	/doc/
api-detail	/doc/api/component/
guide-detail	/doc/guide/install-and-first-program/
TARGETS

python3 - "$results" <<'PY'
import collections
import math
import sys

values = collections.defaultdict(list)
for line in open(sys.argv[1]):
    target, field, value = line.rstrip('\n').split('\t')
    values[target, field].append(int(value))

def percentile(data, fraction):
    data.sort()
    return data[max(0, math.ceil(fraction * len(data)) - 1)]

print('target\tmetric\tn\tp50_us\tp95_us')
for (target, field), data in sorted(values.items()):
    print(target, field, len(data), percentile(data, .50), percentile(data, .95), sep='\t')
PY
