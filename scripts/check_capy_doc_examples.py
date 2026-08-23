#!/usr/bin/env python3
"""Check Capy API examples and the canonical Capy guide grammar."""
from __future__ import annotations

import argparse
import re
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOWED_ENTRIES = {"render", "cli", "component", "init", "once", "ws"}
PAGE_SECTIONS = {"title", "sig", "params", "returns", "errors", "content", "see", "output"}
LEGACY_GUIDE_HEADINGS = {"purpose", "minimal executable example", "explanation", "common variants", "edge cases", "related reference"}
CANONICAL_GUIDES = {
    "01-install-and-first-program", "02-source-structure-and-syntax", "03-values-and-types",
    "04-expressions-and-control-flow", "05-functions-and-closures", "06-strings-and-markup",
    "07-collections-and-records", "08-dynamic-values", "09-web-handlers-and-requests",
    "10-units-components-and-exports", "11-tasks-and-jobs", "12-errors-testing-and-style",
    "13-coming-from-react",
}
ARRAYS = ("supported", "partial", "unsupported_by_design", "missing_notes")
EVIDENCE_ARRAY = re.compile(r"inline constexpr std::array<Evidence,\s*\d+>\s+(\w+)\s*\{\{(.*?)\}\};", re.S)
STRING_ARRAY = re.compile(r"inline constexpr std::array<std::string_view,\s*\d+>\s+(\w+)\s*\{\{(.*?)\}\};", re.S)
NAME = re.compile(r'\{\s*"([^"]+)"')
CPP_TOKENS = re.compile(r"\b(?:DValue|Request|String(?:List|Map)?|SharedUnit|std|void|const|auto|template|typename|class|nullptr|new)\b|::|\b\w+\s*->|#\s*include")
CAPY_HANDLER = re.compile(r"(?m)^\s*function\s+(?:INIT|ONCE|CLI|RENDER|COMPONENT(?::[A-Za-z_][A-Za-z0-9_]*)?|WS|TASK(?::[A-Za-z_][A-Za-z0-9_]*)?|SERVE_HTTP(?::[A-Za-z_][A-Za-z0-9_]*)?)\s*\(")
CPP_HANDLER = re.compile(r"(?m)^\s*(?:INIT|ONCE|CLI|RENDER|COMPONENT(?::[A-Za-z_][A-Za-z0-9_]*)?|WS|TASK(?::[A-Za-z_][A-Za-z0-9_]*)?|SERVE_HTTP(?::[A-Za-z_][A-Za-z0-9_]*)?)\s*\(")
CAPY_REQUEST_DECLARATION = re.compile(r"(?m)^\s*(?:var\s+request\b|request\s*:=)")


def manifest_statuses(path: Path) -> dict[str, str]:
    text = path.read_text()
    found: dict[str, str] = {}
    labels = {"supported": "supported", "partial": "partial", "unsupported_by_design": "unsupported", "missing_notes": "missing"}
    for array, body in EVIDENCE_ARRAY.findall(text):
        if array in ARRAYS:
            for name in NAME.findall(body):
                if name in found:
                    raise ValueError(f"manifest entry appears more than once: {name}")
                found[name] = labels[array]
    if not found:
        raise ValueError("no parity entries found in manifest")
    return found


def parse_sections(page: Path) -> list[tuple[int, str, str]]:
    """Use the page directive grammar. A directive body ends at the next directive."""
    lines = page.read_text().splitlines()
    result: list[tuple[int, str, str]] = []
    index = 0
    while index < len(lines):
        line = lines[index]
        if not line.startswith(":"):
            index += 1
            continue
        start = index + 1
        index += 1
        while index < len(lines) and not lines[index].startswith(":"):
            index += 1
        header = line[1:].strip()
        body = "\n".join(lines[start:index])
        if header == "output":
            body = re.split(r"\n(?=## )", body, maxsplit=1)[0]
            if body.endswith("\n"):
                body = body[:-1]
        else:
            body = body.strip()
        result.append((start, header, body))
    return result


def check_example_body(page: Path, line: int, entry: str, body: str) -> list[str]:
    errors = []
    if entry not in ALLOWED_ENTRIES:
        errors.append(f"{page.name}:{line}: unknown example entry: {entry}")
    if not body:
        errors.append(f"{page.name}:{line}: empty Capy example")
    if "__bearer" in body:
        errors.append(f"{page.name}:{line}: Capy example exposes private __bearer API")
    checked = re.sub(r'"(?:\\.|[^"\\])*"', '""', body)
    checked = re.sub(r"//[^\n]*", "", checked)
    if page.stem not in CANONICAL_GUIDES and CPP_TOKENS.search(checked):
        errors.append(f"{page.name}:{line}: Capy example contains a host-language type")
    if page.stem not in CANONICAL_GUIDES and CAPY_HANDLER.search(checked):
        errors.append(f"{page.name}:{line}: API examples must contain a handler body, not a complete Capy handler")
    if page.stem not in CANONICAL_GUIDES and CAPY_REQUEST_DECLARATION.search(checked):
        errors.append(f"{page.name}:{line}: API examples cannot redeclare the handler request parameter")
    return errors

