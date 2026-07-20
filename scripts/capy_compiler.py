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
class If(Expr):
    condition: Expr
    then_body: Block
    else_body: Block | None


@dataclass
class While(Expr):
    condition: Expr
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
        if self.token.kind != "string" and self.token.text == text:
            self.take()
            return True
        return False

    def require(self, text: str) -> Token:
        if self.token.kind == "string" or self.token.text != text:
            raise CapyError(self.token.location, f"expected {text!r}, found {self.token.text or 'end of file'!r}")
        return self.take()

    def require_identifier(self, purpose: str) -> Token:
        if self.token.kind != "identifier":
            raise CapyError(self.token.location, f"expected {purpose}, found {self.token.text or 'end of file'!r}")
        return self.take()

    def skip_separators(self) -> None:
        while self.token.kind == "newline" or (self.token.kind == "symbol" and self.token.text == ";"):
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
            if token.text == "if":
                return self.if_expr(token.location)
            if token.text == "while":
                return self.while_expr(token.location)
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

    def if_expr(self, location: Location) -> If:
        condition = self.expression()
        body_location = self.require("{").location
        then_body = self.block(body_location)
        separator_position = self.position
        self.skip_separators()
        else_body = None
        if self.match("else"):
            else_location = self.require("{").location
            else_body = self.block(else_location)
        else:
            self.position = separator_position
        return If(location, condition, then_body, else_body)

    def while_expr(self, location: Location) -> While:
        condition = self.expression()
        body_location = self.require("{").location
        return While(location, condition, self.block(body_location))


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


SCALAR_TYPES = {"s32", "bool"}
HANDLER_EXPORTS = {
    "RENDER": "__bearer_render",
    "CLI": "__bearer_cli",
    "WS": "__bearer_websocket",
    "ONCE": "__bearer_once",
    "INIT": "__bearer_init",
}


def capy_type(expression: Expr | None, location: Location, allow_void: bool = False) -> str:
    if expression is None:
        if allow_void:
            return "void"
        raise CapyError(location, "function return type cannot be inferred yet; declare it explicitly")
    name = type_name(expression)
    if name == "any" or "::type" in name:
        raise CapyError(expression.location, "compile-time any and dependent result types are reserved for phase 3")
    if name not in SCALAR_TYPES | {"string", "request", "void"}:
        raise CapyError(expression.location, f"phase-1 backend does not yet support type {name!r}")
    return name


@dataclass
class CompiledDefinition:
    function: Function
    parameter_types: tuple[str, ...]
    result_type: str
    export_name: str | None
    function_index: int = 0
    type_index: int = 0


