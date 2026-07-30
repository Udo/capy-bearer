#!/usr/bin/env python3
"""Check typed Capy/C++ documentation example pairs against parity_manifest.h.

Strict mode is the final gate.  --allow-incomplete reports conversion defects
without failing, so it can be run against the legacy C++-only corpus.
"""
from __future__ import annotations

import argparse
import re
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOWED_ENTRIES = {"render", "cli", "component", "init", "once", "ws"}
ARRAYS = ("supported", "partial", "unsupported_by_design", "missing_notes")
CPP_SPECIFIC = "cpp_specific"
EVIDENCE_ARRAY = re.compile(
    r"inline constexpr std::array<Evidence,\s*\d+>\s+(\w+)\s*\{\{(.*?)\}\};", re.S
)
STRING_ARRAY = re.compile(
    r"inline constexpr std::array<std::string_view,\s*\d+>\s+(\w+)\s*\{\{(.*?)\}\};", re.S
)
NAME = re.compile(r'\{\s*"([^"]+)"')
CPP_TOKENS = re.compile(
    r"\b(?:DValue|Request|String(?:List|Map)?|SharedUnit|std|void|const|"
    r"auto|template|typename|class|nullptr|new|delete)\b|::|->|#\s*include"
)


def manifest_statuses(path: Path) -> dict[str, str]:
    """Read every documentation classification directly from the C++ manifest."""
    text = path.read_text()
    found: dict[str, str] = {}
    labels = {"supported": "supported", "partial": "partial",
              "unsupported_by_design": "unsupported", "missing_notes": "missing"}
    for array, body in EVIDENCE_ARRAY.findall(text):
        if array not in ARRAYS:
            continue
        for name in NAME.findall(body):
            if name in found:
                raise ValueError(f"manifest entry appears more than once: {name}")
            found[name] = labels[array]
    for array, body in STRING_ARRAY.findall(text):
        if array != CPP_SPECIFIC:
            continue
        for name in re.findall(r'"([^"]+)"', body):
            if name in found:
                raise ValueError(f"manifest entry appears more than once: {name}")
            found[name] = "cpp-specific"
    if not found:
        raise ValueError("no parity entries found in manifest")
    return found


def directives(page: Path) -> list[tuple[int, str, str, int]]:
    """Return example directive line/content blocks; content ends at next section."""
    lines = page.read_text().splitlines()
    result = []
    for index, line in enumerate(lines):
        if not line.startswith(":example"):
            continue
        end = index + 1
        while end < len(lines) and not lines[end].startswith(":"):
            end += 1
        result.append((index + 1, line, "\n".join(lines[index + 1:end]).strip(), end + 1))
    return result


def check_page(page: Path, status: str) -> list[str]:
    errors: list[str] = []
    text = page.read_text()
    required_status = {"unsupported": "unsupported", "cpp-specific": "cpp-specific"}.get(status)
    if required_status and f":capy-status {required_status}" not in text.splitlines():
        errors.append(f"{page.name}: missing :capy-status {required_status}")

    examples = []
    for line, directive, body, end_line in directives(page):
        match = re.fullmatch(r":example\s+(capy|cpp)\s+([a-z]+)", directive)
        if not match:
            errors.append(f"{page.name}:{line}: bare or invalid example directive: {directive}")
            continue
        language, entry = match.groups()
        if entry not in ALLOWED_ENTRIES:
            errors.append(f"{page.name}:{line}: unknown example entry: {entry}")
        if not body:
            errors.append(f"{page.name}:{line}: empty {language} example")
        if language == "capy":
            if "__bearer" in body:
                errors.append(f"{page.name}:{line}: Capy example exposes private __bearer API")
            checked_body = re.sub(r'"(?:\\.|[^"\\])*"', '""', body)
            checked_body = re.sub(r"//[^\n]*", "", checked_body)
            if CPP_TOKENS.search(checked_body):
                errors.append(f"{page.name}:{line}: Capy example contains obvious C++ syntax or type")
        examples.append((line, language, entry, end_line))

    capy = [example for example in examples if example[1] == "capy"]
    if required_status:
        if capy:
            errors.append(f"{page.name}: {status} page must not contain a Capy example")
        # C++ examples are intentionally permitted here without a fake Capy pair.
        return errors

    paired_capy = 0
    index = 0
    while index < len(examples):
        line, language, entry, end_line = examples[index]
        if language != "capy":
            errors.append(f"{page.name}:{line}: C++ example is unpaired or reversed")
            index += 1
            continue
        if index + 1 == len(examples):
            errors.append(f"{page.name}:{line}: Capy example has no contiguous C++ pair")
            index += 1
            continue
        next_line, next_language, next_entry, _ = examples[index + 1]
        if next_line != end_line:
            errors.append(f"{page.name}:{line}: Capy example is not contiguous with C++")
            index += 1
            continue
        if next_language != "cpp":
            errors.append(f"{page.name}:{line}: Capy example is not followed by C++")
            index += 1
            continue
        if entry != next_entry:
            errors.append(f"{page.name}:{line}: pair entry mismatch ({entry} != {next_entry})")
        paired_capy += 1
        index += 2
    if status == "supported" and not paired_capy:
        errors.append(f"{page.name}: supported page needs at least one Capy/C++ pair")
    return errors


