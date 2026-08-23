#!/usr/bin/env python3
from __future__ import annotations

import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "site" / "doc"
OUT = DOC / "content"
API_OUT = OUT / "api"
TYPE_OUT = OUT / "type"
HANDLER_OUT = OUT / "handler"
GUIDE_OUT = OUT / "guide"
PRESENTATION_OUT = DOC / "components" / "doc_page.capy"
LEGACY_CONTENT = ("content-api-*.capy", "content-guide.capy", "content-handler.capy", "content-type.capy")
LEGACY_COMPONENTS = (DOC / "components" / "page.capy",)


SECTION_NAMES = {"title", "sig", "params", "returns", "errors", "note", "warning", "content", "see", "output"}
STOP_WORDS = {"the", "and", "for", "how", "with", "to", "of", "in"}


def capy_string(value: str) -> str:
    value = value.replace("\\", "\\\\").replace('"', '\\"')
    value = value.replace("\r", "\\r").replace("\n", "\\n")
    return '"' + value + '"'


def legacy_heading(section: str) -> str:
    if section == "desc":
        return ""
    if section == "related":
        return "## PHP & JS Equivalents"
    return "## " + section



def slugify(value: str) -> str:
    value = value.replace("::", "-").replace("C++", "cpp")
    value = value.replace("DValue", "dvalue").replace("StringList", "string-list").replace("StringMap", "string-map")
    value = re.sub(r"[^A-Za-z0-9]+", "-", value).strip("-").lower()
    return value or "page"


def unique_slug(base: str, used: dict[str, str], source_page: str) -> str:
    existing = used.get(base)
    if existing:
        raise ValueError(f"duplicate documentation slug {base}: {existing}, {source_page}")
    used[base] = source_page
    return base


def add_example(page: dict, source_page: str, language: str, entry: str, caption: str, body: str) -> None:
    if language != "capy":
        page["example_error"] = "examples must use :example capy ENTRY"
        return
    if entry not in {"render", "cli", "component", "init", "once", "ws"}:
        page["example_error"] = "unknown example entry: " + entry
        return
    if source_page.startswith("capy-"):
        page["guide_examples"].append({"entry": entry, "caption": caption, "body": body})
    else:
        page["examples"].append({"entry": entry, "caption": caption, "body": body})


def flush_section(page: dict, source_page: str, section: str, lines: list[str], content: list[str]) -> None:
    if not section:
        return
    if section == "title":
        title = "\n".join(lines).strip()
        if title != source_page:
            page["title"] = title
    elif section == "sig":
        pass
    elif section == "params":
        page["param_lines"].extend(lines)
    elif section == "returns":
        page["returns"] = "\n".join(lines).strip()
    elif section == "errors":
        page["errors"] = "\n".join(lines).strip()
    elif section == "note":
        text = "\n".join(lines).strip()
        if text:
            page["notes"].append(text)
    elif section == "warning":
        text = "\n".join(lines).strip()
        if text:
            page["warnings"].append(text)
    elif section == "see":
        page["see"].extend(line.strip() for line in lines if line.strip())
    elif section == "output":
        if not page["guide_examples"]:
            page["example_error"] = "output must follow a Capy guide example"
        else:
            output = "\n".join(lines)
            page["guide_examples"][-1]["output"] = output
            if source_page.startswith("capy-"):
                content.extend(["**Output**", "", "```text", *output.split("\n"), "```", ""])
    else:
        content.extend(lines)


