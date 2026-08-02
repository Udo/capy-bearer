#!/usr/bin/env python3
"""Check API pairs and the canonical Capy guide grammar."""
from __future__ import annotations

import argparse
import re
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOWED_ENTRIES = {"render", "cli", "component", "init", "once", "ws"}
PAGE_SECTIONS = {"title", "sig", "params", "content", "see", "output"}
GUIDE_HEADINGS = {"purpose", "minimal executable example", "output", "explanation", "common variants", "edge cases", "related reference"}
CANONICAL_GUIDES = {
    "01-install-and-first-program", "02-source-structure-and-syntax", "03-values-and-types",
    "04-expressions-and-control-flow", "05-functions-and-closures", "06-strings-and-markup",
    "07-collections-and-records", "08-dynamic-values", "09-web-handlers-and-requests",
    "10-units-components-and-exports", "11-tasks-and-jobs", "12-errors-testing-and-style",
}
ARRAYS = ("supported", "partial", "unsupported_by_design", "missing_notes")
CPP_SPECIFIC = "cpp_specific"
EVIDENCE_ARRAY = re.compile(r"inline constexpr std::array<Evidence,\s*\d+>\s+(\w+)\s*\{\{(.*?)\}\};", re.S)
STRING_ARRAY = re.compile(r"inline constexpr std::array<std::string_view,\s*\d+>\s+(\w+)\s*\{\{(.*?)\}\};", re.S)
NAME = re.compile(r'\{\s*"([^"]+)"')
CPP_TOKENS = re.compile(r"\b(?:DValue|Request|String(?:List|Map)?|SharedUnit|std|void|const|auto|template|typename|class|nullptr|new|delete)\b|::|->|#\s*include")
REDIRECT = re.compile(r'redirects\["([^"]+)"\]\s*=\s*"([^"]+)";')


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
    for array, body in STRING_ARRAY.findall(text):
        if array == CPP_SPECIFIC:
            for name in re.findall(r'"([^"]+)"', body):
                if name in found:
                    raise ValueError(f"manifest entry appears more than once: {name}")
                found[name] = "cpp-specific"
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


def check_example_body(page: Path, line: int, language: str, entry: str, body: str) -> list[str]:
    errors = []
    if entry not in ALLOWED_ENTRIES:
        errors.append(f"{page.name}:{line}: unknown example entry: {entry}")
    if not body:
        errors.append(f"{page.name}:{line}: empty {language} example")
    if language == "capy":
        if "__bearer" in body:
            errors.append(f"{page.name}:{line}: Capy example exposes private __bearer API")
        checked = re.sub(r'"(?:\\.|[^"\\])*"', '""', body)
        checked = re.sub(r"//[^\n]*", "", checked)
        if CPP_TOKENS.search(checked):
            errors.append(f"{page.name}:{line}: Capy example contains C++ syntax or a C++ type")
    return errors


def check_page(page: Path, status: str) -> list[str]:
    errors: list[str] = []
    sections = parse_sections(page)
    directives = [(line, header, body) for line, header, body in sections if header.startswith("example")]
    required_status = {"unsupported": "unsupported", "cpp-specific": "cpp-specific"}.get(status)
    headers = {header for _, header, _ in sections}
    if required_status and f"capy-status {required_status}" not in headers:
        errors.append(f"{page.name}: missing :capy-status {required_status}")
    examples = []
    for line, header, body in directives:
        match = re.fullmatch(r"example\s+(capy|cpp)\s+([a-z]+)", header)
        if not match:
            errors.append(f"{page.name}:{line}: bare or invalid example directive: :{header}")
            continue
        language, entry = match.groups()
        errors.extend(check_example_body(page, line, language, entry, body))
        examples.append((line, language, entry))
    capy = [example for example in examples if example[1] == "capy"]
    if required_status:
        if capy:
            errors.append(f"{page.name}: {status} page must not contain a Capy example")
        return errors
    pairs = 0
    index = 0
    while index < len(sections):
        line, header, _ = sections[index]
        match = re.fullmatch(r"example\s+(capy|cpp)\s+([a-z]+)", header)
        if not match:
            index += 1
            continue
        language, entry = match.groups()
        if language == "cpp":
            errors.append(f"{page.name}:{line}: C++ example is unpaired or reversed")
            index += 1
            continue
        if index + 1 == len(sections):
            errors.append(f"{page.name}:{line}: Capy example has no contiguous C++ pair")
            index += 1
            continue
        next_line, next_header, _ = sections[index + 1]
        next_match = re.fullmatch(r"example\s+cpp\s+([a-z]+)", next_header)
        if next_match is None or entry != next_match.group(1):
            errors.append(f"{page.name}:{line}: Capy example needs a matching contiguous C++ pair")
            index += 1
            continue
        pairs += 1
        index += 2
    if status == "supported" and not pairs:
        errors.append(f"{page.name}: supported page needs a Capy/C++ pair")
    return errors


