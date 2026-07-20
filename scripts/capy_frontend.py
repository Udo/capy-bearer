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
class ArrayLiteral(Expr):
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
            if operator.text == ":":
                # A type annotation occupies one expression slot; a following
                # grouped expression belongs to the next declaration slot.
                return Annotation(operator.location, left, self.expression(81))
            right = self.expression(precedence + (0 if operator.text in {"=", ":="} else 1))
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
        if token.text == "[":
            return self.array_literal(token.location)
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

    def array_literal(self, location: Location) -> ArrayLiteral:
        items: list[Expr] = []
        self.skip_separators()
        if not self.match("]"):
            while True:
                items.append(self.expression())
                self.skip_separators()
                if not self.match(","):
                    break
                self.skip_separators()
            self.require("]")
        return ArrayLiteral(location, items)

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
        if len(header) == 1 and not self.is_parameter_expression(header[0]):
            parameter_expr = TupleExpr(location, [])
            return_type = header[0]
        else:
            parameter_expr = header[0] if header else TupleExpr(location, [])
            return_type = header[1] if len(header) == 2 else None
        parameters = self.parameters(parameter_expr)
        return Function(location, name.text, parameters, return_type, body)

    def is_parameter_expression(self, expression: Expr) -> bool:
        expressions = expression.items if isinstance(expression, TupleExpr) else [expression]
        return all(isinstance(item, Annotation) and isinstance(item.value, Name) for item in expressions)

    def parameters(self, expression: Expr) -> list[Parameter]:
        expressions = expression.items if isinstance(expression, TupleExpr) else [expression]
        result: list[Parameter] = []
        names: set[str] = set()
        for item in expressions:
            if not isinstance(item, Annotation) or not isinstance(item.value, Name):
                raise CapyError(item.location, "function parameter expression must contain name:type annotations")
            if item.value.value in names:
                raise CapyError(item.location, f"function parameter {item.value.value!r} is already declared")
            names.add(item.value.value)
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
    if isinstance(expression, ArrayLiteral):
        return "[" + ",".join(type_name(item) for item in expression.items) + "]"
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
