#!/usr/bin/env python3
import argparse
import json
import re
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "docs/capy-language.md"
FRONTEND = ROOT / "src/capy/frontend.cpp"
STDLIB = ROOT / "src/capy/stdlib.capy"
GRAMMAR = ROOT / "editors/vscode/syntaxes/capy.tmLanguage.json"
IDENTIFIER = r"[A-Za-z_][A-Za-z0-9_]*"
LEFT = r"(?<![A-Za-z0-9_])"
RIGHT = r"(?![A-Za-z0-9_])"


def section(source: str, heading: str) -> str:
    match = re.search(rf"^## {re.escape(heading)}\n(.*?)(?=^## |\Z)", source, re.MULTILINE | re.DOTALL)
    if not match:
        raise RuntimeError(f"missing specification section: {heading}")
    return match.group(1)


def alternation(words: set[str] | list[str]) -> str:
    if not words:
        raise RuntimeError("cannot generate an empty grammar alternation")
    return "(?:" + "|".join(re.escape(word) for word in sorted(words, key=lambda word: (-len(word), word))) + ")"


def word_pattern(words: set[str] | list[str]) -> str:
    return LEFT + alternation(words) + RIGHT


def authoritative_words() -> dict[str, list[str]]:
    spec = SPEC.read_text()
    frontend = FRONTEND.read_text()
    types_section = section(spec, "3. Type expressions").split("The following forms", 1)[0]
    types = set(re.findall(r"`([A-Za-z_][A-Za-z0-9_]*)`", types_section))

    handlers_section = section(spec, "12. Handlers and unit composition")
    handler_block = re.search(r"```text\n(.*?)```", handlers_section, re.DOTALL)
    if not handler_block:
        raise RuntimeError("missing handler list in the specification")
    handlers = set(re.findall(r"\b[A-Z][A-Z_]*\b", handler_block.group(1))) - {"NAME"}
    named_handlers = set(re.findall(r"\b([A-Z][A-Z_]*)\s+and\s+\1:NAME\b", handler_block.group(1)))

    directives = set(re.findall(r"`#([a-z_]+)`", section(spec, "1. Source files and tokens")))
    prefix = frontend.split("Expr* Parser::prefix()", 1)[1].split("Expr* Parser::parenthesized", 1)[0]
    keywords = set(re.findall(r'current\.text == "([A-Za-z_][A-Za-z0-9_]*)"', prefix))
    removed = set(re.findall(r'if \(current\.text == "([A-Za-z_][A-Za-z0-9_]*)"\)\n\s+fail\(', prefix))
    keywords -= removed | directives
    if 'match("else")' not in frontend or 'match("as")' not in frontend:
        raise RuntimeError("could not derive the else and as parser words")
    keywords.add("else")
    operator_words = {"as"}
    keywords -= operator_words

    generic_types = set(re.findall(r"`(any)`", section(spec, "8. Functions, overloads, and constructors")))
    literals = set(re.findall(r"`(none)`", section(spec, "1. Source files and tokens"))) | {"true", "false"}
    reserved = set(re.findall(r"`(emit)`", section(spec, "1. Source files and tokens")))

    expected_types = {"bool", "s8", "s16", "s32", "s64", "u8", "u16", "u32", "u64", "f32", "f64", "string", "dval", "module", "void"}
    if types != expected_types:
        raise RuntimeError(f"unexpected built-in type set: {sorted(types)}")
    if directives != {"exports", "import", "compile", "callsite"}:
        raise RuntimeError(f"unexpected directive set: {sorted(directives)}")
    if not named_handlers <= handlers:
        raise RuntimeError("named handlers are not in the handler set")

    stdlib = STDLIB.read_text()
    for syntax in ("host function", "trace host function", "dval", "module", "->", "::"):
        if syntax not in stdlib:
            raise RuntimeError(f"the standard library does not exercise {syntax!r}")

    return {
        "keywords": sorted(keywords),
        "operatorWords": sorted(operator_words),
        "genericTypes": sorted(generic_types),
        "literals": sorted(literals),
        "reserved": sorted(reserved),
        "types": sorted(types),
        "directives": sorted(directives),
        "handlers": sorted(handlers),
        "namedHandlers": sorted(named_handlers),
    }