def generated_capy_signature_pages(path: Path) -> set[str]:
    return set(re.findall(r'^\s*\{"([^"]+)", "', path.read_text(), re.M))


def check(pages: Path, manifest: Path, signatures: Path) -> list[str]:
    try:
        statuses = manifest_statuses(manifest)
    except (OSError, ValueError) as error:
        return [f"manifest: {error}"]
    actual = {path.stem for path in pages.glob("*.txt")}
    errors = [f"page set: manifest page missing from docs: {name}" for name in sorted(set(statuses) - actual)]
    errors += [f"page set: documentation page missing from manifest: {name}" for name in sorted(actual - set(statuses))]
    for name in sorted(actual & set(statuses)):
        page = pages / f"{name}.txt"
        errors.extend(check_page(page, statuses[name]))
        if name not in {"3_Blocked functions", "3_Documentation format"} and ":sig\n" not in page.read_text():
            errors.append(f"{page.name}: missing C++ :sig declaration")
    try:
        generated = generated_capy_signature_pages(signatures)
    except OSError as error:
        errors.append(f"Capy signatures: {error}")
        generated = set()
    required = {name for name, status in statuses.items() if status == "supported"} - {"3_Documentation format"}
    errors += [f"Capy signatures: supported page has no generated declaration: {name}" for name in sorted(required - generated)]
    errors += [f"Capy signatures: stale or unsupported generated page: {name}" for name in sorted(generated - required)]
    return errors


def guide_redirects(path: Path) -> dict[str, str]:
    redirects = dict(REDIRECT.findall(path.read_text()))
    if len(redirects) != 14:
        raise ValueError(f"guide redirect map needs 14 old slugs, found {len(redirects)}")
    if set(redirects.values()) != CANONICAL_GUIDES:
        raise ValueError("guide redirect map does not produce the canonical guide set")
    return redirects


def check_language_guides(guides: Path, pages: Path, redirect_header: Path) -> list[str]:
    errors: list[str] = []
    try:
        redirects = guide_redirects(redirect_header)
    except (OSError, ValueError) as error:
        return [f"guide redirects: {error}"]
    articles = {article.stem: article for article in guides.glob("*.txt")}
    errors += [f"noncanonical guide source remains: {slug}" for slug in sorted(set(articles) - CANONICAL_GUIDES)]
    valid_targets = {f"capy-{slug}" for slug in CANONICAL_GUIDES}
    valid_targets.update(page.stem for page in pages.glob("*.txt"))
    for canonical in sorted(CANONICAL_GUIDES):
        page = articles.get(canonical)
        if page is None:
            errors.append(f"canonical guide source is missing: {canonical}")
            continue
        sections = parse_sections(page)
        headers = [header for _, header, _ in sections]
        if headers.count("title") != 1 or headers.count("content") != 1:
            errors.append(f"{page.name}: guide needs one :title and one :content")
        unknown = [header for header in headers if header not in PAGE_SECTIONS and not re.fullmatch(r"example\s+capy\s+[a-z]+", header)]
        if unknown:
            errors.append(f"{page.name}: unsupported directive: :{unknown[0]}")
        headings = {heading.strip().lower() for heading in re.findall(r"^##\s+(.+)$", page.read_text(), re.M)}
        for heading in sorted(GUIDE_HEADINGS - headings):
            errors.append(f"{page.name}: missing section: {heading}")
        examples = [(line, header, body) for line, header, body in sections if header.startswith("example")]
        render_examples = []
        for line, header, body in examples:
            match = re.fullmatch(r"example\s+capy\s+([a-z]+)", header)
            if not match:
                errors.append(f"{page.name}:{line}: guide examples must use :example capy")
                continue
            entry = match.group(1)
            errors.extend(check_example_body(page, line, "capy", entry, body))
            if re.search(r"^##\s", body, re.M):
                errors.append(f"{page.name}:{line}: :output must immediately follow the render example")
            if entry == "render":
                render_examples.append((line, body))
        if len(render_examples) != 1:
            errors.append(f"{page.name}: guide needs one :example capy render")
        output_sections = [(line, body) for line, header, body in sections if header == "output"]
        if len(output_sections) != 1 or not output_sections[0][1]:
            errors.append(f"{page.name}: guide needs one exact nonempty :output")
        elif render_examples and output_sections[0][0] < render_examples[0][0]:
            errors.append(f"{page.name}: :output must follow :example capy render")
        for line, header, body in sections:
            if header == "see":
                for target in (item.strip() for item in body.splitlines()):
                    if not target or target.startswith(">"):
                        continue
                    if target in {f"capy-{old}" for old in redirects}:
                        errors.append(f"{page.name}:{line}: :see target is not canonical: {target}")
                    elif target not in valid_targets:
                        errors.append(f"{page.name}:{line}: unknown :see target: {target}")
    return errors


