#!/usr/bin/env python3
"""Guard the hand-maintained unit-facing API coverage manifest.

This intentionally avoids network/external services. It checks that public API
names we expose to wasm units are either mentioned by a site test or explicitly
marked internal/integration-only, and that active docs exist for doc-required
APIs. The manifest is deliberately source-controlled so a new public function
requires an explicit coverage decision.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST_DIR = ROOT / "site" / "tests"
DOC_DIR = ROOT / "site" / "doc" / "pages"

# Public source names that intentionally do not need public docs.
DOC_INTERNAL_APIS = {
    "__bearer_dval_replace",
    "arc_live",
    "cleanup_mysql_connections",
    "cleanup_sqlite_connections",
    "json_consume_space",
    "mysql_escape_default",
    "string_list_find",
}

# Public source names whose public page uses the object-oriented page slug.
DOC_PAGE_ALIASES = {
    "clear": "clear",
    "delete": "delete",
    "get": "get",
    "get_by_path": "get_by_path",
    "get_or_create": "get_or_create",
    "get_type_name": "get_type_name",
    "has": "has",
    "is_array": "is_array",
    "is_list": "is_list",
    "keys": "keys",
    "pop": "pop",
    "push": "push",
    "set": "set",
    "values": "values",
    "each": "each",
    "every": "every",
    "find": "find",
    "some": "some",
    "sort": "sort",
    "unique": "unique",
}

# name, needs_doc, status. status: public | internal | integration
PUBLIC_APIS = [
    ("http_request", True, "public"), ("http_request_async", True, "public"),
    ("shell_exec", True, "public"), ("shell_escape", True, "public"), ("shell_spawn", True, "public"),
    ("job_status", True, "public"), ("job_result", True, "public"), ("job_await", True, "public"), ("job_cancel", True, "public"),
    # The frozen historical task pages describe the removed callback/PID API;
    # current task coverage is runtime fixtures until their replacement lands.
    ("task", False, "public"), ("task_status", False, "public"), ("task_await", False, "public"), ("task_cancel", False, "public"),
    ("basename", True, "public"), ("dirname", True, "public"), ("path_join", True, "public"),
    ("path_real", True, "public"), ("path_is_within", True, "public"),
    ("file_get_contents", True, "public"),
    ("file_put_contents", True, "public"), ("file_append", True, "public"),
    ("cwd_get", True, "public"), ("cwd_set", True, "public"), ("process_start_directory", True, "public"),
    ("file_mtime", True, "public"), ("file_unlink", True, "public"), ("expand_path", True, "public"),
    ("ls", True, "public"),
    ("runtime_perf", True, "public"), ("time_format_local", True, "public"),
    ("time_format_relative", True, "public"), ("time_parse", True, "public"),
    ("backtrace_get_frames", False, "public"), ("backtrace_capture", False, "public"),
    ("signal_name", False, "public"), ("memcache_escape_key", True, "public"),
    ("memcache_command", True, "public"),
    ("memcache_get_multiple", True, "public"), ("runtime_safe_key", True, "public"),
    ("nibble", True, "public"), ("json_consume_space", False, "internal"),
    ("array_merge", True, "public"), ("safe_name", True, "public"), ("ascii_safe_name", True, "public"),
    ("remove", False, "public"), ("clear", False, "public"),
    ("gen_sha1", True, "public"), ("sha256", True, "public"), ("sha256_hex", True, "public"),
    ("hmac_sha256", True, "public"), ("hmac_sha256_hex", True, "public"), ("random_bytes", True, "public"),
    ("crypto_equal", True, "public"), ("password_hash", True, "public"),
    ("password_verify", True, "public"), ("password_needs_rehash", True, "public"),
    ("gen_noise32", True, "public"), ("gen_noise64", True, "public"),
    ("gen_noise01", True, "public"), ("gen_int", True, "public"), ("gen_float", True, "public"),
    ("draw_int", True, "public"), ("draw_float", True, "public"),
    ("encode_query", True, "public"), ("csrf_token", True, "public"),
    ("csrf_valid", True, "public"), ("csrf_rotate", True, "public"),
    ("route_from_raw_path", True, "public"), ("unit_compile", True, "public"),
    ("cleanup_sqlite_connections", False, "internal"), ("cleanup_mysql_connections", False, "internal"),
    ("mysql_connect", True, "integration"), ("mysql_query", True, "integration"),
]

REMOVED_APIS = ["concat"]


def all_test_text() -> str:
    parts = []
    for pattern in ("*.capy",):
        for path in TEST_DIR.glob(pattern):
            parts.append(path.read_text(errors="ignore"))
    return "\n".join(parts)


def doc_exists(name: str) -> bool:
    page_name = DOC_PAGE_ALIASES.get(name, name)
    path = DOC_DIR / f"{page_name}.txt"
    return path.exists() and "Removed" not in path.read_text(errors="ignore")[:200]


def stdlib_public_functions() -> set[str]:
    source = (ROOT / "src" / "capy" / "stdlib.capy").read_text(errors="ignore")
    names = set(re.findall(r"^function\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(", source, re.M))
    return {name for name in names if not name.startswith("__")}


def compiler_intrinsics() -> set[str]:
    source = (ROOT / "src" / "capy" / "compiler.cpp").read_text(errors="ignore")
    names = set(re.findall(r'!member && callee == "([A-Za-z_][A-Za-z0-9_]*)"', source))
    names.update(re.findall(r'name->value == "(trap|clone)"', source))
    return {name for name in names if not name.startswith("__")}


def required_doc_apis() -> set[str]:
    return (stdlib_public_functions() | compiler_intrinsics()) - DOC_INTERNAL_APIS


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
    for name in sorted(required_doc_apis()):
        if not doc_exists(name):
            errors.append(f"missing source-derived doc page: {name}")
    compiler_h = (ROOT / "src" / "lib" / "compiler.h").read_text(errors="ignore")
    if "#ifndef __BEARER_WASM_UNIT__\nSharedUnit* unit_load" not in compiler_h:
        errors.append("unit_load is not guarded out of wasm-unit exposure")
    for name in REMOVED_APIS:
        page = DOC_DIR / f"{name}.txt"
        if name == "concat" and page.exists() and "Removed" not in page.read_text(errors="ignore")[:300]:
            errors.append("concat doc is not tombstoned")
    if errors:
        print("API coverage manifest FAILED")
        for error in errors:
            print("- " + error)
        return 1
    print(f"API coverage manifest ok: {len(PUBLIC_APIS)} manifest entries and {len(required_doc_apis())} source-derived docs checked")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