def check(pages: Path, manifest: Path) -> list[str]:
    try:
        statuses = manifest_statuses(manifest)
    except (OSError, ValueError) as error:
        return [f"manifest: {error}"]
    actual = {path.stem for path in pages.glob("*.txt")}
    expected = set(statuses)
    errors = []
    for name in sorted(expected - actual):
        errors.append(f"page set: manifest page missing from docs: {name}")
    for name in sorted(actual - expected):
        errors.append(f"page set: documentation page missing from manifest: {name}")
    for name in sorted(actual & expected):
        errors.extend(check_page(pages / f"{name}.txt", statuses[name]))
    return errors


def fixture_manifest(path: Path, names: dict[str, str]) -> None:
    groups = {key: [] for key in ("supported", "partial", "unsupported_by_design", "missing_notes", "cpp_specific")}
    reverse = {"supported": "supported", "partial": "partial", "unsupported": "unsupported_by_design",
               "missing": "missing_notes", "cpp-specific": "cpp_specific"}
    for name, status in names.items():
        groups[reverse[status]].append(name)
    def evidence(group: str) -> str:
        values = ",".join(f'{{"{name}","",""}}' for name in groups[group])
        return f"inline constexpr std::array<Evidence, {len(groups[group])}> {group}{{{{{values}}}}};"
    cpp = ",".join(f'"{name}"' for name in groups["cpp_specific"])
    path.write_text("\n".join(evidence(group) for group in groups if group != "cpp_specific") +
                    f"\ninline constexpr std::array<std::string_view, {len(groups['cpp_specific'])}> cpp_specific{{{{{cpp}}}}};\n")


def self_test() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        pages, manifest = root / "pages", root / "parity_manifest.h"
        pages.mkdir()
        names = {"ok": "supported", "unsupported": "unsupported", "cpp": "cpp-specific"}
        fixture_manifest(manifest, names)
        def write(name: str, body: str) -> None:
            (pages / f"{name}.txt").write_text(body)
        valid = ":example capy render\nprint(\"ok\")\n:example cpp render\nprint(\"ok\\n\");\n"
        write("ok", valid)
        write("unsupported", ":capy-status unsupported\n:example cpp render\nprint(\"ok\\n\");\n")
        write("cpp", ":capy-status cpp-specific\n:example cpp render\nprint(\"ok\\n\");\n")
        if check(pages, manifest):
            print("self-test valid fixture failed:", *check(pages, manifest), sep="\n- ")
            return 1
        cases = {
            "bare": ("ok", ":example\nprint(\"x\")\n"),
            "empty": ("ok", ":example capy render\n:example cpp render\nprint(\"x\");\n"),
            "reversed": ("ok", ":example cpp render\nprint(\"x\");\n:example capy render\nprint(\"x\")\n"),
            "unknown-entry": ("ok", ":example capy nope\nprint(\"x\")\n:example cpp nope\nprint(\"x\");\n"),
            "unknown-language": ("ok", ":example rust render\nprint(\"x\")\n"),
            "missing-pair": ("ok", ":content\nno examples yet\n"),
            "mismatch": ("ok", ":example capy render\nprint(\"x\")\n:example cpp cli\nprint(\"x\");\n"),
            "noncontiguous": ("ok", ":example capy render\nprint(\"x\")\n:content\nno gap\n:example cpp render\nprint(\"x\");\n"),
            "unpaired": ("ok", ":example capy render\nprint(\"x\")\n"),
            "private": ("ok", ":example capy render\n__bearer_secret()\n:example cpp render\nprint(\"x\");\n"),
            "cpp-syntax": ("ok", ":example capy render\nDValue value\n:example cpp render\nprint(\"x\");\n"),
            "missing-status": ("unsupported", ":example cpp render\nprint(\"x\");\n"),
            "status-capy": ("cpp", ":capy-status cpp-specific\n:example capy render\nprint(\"x\")\n"),
        }
        for label, (name, body) in cases.items():
            original = (pages / f"{name}.txt").read_text()
            write(name, body)
            if not check(pages, manifest):
                print(f"self-test {label} was accepted")
                return 1
            write(name, original)
        (pages / "ghost.txt").write_text("")
        if not any(error.startswith("page set:") for error in check(pages, manifest)):
            print("self-test extra page was accepted")
            return 1
        (pages / "ghost.txt").unlink()
        fixture_manifest(manifest, {"ok": "supported"})
        if not any(error.startswith("page set:") for error in check(pages, manifest)):
            print("self-test missing manifest page was accepted")
            return 1
    print("Capy documentation example checker self-tests passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pages", type=Path, default=ROOT / "site/doc/pages")
    parser.add_argument("--manifest", type=Path, default=ROOT / "src/capy/parity_manifest.h")
    parser.add_argument("--allow-incomplete", action="store_true", help="report conversion defects without failing")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    errors = check(args.pages, args.manifest)
    if errors:
        heading = "Capy documentation examples INCOMPLETE" if args.allow_incomplete else "Capy documentation examples FAILED"
        print(heading)
        print(*("- " + error for error in errors), sep="\n")
        return 0 if args.allow_incomplete else 1
    print("Capy documentation examples ok")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
