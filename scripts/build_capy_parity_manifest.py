#!/usr/bin/env python3
"""Build the checked Capy/.uce public capability parity inventory."""

from collections import Counter
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
PAGES = ROOT / "site/doc/pages"

SUPPORTED = {
    "1_CLI": ("site/tests/capy-strings.capy", "function CLI"),
    "1_RENDER": ("site/tests/capy-request-context.capy", "function RENDER"),
    "1_COMPONENT": ("site/tests/capy-request-context.capy", "function COMPONENT"),
    "1_WS": ("site/tests/capy-websocket.capy", "function WS"),
    "1_SERVE_HTTP": ("site/tests/capy-serve-http.capy", "function SERVE_HTTP"),
    "print": ("site/tests/capy-phase1.capy", "print("),
    "unit_render": ("site/tests/capy-cross.capy", "unit_render("),
    "2_DValue_each": ("site/tests/capy-dval-rich.capy", "for key, value"),
    "2_DValue_has": ("site/tests/capy-dval-rich.capy", "dval_has("),
    "2_DValue_operator_index": ("site/tests/capy-dval-rich.capy", "profile["),
    "component_exists": ("site/tests/capy-component-props.capy", "component_exists("),
    "component_resolve": ("site/tests/capy-component-props.capy", "component_resolve("),
    "time": ("site/tests/capy-wide-scalars.capy", "time()"),
    "time_precise": ("site/tests/capy-wide-scalars.capy", "time_precise()"),
    "file_open": ("site/tests/capy-files.capy", "file_open("),
    "file_read": ("site/tests/capy-files.capy", "file_read("),
    "file_write": ("site/tests/capy-files.capy", "file_write("),
    "file_seek": ("site/tests/capy-files.capy", "file_seek("),
    "file_tell": ("site/tests/capy-files.capy", "file_tell("),
    "file_fsync": ("site/tests/capy-files.capy", "file_fsync("),
    "file_close": ("site/tests/capy-files.capy", "file_close("),
    "file_temp": ("site/tests/capy-files.capy", "file_temp("),
    "file_unlink": ("site/tests/capy-files.capy", "file_unlink("),
    "unit_info": ("site/tests/capy-unit-admin.capy", "unit_info("),
    "unit_compile": ("site/tests/capy-unit-admin.capy", "unit_compile("),
    "json_decode": ("site/tests/capy-codecs.capy", "json_decode("),
    "regex_match": ("site/tests/capy-regex.capy", "regex_match("),
    "regex_search": ("site/tests/capy-regex.capy", "regex_search("),
    "regex_search_all": ("site/tests/capy-regex.capy", "regex_search_all("),
    "regex_replace": ("site/tests/capy-regex.capy", "regex_replace("),
    "regex_split": ("site/tests/capy-regex.capy", "regex_split("),
    "contains": ("site/tests/capy-strings.capy", "contains("),
    "split": ("site/tests/capy-string-lists.capy", "split("),
    "join": ("site/tests/capy-string-lists.capy", "join("),
    "first": ("site/tests/capy-string-lists.capy", "first("),
    "base64_encode": ("site/tests/capy-codecs.capy", "base64_encode("),
    "base64_decode": ("site/tests/capy-codecs.capy", "base64_decode("),
    "uri_encode": ("site/tests/capy-codecs.capy", "uri_encode("),
    "uri_decode": ("site/tests/capy-codecs.capy", "uri_decode("),
}
PARTIAL = {
    "0_Request": ("site/tests/capy-request-context.capy", "request_context("),
    "0_String": ("site/tests/capy-strings.capy", "left +"),
    "2_Request_set_status": ("site/tests/capy-request-context.capy", "response_status("),
    "component_render": ("site/tests/capy-component-props.capy", "component_render("),
    "unit_call": ("site/tests/capy-cross.capy", "unit_call("),
    "units_list": ("site/tests/capy-unit-admin.capy", "units_list("),
    "json_encode": ("site/tests/capy-codecs.capy", "json_encode("),
    "html_escape": ("site/tests/capy-codecs.capy", "html_escape("),
    "2_DValue_get_type_name": ("site/tests/capy-dval-rich.capy", "dval_bool("),
    "2_DValue_is_array": ("site/tests/capy-dval-rich.capy", "dval(["),
    "2_DValue_is_list": ("site/tests/capy-dval-rich.capy", "dval(["),
    "2_DValue_key": ("site/tests/capy-dval-rich.capy", "for key, value"),
    "2_DValue_keys": ("site/tests/capy-dval-rich.capy", "for key, value"),
    "0_StringList": ("site/tests/capy-string-lists.capy", "split("),
    "array_merge": ("site/tests/capy-dval-merge.capy", "array_merge("),
    "2_DValue_to_bool": ("site/tests/capy-dval-rich.capy", "dval_bool("),
    "2_DValue_to_f64": ("site/tests/capy-codecs.capy", "dval_f64("),
    "2_DValue_to_string": ("site/tests/capy-dval-rich.capy", "dval_string("),
    "2_DValue_values": ("site/tests/capy-dval-rich.capy", "for key, value"),
    "substr": ("site/tests/capy-strings.capy", "substr("),
    "to_s64": ("site/tests/capy-wide-scalars.capy", "s64"),
    "to_u64": ("site/tests/capy-wide-scalars.capy", "as u64"),
    "to_f64": ("site/tests/capy-wide-scalars.capy", "as f64"),
    "strpos": ("site/tests/capy-strings.capy", "find("),
    "replace": ("site/tests/capy-strings.capy", "replace("),
    "to_lower": ("site/tests/capy-strings.capy", "lower("),
    "to_upper": ("site/tests/capy-strings.capy", "upper("),
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
    runtime_driver = "".join(
        (ROOT / path).read_text()
        for path in ("scripts/test_capy_phase1.sh", "scripts/test_capy_websocket.py", "site/tests/capy-serve-http-caller.uce")
    )
    for capability, (relative_path, marker) in entries.items():
        path = ROOT / relative_path
        if not path.is_file() or marker not in path.read_text():
            raise SystemExit(f"{capability}: missing parity evidence {relative_path!r} marker {marker!r}")
        if relative_path.startswith("site/tests/") and path.name not in runtime_driver:
            raise SystemExit(f"{capability}: parity fixture is not wired into scripts/test_capy_phase1.sh: {relative_path}")


def classify(name: str) -> tuple[str, str]:
    if name in CPP_SPECIFIC:
        return "cpp-specific", "No Capy runtime equivalent required."
    if name in SUPPORTED:
        return "supported", f"Runtime evidence: `{SUPPORTED[name][0]}`."
    if name.startswith("1_"):
        return "export-only", "Capy emits the handler export; lifecycle behavior still needs parity acceptance."
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