class WasmFunctionCompiler:
    def __init__(self, module: "CapyModuleCompiler", definition: CompiledDefinition):
        self.module = module
        self.definition = definition
        self.scopes: list[dict[str, tuple[int, str]]] = [{}]
        self.parameter_count = 1 if definition.export_name else len(definition.parameter_types)
        self.local_count = 0
        if definition.export_name:
            if len(definition.function.parameters) == 1:
                parameter = definition.function.parameters[0]
                self.scopes[0][parameter.name] = (0, "request")
        else:
            for index, parameter in enumerate(definition.function.parameters):
                self.scopes[0][parameter.name] = (index, definition.parameter_types[index])

    def lookup(self, name: Name) -> tuple[int, str]:
        for scope in reversed(self.scopes):
            if name.value in scope:
                return scope[name.value]
        raise CapyError(name.location, f"unknown local {name.value!r}")

    def allocate_local(self, value_type: str, location: Location) -> int:
        if value_type not in SCALAR_TYPES:
            raise CapyError(location, f"phase-1 local type {value_type!r} is not scalar")
        index = self.parameter_count + self.local_count
        self.local_count += 1
        return index

    def add_local(self, name: str, value_type: str, location: Location) -> int:
        if name in self.scopes[-1]:
            raise CapyError(location, f"local {name!r} is already declared in this scope")
        index = self.allocate_local(value_type, location)
        self.scopes[-1][name] = (index, value_type)
        return index

    def compile_body(self) -> bytes:
        code = self.compile_block(self.definition.function.body, self.definition.result_type)
        if self.definition.result_type == "void":
            code += b"\x0b"
        else:
            code += b"\x0b"
        locals_prefix = b"\x00" if self.local_count == 0 else b"\x01" + uleb(self.local_count) + b"\x7f"
        body = locals_prefix + code
        return uleb(len(body)) + body

    def compile_block(self, block: Block, expected_result: str = "void") -> bytes:
        self.scopes.append({})
        code = bytearray()
        for index, expression in enumerate(block.items):
            is_last = index == len(block.items) - 1
            expression_code, result_type = self.compile_expr(expression)
            code.extend(expression_code)
            if result_type != "void":
                if is_last and expected_result != "void":
                    self.require_type(expression.location, expected_result, result_type)
                else:
                    code.append(0x1A)  # drop
        self.scopes.pop()
        if expected_result != "void" and not block.items:
            raise CapyError(block.location, f"function must produce {expected_result}")
        return bytes(code)

    def require_type(self, location: Location, expected: str, actual: str) -> None:
        if expected != actual:
            raise CapyError(location, f"expected {expected}, found {actual}")

    def compile_expr(self, expression: Expr) -> tuple[bytes, str]:
        if isinstance(expression, Integer):
            return b"\x41" + sleb32(expression.value), "s32"
        if isinstance(expression, String):
            return b"", "string"
        if isinstance(expression, Name):
            if expression.value in {"true", "false"}:
                return b"\x41" + sleb32(1 if expression.value == "true" else 0), "bool"
            index, value_type = self.lookup(expression)
            return b"\x20" + uleb(index), value_type
        if isinstance(expression, Variable):
            value_code, value_type = self.compile_expr(expression.value)
            declared = capy_type(expression.annotation, expression.location) if expression.annotation else value_type
            self.require_type(expression.location, declared, value_type)
            local = self.add_local(expression.name, declared, expression.location)
            return value_code + b"\x21" + uleb(local), "void"
        if isinstance(expression, Binary):
            if expression.operator == "=":
                if not isinstance(expression.left, Name):
                    raise CapyError(expression.left.location, "assignment target must be a local name")
                local, target_type = self.lookup(expression.left)
                value_code, value_type = self.compile_expr(expression.right)
                self.require_type(expression.location, target_type, value_type)
                return value_code + b"\x22" + uleb(local), target_type
            left_code, left_type = self.compile_expr(expression.left)
            right_code, right_type = self.compile_expr(expression.right)
            self.require_type(expression.location, left_type, right_type)
            arithmetic = {"+": 0x6A, "-": 0x6B, "*": 0x6C, "/": 0x6D, "%": 0x6F}
            comparison = {"==": 0x46, "!=": 0x47, "<": 0x48, ">": 0x4A, "<=": 0x4C, ">=": 0x4E}
            if expression.operator in arithmetic:
                self.require_type(expression.location, "s32", left_type)
                return left_code + right_code + bytes([arithmetic[expression.operator]]), "s32"
            if expression.operator in comparison:
                if left_type not in SCALAR_TYPES:
                    raise CapyError(expression.location, "comparison requires scalar operands")
                return left_code + right_code + bytes([comparison[expression.operator]]), "bool"
            raise CapyError(expression.location, f"phase-1 backend does not support operator {expression.operator!r}")
        if isinstance(expression, Call):
            if not isinstance(expression.function, Name):
                raise CapyError(expression.function.location, "phase-1 calls require a named function")
            if expression.function.value == "print":
                code = bytearray()
                for argument in expression.arguments:
                    if isinstance(argument, String):
                        offset, length = self.module.add_data(argument.value.encode("utf-8"))
                        code.extend(b"\x23\x00\x41" + sleb32(offset) + b"\x6a")
                        code.extend(b"\x41" + sleb32(length) + b"\x10\x00")
                    else:
                        argument_code, argument_type = self.compile_expr(argument)
                        if argument_type not in SCALAR_TYPES:
                            raise CapyError(argument.location, f"print does not yet support {argument_type}")
                        code.extend(argument_code + b"\x10\x01")
                return bytes(code), "void"
            arguments = [self.compile_expr(argument) for argument in expression.arguments]
            parameter_types = tuple(value_type for _, value_type in arguments)
            target = self.module.resolve_function(expression.function.value, parameter_types, expression.location)
            code = b"".join(argument_code for argument_code, _ in arguments) + b"\x10" + uleb(target.function_index)
            return code, target.result_type
        if isinstance(expression, Return):
            expected = self.definition.result_type
            if expression.value is None:
                self.require_type(expression.location, "void", expected)
                return b"\x0f", "void"
            value_code, value_type = self.compile_expr(expression.value)
            self.require_type(expression.location, expected, value_type)
            return value_code + b"\x0f", "void"
        if isinstance(expression, If):
            condition_code, condition_type = self.compile_expr(expression.condition)
            if condition_type not in SCALAR_TYPES:
                raise CapyError(expression.condition.location, "if condition must be scalar")
            then_code = self.compile_block(expression.then_body)
            code = bytearray(condition_code + b"\x04\x40" + then_code)
            if expression.else_body:
                code.extend(b"\x05" + self.compile_block(expression.else_body))
            code.append(0x0B)
            return bytes(code), "void"
        if isinstance(expression, While):
            condition_code, condition_type = self.compile_expr(expression.condition)
            if condition_type not in SCALAR_TYPES:
                raise CapyError(expression.condition.location, "while condition must be scalar")
            body = self.compile_block(expression.body)
            return b"\x02\x40\x03\x40" + condition_code + b"\x45\x0d\x01" + body + b"\x0c\x00\x0b\x0b", "void"
        if isinstance(expression, For):
            if not isinstance(expression.iterable, Binary) or expression.iterable.operator != "..":
                raise CapyError(expression.iterable.location, "phase-1 for loop requires an exclusive range")
            start_code, start_type = self.compile_expr(expression.iterable.left)
            end_code, end_type = self.compile_expr(expression.iterable.right)
            self.require_type(expression.location, "s32", start_type)
            self.require_type(expression.location, "s32", end_type)
            loop_local = self.allocate_local("s32", expression.location)
            end_local = self.allocate_local("s32", expression.location)
            self.scopes.append({expression.name: (loop_local, "s32")})
            body = self.compile_block(expression.body)
            self.scopes.pop()
            code = start_code + b"\x21" + uleb(loop_local) + end_code + b"\x21" + uleb(end_local)
            code += b"\x02\x40\x03\x40\x20" + uleb(loop_local) + b"\x20" + uleb(end_local) + b"\x4e\x0d\x01"
            code += body + b"\x20" + uleb(loop_local) + b"\x41\x01\x6a\x21" + uleb(loop_local) + b"\x0c\x00\x0b\x0b"
            return code, "void"
        if isinstance(expression, Block):
            return self.compile_block(expression), "void"
        raise CapyError(expression.location, f"phase-1 backend cannot lower {expression.__class__.__name__}")


