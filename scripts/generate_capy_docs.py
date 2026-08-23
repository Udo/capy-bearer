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


def unique_slug(base: str, used: set[str]) -> str:
    slug = base
    index = 2
    while slug in used:
        slug = f"{base}-{index}"
        index += 1
    used.add(slug)
    return slug


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
    lines = ["#exports data", "", "function data(input : dval) dval {", "    var page := dval({source_page: \"\"})"]
    for key in ["source_page", "kind", "slug", "route", "title", "content", "returns", "errors", "capy_status", "example_error", "label"]:
        lines.extend(assign_string(key, page.get(key, "")))
    for key in ["capy_sig_lines", "param_lines", "notes", "warnings", "see", "examples", "guide_examples"]:
        lines.append(f"    page[{capy_string(key)}] = {dval_array(page.get(key, []))}")
    lines.extend(["    return page", "}", ""])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def write_content_dispatch(path: Path, branches: list[tuple[str, str]]) -> None:
    lines = ["#exports data", "", "function data(input : dval) dval {", "    var slug := string(input.slug, \"\")"]
    for slug, body in branches:
        lines.append(f"    if slug == {capy_string(slug)} {{")
        lines.extend("    " + line for line in body.split("\n") if line.strip())
        lines.append("        return page")
        lines.append("    }")
    lines.append("    return dval({source_page: \"\", kind: \"\", slug: \"\", title: \"Page not found\", label: \"Page not found\", content: \"\"})")
    lines.append("}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def combine_content(kind: str) -> None:
    branches = []
    for path in sorted((OUT / kind).glob("*.capy")):
        text = path.read_text(encoding="utf-8")
        body = text.split("function data(input : dval) dval {", 1)[1].rsplit("    return page", 1)[0]
        branches.append((path.stem, body.rstrip()))
    if kind == "api":
        shards: dict[str, list[tuple[str, str]]] = {}
        for slug, body in branches:
            shard = re.sub(r"[^A-Za-z0-9]+", "", slug[:8]) or "other"
            shards.setdefault(shard, []).append((slug, body))
        for old in DOC.glob("content-api-*.capy"):
            old.unlink()
        for shard, shard_branches in shards.items():
            write_content_dispatch(DOC / f"content-api-{shard}.capy", shard_branches)
    else:
        write_content_dispatch(DOC / f"content-{kind}.capy", branches)


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
    for output in (API_OUT, TYPE_OUT, HANDLER_OUT, GUIDE_OUT):
        output.mkdir(parents=True)
    signatures = read_signatures()

    pages: list[dict] = []
    used = {"api": set(), "type": set(), "handler": set(), "guide": set()}
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
        page["slug"] = unique_slug(slugify(relative.name), used[kind])
        page["route"] = f"/doc/{kind}/{page['slug']}/"
        page["capy_sig_lines"] = signatures.get(source_page, [])
        pages.append(page)
    for txt in sorted((DOC / "capy").glob("*.txt")):
        source_page = "capy-" + txt.stem
        page = parse_doc(source_page, txt.read_text(encoding="utf-8"))
        page["label"] = page["title"] or txt.stem
        page["title"] = page["label"]
        page["slug"] = unique_slug(slugify(re.sub(r"^[0-9]+-", "", txt.stem)), used["guide"])
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
    for kind in ("api", "type", "handler", "guide"):
        combine_content(kind)
    shutil.rmtree(OUT)


if __name__ == "__main__":
    main()
