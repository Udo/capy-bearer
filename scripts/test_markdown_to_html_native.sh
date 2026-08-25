#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

binary="$PWD/bin/bearer_fastcgi.linux.bin"
[[ -x "$binary" ]] || { echo "build the native executable before this test" >&2; exit 2; }

timeout --signal=TERM --kill-after=1s 5s python3 - "$binary" <<'PY'
import subprocess
import sys

binary = sys.argv[1]


def frame(value):
    return len(value).to_bytes(8, "big") + value


def decode_frames(value):
    result = []
    offset = 0
    while offset < len(value):
        if len(value) - offset < 8:
            raise AssertionError("output has a partial frame header")
        size = int.from_bytes(value[offset:offset + 8], "big")
        offset += 8
        if len(value) - offset < size:
            raise AssertionError("output has a partial frame body")
        result.append(value[offset:offset + size])
        offset += size
    return result


def invoke(payload):
    return subprocess.run(
        [binary, "--markdown-to-html"], input=payload,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )

sources = [
    b"",
    b"# Title\n\nHello **world**",
    b"- alpha\n- beta",
    b"[site](https://example.test)\n\n`code`",
]
expected = [
    b"",
    b"<h1>Title</h1><p>Hello <strong>world</strong></p>",
    b"<ul><li><p>alpha</p></li><li><p>beta</p></li></ul>",
    b'<p><a href="https://example.test">site</a></p><p><code>code</code></p>',
]
payload = b"".join(frame(source) for source in sources)
first = invoke(payload)
assert first.returncode == 0, first.stderr
assert first.stderr == b"", first.stderr
assert decode_frames(first.stdout) == expected
second = invoke(payload)
assert second.returncode == 0, second.stderr
assert second.stdout == first.stdout

for payload in (b"\x00\x00\x00", frame(b"partial")[:-1], (16 * 1024 * 1024 + 1).to_bytes(8, "big")):
    result = invoke(payload)
    assert result.returncode == 2, (result.returncode, result.stderr)
    assert result.stdout == b"", result.stdout
    assert result.stderr, "invalid input did not report an error"
PY

echo 'markdown batch native test passed'