def parse_doc(source_page: str, source: str) -> dict:
    page = {
        "source_page": source_page,
        "kind": "guide" if source_page.startswith("capy-") else "api",
        "title": "",
        "content": "",
        "capy_sig_lines": [],
        "param_lines": [],
        "returns": "",
        "errors": "",
        "notes": [],
        "warnings": [],
        "see": [],
        "examples": [],
        "guide_examples": [],
        "capy_status": "",
        "example_error": "",
    }
    current = ""
    current_lines: list[str] = []
    content: list[str] = []
    ex_language = ""
    ex_entry = ""
    ex_caption = ""

    for line in source.split("\n"):
        if current == "output" and line.startswith("## "):
            flush_section(page, source_page, current, current_lines, content)
            current_lines = []
            current = "content"
        if line and line.startswith(":"):
            if current == "example":
                body = "\n".join(current_lines)
                if not body.strip():
                    page["example_error"] = "empty example block"
                else:
                    add_example(page, source_page, ex_language, ex_entry, ex_caption, body)
                    if source_page.startswith("capy-") and ex_language == "capy":
                        content.extend(["```capy", *body.split("\n"), "```", ""])
            else:
                flush_section(page, source_page, current, current_lines, content)
            current_lines = []
            section = line[1:].strip()
            if section == "example" or section.startswith("example "):
                current = "example"
                ex_language = "legacy"
                ex_entry = "render"
                ex_caption = ""
                if section != "example":
                    header = section[8:].strip()
                    parts = header.split(" ", 2)
                    if len(parts) >= 2 and parts[0] in {"capy", "cpp"}:
                        ex_language = parts[0]
                        ex_entry = parts[1]
                        ex_caption = parts[2].strip() if len(parts) == 3 else ""
                    else:
                        page["example_error"] = "invalid example header: " + section
                continue
            if section.startswith("capy-status "):
                status = section[12:].strip()
                if status == "unsupported":
                    page["capy_status"] = status
                else:
                    page["example_error"] = "unknown Capy status: " + status
                current = ""
                continue
            if section in SECTION_NAMES:
                current = section
                continue
            current = "legacy"
            heading = legacy_heading(section)
            if heading:
                if content and content[-1] != "":
                    content.append("")
                content.extend([heading, ""])
            continue
        current_lines.append(line)

    if current == "example":
        body = "\n".join(current_lines)
        if not body.strip():
            page["example_error"] = "empty example block"
        else:
            add_example(page, source_page, ex_language, ex_entry, ex_caption, body)
            if source_page.startswith("capy-") and ex_language == "capy":
                content.extend(["```capy", *body.split("\n"), "```", ""])
    else:
        flush_section(page, source_page, current, current_lines, content)
    page["content"] = "\n".join(content)
    page["title"] = page["title"].strip()
    return page


def assign_string(field: str, value: str, indent: str = "    ") -> list[str]:
    if not value:
        return []
    chunks = [value[i:i + 1800] for i in range(0, len(value), 1800)] or [""]
    lines = [f"{indent}page[{capy_string(field)}] = {capy_string(chunks[0])}"]
    for chunk in chunks[1:]:
        lines.append(f"{indent}page[{capy_string(field)}] = string(page[{capy_string(field)}], {capy_string('')}) + {capy_string(chunk)}")
    return lines


def dval_array(values) -> str:
    return "dval([" + ", ".join(capy_value(value) for value in values) + "])"


def capy_value(value) -> str:
    if isinstance(value, str):
        return capy_string(value)
    if isinstance(value, list):
        return "[" + ", ".join(capy_value(item) for item in value) + "]"
    if isinstance(value, dict):
        parts = []
        for key, item in value.items():
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
                parts.append(f"{key}: {capy_value(item)}")
            else:
                parts.append(f"{capy_string(key)}: {capy_value(item)}")
        return "{" + ", ".join(parts) + "}"
    raise TypeError(value)


