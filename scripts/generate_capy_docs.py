#!/usr/bin/env python3
from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "site" / "doc"
SOURCE = DOC / "source"
OUT = DOC / "content"
LEGACY_CONTENT = ("content-api-*.capy", "content-guide.capy", "content-handler.capy", "content-type.capy")
LEGACY_COMPONENTS = (DOC / "components" / "page.capy",)
MARKDOWN_RENDERER = ROOT / "bin" / "bearer_fastcgi.linux.bin"


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


def html_escape(value: str) -> str:
    return value.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def detail_html(page: dict) -> str:
    parts = [
        '<div class="doc-detail-layout"><main class="content doc-page doc-detail"><h2>',
        html_escape(page["title"] or page["label"] or "Documentation"),
        "</h2>",
    ]
    if page["capy_sig_lines"]:
        parts.extend(['<div class="doc-section signatures"><h3>Capy</h3><pre>', html_escape("\n".join(page["capy_sig_lines"])), "</pre></div>"])
    if page["content_html"]:
        parts.append('<div class="doc-section content">')
        if page["kind"] == "api":
            parts.append("<h3>Description</h3>")
        parts.extend([page["content_html"], "</div>"])
    if page["param_html"]:
        parts.append('<div class="doc-section params"><h3>Parameters</h3>')
        for body in page["param_html"]:
            parts.extend(["<div>", body, "</div>"])
        parts.append("</div>")
    for title, body in (("Return Values", page["returns_html"]), ("Errors", page["errors_html"])):
        if body.strip():
            parts.extend(['<div class="doc-section"><h3>', title, "</h3>", body, "</div>"])
    for class_name, title, blocks in (("note", "Note", page["notes_html"]), ("warning", "Warning", page["warnings_html"])):
        for body in blocks:
            parts.extend(['<div class="doc-section doc-callout ', class_name, '"><h3>', title, "</h3>", body, "</div>"])
    if page["example_error"]:
        parts.extend(['<div class="doc-section example error"><h3>Example format error</h3><pre>', html_escape(page["example_error"]), "</pre></div>"])
    else:
        for example in page["examples"]:
            parts.extend(['<div class="doc-section example"><h3>Example</h3><div class="example-language capy"><h4>Capy</h4><pre class="example-source">', html_escape(example["body"]), "</pre></div></div>"])
        for example in page["guide_examples"]:
            parts.extend(['<div class="doc-section example"><h3>Example</h3><div class="example-language capy"><h4>Capy</h4><pre class="example-source">', html_escape(example["body"]), "</pre></div>"])
            if "output" in example:
                parts.extend(['<p><strong>Output</strong></p><div class="example-output"><div class="example-output-label">Output</div><pre>', html_escape(example["output"]), "</pre></div>"])
            parts.append("</div>")
    if page["kind"] == "guide":
        parts.append('<nav class="guide-navigation" aria-label="Capy guide navigation"><div class="guide-previous">Previous: <a href="/doc/">Guide index</a></div><div class="guide-index"><a href="/doc/">Guide index</a></div><div class="guide-next">Next: <a href="/doc/">Guide index</a></div></nav>')
    parts.append("</main>")
    if page["see"]:
        parts.append('<aside class="doc-section see-also"><h3>See also</h3>')
        for item in page["see"]:
            parts.extend(['<a class="related-link" href="', html_escape(item["href"]), '">', html_escape(item["label"]), "</a>"])
        parts.append("</aside>")
    parts.append("</div>")
    return "".join(parts)


