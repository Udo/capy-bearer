#!/usr/bin/env python3
"""Guard the hand-maintained unit-facing API coverage manifest.

This intentionally avoids network/external services. It checks that public API
names we expose to wasm units are either mentioned by a site test or explicitly
marked internal/integration-only, and that active docs exist for doc-required
APIs. The manifest is deliberately source-controlled so a new public function
requires an explicit coverage decision.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST_DIR = ROOT / "site" / "tests"
DOC_DIR = ROOT / "site" / "doc" / "pages"

# name, needs_doc, status. status: public | internal | integration
PUBLIC_APIS = [
    ("http_request", True, "public"), ("http_request_async", True, "public"),
    ("shell_exec", True, "public"), ("shell_escape", True, "public"), ("shell_spawn", True, "public"),
    ("job_status", True, "public"), ("job_result", True, "public"), ("job_await", True, "public"), ("job_cancel", True, "public"),
    ("basename", True, "public"), ("dirname", True, "public"), ("path_join", True, "public"),
    ("path_real", True, "public"), ("path_is_within", True, "public"),
    ("file_get_contents", True, "public"),
    ("file_put_contents", True, "public"), ("file_append", True, "public"),
    ("cwd_get", True, "public"), ("cwd_set", True, "public"), ("process_start_directory", True, "public"),
    ("file_mtime", True, "public"), ("file_unlink", True, "public"), ("expand_path", True, "public"),
    ("ls", True, "public"), ("to_u64", True, "public"), ("to_s64", True, "public"),
    ("to_f64", True, "public"), ("to_bool", True, "public"),
    ("request_perf", True, "public"), ("time_format_local", True, "public"),
    ("time_format_relative", True, "public"), ("time_parse", True, "public"),
    ("backtrace_get_frames", False, "public"), ("backtrace_capture", False, "public"),
    ("signal_name", False, "public"), ("memcache_escape_key", True, "public"),
    ("memcache_escape_keys", True, "public"), ("memcache_command", True, "public"),
    ("memcache_get_multiple", True, "public"), ("runtime_safe_key", True, "public"),
    ("float_val", True, "public"), ("nibble", True, "public"), ("json_consume_space", False, "public"),
    ("array_merge", True, "public"), ("safe_name", True, "public"), ("ascii_safe_name", True, "public"),
    ("to_json", False, "public"), ("remove", False, "public"), ("clear", False, "public"),
    ("gen_sha1", True, "public"), ("sha256", True, "public"), ("sha256_hex", True, "public"),
    ("hmac_sha256", True, "public"), ("hmac_sha256_hex", True, "public"), ("random_bytes", True, "public"),
    ("crypto_equal", True, "public"), ("password_hash", True, "public"),
    ("password_verify", True, "public"), ("password_needs_rehash", True, "public"),
    ("gen_noise32", True, "public"), ("gen_noise64", True, "public"),
    ("gen_noise01", True, "public"), ("gen_int", True, "public"), ("gen_float", True, "public"),
    ("draw_int", True, "public"), ("draw_float", True, "public"),
    ("encode_query", True, "public"), ("csrf_token", True, "public"),
    ("csrf_valid", True, "public"), ("csrf_rotate", True, "public"),
    ("request_script_url", True, "public"),
    ("request_base_url", True, "public"), ("request_route_from_raw_path", True, "public"),
    ("cli_arg", True, "public"), ("unit_compile", True, "public"),
    ("cleanup_sqlite_connections", False, "internal"), ("cleanup_mysql_connections", False, "internal"),
    ("mysql_connect", True, "integration"), ("mysql_query", True, "integration"),
    ("mysql_affected_rows", True, "integration"),
]

REMOVED_APIS = ["unit_load", "concat"]


def all_test_text() -> str:
    parts = []
    for path in TEST_DIR.glob("*.uce"):
        parts.append(path.read_text(errors="ignore"))
    return "\n".join(parts)


def doc_exists(name: str) -> bool:
    path = DOC_DIR / f"{name}.txt"
    return path.exists() and "Removed" not in path.read_text(errors="ignore")[:200]


def has_call(text: str, name: str) -> bool:
    return f"{name}(" in text or f".{name}(" in text or f'"{name}"' in text


def main() -> int:
    tests = all_test_text()
    errors = []
    for name, needs_doc, status in PUBLIC_APIS:
        if status == "public" and not has_call(tests, name):
            errors.append(f"missing test coverage: {name}")
        if needs_doc and status in {"public", "integration"} and not doc_exists(name):
            errors.append(f"missing active doc page: {name}")
    compiler_h = (ROOT / "src" / "lib" / "compiler.h").read_text(errors="ignore")
    if "#ifndef __BEARER_WASM_UNIT__\nSharedUnit* unit_load" not in compiler_h:
        errors.append("unit_load is not guarded out of wasm-unit exposure")
    for name in REMOVED_APIS:
        page = DOC_DIR / f"{name}.txt"
        if name == "concat" and page.exists() and "Removed" not in page.read_text(errors="ignore")[:300]:
            errors.append("concat doc is not tombstoned")
        if name == "unit_load" and page.exists() and "native-only" not in page.read_text(errors="ignore"):
            errors.append("unit_load doc is not native-only/tombstoned")
    if errors:
        print("API coverage manifest FAILED")
        for error in errors:
            print("- " + error)
        return 1
    print(f"API coverage manifest ok: {len(PUBLIC_APIS)} entries checked")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
