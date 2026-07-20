#!/usr/bin/env python3

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from capy_compiler import (
    Annotation,
    Binary,
    CapyError,
    DeclarationIndex,
    Function,
    Name,
    Return,
    ScopeLookup,
    String,
    TupleExpr,
    Variable,
    compile_bearer_unit,
    parse,
    sleb32,
    type_name,
)


class LexerParserTests(unittest.TestCase):
    def parse_one(self, source):
        program = parse(source, "test.capy")
        self.assertEqual(len(program.items), 1)
        return program.items[0]

    def test_function_header_is_expression_based(self):
        function = self.parse_one("function test(x : s32) s32 { return x + 100 }\n")
        self.assertIsInstance(function, Function)
        self.assertEqual(function.name, "test")
        self.assertEqual([(p.name, type_name(p.type_expr)) for p in function.parameters], [("x", "s32")])
        self.assertEqual(type_name(function.return_type), "s32")
        self.assertIsInstance(function.body.items[0], Return)

    def test_parameter_and_return_expressions_are_optional(self):
        function = self.parse_one('function hello_world { print("Hello World") }\n')
        self.assertEqual(function.parameters, [])
        self.assertIsNone(function.return_type)

    def test_parenthesized_return_expression_is_a_tuple(self):
        function = self.parse_one("function pair() (s32, s32) { return 10, 20 }\n")
        self.assertEqual(function.parameters, [])
        self.assertIsInstance(function.return_type, TupleExpr)
        self.assertEqual(type_name(function.return_type), "(s32,s32)")
        returned = function.body.items[0]
        self.assertIsInstance(returned.value, TupleExpr)
        self.assertEqual([item.value for item in returned.value.items], [10, 20])

    def test_any_is_a_normal_compile_time_type_expression(self):
        function = self.parse_one("function identity(x : any) x::type { return x }\n")
        self.assertEqual(type_name(function.parameters[0].type_expr), "any")
        self.assertIsInstance(function.return_type, ScopeLookup)
        self.assertEqual(type_name(function.return_type), "x::type")

    def test_overload_identity_excludes_return_type(self):
        program = parse(
            "function value(x : s32) s32 { return x }\n"
            "function value(x : s32) string { return string(x) }\n",
            "overload.capy",
        )
        with self.assertRaisesRegex(CapyError, "return type does not distinguish overloads"):
            DeclarationIndex().add_program(program)

    def test_bearer_handlers_cannot_be_overloaded(self):
        program = parse(
            'function CLI { print("a") }\n'
            'function CLI(x : request) { print("b") }\n',
            "handlers.capy",
        )
        DeclarationIndex().add_program(program)
        with self.assertRaisesRegex(CapyError, "handlers cannot be overloaded"):
            compile_bearer_unit(program, "handlers.capy", "handlers.wasm", 8)

    def test_parameter_overloads_are_distinct(self):
        program = parse(
            "function value(x : s32) s32 { return x }\n"
            "function value(x : string) string { return x }\n",
            "overload.capy",
        )
        index = DeclarationIndex()
        index.add_program(program)
        self.assertEqual(len(index.functions), 2)

    def test_explicit_and_inferred_variables(self):
        program = parse("var a : s32 = test(10)\nvar b := test(10, 20)\n", "vars.capy")
        a, b = program.items
        self.assertIsInstance(a, Variable)
        self.assertEqual(type_name(a.annotation), "s32")
        self.assertFalse(a.inferred)
        self.assertTrue(b.inferred)
        self.assertIsNone(b.annotation)

    def test_range_is_an_expression(self):
        expression = self.parse_one("0..10\n")
        self.assertIsInstance(expression, Binary)
        self.assertEqual(expression.operator, "..")

    def test_multiline_string_preserves_newline(self):
        value = self.parse_one('"one and\n  two"\n')
        self.assertIsInstance(value, String)
        self.assertEqual(value.value, "one and\n  two")

    def test_punctuation_string_is_not_consumed_as_parser_structure(self):
        function = self.parse_one('function CLI { print(";", ",", "}") }\n')
        call = function.body.items[0]
        self.assertEqual([argument.value for argument in call.arguments], [";", ",", "}"])

    def test_comments_do_not_hide_newline_separator(self):
        program = parse("var a := 1 // first\nvar b := 2\n", "comments.capy")
        self.assertEqual(len(program.items), 2)

    def test_malformed_parameter_expression_is_rejected(self):
        with self.assertRaisesRegex(CapyError, "name:type annotations"):
            parse("function bad(s32) { return 1 }\n", "bad.capy")

    def test_unterminated_string_has_source_location(self):
        with self.assertRaisesRegex(CapyError, r"bad.capy:1:1: unterminated string"):
            parse('"broken', "bad.capy")

    def test_array_and_managed_struct_program_compiles(self):
        program = parse(
            'struct Pair { label : string; value : s32 }\n'
            'function choose(values : [string], i : s32) string { values[i] }\n'
            'function CLI { var p := Pair(clone("x"), 7); print(choose([p.label], 0), p.value) }\n',
            "managed.capy",
        )
        wasm, _ = compile_bearer_unit(program, "managed.capy", "managed.wasm", 9)
        self.assertTrue(wasm.startswith(b"\0asm"))

    def test_array_literal_rejects_mixed_element_types(self):
        program = parse('function CLI { var values := [1, "two"] }', "mixed.capy")
        with self.assertRaisesRegex(CapyError, "array literal elements must have one type"):
            compile_bearer_unit(program, "mixed.capy", "mixed.wasm", 9)

    def test_handler_parameter_must_be_request(self):
        program = parse('function CLI(value : s32) { print(value) }\n', "handler.capy")
        with self.assertRaisesRegex(CapyError, "Bearer handler parameter must have type request"):
            compile_bearer_unit(program, "handler.capy", "handler.wasm", 9)

    def test_void_parameter_is_rejected_before_wasm(self):
        program = parse(
            'function sink(value : void) void {}\nfunction CLI { sink(print(1)) }\n',
            "void-parameter.capy",
        )
        with self.assertRaisesRegex(CapyError, "function parameters cannot have type void"):
            compile_bearer_unit(program, "void-parameter.capy", "void-parameter.wasm", 9)

    def test_non_total_managed_return_is_rejected_before_wasm(self):
        program = parse(
            'function maybe(x : string, b : bool) string { if b { return x } }\n'
            'function CLI { print(maybe(clone("x"), false)) }\n',
            "returns.capy",
        )
        with self.assertRaisesRegex(CapyError, "not all paths produce string"):
            compile_bearer_unit(program, "returns.capy", "returns.wasm", 9)

    def test_borrowed_managed_loop_value_cannot_be_reassigned(self):
        program = parse(
            'function CLI { for value = ["old"] { value = clone("new") } }\n',
            "borrowed-loop.capy",
        )
        with self.assertRaisesRegex(CapyError, "cannot assign to a borrowed managed value"):
            compile_bearer_unit(program, "borrowed-loop.capy", "borrowed-loop.wasm", 9)

    def test_borrowed_managed_parameter_cannot_be_reassigned(self):
        program = parse(
            'function replace(x : string) void { x = clone("new") }\n'
            'function CLI { var value := clone("old"); replace(value); print(value) }\n',
            "borrowed.capy",
        )
        with self.assertRaisesRegex(CapyError, "cannot assign to a borrowed managed value"):
            compile_bearer_unit(program, "borrowed.capy", "borrowed.wasm", 9)

    def test_range_variable_does_not_escape_its_scope(self):
        program = parse('function CLI { for i = 0..1 { print(i) }\nprint(i) }\n', "scope.capy")
        with self.assertRaisesRegex(CapyError, "unknown local 'i'"):
            compile_bearer_unit(program, "scope.capy", "scope.wasm", 9)

    def test_i32_constants_use_signed_leb32(self):
        self.assertEqual(sleb32(0), b"\x00")
        self.assertEqual(sleb32(63), b"\x3f")
        self.assertEqual(sleb32(64), b"\xc0\x00")
        self.assertEqual(sleb32(127), b"\xff\x00")
        self.assertEqual(sleb32(128), b"\x80\x01")
        self.assertEqual(sleb32(-1), b"\x7f")

    def test_metaprogramming_is_a_targeted_deferred_feature(self):
        with self.assertRaisesRegex(CapyError, "#compile compile-time metaprogramming is deferred beyond Capy phase 3"):
            parse("function meta(x : any) { #compile { emit(x) } }\n", "meta.capy")


if __name__ == "__main__":
    unittest.main()
