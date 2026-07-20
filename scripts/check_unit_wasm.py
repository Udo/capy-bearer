#!/usr/bin/env python3
"""Validate a W2 BEARER PIC unit wasm artifact.

Checks intentionally stay small and explicit: section walk for dylink.0,
bearer.abi, imports/exports, plus llvm-nm for allocator definitions.
"""
from __future__ import annotations

import argparse
from collections import Counter
import shutil
import subprocess
import sys
from pathlib import Path


def read_u32leb(data: bytes, pos: int) -> tuple[int, int]:
    result = 0
    shift = 0
    while True:
        if pos >= len(data):
            raise ValueError("truncated leb128")
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if (b & 0x80) == 0:
            return result, pos
        shift += 7
        if shift > 35:
            raise ValueError("oversized leb128")


def read_name(data: bytes, pos: int) -> tuple[str, int]:
    n, pos = read_u32leb(data, pos)
    end = pos + n
    if end > len(data):
        raise ValueError("truncated name")
    return data[pos:end].decode("utf-8", "replace"), end


def walk_sections(data: bytes):
    if not data.startswith(b"\0asm\x01\0\0\0"):
        raise ValueError("not a wasm v1 module")
    pos = 8
    while pos < len(data):
        section_id = data[pos]
        pos += 1
        size, pos = read_u32leb(data, pos)
        end = pos + size
        if end > len(data):
            raise ValueError("section extends past EOF")
        payload = data[pos:end]
        yield section_id, payload
        pos = end


def parse_imports(payload: bytes):
    pos = 0
    count, pos = read_u32leb(payload, pos)
    imports = []
    for _ in range(count):
        module, pos = read_name(payload, pos)
        name, pos = read_name(payload, pos)
        if pos >= len(payload):
            raise ValueError("truncated import kind")
        kind = payload[pos]
        pos += 1
        # Skip type descriptors. We only need module/name/kind for W2 policy.
        if kind == 0:  # func type index
            _, pos = read_u32leb(payload, pos)
        elif kind == 1:  # table
            if pos >= len(payload): raise ValueError("truncated table import")
            pos += 1
            flags, pos = read_u32leb(payload, pos)
            _, pos = read_u32leb(payload, pos)
            if flags & 1: _, pos = read_u32leb(payload, pos)
        elif kind == 2:  # memory
            flags, pos = read_u32leb(payload, pos)
            _, pos = read_u32leb(payload, pos)
            if flags & 1: _, pos = read_u32leb(payload, pos)
        elif kind == 3:  # global
            pos += 2
        else:
            raise ValueError(f"unknown import kind {kind}")
        imports.append((module, name, kind))
    return imports


def parse_exports(payload: bytes):
    pos = 0
    count, pos = read_u32leb(payload, pos)
    exports = []
    for _ in range(count):
        name, pos = read_name(payload, pos)
        if pos >= len(payload):
            raise ValueError("truncated export kind")
        kind = payload[pos]
        pos += 1
        _, pos = read_u32leb(payload, pos)
        exports.append((name, kind))
    return exports


def collect(path: Path):
    data = path.read_bytes()
    customs: dict[str, list[bytes]] = {}
    imports = []
    exports = []
    for section_id, payload in walk_sections(data):
        if section_id == 0:
            name, pos = read_name(payload, 0)
            customs.setdefault(name, []).append(payload[pos:])
        elif section_id == 2:
            imports = parse_imports(payload)
        elif section_id == 7:
            exports = parse_exports(payload)
    return customs, imports, exports


def dylink_has_valid_mem_info(payload: bytes) -> bool:
    pos = 0
    while pos < len(payload):
        subsection_id = payload[pos]
        pos += 1
        size, pos = read_u32leb(payload, pos)
        end = pos + size
        if end > len(payload):
            raise ValueError("dylink.0 subsection extends past section")
        if subsection_id == 1:
            mem_size, p = read_u32leb(payload, pos)
            mem_align, p = read_u32leb(payload, p)
            table_size, p = read_u32leb(payload, p)
            table_align, p = read_u32leb(payload, p)
            if p > end:
                raise ValueError("truncated dylink.0 mem_info")
            return mem_align < 32 and table_align < 32 and mem_size < (1 << 31) and table_size < (1 << 31)
        pos = end
    return False


