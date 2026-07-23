#!/usr/bin/env python3
"""Build the checked Capy/.uce public capability parity inventory."""

from collections import Counter
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
PAGES = ROOT / "site/doc/pages"

SUPPORTED = {
    "print": ("site/tests/capy-phase1.capy", "print("),
    "unit_render": ("site/tests/capy-cross.capy", "unit_render("),
    "2_DValue_each": ("site/tests/capy-dval-rich.capy", "for key, value"),
    "2_DValue_has": ("site/tests/capy-dval-rich.capy", "dval_has("),
    "2_DValue_operator_index": ("site/tests/capy-dval-rich.capy", "profile["),
}
PARTIAL = {
    "0_Request": ("site/tests/capy-request-context.capy", "request_context("),
    "0_String": ("site/tests/capy-strings.capy", "left +"),
    "2_Request_set_status": ("site/tests/capy-request-context.capy", "response_status("),
    "component_render": ("site/examples/capy-reference/interop.capy", "component_render("),
    "unit_call": ("site/tests/capy-cross.capy", "unit_call("),
    "2_DValue_get_type_name": ("site/tests/capy-dval-rich.capy", "dval_bool("),
    "2_DValue_is_array": ("site/tests/capy-dval-rich.capy", "dval(["),
    "2_DValue_is_list": ("site/tests/capy-dval-rich.capy", "dval(["),
    "2_DValue_key": ("site/tests/capy-dval-rich.capy", "for key, value"),
    "2_DValue_keys": ("site/tests/capy-dval-rich.capy", "for key, value"),
    "2_DValue_to_bool": ("site/tests/capy-dval-rich.capy", "dval_bool("),
    "2_DValue_to_string": ("site/tests/capy-dval-rich.capy", "dval_string("),
    "2_DValue_values": ("site/tests/capy-dval-rich.capy", "for key, value"),
    "substr": ("site/tests/capy-strings.capy", "substr("),
    "redirect": ("site/tests/capy-redirect.capy", "redirect("),
    "session_start": ("site/tests/capy-session.capy", "session_start("),
    "session_destroy": ("site/tests/capy-session.capy", "session_destroy("),
    "csrf_token": ("site/tests/capy-csrf.capy", "csrf_token("),
    "csrf_valid": ("site/tests/capy-csrf.capy", "csrf_valid("),
    "csrf_rotate": ("site/tests/capy-csrf.capy", "csrf_rotate("),
    "ws_message": ("site/tests/capy-websocket.capy", "ws_message("),
    "ws_connection_id": ("site/tests/capy-websocket.capy", "ws_connection_id("),
    "ws_scope": ("site/tests/capy-websocket.capy", "ws_scope("),
    "ws_opcode": ("site/tests/capy-websocket.capy", "ws_opcode("),
    "ws_is_binary": ("site/tests/capy-websocket.capy", "ws_is_binary("),
    "ws_send": ("site/tests/capy-websocket.capy", "ws_send("),
    "ws_send_to": ("site/tests/capy-websocket.capy", "ws_send_to("),
    "ws_close": ("site/tests/capy-websocket.capy", "ws_close("),
}
CPP_SPECIFIC = {"3_Blocked functions", "3_C++ Preprocessor", "3_Coming from React", "3_Documentation format"}


def category(name: str) -> str:
    if name.startswith("1_"):
        return "handler/lifecycle"
    if name.startswith(("0_DValue", "2_DValue")):
        return "structured values"
    if name.startswith(("0_String", "2_String")):
        return "strings/collections"
    if name.startswith(("0_Request", "2_Request", "request_", "cli_", "csrf_", "session_", "redirect")):
        return "request/response"
    if name.startswith(("component", "unit", "units_")):
        return "unit/component"
    if name.startswith(("file_", "dir_", "path_", "cwd_", "ls", "mkdir", "basename", "dirname", "expand_path")):
        return "filesystem"
    if name.startswith(("mysql_", "sqlite_", "memcache_")):
        return "data/cache"
    if name.startswith(("ws_", "socket_", "http_")):
        return "network/websocket"
    if name.startswith(("job_", "task", "server_", "shell_", "process_")):
        return "jobs/process"
    if name.startswith(("sha", "hmac_", "base64_", "crypto_", "password_", "random_", "gen_", "draw_")):
        return "crypto/random"
    if name.startswith(("json_", "xml_", "yaml_", "regex_", "markdown_", "uri_", "html_", "brb_")):
        return "codecs/text"
    if name.startswith(("zip_", "gz_")):
        return "archive"
    if name.startswith("3_"):
        return "documentation/C++"
    return "core utility"


