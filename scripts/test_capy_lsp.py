#!/usr/bin/env python3
import json
import os
from pathlib import Path
import select
import socket
import stat
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
CAPYC = ROOT / "bin/capyc"
TIMEOUT = 10


class Client:
    def __init__(self, process, reader, writer):
        self.process = process
        self.reader = reader
        self.writer = writer
        self.buffer = b""
        self.next_id = 1

    def send(self, method, params=None, request=False):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        request_id = None
        if request:
            request_id = self.next_id
            self.next_id += 1
            message["id"] = request_id
        body = json.dumps(message, separators=(",", ":")).encode()
        self.writer.sendall(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        return request_id

    def receive(self, timeout=TIMEOUT):
        deadline = time.monotonic() + timeout
        while b"\r\n\r\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.reader], [], [], remaining)[0]:
                stderr = self.process.stderr.read().decode() if self.process.poll() is not None else ""
                raise AssertionError(f"timed out waiting for LSP headers: {stderr}")
            chunk = self.reader.recv(8192)
            if not chunk:
                raise AssertionError("LSP connection closed before a response")
            self.buffer += chunk
        header, body = self.buffer.split(b"\r\n\r\n", 1)
        lengths = [line.split(b":", 1)[1].strip() for line in header.split(b"\r\n") if line.lower().startswith(b"content-length:")]
        assert len(lengths) == 1
        length = int(lengths[0])
        while len(body) < length:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.reader], [], [], remaining)[0]:
                raise AssertionError("timed out waiting for an LSP body")
            chunk = self.reader.recv(8192)
            if not chunk:
                raise AssertionError("LSP connection closed during a response")
            body += chunk
        self.buffer = body[length:]
        return json.loads(body[:length])

    def response(self, request_id):
        while True:
            message = self.receive()
            if message.get("id") == request_id:
                assert "error" not in message, message
                return message["result"]

    def request(self, method, params=None):
        return self.response(self.send(method, params, True))

    def notification(self, method):
        while True:
            message = self.receive()
            if message.get("method") == method:
                return message["params"]


class PipeSocket:
    def __init__(self, reader=None, writer=None):
        self.reader = reader
        self.writer = writer

    def fileno(self):
        return self.reader.fileno()

    def recv(self, size):
        return os.read(self.reader.fileno(), size)

    def sendall(self, data):
        self.writer.write(data)
        self.writer.flush()


def initialize(client, root_uri, encodings):
    result = client.request("initialize", {
        "rootUri": root_uri,
        "capabilities": {"general": {"positionEncodings": encodings}},
        "initializationOptions": {"SITE_DIRECTORY": "site", "COMPILER_SYS_PATH": str(ROOT)},
    })
    client.send("initialized", {})
    return result


def text_document(uri, text, version=1):
    return {"textDocument": {"uri": uri, "languageId": "capy", "version": version, "text": text}}


def wait_diagnostics(client, uri):
    while True:
        params = client.notification("textDocument/publishDiagnostics")
        if params["uri"] == uri:
            return params["diagnostics"]


def decode_tokens(data):
    result = []
    line = column = 0
    for index in range(0, len(data), 5):
        delta_line, delta_column, length, kind, _ = data[index:index + 5]
        if delta_line:
            line += delta_line
            column = delta_column
        else:
            column += delta_column
        result.append((line, column, length, kind))
    return result