def write_content_module(path: Path, page: dict) -> None:
    lines = ["function COMPONENT(request : dval) {", "    var page := dval({source_page: \"\"})"]
    for key in ["source_page", "kind", "slug", "route", "title", "content", "returns", "errors", "capy_status", "example_error", "label"]:
        lines.extend(assign_string(key, page.get(key, "")))
    for key in ["capy_sig_lines", "param_lines", "notes", "warnings", "see", "examples", "guide_examples"]:
        lines.append(f"    page[{capy_string(key)}] = {dval_array(page.get(key, []))}")
    lines.extend(["    component_render(\"/doc/components/doc_page.capy\", page)", "}", ""])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def write_presentation_component(path: Path) -> None:
    path.write_text("""function section(title : string, body : string) void {
    if trim(body) == \"\" { return }
    print(\"<div class=\\\"doc-section\\\"><h3>\", html_escape(title), \"</h3>\", markdown_to_html(body), \"</div>\")
}

function render_lines(title : string, lines : dval) void {
    if length(lines) == 0 { return }
    print(\"<div class=\\\"doc-section signatures\\\"><h3>\", html_escape(title), \"</h3><pre>\")
    var first := true
    for line := lines {
        if !first { print(\"\\n\") }
        print(html_escape(string(line, \"\")))
        first = false
    }
    print(\"</pre></div>\")
}

function render_params(lines : dval) void {
    if length(lines) == 0 { return }
    print(\"<div class=\\\"doc-section params\\\"><h3>Parameters</h3>\")
    for line := lines {
        var text := trim(string(line, \"\"))
        if text != \"\" { print(\"<div>\", markdown_to_html(text), \"</div>\") }
    }
    print(\"</div>\")
}

function render_callouts(class_name : string, title : string, blocks : dval) void {
    for body := blocks {
        print(\"<div class=\\\"doc-section doc-callout \", html_escape(class_name), \"\\\"><h3>\", html_escape(title), \"</h3>\", markdown_to_html(string(body, \"\")), \"</div>\")
    }
}

function render_examples(page : dval) void {
    if string(page.example_error, \"\") != \"\" {
        print(\"<div class=\\\"doc-section example error\\\"><h3>Example format error</h3><pre>\", html_escape(string(page.example_error, \"\")), \"</pre></div>\")
        return
    }
    for example := page.examples {
        print(\"<div class=\\\"doc-section example\\\"><h3>Example</h3><div class=\\\"example-language capy\\\"><h4>Capy</h4><pre class=\\\"example-source\\\">\", html_escape(string(example.body, \"\")), \"</pre></div></div>\")
    }
    for example := page.guide_examples {
        print(\"<div class=\\\"doc-section example\\\"><h3>Example</h3><div class=\\\"example-language capy\\\"><h4>Capy</h4><pre class=\\\"example-source\\\">\", html_escape(string(example.body, \"\")), \"</pre></div>\")
        if example.output? {
            print(\"<p><strong>Output</strong></p><div class=\\\"example-output\\\"><div class=\\\"example-output-label\\\">Output</div><pre>\", html_escape(string(example.output, \"\")), \"</pre></div>\")
        }
        print(\"</div>\")
    }
}

function render_see(page : dval) void {
    if length(page.see) == 0 { return }
    print(\"<aside class=\\\"doc-section see-also\\\"><h3>See also</h3>\")
    for item := page.see {
        component_render(\"/doc/components/doc_link.capy\", item)
    }
    print(\"</aside>\")
}

function render_guide_navigation(page : dval) void {
    if string(page.kind, \"\") != \"guide\" { return }
    print(\"<nav class=\\\"guide-navigation\\\" aria-label=\\\"Capy guide navigation\\\"><div class=\\\"guide-previous\\\">Previous: <a href=\\\"/doc/\\\">Guide index</a></div><div class=\\\"guide-index\\\"><a href=\\\"/doc/\\\">Guide index</a></div><div class=\\\"guide-next\\\">Next: <a href=\\\"/doc/\\\">Guide index</a></div></nav>\")
}

function COMPONENT(request : dval) {
    var page := request.props
    print(\"<div class=\\\"doc-detail-layout\\\"><main class=\\\"content doc-page doc-detail\\\"><h2>\", html_escape(string(page.title, string(page.label, \"Documentation\"))), \"</h2>\")
    render_lines(\"Capy\", page.capy_sig_lines)
    if string(page.content, \"\") != \"\" {
        print(\"<div class=\\\"doc-section content\\\">\")
        if string(page.kind, \"\") == \"api\" { print(\"<h3>Description</h3>\") }
        print(markdown_to_html(string(page.content, \"\")), \"</div>\")
    }
    render_params(page.param_lines)
    section(\"Return Values\", string(page.returns, \"\"))
    section(\"Errors\", string(page.errors, \"\"))
    render_callouts(\"note\", \"Note\", page.notes)
    render_callouts(\"warning\", \"Warning\", page.warnings)
    render_examples(page)
    render_guide_navigation(page)
    print(\"</main>\")
    render_see(page)
    print(\"</div>\")
}
""", encoding="utf-8")


def listing_pages(pages: list[dict]) -> list[dict]:
    kind_order = {"api": 0, "type": 1, "handler": 2}
    return sorted(
        pages,
        key=lambda page: (
            0 if page["kind"] == "guide" else 1,
            page["source_page"] if page["kind"] == "guide" else kind_order[page["kind"]],
            page["slug"],
        ),
    )


def write_index_component(pages: list[dict]) -> None:
    guides = [page for page in listing_pages(pages) if page["kind"] == "guide"]
    references = [page for page in listing_pages(pages) if page["kind"] != "guide"]
    lines = [
        "function link(route : string, label : string, kind : string) void {",
        "    print(\"<a href=\\\"\", html_escape(route), \"\\\">\", html_escape(label))",
        "    if kind == \"api\" { print(\"<span class=\\\"dim\\\">()</span>\") }",
        "    print(\"</a>\")",
        "}",
        "",
        "function COMPONENT(request : dval) {",
        "    print(\"<section class=\\\"capy-guide-index\\\"><h2>Learn Capy</h2><div class=\\\"capy-guide-grid\\\">\")",
    ]
    for page in guides:
        lines.append(
            "    print(\"<div class=\\\"capy-guide-card\\\">\"); link("
            + capy_string(page["route"])
            + ", " + capy_string(page["title"])
            + ", " + capy_string(page["kind"])
            + "); print(\"</div>\")"
        )
    lines.extend([
        "    print(\"</div></section><div class=\\\"api-reference-heading\\\"><h2>API Reference</h2><p>Bearer APIs available to Capy units.</p></div>\")",
        "    print(\"<form class=\\\"search-bar\\\" action=\\\"/doc/search/\\\" method=\\\"get\\\"><input id=\\\"doc-search\\\" name=\\\"q\\\" type=\\\"search\\\" aria-label=\\\"Search documentation\\\" placeholder=\\\"Search documentation…\\\" autocomplete=\\\"off\\\" spellcheck=\\\"false\\\"></input><button type=\\\"submit\\\">Search</button></form>\")",
        "    print(\"<div class=\\\"index-pane\\\" id=\\\"index-pane\\\"><main class=\\\"content\\\"><h2>All APIs</h2><div class=\\\"func-grid\\\">\")",
    ])
    for page in references:
        lines.append(
            "    print(\"<div class=\\\"func-item\\\">\"); link("
            + capy_string(page["route"])
            + ", " + capy_string(page["title"])
            + ", " + capy_string(page["kind"])
            + "); print(\"</div>\")"
        )
    lines.extend(["    print(\"</div></main></div>\")", "}", ""])
    (DOC / "components" / "index.capy").write_text("\n".join(lines), encoding="utf-8")