def grammar(words: dict[str, list[str]]) -> dict:
    handler_names = alternation(words["handlers"])
    named_handler_names = alternation(words["namedHandlers"])
    return {
        "$schema": "https://raw.githubusercontent.com/martinring/tmlanguage/master/tmlanguage.json",
        "name": "Capy",
        "scopeName": "source.capy",
        "capyGeneratedWords": words,
        "patterns": [
            {"include": "#directives"},
            {"include": "#comments"},
            {"include": "#markup"},
            {"include": "#strings"},
            {"include": "#declarations"},
            {"include": "#handlers"},
            {"include": "#keywords"},
            {"include": "#types"},
            {"include": "#constants"},
            {"include": "#numbers"},
            {"include": "#function-calls"},
            {"include": "#members"},
            {"include": "#operators"},
            {"include": "#punctuation"},
        ],
        "repository": {
            "directives": {"patterns": [{
                "name": "meta.preprocessor.capy",
                "match": "(#)(" + "|".join(words["directives"]) + ")" + RIGHT,
                "captures": {
                    "1": {"name": "punctuation.definition.directive.capy"},
                    "2": {"name": "keyword.control.directive.capy"},
                },
            }]},
            "comments": {"patterns": [{
                "name": "comment.line.double-slash.capy",
                "begin": "//",
                "beginCaptures": {"0": {"name": "punctuation.definition.comment.capy"}},
                "end": "$",
            }]},
            "markup": {
                "name": "meta.embedded.block.html.capy",
                "contentName": "text.html.basic",
                "begin": r"(?<!\\)<>",
                "beginCaptures": {"0": {"name": "punctuation.definition.markup.begin.capy"}},
                "end": r"(?<!\\)</>",
                "endCaptures": {"0": {"name": "punctuation.definition.markup.end.capy"}},
                "patterns": [
                    {"include": "#markup-script"},
                    {"include": "#markup-style"},
                    {"include": "#markup-tag"},
                    {"include": "#markup-field-text"},
                    {"include": "#markup"},
                    {"include": "text.html.basic"},
                ],
            },
            "markup-field-text": {
                "name": "meta.embedded.expression.capy",
                "begin": r"<\?(?:=|:)",
                "beginCaptures": {"0": {"name": "punctuation.section.embedded.begin.capy"}},
                "end": r"\?>",
                "endCaptures": {"0": {"name": "punctuation.section.embedded.end.capy"}},
                "patterns": [{"include": "$self"}],
            },
            "markup-field-escaped": {
                "name": "meta.embedded.expression.capy",
                "begin": r"<\?=",
                "beginCaptures": {"0": {"name": "punctuation.section.embedded.begin.capy"}},
                "end": r"\?>",
                "endCaptures": {"0": {"name": "punctuation.section.embedded.end.capy"}},
                "patterns": [{"include": "$self"}],
            },
            "markup-tag": {
                "name": "meta.tag.html.capy",
                "begin": r"</?[A-Za-z][A-Za-z0-9:_-]*",
                "end": ">",
                "patterns": [
                    {"name": "string.quoted.double.html", "begin": '"', "end": '"', "patterns": [{"include": "#markup-field-escaped"}]},
                    {"name": "string.quoted.single.html", "begin": "'", "end": "'", "patterns": [{"include": "#markup-field-escaped"}]},
                ],
            },
            "markup-script": {
                "name": "meta.embedded.block.javascript.capy",
                "contentName": "source.js",
                "begin": r"<[Ss][Cc][Rr][Ii][Pp][Tt]\b[^>]*>",
                "end": r"</[Ss][Cc][Rr][Ii][Pp][Tt]\s*>",
                "patterns": [{"include": "#markup-field-escaped"}, {"name": "source.js", "match": r"[^<]+|<(?!\?=)"}],
            },
            "markup-style": {
                "name": "meta.embedded.block.css.capy",
                "contentName": "source.css",
                "begin": r"<[Ss][Tt][Yy][Ll][Ee]\b[^>]*>",
                "end": r"</[Ss][Tt][Yy][Ll][Ee]\s*>",
                "patterns": [{"include": "#markup-field-escaped"}, {"name": "source.css", "match": r"[^<]+|<(?!\?=)"}],
            },
            "strings": {"patterns": [{
                "name": "string.quoted.double.capy",
                "begin": '"',
                "beginCaptures": {"0": {"name": "punctuation.definition.string.begin.capy"}},
                "end": '"',
                "endCaptures": {"0": {"name": "punctuation.definition.string.end.capy"}},
                "patterns": [{"name": "constant.character.escape.capy", "match": r'\\[\\"nrt]'}],
            }]},
            "declarations": {"patterns": [
                {
                    "name": "meta.function.definition.handler.capy",
                    "match": r"(?<![A-Za-z0-9_])(function)(\s+)(" + named_handler_names + r")(:)(" + IDENTIFIER + ")",
                    "captures": {
                        "1": {"name": "keyword.declaration.function.capy"},
                        "3": {"name": "entity.name.function.handler.capy"},
                        "4": {"name": "punctuation.separator.handler.capy"},
                        "5": {"name": "entity.name.function.capy"},
                    },
                },
                {
                    "name": "meta.function.definition.handler.capy",
                    "match": r"(?<![A-Za-z0-9_])(function)(\s+)(" + handler_names + ")" + RIGHT,
                    "captures": {"1": {"name": "keyword.declaration.function.capy"}, "3": {"name": "entity.name.function.handler.capy"}},
                },
                {
                    "name": "meta.function.definition.capy",
                    "match": r"(?<![A-Za-z0-9_])((?:trace\s+)?host\s+function|function)(\s+)(" + IDENTIFIER + r")(?=\s*(?:\(|:))",
                    "captures": {"1": {"name": "keyword.declaration.function.capy"}, "3": {"name": "entity.name.function.capy"}},
                },
                {
                    "name": "meta.type.definition.capy",
                    "match": r"(?<![A-Za-z0-9_])(struct|type)(\s+)(" + IDENTIFIER + ")",
                    "captures": {"1": {"name": "keyword.declaration.type.capy"}, "3": {"name": "entity.name.type.capy"}},
                },
            ]},
            "handlers": {"patterns": [{"name": "support.function.handler.capy", "match": word_pattern(words["handlers"])}]},
            "keywords": {"patterns": [
                {"name": "keyword.control.capy", "match": word_pattern(words["keywords"])},
                {"name": "keyword.operator.word.capy", "match": word_pattern(words["operatorWords"])},
                {"name": "keyword.other.reserved.capy", "match": word_pattern(words["reserved"])},
            ]},
            "types": {"patterns": [
                {"name": "support.type.builtin.capy", "match": word_pattern(words["types"])},
                {"name": "storage.type.generic.capy", "match": word_pattern(words["genericTypes"])},
                {"name": "entity.name.type.capy", "match": r"(?<![A-Za-z0-9_])[A-Z][A-Za-z0-9_]*"},
            ]},
            "constants": {"patterns": [{"name": "constant.language.capy", "match": word_pattern(words["literals"])}]},
            "numbers": {"patterns": [
                {"name": "constant.numeric.float.capy", "match": r"(?<![A-Za-z0-9_])(?:\d+\.\d*|\d*\.\d+|\d+)(?:[eE][+-]?\d+)" + RIGHT},
                {"name": "constant.numeric.integer.capy", "match": r"(?<![A-Za-z0-9_])\d+" + RIGHT},
            ]},
            "function-calls": {"patterns": [{"name": "entity.name.function.call.capy", "match": IDENTIFIER + r"(?=\s*\()"}]},
            "members": {"patterns": [
                {"name": "variable.other.member.capy", "match": r"(?<=\.)" + IDENTIFIER},
                {"name": "entity.name.type.capy", "match": r"(?<=::)" + IDENTIFIER},
            ]},
            "operators": {"patterns": [
                {"name": "keyword.operator.yield.capy", "match": "->"},
                {"name": "keyword.operator.scope.capy", "match": "::"},
                {"name": "keyword.operator.spread.capy", "match": r"\.\.\."},
                {"name": "keyword.operator.range.capy", "match": r"\.\."},
                {"name": "keyword.operator.assignment.capy", "match": ":=|="},
                {"name": "keyword.operator.comparison.capy", "match": "==|!=|<=|>=|<|>"},
                {"name": "keyword.operator.logical.capy", "match": r"&&|\|\||!|\?"},
                {"name": "keyword.operator.arithmetic.capy", "match": r"[+\-*/%]"},
                {"name": "keyword.operator.access.capy", "match": r"\."},
            ]},
            "punctuation": {"patterns": [
                {"name": "punctuation.separator.capy", "match": "[,;:]"},
                {"name": "punctuation.section.group.capy", "match": r"[(){}\[\]]"},
            ]},
        },
    }


def generate() -> str:
    return json.dumps(grammar(authoritative_words()), indent=2) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=GRAMMAR)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    content = generate()
    if args.check:
        with tempfile.TemporaryDirectory(prefix="capy-grammar-") as directory:
            temporary = Path(directory) / "capy.tmLanguage.json"
            temporary.write_text(content)
            if not args.output.is_file() or args.output.read_bytes() != temporary.read_bytes():
                raise SystemExit("generated Capy TextMate grammar is stale")
    elif not args.output.is_file() or args.output.read_text() != content:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(content)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