class CapyModuleCompiler:
    def __init__(self, program: Program, source: str, module_name: str, abi_version: int):
        self.program = program
        self.source = source
        self.module_name = module_name
        self.abi_version = abi_version
        self.data = bytearray()
        self.definitions: list[CompiledDefinition] = []
        self.functions: dict[FunctionKey, CompiledDefinition] = {}
        self.types: list[tuple[tuple[str, ...], str]] = []
        self.type_indices: dict[tuple[tuple[str, ...], str], int] = {}

    def add_data(self, value: bytes) -> tuple[int, int]:
        offset = len(self.data)
        self.data.extend(value)
        return offset, len(value)

    def wasm_type(self, parameters: tuple[str, ...], result: str) -> int:
        key = (parameters, result)
        if key not in self.type_indices:
            self.type_indices[key] = len(self.types)
            self.types.append(key)
        return self.type_indices[key]

    def collect(self) -> None:
        seen_handlers: dict[str, Location] = {}
        top_level: list[Expr] = []
        for item in self.program.items:
            if isinstance(item, Function):
                parameter_types = tuple(capy_type(parameter.type_expr, item.location) for parameter in item.parameters)
                if item.name in HANDLER_EXPORTS:
                    if len(item.parameters) > 1:
                        raise CapyError(item.location, "Bearer handler accepts zero parameters or one request parameter")
                    export_name = HANDLER_EXPORTS[item.name]
                    if export_name in seen_handlers:
                        first = seen_handlers[export_name]
                        raise CapyError(item.location, f"Bearer handler {item.name} is already declared at {first.line}:{first.column}; handlers cannot be overloaded")
                    seen_handlers[export_name] = item.location
                    definition = CompiledDefinition(item, parameter_types, "void", export_name)
                else:
                    result_type = capy_type(item.return_type, item.location)
                    definition = CompiledDefinition(item, parameter_types, result_type, None)
                    key = FunctionKey(item.name, parameter_types)
                    self.functions[key] = definition
                self.definitions.append(definition)
            elif isinstance(item, Struct):
                raise CapyError(item.location, "struct lowering is not implemented yet")
            elif isinstance(item, Variable):
                raise CapyError(item.location, "top-level variables are not implemented yet")
            else:
                top_level.append(item)
        if top_level:
            if "__bearer_cli" in seen_handlers:
                raise CapyError(top_level[0].location, "top-level executable expressions conflict with an explicit CLI handler")
            synthetic = Function(top_level[0].location, "CLI", [], None, Block(top_level[0].location, top_level))
            self.definitions.append(CompiledDefinition(synthetic, (), "void", "__bearer_cli"))
        if not any(definition.export_name for definition in self.definitions):
            raise CapyError(Location(self.source, 1, 1, 0), "Capy Bearer unit exports no CLI, RENDER, WS, ONCE, or INIT handler")

    def resolve_function(self, name: str, parameters: tuple[str, ...], location: Location) -> CompiledDefinition:
        key = FunctionKey(name, parameters)
        if key not in self.functions:
            rendered = ", ".join(parameters)
            raise CapyError(location, f"no overload {name}({rendered})")
        return self.functions[key]

    def compile(self) -> tuple[bytes, str]:
        self.collect()
        # Stable direct-language imports are function indices 0 and 1.
        print_bytes_type = self.wasm_type(("s32", "s32"), "void")
        print_s32_type = self.wasm_type(("s32",), "void")
        for index, definition in enumerate(self.definitions):
            definition.function_index = 2 + index
            wasm_parameters = ("s32",) if definition.export_name else definition.parameter_types
            definition.type_index = self.wasm_type(wasm_parameters, definition.result_type)
        bodies = [WasmFunctionCompiler(self, definition).compile_body() for definition in self.definitions]

        def encode_type(signature: tuple[tuple[str, ...], str]) -> bytes:
            parameters, result = signature
            wasm_parameters = [b"\x7f" for _ in parameters]
            wasm_results = [] if result == "void" else [b"\x7f"]
            return b"\x60" + wasm_vector(wasm_parameters) + wasm_vector(wasm_results)

        imports = [
            wasm_string("env") + wasm_string("memory") + b"\x02\x00\x01",
            wasm_string("env") + wasm_string("__memory_base") + b"\x03\x7f\x00",
            wasm_string("env") + wasm_string("bearer_print_bytes") + b"\x00" + uleb(print_bytes_type),
            wasm_string("env") + wasm_string("bearer_print_s32") + b"\x00" + uleb(print_s32_type),
        ]
        exports = [
            wasm_string(definition.export_name) + b"\x00" + uleb(definition.function_index)
            for definition in self.definitions if definition.export_name
        ]
        mem_info = uleb(len(self.data)) + uleb(0) + uleb(0) + uleb(0)
        data_segment = b"\x00\x23\x00\x0b" + uleb(len(self.data)) + bytes(self.data)
        abi = (
            "format=bearer-wasm-unit-abi-v1\n"
            f"unit_abi_version={self.abi_version}\n"
            "toolchain=capyc-direct-wasm-phase1\n"
            f"source={self.source}\n"
        ).encode("utf-8")
        wasm = b"\x00asm\x01\x00\x00\x00" + b"".join([
            wasm_custom("dylink.0", b"\x01" + uleb(len(mem_info)) + mem_info),
            wasm_section(1, wasm_vector([encode_type(signature) for signature in self.types])),
            wasm_section(2, wasm_vector(imports)),
            wasm_section(3, wasm_vector([uleb(definition.type_index) for definition in self.definitions])),
            wasm_section(7, wasm_vector(exports)),
            wasm_section(10, wasm_vector(bodies)),
            wasm_section(11, wasm_vector([data_segment])),
            wasm_custom("bearer.abi", abi),
            wasm_custom("bearer.module", self.module_name.encode("utf-8")),
        ])
        source_map = "\n".join([
            f"BEARER_SOURCE_MAP_V1\t{self.module_name}",
            f"F\t1\t{self.source}",
            *[f"L\t0\t1\t{definition.function.location.line}\t{definition.function.location.column}" for definition in self.definitions],
            "",
        ])
        return wasm, source_map


def compile_bearer_unit(program: Program, source: str, module_name: str, abi_version: int) -> tuple[bytes, str]:
    return CapyModuleCompiler(program, source, module_name, abi_version).compile()


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