def check_page(page: Path, status: str) -> list[str]:
    errors: list[str] = []
    sections = parse_sections(page)
    headers = {header for _, header, _ in sections}
    for section_name in ("returns", "errors"):
        matching = [(line, body) for line, header, body in sections if header == section_name]
        if len(matching) > 1:
            errors.append(f"{page.name}: duplicate :{section_name} section")
        elif matching and not matching[0][1]:
            errors.append(f"{page.name}:{matching[0][0]}: empty :{section_name} section")
    for line, header, body in sections:
        if header == "params" and re.search(r"(?mi)^\s*return value\s*:", body):
            errors.append(f"{page.name}:{line}: move return value from :params to :returns")
    if status == "unsupported" and "capy-status unsupported" not in headers:
        errors.append(f"{page.name}: missing :capy-status unsupported")
    examples = []
    for line, header, body in sections:
        if not header.startswith("example"):
            continue
        match = re.fullmatch(r"example\s+capy\s+([a-z]+)(?:\s+(.+))?", header)
        if not match:
            errors.append(f"{page.name}:{line}: examples must use :example capy ENTRY")
            continue
        entry, _caption = match.groups()
        errors.extend(check_example_body(page, line, entry, body))
        examples.append((line, entry))
    if status == "supported" and not examples:
        errors.append(f"{page.name}: supported page needs a Capy example")
    return errors

def generated_capy_signatures(path: Path) -> dict[str, list[str]]:
    pages: dict[str, list[str]] = {}
    for page, declaration in re.findall(r'^\s*\{"([^"]+)", "((?:\\.|[^"\\])*)"\},', path.read_text(), re.M):
        pages.setdefault(page, []).append(bytes(declaration, "utf-8").decode("unicode_escape"))
    return pages


def page_names(pages: Path) -> set[str]:
    return {path.relative_to(pages).with_suffix("").as_posix() for path in pages.rglob("*.txt")}

def check(pages: Path, manifest: Path, signatures: Path) -> list[str]:
    try:
        statuses = manifest_statuses(manifest)
    except (OSError, ValueError) as error:
        return [f"manifest: {error}"]
    actual = page_names(pages)
    errors = [f"page set: manifest page missing from docs: {name}" for name in sorted(set(statuses) - actual) if statuses[name] != "legacy"]
    for name in sorted(actual & set(statuses)):
        page = pages / f"{name}.txt"
        errors.extend(check_page(page, statuses[name]))
        if ":sig\n" not in page.read_text():
            errors.append(f"{page.name}: missing Capy :sig declaration")
    try:
        generated = generated_capy_signatures(signatures)
    except OSError as error:
        errors.append(f"Capy signatures: {error}")
        generated = {}
    required = {name for name, status in statuses.items() if status == "supported"}
    errors += [f"Capy signatures: supported page has no generated declaration: {name}" for name in sorted(required - set(generated))]
    errors += [f"Capy signatures: stale or unsupported generated page: {name}" for name in sorted(set(generated) - required)]
    for name in sorted(required & set(generated)):
        sections = parse_sections(pages / f"{name}.txt")
        source = next((body.splitlines() for _line, header, body in sections if header == "sig"), [])
        if source != generated[name]:
            errors.append(f"{name}.txt: :sig does not match generated Capy signatures")
    return errors


def check_language_guides(guides: Path, pages: Path) -> list[str]:
    errors: list[str] = []
    articles = {article.stem: article for article in guides.glob("*.txt")}
    errors += [f"noncanonical guide source remains: {slug}" for slug in sorted(set(articles) - CANONICAL_GUIDES)]
    valid_targets = {f"capy-{slug}" for slug in CANONICAL_GUIDES}
    valid_targets.update(page_names(pages))
    for canonical in sorted(CANONICAL_GUIDES):
        page = articles.get(canonical)
        if page is None:
            errors.append(f"canonical guide source is missing: {canonical}")
            continue
        sections = parse_sections(page)
        headers = [header for _, header, _ in sections]
        if headers.count("title") != 1 or headers.count("content") < 1:
            errors.append(f"{page.name}: guide needs one :title and at least one :content")
        unknown = [header for header in headers if header not in PAGE_SECTIONS and not re.fullmatch(r"example\s+capy\s+[a-z]+(?:\s+.+)?", header)]
        if unknown:
            errors.append(f"{page.name}: unsupported directive: :{unknown[0]}")
        headings = {heading.strip().lower() for heading in re.findall(r"^##\s+(.+)$", page.read_text(), re.M)}
        if len(headings) < 2:
            errors.append(f"{page.name}: guide needs at least two descriptive sections")
        for heading in sorted(LEGACY_GUIDE_HEADINGS & headings):
            errors.append(f"{page.name}: remove legacy template section: {heading}")
        examples = [(line, header, body) for line, header, body in sections if header.startswith("example")]
        render_examples = []
        render_with_output = 0
        for index, (line, header, body) in enumerate(sections):
            if not header.startswith("example"):
                continue
            match = re.fullmatch(r"example\s+capy\s+([a-z]+)(?:\s+.+)?", header)
            if not match:
                errors.append(f"{page.name}:{line}: guide examples must use :example capy")
                continue
            entry = match.group(1)
            errors.extend(check_example_body(page, line, entry, body))
            if entry == "render":
                render_examples.append((line, body))
                if index + 1 < len(sections) and sections[index + 1][1] == "output":
                    render_with_output += 1
        if len(render_examples) < 1:
            errors.append(f"{page.name}: guide needs at least one :example capy render")
        if render_with_output < 1:
            errors.append(f"{page.name}: guide needs one render example with exact output")
        output_sections = [(line, body) for line, header, body in sections if header == "output"]
        if len(output_sections) < 1 or not output_sections[0][1]:
            errors.append(f"{page.name}: guide needs one exact nonempty :output")
        elif render_examples and output_sections[0][0] < render_examples[0][0]:
            errors.append(f"{page.name}: :output must follow :example capy render")
        for line, header, body in sections:
            if header == "see":
                for target in (item.strip() for item in body.splitlines()):
                    if not target or target.startswith(">"):
                        continue
                    if target not in valid_targets:
                        errors.append(f"{page.name}:{line}: unknown :see target: {target}")
    return errors