def write_content_module(path: Path, page: dict) -> None:
    html = detail_html(page)
    lines = ["function COMPONENT(request : dval) {"]
    for offset in range(0, len(html), 1800):
        lines.append(f"    print({capy_string(html[offset:offset + 1800])})")
    lines.extend(["}", ""])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def render_markdown(values: list[str]) -> list[str]:
    payload = b"".join(len(value.encode()).to_bytes(8, "big") + value.encode() for value in values)
    environment = os.environ.copy()
    wasmtime_lib = Path(environment.get("WASMTIME_HOME", "/opt/wasmtime")) / "lib"
    environment["LD_LIBRARY_PATH"] = str(wasmtime_lib) + (":" + environment["LD_LIBRARY_PATH"] if environment.get("LD_LIBRARY_PATH") else "")
    result = subprocess.run([MARKDOWN_RENDERER, "--markdown-to-html"], input=payload, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace").strip() or "Markdown renderer failed")
    output = []
    offset = 0
    while offset < len(result.stdout):
        if len(result.stdout) - offset < 8:
            raise RuntimeError("Markdown renderer returned a truncated frame")
        size = int.from_bytes(result.stdout[offset:offset + 8], "big")
        offset += 8
        if len(result.stdout) - offset < size:
            raise RuntimeError("Markdown renderer returned a truncated frame")
        output.append(result.stdout[offset:offset + size].decode())
        offset += size
    if len(output) != len(values):
        raise RuntimeError("Markdown renderer returned the wrong number of frames")
    return output


def render_page_html(pages: list[dict]) -> None:
    values = []
    for page in pages:
        values.extend([page["content"], page["returns"], page["errors"], *page["param_lines"], *page["notes"], *page["warnings"]])
    rendered = iter(render_markdown(values))
    for page in pages:
        page["content_html"] = next(rendered)
        page["returns_html"] = next(rendered)
        page["errors_html"] = next(rendered)
        page["param_html"] = [next(rendered) for _ in page["param_lines"]]
        page["notes_html"] = [next(rendered) for _ in page["notes"]]
        page["warnings_html"] = [next(rendered) for _ in page["warnings"]]


def listing_pages(pages: list[dict]) -> list[dict]:
    return sorted(
        pages,
        key=lambda page: (
            0 if page["kind"] == "guide" else 1,
            page["source_page"] if page["kind"] == "guide" else page["kind"],
            page["slug"],
        ),
    )


def pages_of_kind(pages: list[dict], kind: str) -> list[dict]:
    return [page for page in listing_pages(pages) if page["kind"] == kind]


SECTION_DEFINITIONS = (
    ("api", "API Functions"),
    ("type", "Types"),
    ("handler", "Handlers"),
    ("how-to", "How-to"),
    ("guide", "Learn Capy"),
)


def listing_label(page: dict) -> str:
    if page["kind"] in {"api", "type", "handler"}:
        return page["label"]
    return page["title"]


def write_index_component(pages: list[dict]) -> None:
    lines = [
        "function link(route : string, label : string, kind : string) void {",
        "    print(\"<a href=\\\"\", html_escape(route), \"\\\">\", html_escape(label))",
        "    if kind == \"api\" { print(\"<span class=\\\"dim\\\">()</span>\") }",
        "    print(\"</a>\")",
        "}",
        "",
        "function COMPONENT(request : dval) {",
        "    print(\"<div class=\\\"api-reference-heading\\\"><h2>API Reference</h2><p>Bearer APIs available to Capy units.</p></div>\")",
        "    print(\"<form class=\\\"search-bar\\\" action=\\\"/doc/search/\\\" method=\\\"get\\\"><input id=\\\"doc-search\\\" name=\\\"q\\\" type=\\\"search\\\" aria-label=\\\"Search documentation\\\" placeholder=\\\"Search documentation…\\\" autocomplete=\\\"off\\\" spellcheck=\\\"false\\\"></input><button type=\\\"submit\\\">Search</button></form>\")",
    ]
    for kind, title in SECTION_DEFINITIONS:
        section_pages = pages_of_kind(pages, kind)
        if not section_pages:
            continue
        class_name = "capy-guide-index" if kind == "guide" else "doc-list-section"
        grid_class = "capy-guide-grid" if kind in {"guide", "how-to"} else "func-grid"
        item_class = "capy-guide-card" if kind in {"guide", "how-to"} else "func-item"
        lines.append("    print(\"<section class=\\\"" + class_name + "\\\"><h2>" + title + "</h2><div class=\\\"" + grid_class + "\\\">\")")
        for page in section_pages:
            lines.append(
                "    print(\"<div class=\\\"" + item_class + "\\\">\"); link("
                + capy_string(page["route"])
                + ", " + capy_string(listing_label(page))
                + ", " + capy_string(page["kind"])
                + "); print(\"</div>\")"
            )
        lines.append("    print(\"</div></section>\")")
    lines.extend(["}", ""])
    (DOC / "components" / "index.capy").write_text("\n".join(lines), encoding="utf-8")


def write_all_component(pages: list[dict]) -> None:
    lines = ["function COMPONENT(request : dval) {", "    print(\"<main class=\\\"content\\\"><h2>All documentation</h2>\")"]
    for kind, title in SECTION_DEFINITIONS:
        section_pages = pages_of_kind(pages, kind)
        if not section_pages:
            continue
        lines.append("    print(\"<section class=\\\"doc-list-section\\\"><h3>" + title + "</h3><div class=\\\"func-grid\\\">\")")
        for page in section_pages:
            lines.append(
                "    print(\"<div class=\\\"func-item\\\"><a href=\\\"\", html_escape("
                + capy_string(page["route"])
                + "), \"\\\">\", html_escape(" + capy_string(listing_label(page))
                + "), \"</a></div>\")"
            )
        lines.append("    print(\"</div></section>\")")
    lines.extend(["    print(\"</main>\")", "}", ""])
    (DOC / "components" / "all.capy").write_text("\n".join(lines), encoding="utf-8")


def search_text(page: dict) -> str:
    fields = [page["title"], page["content"], *page["param_lines"], page["returns"], page["errors"], *page["notes"], *page["warnings"]]
    return "\n".join(field for field in fields if field)


def write_search_component(pages: list[dict]) -> None:
    lines = [
        "function token_hit(text : string, tokens : dval) bool {",
        "    var lower_text := lower(text)",
        "    for token := tokens { if contains(lower_text, string(token)) { return true } }",
        "    return false",
        "}",
        "",
        "function search_tokens(query : string) dval {",
        "    var tokens := dval([])",
        "    for raw := split_space(lower(query)) {",
        "        var token := trim(string(raw))",
        "        if length(token) >= 2 {",
        "            tokens = push(tokens, token)",
        "            if token == \"strlen\" || token == \"count\" { tokens = push(tokens, \"length\") }",
        "            if token == \"json\" { tokens = push(tokens, \"json_decode\") }",
        "        }",
        "    }",
        "    return tokens",
        "}",
        "",
        "function consider(tokens : dval, route : string, label : string, kind : string, body : string) bool {",
        "    if !token_hit(label + \"\\n\" + body, tokens) { return false }",
        "    print(\"<div class=\\\"search-hit\\\"><span class=\\\"search-kind\\\">\", html_escape(kind), \"</span><a href=\\\"\", html_escape(route), \"\\\">\", html_escape(label), \"</a></div>\")",
        "    return true",
        "}",
        "",
        "function COMPONENT(request : dval) {",
        "    var query := string(request.query.q)",
        "    var tokens := search_tokens(query)",
        "    print(\"<main class=\\\"content\\\"><h2>Search</h2>\")",
        "    print(\"<form class=\\\"search-bar\\\" action=\\\"/doc/search/\\\" method=\\\"get\\\"><input name=\\\"q\\\" type=\\\"search\\\" value=\\\"\", html_escape(query), \"\\\" aria-label=\\\"Search documentation\\\"></input><button type=\\\"submit\\\">Search</button></form>\")",
        "    if length(query) < 2 || length(tokens) == 0 { print(\"<p class=\\\"no-results\\\">Enter at least two characters.</p></main>\"); return }",
        "    var found := false",
    ]
    for kind, title in SECTION_DEFINITIONS:
        section_pages = pages_of_kind(pages, kind)
        if not section_pages:
            continue
        lines.append("    print(\"<section class=\\\"search-section search-" + kind + "\\\"><h3>" + title + "</h3>\")")
        for page in section_pages:
            lines.append(
                "    if consider(tokens, " + capy_string(page["route"]) + ", "
                + capy_string(listing_label(page)) + ", " + capy_string(title) + ", " + capy_string(search_text(page))
                + ") { found = true }"
            )
        lines.append("    print(\"</section>\")")
    lines.extend([
        "    if !found { print(\"<p class=\\\"no-results\\\">No results for \\\"<strong>\", html_escape(query), \"</strong>\\\"</p>\") }",
        "    print(\"</main>\")",
        "}",
        "",
    ])
    (DOC / "components" / "search.capy").write_text("\n".join(lines), encoding="utf-8")


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
    if not MARKDOWN_RENDERER.is_file():
        raise RuntimeError(f"build the Markdown renderer first: {MARKDOWN_RENDERER}")
    signatures = read_signatures()

    pages: list[dict] = []
    used: dict[str, dict[str, str]] = {}
    for txt in sorted(SOURCE.glob("*/*.txt")):
        relative = txt.relative_to(SOURCE).with_suffix("")
        kind, name = relative.parts
        used.setdefault(kind, {})
        source_page = ("capy-" + name) if kind == "guide" else (name if kind == "api" else relative.as_posix())
        page = parse_doc(source_page, txt.read_text(encoding="utf-8"))
        page["kind"] = kind
        if kind == "guide":
            page["label"] = page["title"] or name
            page["title"] = page["label"]
        else:
            page["label"] = name
            page["title"] = page["title"] or name
        page["slug"] = unique_slug(slugify(re.sub(r"^[0-9]+-", "", name) if kind == "guide" else name), used[kind], source_page)
        page["route"] = f"/doc/{kind}/{page['slug']}/"
        page["capy_sig_lines"] = signatures.get(source_page, [])
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
    render_page_html(pages)
    if OUT.exists():
        shutil.rmtree(OUT)
    for pattern in LEGACY_CONTENT:
        for path in DOC.glob(pattern):
            path.unlink()
    for path in LEGACY_COMPONENTS:
        path.unlink(missing_ok=True)
    (DOC / "components" / "doc_page.capy").unlink(missing_ok=True)
    for page in pages:
        write_content_module(OUT / page["kind"] / (page["slug"] + ".capy"), page)
    write_index_component(pages)
    write_all_component(pages)
    write_search_component(pages)


if __name__ == "__main__":
    main()
