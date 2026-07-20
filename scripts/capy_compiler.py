#!/usr/bin/env python3
"""Capy lexer, expression parser, and compiler entry point.

The compiler intentionally owns its grammar instead of translating Capy to C++.
The Wasm backend is added behind the typed AST built here.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import argparse
import json
import struct
import sys
from typing import NoReturn


@dataclass(frozen=True)
class Location:
    file: str
    line: int
    column: int
    offset: int


class CapyError(Exception):
    def __init__(self, location: Location, message: str):
        self.location = location
        self.message = message
        super().__init__(f"{location.file}:{location.line}:{location.column}: {message}")


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    location: Location


class Lexer:
    THREE = {"..."}
    TWO = {"::", ":=", "..", "==", "!=", "<=", ">=", "&&", "||"}
    ONE = set("(){}[],:=+-*/%<>.!;")

    def __init__(self, source: str, file: str = "<input>"):
        self.source = source
        self.file = file
        self.offset = 0
        self.line = 1
        self.column = 1

    def location(self) -> Location:
        return Location(self.file, self.line, self.column, self.offset)

    def advance(self) -> str:
        ch = self.source[self.offset]
        self.offset += 1
        if ch == "\n":
            self.line += 1
            self.column = 1
        else:
            self.column += 1
        return ch

    def error(self, location: Location, message: str) -> NoReturn:
        raise CapyError(location, message)

    def tokens(self) -> list[Token]:
        result: list[Token] = []
        while self.offset < len(self.source):
            ch = self.source[self.offset]
            if ch in " \t\r":
                self.advance()
                continue
            if ch == "\n":
                loc = self.location()
                self.advance()
                result.append(Token("newline", "\n", loc))
                continue
            if self.source.startswith("//", self.offset):
                while self.offset < len(self.source) and self.source[self.offset] != "\n":
                    self.advance()
                continue
            loc = self.location()
            if ch.isalpha() or ch == "_":
                start = self.offset
                while self.offset < len(self.source):
                    current = self.source[self.offset]
                    if not (current.isalnum() or current == "_"):
                        break
                    self.advance()
                result.append(Token("identifier", self.source[start:self.offset], loc))
                continue
            if ch.isdigit():
                start = self.offset
                while self.offset < len(self.source) and self.source[self.offset].isdigit():
                    self.advance()
                result.append(Token("integer", self.source[start:self.offset], loc))
                continue
            if ch == '"':
                result.append(self.string_token())
                continue
            if ch == "#":
                self.advance()
                start = self.offset
                while self.offset < len(self.source) and (self.source[self.offset].isalnum() or self.source[self.offset] == "_"):
                    self.advance()
                if start == self.offset:
                    self.error(loc, "expected directive name after '#'")
                result.append(Token("directive", self.source[start:self.offset], loc))
                continue
            three = self.source[self.offset:self.offset + 3]
            if three in self.THREE:
                for _ in three:
                    self.advance()
                result.append(Token("symbol", three, loc))
                continue
            two = self.source[self.offset:self.offset + 2]
            if two in self.TWO:
                for _ in two:
                    self.advance()
                result.append(Token("symbol", two, loc))
                continue
            if ch in self.ONE:
                self.advance()
                result.append(Token("symbol", ch, loc))
                continue
            self.error(loc, f"unexpected character {ch!r}")
        result.append(Token("eof", "", self.location()))
        return result

    def string_token(self) -> Token:
        loc = self.location()
        self.advance()
        value: list[str] = []
        while self.offset < len(self.source):
            ch = self.advance()
            if ch == '"':
                return Token("string", "".join(value), loc)
            if ch == "\\":
                if self.offset >= len(self.source):
                    break
                escaped = self.advance()
                escapes = {"n": "\n", "r": "\r", "t": "\t", '"': '"', "\\": "\\"}
                if escaped not in escapes:
                    self.error(loc, f"unsupported string escape \\{escaped}")
                value.append(escapes[escaped])
            else:
                value.append(ch)
        self.error(loc, "unterminated string literal")


@dataclass
class Expr:
    location: Location


@dataclass
class Name(Expr):
    value: str


@dataclass
class Integer(Expr):
    value: int


@dataclass
class String(Expr):
    value: str


@dataclass
class TupleExpr(Expr):
    items: list[Expr]


@dataclass
class Annotation(Expr):
    value: Expr
    type_expr: Expr


@dataclass
class Binary(Expr):
    operator: str
    left: Expr
    right: Expr


@dataclass
class ScopeLookup(Expr):
    value: Expr
    member: str


@dataclass
class Call(Expr):
    function: Expr
    arguments: list[Expr]


@dataclass
class Index(Expr):
    value: Expr
    index: Expr


@dataclass
class Member(Expr):
    value: Expr
    member: str


@dataclass
class Block(Expr):
    items: list[Expr]


@dataclass
class Return(Expr):
    value: Expr | None


@dataclass
class Variable(Expr):
    name: str
    annotation: Expr | None
    value: Expr
    inferred: bool


@dataclass
class Parameter:
    name: str
    type_expr: Expr
    variadic: bool = False
    convert: bool = False


@dataclass
class Function(Expr):
    name: str
    parameters: list[Parameter]
    return_type: Expr | None
    body: Block


@dataclass
class Struct(Expr):
    name: str
    members: list[Expr]


@dataclass
class For(Expr):
    name: str
    iterable: Expr
    body: Block


@dataclass
class Program:
    items: list[Expr]


class Parser:
    INFIX = {
        "=": 5,
        ":=": 5,
        ":": 10,
        "..": 20,
        "||": 25,
        "&&": 30,
        "==": 35,
        "!=": 35,
        "<": 40,
        "<=": 40,
        ">": 40,
        ">=": 40,
        "+": 50,
        "-": 50,
        "*": 60,
        "/": 60,
        "%": 60,
    }

    def __init__(self, tokens: list[Token]):
        self.tokens = tokens
        self.position = 0

    @property
    def token(self) -> Token:
        return self.tokens[self.position]

    def take(self) -> Token:
        token = self.token
        self.position += 1
        return token

    def match(self, text: str) -> bool:
        if self.token.text == text:
            self.take()
            return True
        return False

    def require(self, text: str) -> Token:
        if self.token.text != text:
            raise CapyError(self.token.location, f"expected {text!r}, found {self.token.text or 'end of file'!r}")
        return self.take()

    def require_identifier(self, purpose: str) -> Token:
        if self.token.kind != "identifier":
            raise CapyError(self.token.location, f"expected {purpose}, found {self.token.text or 'end of file'!r}")
        return self.take()

    def skip_separators(self) -> None:
        while self.token.kind == "newline" or self.token.text == ";":
            self.take()

    def parse(self) -> Program:
        items: list[Expr] = []
        self.skip_separators()
        while self.token.kind != "eof":
            items.append(self.expression())
            if self.token.kind not in {"newline", "eof"} and self.token.text not in {";", "}"}:
                raise CapyError(self.token.location, f"expected expression separator, found {self.token.text!r}")
            self.skip_separators()
        return Program(items)

    def expression(self, minimum: int = 0) -> Expr:
        while self.token.kind == "newline":
            self.take()
        left = self.prefix()
        while True:
            token = self.token
            if token.kind in {"newline", "eof"} or token.text in {",", ")", "]", "}", ";", "{"}:
                break
            if token.text == "(":
                if 80 < minimum:
                    break
                left = self.finish_call(left)
                continue
            if token.text == "[":
                if 80 < minimum:
                    break
                loc = self.take().location
                index = self.expression()
                self.require("]")
                left = Index(loc, left, index)
                continue
            if token.text == ".":
                if 80 < minimum:
                    break
                loc = self.take().location
                member = self.require_identifier("member name")
                left = Member(loc, left, member.text)
                continue
            if token.text == "::":
                if 80 < minimum:
                    break
                loc = self.take().location
                member = self.require_identifier("scope member")
                left = ScopeLookup(loc, left, member.text)
                continue
            precedence = self.INFIX.get(token.text)
            if precedence is None or precedence < minimum:
                break
            operator = self.take()
            right = self.expression(precedence + (0 if operator.text in {"=", ":="} else 1))
            if operator.text == ":":
                left = Annotation(operator.location, left, right)
            else:
                left = Binary(operator.location, operator.text, left, right)
        return left

    def prefix(self) -> Expr:
        token = self.take()
        if token.kind == "integer":
            return Integer(token.location, int(token.text))
        if token.kind == "string":
            return String(token.location, token.text)
        if token.kind == "directive":
            if token.text in {"compile", "callsite"}:
                raise CapyError(token.location, f"#{token.text} compile-time metaprogramming is deferred beyond Capy phase 3")
            raise CapyError(token.location, f"unknown compiler directive #{token.text}")
        if token.kind == "identifier":
            if token.text == "function":
                return self.function(token.location)
            if token.text == "struct":
                return self.struct(token.location)
            if token.text == "var":
                return self.variable(token.location)
            if token.text == "return":
                return self.return_expr(token.location)
            if token.text == "for":
                return self.for_expr(token.location)
            return Name(token.location, token.text)
        if token.text == "(":
            return self.parenthesized(token.location)
        if token.text == "{":
            return self.block(token.location)
        if token.text in {"-", "!"}:
            return Binary(token.location, "unary" + token.text, Integer(token.location, 0), self.expression(70))
        raise CapyError(token.location, f"expected expression, found {token.text or 'end of file'!r}")

    def parenthesized(self, location: Location) -> Expr:
        items: list[Expr] = []
        self.skip_separators()
        if self.match(")"):
            return TupleExpr(location, items)
        while True:
            items.append(self.expression())
            self.skip_separators()
            if not self.match(","):
                break
            self.skip_separators()
        self.require(")")
        if len(items) == 1:
            return items[0]
        return TupleExpr(location, items)

    def finish_call(self, function: Expr) -> Expr:
        location = self.require("(").location
        arguments: list[Expr] = []
        self.skip_separators()
        if not self.match(")"):
            while True:
                arguments.append(self.expression())
                self.skip_separators()
                if not self.match(","):
                    break
                self.skip_separators()
            self.require(")")
        return Call(location, function, arguments)

    def block(self, location: Location) -> Block:
        items: list[Expr] = []
        self.skip_separators()
        while not self.match("}"):
            if self.token.kind == "eof":
                raise CapyError(location, "unterminated code block")
            items.append(self.expression())
            if self.token.kind not in {"newline", "eof"} and self.token.text not in {";", "}"}:
                raise CapyError(self.token.location, f"expected expression separator, found {self.token.text!r}")
            self.skip_separators()
        return Block(location, items)

    def function(self, location: Location) -> Function:
        name = self.require_identifier("function name")
        header: list[Expr] = []
        while self.token.text != "{":
            if self.token.kind in {"newline", "eof"}:
                raise CapyError(self.token.location, "expected function code block")
            # A parenthesized header expression occupies one declaration slot.
            # Do not reinterpret the following return expression as a call on it.
            if self.token.text == "(":
                header.append(self.parenthesized(self.take().location))
            else:
                header.append(self.expression())
            if len(header) > 2:
                raise CapyError(header[-1].location, "function declaration has more than parameter and return expressions")
        body_location = self.require("{").location
        body = self.block(body_location)
        parameter_expr = header[0] if header else TupleExpr(location, [])
        return_type = header[1] if len(header) == 2 else None
        parameters = self.parameters(parameter_expr)
        return Function(location, name.text, parameters, return_type, body)

    def parameters(self, expression: Expr) -> list[Parameter]:
        expressions = expression.items if isinstance(expression, TupleExpr) else [expression]
        result: list[Parameter] = []
        for item in expressions:
            if not isinstance(item, Annotation) or not isinstance(item.value, Name):
                raise CapyError(item.location, "function parameter expression must contain name:type annotations")
            result.append(Parameter(item.value.value, item.type_expr))
        return result

    def struct(self, location: Location) -> Struct:
        name = self.require_identifier("struct name")
        self.require("{")
        body = self.block(location)
        return Struct(location, name.text, body.items)

    def variable(self, location: Location) -> Variable:
        name = self.require_identifier("variable name")
        annotation: Expr | None = None
        inferred = False
        if self.match(":="):
            inferred = True
        else:
            self.require(":")
            annotation = self.expression(11)
            self.require("=")
        value = self.expression()
        return Variable(location, name.text, annotation, value, inferred)

    def return_expr(self, location: Location) -> Return:
        if self.token.kind in {"newline", "eof"} or self.token.text in {";", "}"}:
            return Return(location, None)
        values = [self.expression()]
        while self.match(","):
            values.append(self.expression())
        value: Expr = values[0] if len(values) == 1 else TupleExpr(location, values)
        return Return(location, value)

    def for_expr(self, location: Location) -> For:
        name = self.require_identifier("loop variable")
        if not (self.match("=") or self.match("in")):
            raise CapyError(self.token.location, "expected '=' or 'in' after loop variable")
        iterable = self.expression()
        body_location = self.require("{").location
        return For(location, name.text, iterable, self.block(body_location))


@dataclass(frozen=True)
class FunctionKey:
    name: str
    parameter_types: tuple[str, ...]


class DeclarationIndex:
    """Initial overload index; return types deliberately do not identify overloads."""

    def __init__(self) -> None:
        self.functions: dict[FunctionKey, Function] = {}

    def add_program(self, program: Program) -> None:
        for item in program.items:
            if not isinstance(item, Function):
                continue
            types = tuple(type_name(parameter.type_expr) for parameter in item.parameters)
            key = FunctionKey(item.name, types)
            if key in self.functions:
                previous = self.functions[key]
                raise CapyError(item.location, f"duplicate overload {item.name}({', '.join(types)}); return type does not distinguish overloads (first declared at {previous.location.line}:{previous.location.column})")
            self.functions[key] = item


def type_name(expression: Expr) -> str:
    if isinstance(expression, Name):
        return expression.value
    if isinstance(expression, ScopeLookup):
        return f"{type_name(expression.value)}::{expression.member}"
    if isinstance(expression, TupleExpr):
        return "(" + ",".join(type_name(item) for item in expression.items) + ")"
    raise CapyError(expression.location, "expected type expression")


def parse(source: str, file: str = "<input>") -> Program:
    return Parser(Lexer(source, file).tokens()).parse()


def ast_json(value):
    if isinstance(value, Location):
        return {"file": value.file, "line": value.line, "column": value.column}
    if hasattr(value, "__dataclass_fields__"):
        result = {"kind": value.__class__.__name__}
        for name in value.__dataclass_fields__:
            result[name] = ast_json(getattr(value, name))
        return result
    if isinstance(value, list):
        return [ast_json(item) for item in value]
    if isinstance(value, tuple):
        return [ast_json(item) for item in value]
    return value


def uleb(value: int) -> bytes:
    if value < 0:
        raise ValueError("ULEB value cannot be negative")
    result = bytearray()
    while True:
        byte = value & 0x7f
        value >>= 7
        if value:
            result.append(byte | 0x80)
        else:
            result.append(byte)
            return bytes(result)


def sleb32(value: int) -> bytes:
    if value < -(1 << 31) or value >= (1 << 31):
        raise ValueError("signed LEB32 value is out of range")
    result = bytearray()
    while True:
        byte = value & 0x7f
        value >>= 7
        sign = byte & 0x40
        done = (value == 0 and sign == 0) or (value == -1 and sign != 0)
        result.append(byte if done else byte | 0x80)
        if done:
            return bytes(result)


def wasm_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return uleb(len(encoded)) + encoded


def wasm_vector(items: list[bytes]) -> bytes:
    return uleb(len(items)) + b"".join(items)


def wasm_section(section_id: int, payload: bytes) -> bytes:
    return bytes([section_id]) + uleb(len(payload)) + payload


def wasm_custom(name: str, payload: bytes) -> bytes:
    return wasm_section(0, wasm_string(name) + payload)


def handler_output(function: Function) -> bytes:
    output: list[str] = []
    for expression in function.body.items:
        if isinstance(expression, Call) and isinstance(expression.function, Name) and expression.function.value == "print":
            for argument in expression.arguments:
                if isinstance(argument, String):
                    output.append(argument.value)
                elif isinstance(argument, Integer):
                    output.append(str(argument.value))
                else:
                    raise CapyError(argument.location, "phase-1 print arguments must be string or integer constants")
        elif isinstance(expression, Return) and expression.value is None:
            continue
        else:
            raise CapyError(expression.location, "phase-1 Bearer handlers currently support print(constants) and empty return")
    return "".join(output).encode("utf-8")


def compile_bearer_unit(program: Program, source: str, module_name: str, abi_version: int) -> tuple[bytes, str]:
    handlers: list[tuple[str, bytes, Location]] = []
    export_names = {
        "RENDER": "__bearer_render",
        "CLI": "__bearer_cli",
        "WS": "__bearer_websocket",
        "ONCE": "__bearer_once",
        "INIT": "__bearer_init",
    }
    seen_handlers: dict[str, Location] = {}
    for item in program.items:
        if isinstance(item, Function) and item.name in export_names:
            if len(item.parameters) > 1:
                raise CapyError(item.location, "Bearer handler accepts zero parameters or one request parameter")
            export_name = export_names[item.name]
            if export_name in seen_handlers:
                first = seen_handlers[export_name]
                raise CapyError(item.location, f"Bearer handler {item.name} is already declared at {first.line}:{first.column}; handlers cannot be overloaded")
            seen_handlers[export_name] = item.location
            handlers.append((export_name, handler_output(item), item.location))
    top_level = [item for item in program.items if not isinstance(item, (Function, Struct, Variable))]
    if top_level:
        synthetic = Function(top_level[0].location, "CLI", [], None, Block(top_level[0].location, top_level))
        if any(name == "__bearer_cli" for name, _, _ in handlers):
            raise CapyError(top_level[0].location, "top-level executable expressions conflict with an explicit CLI handler")
        handlers.append(("__bearer_cli", handler_output(synthetic), top_level[0].location))
    if not handlers:
        raise CapyError(Location(source, 1, 1, 0), "Capy Bearer unit exports no CLI, RENDER, WS, ONCE, or INIT handler")

    # Type 0: bearer_print_bytes(i32, i32) -> (); type 1: Bearer handler(i32) -> ().
    type_payload = wasm_vector([
        b"\x60" + wasm_vector([b"\x7f", b"\x7f"]) + wasm_vector([]),
        b"\x60" + wasm_vector([b"\x7f"]) + wasm_vector([]),
    ])
    imports = [
        wasm_string("env") + wasm_string("memory") + b"\x02\x00\x01",
        wasm_string("env") + wasm_string("__memory_base") + b"\x03\x7f\x00",
        wasm_string("env") + wasm_string("bearer_print_bytes") + b"\x00" + uleb(0),
    ]
    import_payload = wasm_vector(imports)
    function_payload = wasm_vector([uleb(1) for _ in handlers])

    data = bytearray()
    bodies: list[bytes] = []
    exports: list[bytes] = []
    source_rows: list[str] = []
    for index, (export_name, output, location) in enumerate(handlers):
        offset = len(data)
        data.extend(output)
        code = bytearray()
        code.extend(b"\x00")  # local declaration count
        code.extend(b"\x23\x00")  # global.get __memory_base
        code.extend(b"\x41" + sleb32(offset))
        code.extend(b"\x6a")  # i32.add
        code.extend(b"\x41" + sleb32(len(output)))
        code.extend(b"\x10\x00")  # call imported bearer_print_bytes
        code.extend(b"\x0b")
        bodies.append(uleb(len(code)) + code)
        function_index = 1 + index  # one imported function precedes definitions
        exports.append(wasm_string(export_name) + b"\x00" + uleb(function_index))
        source_rows.append(f"L\t0\t1\t{location.line}\t{location.column}")

    # PIC memory is allocated by Bearer from dylink.0 and addressed through __memory_base.
    mem_info = uleb(len(data)) + uleb(0) + uleb(0) + uleb(0)
    dylink = b"\x01" + uleb(len(mem_info)) + mem_info
    data_segment = b"\x00\x23\x00\x0b" + uleb(len(data)) + bytes(data)
    abi = (
        "format=bearer-wasm-unit-abi-v1\n"
        f"unit_abi_version={abi_version}\n"
        "toolchain=capyc-direct-wasm-phase1\n"
        f"source={source}\n"
    ).encode("utf-8")
    module = module_name.encode("utf-8")
    wasm = b"\x00asm\x01\x00\x00\x00" + b"".join([
        wasm_custom("dylink.0", dylink),
        wasm_section(1, type_payload),
        wasm_section(2, import_payload),
        wasm_section(3, function_payload),
        wasm_section(7, wasm_vector(exports)),
        wasm_section(10, wasm_vector(bodies)),
        wasm_section(11, wasm_vector([data_segment])),
        wasm_custom("bearer.abi", abi),
        wasm_custom("bearer.module", module),
    ])
    source_map = "\n".join([
        f"BEARER_SOURCE_MAP_V1\t{module_name}",
        f"F\t1\t{source}",
        *source_rows,
        "",
    ])
    return wasm, source_map


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="capyc", description="Compile Capy directly to WebAssembly")
    parser.add_argument("source")
    parser.add_argument("-o", "--output")
    parser.add_argument("--source-map")
    parser.add_argument("--dump-ast", action="store_true")
    parser.add_argument("--bearer-unit", action="store_true")
    parser.add_argument("--abi-version", type=int)
    args = parser.parse_args(argv)
    path = Path(args.source)
    try:
        program = parse(path.read_text(), str(path))
        DeclarationIndex().add_program(program)
        if args.dump_ast:
            print(json.dumps(ast_json(program), indent=2))
            return 0
        if args.bearer_unit:
            if not args.output or not args.source_map or args.abi_version is None:
                parser.error("--bearer-unit requires --output, --source-map, and --abi-version")
            wasm, source_map = compile_bearer_unit(program, str(path.resolve()), Path(args.output).name, args.abi_version)
            Path(args.output).write_bytes(wasm)
            Path(args.source_map).write_text(source_map)
            return 0
    except (OSError, CapyError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    print("capyc: select --dump-ast or --bearer-unit", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