def fixture_manifest(path: Path) -> None:
    path.write_text('inline constexpr std::array<Evidence, 1> supported{{{"ok","",""}}};\ninline constexpr std::array<Evidence, 0> partial{{}};\ninline constexpr std::array<Evidence, 0> unsupported_by_design{{}};\ninline constexpr std::array<Evidence, 0> missing_notes{{}};\n')


def self_test() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        pages, guides = root / "pages", root / "guides"
        pages.mkdir(); guides.mkdir()
        manifest, signatures = root / "manifest.h", root / "signatures.h"
        fixture_manifest(manifest); signatures.write_text('{"ok", "function ok"},\n')
        (pages / "ok.txt").write_text(':sig\nfunction ok\n:example capy render\nprint("ok")\n')
        if check(pages, manifest, signatures):
            print("self-test API fixture failed")
            return 1
        (pages / "ok.txt").write_text(':sig\nfunction ok\n:params\nreturn value : old location\n:returns\n\n:example capy render\nprint("ok")\n')
        contract_errors = check(pages, manifest, signatures)
        if not any("move return value" in error for error in contract_errors) or not any("empty :returns" in error for error in contract_errors):
            print("self-test accepted an invalid return contract")
            return 1
        (pages / "ok.txt").write_text(':sig\nfunction ok\n:example capy render\nprint("ok")\n')
        if not check_example_body(pages / "ok.txt", 1, "render", "function RENDER(request : dval) {}"):
            print("self-test accepted a complete handler in an API example")
            return 1
        if not check_example_body(pages / "ok.txt", 1, "render", "var request := dval({:})"):
            print("self-test accepted a handler request redeclaration")
            return 1
        if check_example_body(pages / "ok.txt", 1, "render", "function value() s32 { -> 1 }"):
            print("self-test rejected a Capy block yield")
            return 1
        if not check_example_body(pages / "ok.txt", 1, "render", "value->member"):
            print("self-test accepted C++ member access in a Capy example")
            return 1
        for canonical in CANONICAL_GUIDES:
            (guides / f"{canonical}.txt").write_text(":title\nGuide\n:content\n## First step\ntext\n:example capy render\nprint(\"ok\")\n:output\nok\n:content\n## Next step\ntext\n:see\nok\n")
        guide_errors = check_language_guides(guides, pages)
        if guide_errors:
            print("self-test guide fixture failed:", *guide_errors, sep="\n- ")
            return 1
        output_fixture = root / "output.txt"
        output_fixture.write_text(":output\n  exact output  \n\n## Output\ntext\n")
        output_body = next(body for _line, section, body in parse_sections(output_fixture) if section == "output")
        if output_body != "  exact output  ":
            print("self-test output whitespace was not preserved")
            return 1
        (guides / sorted(CANONICAL_GUIDES)[0]).with_suffix('.txt').write_text(':title\nGuide\n:content\n## Purpose\ntext\n')
        if not check_language_guides(guides, pages):
            print("self-test incomplete guide was accepted")
            return 1
    print("Capy documentation example checker self-tests passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pages", type=Path, default=ROOT / "site/doc/pages")
    parser.add_argument("--manifest", type=Path, default=ROOT / "src/capy/parity_manifest.h")
    parser.add_argument("--guides", type=Path, default=ROOT / "site/doc/capy")
    parser.add_argument("--signatures", type=Path, default=ROOT / "site/doc/lib/capy_signatures.generated.h")
    parser.add_argument("--allow-incomplete", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    errors = check(args.pages, args.manifest, args.signatures) + check_language_guides(args.guides, args.pages)
    if errors:
        print("Capy documentation examples INCOMPLETE" if args.allow_incomplete else "Capy documentation examples FAILED")
        print(*("- " + error for error in errors), sep="\n")
        return 0 if args.allow_incomplete else 1
    print("Capy documentation examples ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