def checked_evidence(entries: dict[str, tuple[str, str]]) -> None:
    runtime_driver = (ROOT / "scripts/test_capy_phase1.sh").read_text() + (ROOT / "scripts/test_capy_websocket.py").read_text()
    for capability, (relative_path, marker) in entries.items():
        path = ROOT / relative_path
        if not path.is_file() or marker not in path.read_text():
            raise SystemExit(f"{capability}: missing parity evidence {relative_path!r} marker {marker!r}")
        if relative_path.startswith("site/tests/") and path.name not in runtime_driver:
            raise SystemExit(f"{capability}: parity fixture is not wired into scripts/test_capy_phase1.sh: {relative_path}")


def classify(name: str) -> tuple[str, str]:
    if name in CPP_SPECIFIC:
        return "cpp-specific", "No Capy runtime equivalent required."
    if name.startswith("1_"):
        return "export-only", "Capy emits the handler export; lifecycle behavior still needs parity acceptance."
    if name in SUPPORTED:
        return "supported", f"Runtime evidence: `{SUPPORTED[name][0]}`."
    if name in PARTIAL:
        return "partial", f"Partial runtime evidence: `{PARTIAL[name][0]}`; the documented convenience API remains incomplete."
    return "missing", "No native Capy binding in the checked compiler surface."


def render() -> str:
    checked_evidence(SUPPORTED)
    checked_evidence(PARTIAL)
    rows = []
    for page in sorted(PAGES.glob("*.txt"), key=lambda path: path.name.lower()):
        name = page.stem
        status, note = classify(name)
        rows.append((name, category(name), status, note))
    counts = Counter(row[2] for row in rows)
    lines = [
        "# Capy / C++ `.uce` capability parity manifest",
        "",
        "Generated by `scripts/build_capy_parity_manifest.py` from `site/doc/pages/`.",
        "This inventory is a progress boundary: `partial`, `export-only`, and `missing` are not parity.",
        "",
        f"Documented pages: **{len(rows)}**. " + ", ".join(f"{key}: **{counts[key]}**" for key in sorted(counts)),
        "",
        "## Binding boundary",
        "",
        "- Pure, common value operations (strings, numeric conversion, collection access) lower directly in Capy when compact and faster than a call.",
        "- Request/response, sessions, WebSockets, and other current-workspace operations use narrow typed core adapters; Capy never reads C++ layouts.",
        "- Structured policy-heavy operations already represented by DValues (HTTP client, jobs, regex/codecs, unit administration) use bounded copied BRRB capability adapters rather than `unit_call` shims.",
        "- Filesystem, sockets, shell, cache, database, and archive operations reuse Bearer's existing core/host policy. Opaque resources become typed numeric handles only after u64 support; no direct OS imports are added to side units.",
        "- `unit_call` remains application composition, not a substitute for missing standard bindings.",
        "",
        "| Documented capability | Category | Status | Evidence/remaining work |",
        "|---|---|---|---|",
    ]
    lines.extend(f"| `{name}` | {group} | **{status}** | {note} |" for name, group, status, note in rows)
    lines.append("")
    return "\n".join(lines)


if __name__ == "__main__":
    content = render()
    if len(sys.argv) == 2:
        Path(sys.argv[1]).write_text(content)
    else:
        sys.stdout.write(content)