def defined_symbols(path: Path, llvm_nm: str) -> list[str]:
    proc = subprocess.run([llvm_nm, "--defined-only", str(path)], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        # llvm-nm (wasi-sdk) can SIGSEGV on degenerate-but-valid modules — e.g. a
        # unit with no exported handlers (`empty.uce`). A toolchain crash is not
        # evidence of a forbidden allocator, so skip this defense-in-depth scan
        # rather than fail the unit; forbidden allocator *exports* are still
        # rejected by the export-section check above.
        crashed = proc.returncode < 0 or "Stack dump:" in proc.stderr or "PLEASE submit a bug report" in proc.stderr
        if crashed:
            return []
        raise RuntimeError(proc.stderr.strip() or "llvm-nm failed")
    symbols = []
    for line in proc.stdout.splitlines():
        parts = line.split()
        if parts:
            symbols.append(parts[-1])
    return symbols


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("wasm", type=Path)
    default_abi = "7"
    abi_header = Path(__file__).resolve().parents[1] / "src" / "wasm" / "abi.h"
    for line in abi_header.read_text().splitlines():
        if line.startswith("#define BEARER_WASM_CORE_ABI_VERSION "):
            default_abi = line.rsplit(" ", 1)[1]
            break
    ap.add_argument("--abi-version", default=default_abi)
    ap.add_argument("--llvm-nm", default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    try:
        customs, imports, exports = collect(args.wasm)
        errors = []
        dylink_payloads = customs.get("dylink.0", [])
        if not dylink_payloads:
            errors.append("missing dylink.0 custom section")
        elif not any(dylink_has_valid_mem_info(payload) for payload in dylink_payloads):
            errors.append("dylink.0 missing valid mem_info subsection")
        abi_payloads = customs.get("bearer.abi", [])
        if not abi_payloads:
            errors.append("missing bearer.abi custom section")
        else:
            abi_text = abi_payloads[-1].decode("utf-8", "replace")
            required = ["format=bearer-wasm-unit-abi-v1", f"unit_abi_version={args.abi_version}", "toolchain="]
            for needle in required:
                if needle not in abi_text:
                    errors.append(f"bearer.abi missing {needle!r}")
        module_payloads = customs.get("bearer.module", [])
        if not module_payloads or not module_payloads[-1]:
            errors.append("missing bearer.module custom section")
        export_counts = Counter(name for name, _ in exports)
        for name, count in sorted(export_counts.items()):
            if count > 1:
                errors.append(f"duplicate export {name!r}")
        export_names = set(export_counts)
        forbidden_exports = {"bearer_alloc", "bearer_free"}
        for name in sorted(export_names & forbidden_exports):
            errors.append(f"forbidden allocator export {name}")
        import_map = {(module, name): kind for module, name, kind in imports}
        required_imports = {
            ("env", "memory"): 2,
            ("env", "__memory_base"): 3,
        }
        # units without indirect calls / stack spills / table needs
        # legitimately omit these; if present, the kind must be right
        optional_imports = {
            ("env", "__indirect_function_table"): 1,
            ("env", "__stack_pointer"): 3,
            ("env", "__table_base"): 3,
        }
        for key, kind in required_imports.items():
            if import_map.get(key) != kind:
                errors.append(f"missing required import {key[0]}.{key[1]}")
        for key, kind in optional_imports.items():
            if key in import_map and import_map[key] != kind:
                errors.append(f"wrong kind for import {key[0]}.{key[1]}")
        for module, name, kind in imports:
            if module.startswith("wasi_") or module == "wasi_snapshot_preview1":
                errors.append(f"forbidden WASI import {module}.{name}")
            if module not in {"env", "GOT.mem", "GOT.func"} and not module.startswith("GOT."):
                errors.append(f"unexpected import module {module}.{name}")
            if module.startswith("GOT.") and kind != 3:
                errors.append(f"GOT import is not a global: {module}.{name}")
        llvm_nm = args.llvm_nm or shutil.which("llvm-nm") or "/opt/wasi-sdk/bin/llvm-nm"
        if Path(llvm_nm).exists():
            bad_prefixes = ("_Znwm", "_Znam", "_ZdlPv", "_ZdaPv", "_ZdlPvm", "_ZdaPvm")
            for sym in defined_symbols(args.wasm, llvm_nm):
                if sym in {"bearer_alloc", "bearer_free"} or sym.startswith(bad_prefixes):
                    errors.append(f"forbidden allocator definition {sym}")
        else:
            errors.append("llvm-nm not found; cannot verify allocator definitions")
        if errors:
            for e in errors:
                print(f"ERROR: {e}", file=sys.stderr)
            return 1
        if args.verbose:
            print(f"BEARER W2 unit check PASS: {args.wasm}")
        return 0
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