def stdio_test(workspace):
    process = subprocess.Popen(
        [str(CAPYC), "--lsp"], cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    pipe = PipeSocket(process.stdout, process.stdin)
    client = Client(process, pipe, pipe)
    result = initialize(client, workspace.as_uri(), ["utf-16"])
    capabilities = result["capabilities"]
    assert capabilities["positionEncoding"] == "utf-16"
    for provider in (
        "documentSymbolProvider", "hoverProvider", "completionProvider", "definitionProvider",
        "signatureHelpProvider", "semanticTokensProvider",
    ):
        assert provider in capabilities, provider

    uri = (workspace / "main.capy").as_uri()
    broken = 'function CLI(request : dval) {\n    print("😀"); missing()\n}\n'
    client.send("textDocument/didOpen", text_document(uri, broken))
    diagnostics = wait_diagnostics(client, uri)
    assert len(diagnostics) == 1, diagnostics
    diagnostic = diagnostics[0]
    assert "no overload missing()" in diagnostic["message"]
    assert diagnostic["range"]["start"] == {"line": 1, "character": 17}, diagnostic
    assert diagnostic["range"]["end"] == {"line": 1, "character": 24}, diagnostic
    semantic = client.request("textDocument/semanticTokens/full", {"textDocument": {"uri": uri}})["data"]
    string_kind = capabilities["semanticTokensProvider"]["legend"]["tokenTypes"].index("string")
    assert (1, 10, 4, string_kind) in decode_tokens(semantic), decode_tokens(semantic)

    valid_utf8 = 'function CLI(request : dval) {\n    print("😀")\n}\n'
    client.send("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": valid_utf8}],
    })
    assert wait_diagnostics(client, uri) == []

    source = """type Count = s32
struct Box {
    value : Count
}
function double(value : Count) Count {
    -> value + value
}
function CLI(request : dval) {
    print(double(2))
}
"""
    client.send("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 3}, "contentChanges": [{"text": source}],
    })
    assert wait_diagnostics(client, uri) == []

    symbols = client.request("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
    assert {item["name"] for item in symbols} >= {"Count", "Box", "double", "CLI"}

    hover = client.request("textDocument/hover", {"textDocument": {"uri": uri}, "position": {"line": 8, "character": 5}})
    assert "function print(...values : as string)" in hover["contents"]["value"], hover
    assert "/doc/api/print/" in hover["contents"]["value"]

    completion = client.request("textDocument/completion", {"textDocument": {"uri": uri}, "position": {"line": 8, "character": 4}})
    assert "print" in {item["label"] for item in completion}

    definition = client.request("textDocument/definition", {"textDocument": {"uri": uri}, "position": {"line": 8, "character": 11}})
    assert definition and definition[0]["uri"] == uri
    assert definition[0]["range"]["start"]["line"] == 4, definition

    signatures = client.request("textDocument/signatureHelp", {"textDocument": {"uri": uri}, "position": {"line": 8, "character": 10}})
    assert signatures and any("function print(" in item["label"] for item in signatures["signatures"]), signatures

    workspace_symbols = client.request("workspace/symbol", {"query": "workspace"})
    assert {item["name"] for item in workspace_symbols} >= {"WorkspaceBox", "workspace_echo"}

    markup_uri = (workspace / "markup.capy").as_uri()
    markup = """function page(value : string) string {
    -> <><p title="<?= value ?>"><?= value ?></p><script>const item = <?= value ?>;</script><style>.x { content: <?= value ?>; }</style></>
}
function CLI(request : dval) {
    print(page("x"))
}
"""
    client.send("textDocument/didOpen", text_document(markup_uri, markup))
    assert wait_diagnostics(client, markup_uri) == []
    tokens = client.request("textDocument/semanticTokens/full", {"textDocument": {"uri": markup_uri}})["data"]
    assert tokens and len(tokens) % 5 == 0
    token_types = capabilities["semanticTokensProvider"]["legend"]["tokenTypes"]
    standard_types = {"namespace", "type", "class", "enum", "interface", "struct", "typeParameter", "parameter", "variable", "property", "enumMember", "event", "function", "method", "macro", "keyword", "modifier", "comment", "string", "number", "regexp", "operator", "decorator"}
    assert set(token_types) <= standard_types, token_types
    decoded = decode_tokens(tokens)
    markup_line = markup.splitlines()[1]
    offsets = []
    start = 0
    while len(offsets) < 4:
        start = markup_line.index("<?=", start)
        offsets.append(start + 3)
        start += 3
    expected = ["property", "string", "variable", "number"]
    for column, kind in zip(offsets, expected):
        assert (1, column, 7, token_types.index(kind)) in decoded, (kind, decoded)

    client.send("workspace/didChangeConfiguration", {"settings": {"capy": {"siteDirectory": "other-site"}}})
    assert client.request("shutdown") is None
    client.send("exit")
    assert process.wait(TIMEOUT) == 0
    assert process.stderr.read() == b""


def socket_test(workspace):
    socket_path = workspace / "capyc.sock"
    process = subprocess.Popen(
        [str(CAPYC), "--lsp", "--socket", str(socket_path)], cwd=ROOT,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    deadline = time.monotonic() + TIMEOUT
    while not socket_path.exists() and time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(process.stderr.read().decode())
        time.sleep(0.01)
    assert socket_path.exists()
    assert stat.S_IMODE(socket_path.stat().st_mode) == 0o600
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.connect(str(socket_path))
    client = Client(process, connection, connection)
    result = initialize(client, workspace.as_uri(), ["utf-16", "utf-32"])
    assert result["capabilities"]["positionEncoding"] == "utf-32"
    uri = (workspace / "socket.capy").as_uri()
    broken = 'function CLI(request : dval) {\n    print("😀"); missing()\n}\n'
    client.send("textDocument/didOpen", text_document(uri, broken))
    diagnostic = wait_diagnostics(client, uri)[0]
    assert diagnostic["range"]["start"] == {"line": 1, "character": 16}, diagnostic
    assert diagnostic["range"]["end"] == {"line": 1, "character": 23}, diagnostic
    assert client.request("shutdown") is None
    client.send("exit")
    connection.close()
    assert process.wait(TIMEOUT) == 0
    assert process.stderr.read() == b""
    assert not socket_path.exists()


def check_test(workspace):
    good = workspace / "good.capy"
    bad = workspace / "bad.capy"
    good.write_text('function CLI(request : dval) {\n    print("ok")\n}\n')
    bad.write_text("function CLI(request : dval) {\n    @\n}\n")
    result = subprocess.run([CAPYC, "--check", good], cwd=ROOT, capture_output=True, timeout=TIMEOUT)
    assert result.returncode == 0 and result.stdout == b"" and result.stderr == b"", result
    result = subprocess.run([CAPYC, "--check", bad], cwd=ROOT, capture_output=True, timeout=TIMEOUT)
    assert result.returncode != 0 and result.stdout == b""
    assert f"{bad}:2:5: unexpected character '@'\n".encode() == result.stderr, result.stderr
    result = subprocess.run(
        [CAPYC, "--check", "-"], cwd=ROOT, input=b"function CLI(request : dval) { @ }\n", capture_output=True, timeout=TIMEOUT,
    )
    assert result.returncode != 0 and result.stderr.startswith(b"-:1:32: "), result.stderr

    second_bad = workspace / "second-bad.capy"
    second_bad.write_text("function CLI(request : dval) {\n    %\n}\n")
    result = subprocess.run([CAPYC, "--check", good, bad, second_bad], cwd=ROOT, capture_output=True, timeout=TIMEOUT)
    assert result.returncode != 0 and result.stdout == b""
    lines = result.stderr.decode().splitlines()
    assert len(lines) == 2, lines
    assert lines[0].startswith(f"{bad}:2:5: ") and lines[1].startswith(f"{second_bad}:2:5: "), lines

    directory = workspace / "check-tree"
    nested = directory / "nested"
    nested.mkdir(parents=True)
    (directory / "valid.capy").write_text(good.read_text())
    directory_bad = directory / "broken.capy"
    nested_bad = nested / "broken.capy"
    directory_bad.write_text("function CLI(request : dval) { @ }\n")
    nested_bad.write_text("function CLI(request : dval) { missing() }\n")
    (nested / "ignored.txt").write_text("@")
    result = subprocess.run([CAPYC, "--check", directory], cwd=ROOT, capture_output=True, timeout=TIMEOUT)
    assert result.returncode != 0 and result.stdout == b""
    lines = result.stderr.decode().splitlines()
    assert len(lines) == 2, lines
    assert lines[0].startswith(f"{directory_bad}:1:32: "), lines
    assert lines[1].startswith(f"{nested_bad}:1:32: "), lines


def main():
    assert CAPYC.is_file(), f"build {CAPYC} first"
    with tempfile.TemporaryDirectory(prefix="capy-lsp-") as temporary:
        workspace = Path(temporary)
        (workspace / "workspace.capy").write_text("""struct WorkspaceBox {
    value : s32
}
function workspace_echo(value : s32) s32 {
    -> value
}
""")
        stdio_test(workspace)
        socket_test(workspace)
        check_test(workspace)
    print("Capy LSP and check-mode tests passed")


if __name__ == "__main__":
    main()