def fixture_manifest(path: Path) -> None:
    path.write_text('inline constexpr std::array<Evidence, 1> supported{{{"ok","",""}}};\ninline constexpr std::array<Evidence, 0> partial{{}};\ninline constexpr std::array<Evidence, 0> unsupported_by_design{{}};\ninline constexpr std::array<Evidence, 0> missing_notes{{}};\ninline constexpr std::array<std::string_view, 0> cpp_specific{{}};\n')


def self_test() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        pages, guides, header = root / "pages", root / "guides", root / "doc_page.h"
        pages.mkdir(); guides.mkdir()
        manifest, signatures = root / "manifest.h", root / "signatures.h"
        fixture_manifest(manifest); signatures.write_text('{"ok", "function ok"},\n')
        (pages / "ok.txt").write_text(':sig\nvoid ok()\n:example capy render\nprint("ok")\n:example cpp render\nprint("ok\\n");\n')
        if check(pages, manifest, signatures):
            print("self-test API fixture failed")
            return 1
        pairs = []
        canonical_sources = sorted(CANONICAL_GUIDES)
        for index, canonical in enumerate(canonical_sources + canonical_sources[3:5], 1):
            old = f"{index:02d}-old"
            pairs.append(f'\tredirects["{old}"] = "{canonical}";')
            (guides / canonical).with_suffix('.txt').write_text(':title\nGuide\n:content\n## Purpose\ntext\n## Minimal executable example\ntext\n:example capy render\nprint("ok")\n:output\nok\n## Output\ntext\n## Explanation\ntext\n## Common variants\ntext\n## Edge cases\ntext\n## Related reference\ntext\n:see\nok\n')
        header.write_text('\n'.join(pairs))
        guide_errors = check_language_guides(guides, pages, header)
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
        if not check_language_guides(guides, pages, header):
            print("self-test incomplete guide was accepted")
            return 1
    print("Capy documentation example checker self-tests passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pages", type=Path, default=ROOT / "site/doc/pages")
    parser.add_argument("--manifest", type=Path, default=ROOT / "src/capy/parity_manifest.h")
    parser.add_argument("--guides", type=Path, default=ROOT / "site/doc/capy")
    parser.add_argument("--redirect-header", type=Path, default=ROOT / "site/doc/lib/doc_page.h")
    parser.add_argument("--signatures", type=Path, default=ROOT / "site/doc/lib/capy_signatures.generated.h")
    parser.add_argument("--allow-incomplete", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    errors = check(args.pages, args.manifest, args.signatures) + check_language_guides(args.guides, args.pages, args.redirect_header)
    if errors:
        print("Capy documentation examples INCOMPLETE" if args.allow_incomplete else "Capy documentation examples FAILED")
        print(*("- " + error for error in errors), sep="\n")
        return 0 if args.allow_incomplete else 1
    print("Capy documentation examples ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