def write_all_component(pages: list[dict]) -> None:
    lines = [
        "function COMPONENT(request : dval) {",
        "    print(\"<main class=\\\"content\\\"><h2>All documentation</h2><div class=\\\"func-grid\\\">\")",
    ]
    for page in listing_pages(pages):
        lines.append(
            "    print(\"<div class=\\\"func-item\\\"><a href=\\\"\", html_escape("
            + capy_string(page["route"])
            + "), \"\\\">\", html_escape(" + capy_string(page["title"])
            + "), \"</a></div>\")"
        )
    lines.extend(["    print(\"</div></main>\")", "}", ""])
    (DOC / "components" / "all.capy").write_text("\n".join(lines), encoding="utf-8")


def read_signatures() -> dict[str, list[str]]:
    path = DOC / "lib" / "capy_signatures.generated.h"
    if not path.exists():
        return {}
    data = path.read_text(encoding="utf-8")
    out: dict[str, list[str]] = {}
    for match in re.finditer(r'\{"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)"\}', data):
        page = bytes(match.group(1), "utf-8").decode("unicode_escape")
        sig = bytes(match.group(2), "utf-8").decode("unicode_escape")
        out.setdefault(page, []).append(sig)
    return out


def main() -> None:
    if OUT.exists():
        shutil.rmtree(OUT)
    for pattern in LEGACY_CONTENT:
        for path in DOC.glob(pattern):
            path.unlink()
    for path in LEGACY_COMPONENTS:
        path.unlink(missing_ok=True)
    for output in (API_OUT, TYPE_OUT, HANDLER_OUT, GUIDE_OUT):
        output.mkdir(parents=True)
    write_presentation_component(PRESENTATION_OUT)
    signatures = read_signatures()

    pages: list[dict] = []
    used = {"api": {}, "type": {}, "handler": {}, "guide": {}}
    for txt in sorted((DOC / "pages").rglob("*.txt")):
        relative = txt.relative_to(DOC / "pages").with_suffix("")
        source_page = relative.as_posix()
        kind = relative.parts[0] if len(relative.parts) > 1 else "api"
        if kind not in {"api", "type", "handler"}:
            raise ValueError(f"unknown documentation page directory: {relative}")
        page = parse_doc(source_page, txt.read_text(encoding="utf-8"))
        page["kind"] = kind
        page["label"] = relative.name
        page["title"] = page["title"] or relative.name
        if len(relative.parts) > 2:
            raise ValueError(f"nested documentation page is not supported: {relative}")
        page["slug"] = unique_slug(slugify(relative.name), used[kind], source_page)
        page["route"] = f"/doc/{kind}/{page['slug']}/"
        page["capy_sig_lines"] = signatures.get(source_page, [])
        pages.append(page)
    for txt in sorted((DOC / "capy").glob("*.txt")):
        source_page = "capy-" + txt.stem
        page = parse_doc(source_page, txt.read_text(encoding="utf-8"))
        page["label"] = page["title"] or txt.stem
        page["title"] = page["label"]
        page["slug"] = unique_slug(slugify(re.sub(r"^[0-9]+-", "", txt.stem)), used["guide"], source_page)
        page["route"] = "/doc/guide/" + page["slug"] + "/"
        pages.append(page)
    targets = {page["source_page"]: page for page in pages}
    for page in pages:
        resolved = []
        for reference in page["see"]:
            target = reference[1:].strip() if reference.startswith(">") else reference
            if target not in targets:
                raise ValueError(f"{page['source_page']}: unknown :see target: {reference}")
            resolved.append({"label": targets[target]["label"], "href": targets[target]["route"]})
        page["see"] = resolved
        write_content_module(OUT / page["kind"] / (page["slug"] + ".capy"), page)
    write_index_component(pages)
    write_all_component(pages)


if __name__ == "__main__":
    main()
