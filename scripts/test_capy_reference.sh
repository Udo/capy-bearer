#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

host="${BEARER_TEST_HTTP_HOST:-bearer.openfu.com}"
base="${BEARER_TEST_HTTP_BASE:-http://127.0.0.1}/examples/capy-reference"
reference_dir=site/examples/capy-reference

[[ -f "$reference_dir/index.capy" && -f "$reference_dir/style.css" ]]
if find "$reference_dir" -type f -name '*.uce' -print -quit | grep -q .; then
	echo "Capy reference must not contain C++ .uce units" >&2
	exit 1
fi

pages=(index syntax types ownership interop status)
markers=(
	'Capy language reference'
	'Syntax'
	'Types and overloads'
	'Automatic reference counting'
	'Bearer interoperability'
	'Implementation status'
)
for index in "${!pages[@]}"; do
	path="$base/${pages[$index]}.capy"
	body=$(curl -fsS --max-time 30 -H "Host: $host" "$path")
	[[ "$body" == *"${markers[$index]}"* ]] || {
		echo "Capy reference marker missing from $path" >&2
		exit 1
	}
done

root_body=$(curl -fsS --max-time 30 -H "Host: $host" "$base/")
[[ "$root_body" == *'<title>Capy Language Reference</title>'* ]]
css=$(curl -fsS --max-time 30 -H "Host: $host" "$base/style.css")
[[ "$css" == *'--accent'* ]]

echo "Capy language reference passed: directory root, ${#pages[@]} Capy pages, and CSS"
