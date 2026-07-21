#!/usr/bin/env python3

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from capy_compiler import (
    Annotation,
    Binary,
    CapyError,
    CapyModuleCompiler,
    DeclarationIndex,
    Function,
    Name,
    Markup,
    MarkupField,
    MarkupText,
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

    def test_unparenthesized_parameter_expression_precedes_tuple_result(self):
        function = self.parse_one("function pair value : s32 (s32, string) { return (value, \"x\") }\n")
        self.assertEqual([(parameter.name, type_name(parameter.type_expr)) for parameter in function.parameters], [("value", "s32")])
        self.assertEqual(type_name(function.return_type), "(s32,string)")

    def test_single_non_parameter_header_expression_is_return_type(self):
        function = self.parse_one("function pair (s32, string) { return (1, \"x\") }\n")
        self.assertEqual(function.parameters, [])
        self.assertEqual(type_name(function.return_type), "(s32,string)")

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

    def test_markup_fragment_has_static_escaped_and_raw_parts(self):
        markup = self.parse_one('<><h1><?= title ?></h1><?: trusted ?></>\n')
        self.assertIsInstance(markup, Markup)
        self.assertEqual(len(markup.parts), 4)
        self.assertIsInstance(markup.parts[0], MarkupText)
        self.assertEqual(markup.parts[0].value, "<h1>")
        self.assertIsInstance(markup.parts[1], MarkupField)
        self.assertTrue(markup.parts[1].escaped)
        self.assertEqual(markup.parts[1].value.value, "title")
        self.assertFalse(markup.parts[3].escaped)

    def test_nested_markup_fragment_delimiters_are_not_output(self):
        markup = self.parse_one('<><div><><span>x</span></></div></>\n')
        self.assertEqual([part.value for part in markup.parts], ["<div><span>x</span></div>"])

    def test_markup_interpolation_tracks_nested_markup_and_comments(self):
        nested = self.parse_one('<><p><?= <><b><?= clone("x") ?></b></> ?></p></>\n')
        outer = next(part for part in nested.parts if isinstance(part, MarkupField))
        self.assertIsInstance(outer.value, Markup)
        self.assertIsInstance(outer.value.parts[1], MarkupField)

        commented = self.parse_one('<><?= value // ?>\n ?></>\n')
        self.assertEqual(commented.parts[0].value.value, "value")

    def test_markup_literal_delimiter_escape(self):
        markup = self.parse_one('<><script>const close = "\\</>";</script></>\n')
        self.assertEqual(markup.parts[0].value, '<script>const close = "</>";</script>')

    def test_markup_reports_empty_and_unterminated_interpolation(self):
        with self.assertRaisesRegex(CapyError, "empty markup interpolation"):
            parse("<><?=   ?></>", "empty-markup.capy")
        with self.assertRaisesRegex(CapyError, "unterminated markup interpolation"):
            parse("<><?= value </>", "unterminated-field.capy")
        with self.assertRaisesRegex(CapyError, "unterminated markup expression"):
            parse("<><p>missing", "unterminated-markup.capy")

    def test_punctuation_string_is_not_consumed_as_parser_structure(self):
        function = self.parse_one('function CLI { print(";", ",", "}") }\n')
        call = function.body.items[0]
        self.assertEqual([argument.value for argument in call.arguments], [";", ",", "}"])

    def test_comments_do_not_hide_newline_separator(self):
        program = parse("var a := 1 // first\nvar b := 2\n", "comments.capy")
        self.assertEqual(len(program.items), 2)

    def test_malformed_parameter_expression_is_rejected(self):
        with self.assertRaisesRegex(CapyError, "name:type annotations"):
            parse("function bad(s32) s32 { return 1 }\n", "bad.capy")

    def test_unterminated_string_has_source_location(self):
        with self.assertRaisesRegex(CapyError, r"bad.capy:1:1: unterminated string"):
            parse('"broken', "bad.capy")

    def test_duplicate_parameter_names_are_rejected(self):
        with self.assertRaisesRegex(CapyError, "function parameter 'value' is already declared"):
            parse('function bad(value : any, value : any) value::type { value }\n', "duplicate-parameter.capy")

    def test_generic_function_value_is_not_inferred_from_prior_specialization(self):
        program = parse(
            'function identity(x : any) x::type { x }\n'
            'function CLI { print(identity(1)); var selected := identity }\n',
            "generic-function-value.capy",
        )
        with self.assertRaisesRegex(CapyError, "generic function value 'identity' requires an explicit concrete function type"):
            compile_bearer_unit(program, "generic-function-value.capy", "generic-function-value.wasm", 9)

    def test_function_value_requires_one_concrete_overload(self):
        program = parse(
            'function value(x : s32) s32 { x }\n'
            'function value(x : bool) bool { x }\n'
            'function CLI { var selected := value }\n',
            "function-value.capy",
        )
        with self.assertRaisesRegex(CapyError, "function value 'value' requires exactly one concrete overload"):
            compile_bearer_unit(program, "function-value.capy", "function-value.wasm", 9)

    def test_explicit_scalar_casts_and_invalid_managed_cast(self):
        program = parse('function CLI { print(0 as bool, true as s32, (1 + 0) as bool) }\n', "casts.capy")
        wasm, _ = compile_bearer_unit(program, "casts.capy", "casts.wasm", 9)
        self.assertTrue(wasm.startswith(b"\0asm"))
        invalid = parse('function CLI { print("x" as s32) }\n', "bad-cast.capy")
        with self.assertRaisesRegex(CapyError, "no explicit conversion from string to s32"):
            compile_bearer_unit(invalid, "bad-cast.capy", "bad-cast.wasm", 9)

    def test_tuple_index_must_be_static_and_in_bounds(self):
        dynamic = parse('function CLI { var pair := (1, 2); print(pair[1 + 0]) }\n', "tuple-dynamic.capy")
        with self.assertRaisesRegex(CapyError, "tuple index must be a compile-time integer"):
            compile_bearer_unit(dynamic, "tuple-dynamic.capy", "tuple-dynamic.wasm", 9)
        outside = parse('function CLI { var pair := (1, 2); print(pair[2]) }\n', "tuple-bounds.capy")
        with self.assertRaisesRegex(CapyError, "tuple index is out of bounds"):
            compile_bearer_unit(outside, "tuple-bounds.capy", "tuple-bounds.wasm", 9)

    def test_any_specializations_are_cached_by_concrete_parameter_types(self):
        program = parse(
            'function identity(value : any) value::type { value }\n'
            'function CLI { print(identity(1), identity(2), identity(clone("x"))) }\n',
            "generic.capy",
        )
        module = CapyModuleCompiler(program, "generic.capy", "generic.wasm", 9)
        module.compile()
        specializations = [
            definition.parameter_types for definition in module.definitions
            if definition.function.name == "identity"
        ]
        self.assertEqual(specializations, [("s32",), ("string",)])

    def test_any_specialization_validates_operators_for_concrete_type(self):
        program = parse(
            'function square(value : any) value::type { value * value }\n'
            'function CLI { print(square(clone("x"))) }\n',
            "generic-operator.capy",
        )
        with self.assertRaisesRegex(CapyError, "expected s32, found string"):
            compile_bearer_unit(program, "generic-operator.capy", "generic-operator.wasm", 9)

    def test_equally_ranked_generic_overloads_are_ambiguous(self):
        program = parse(
            'function choose(a : any, b : s32) a::type { a }\n'
            'function choose(a : s32, b : any) b::type { b }\n'
            'function CLI { print(choose(1, 2)) }\n',
            "ambiguous.capy",
        )
        with self.assertRaisesRegex(CapyError, "ambiguous generic overload choose"):
            compile_bearer_unit(program, "ambiguous.capy", "ambiguous.wasm", 9)

    def test_literal_output_omits_unused_imports_helpers_and_arc_header(self):
        program = parse('function CLI { print("hé") }\n', "literal.capy")
        module = CapyModuleCompiler(program, "literal.capy", "literal.wasm", 10)
        wasm, _ = module.compile()
        self.assertEqual(module.used_host_imports, {"bearer_print_bytes"})
        self.assertEqual(module.used_helpers, set())
        self.assertEqual(module.first_user_index, 1)
        self.assertFalse(module.uses_arc_global)
        self.assertEqual(module.data, "hé".encode("utf-8"))
        self.assertLess(len(wasm), 400)

    def test_empty_handler_has_no_function_imports_or_helpers(self):
        program = parse("function CLI {}\n", "empty.capy")
        module = CapyModuleCompiler(program, "empty.capy", "empty.wasm", 10)
        module.compile()
        self.assertEqual(module.host_indices, {})
        self.assertEqual(module.helper_indices, {})
        self.assertEqual(module.first_user_index, 0)
        self.assertFalse(module.uses_arc_global)

    def test_arc_unit_emits_only_required_runtime_surface(self):
        program = parse('function CLI { var value := clone("x"); print(value) }\n', "arc-surface.capy")
        module = CapyModuleCompiler(program, "arc-surface.capy", "arc-surface.wasm", 10)
        module.compile()
        self.assertEqual(
            set(module.host_indices),
            {"bearer_print_bytes", "bearer_alloc", "bearer_free"},
        )
        self.assertEqual(module.used_helpers, {"clone", "release"})
        self.assertNotIn("retain", module.helper_indices)
        self.assertTrue(module.uses_arc_global)

    def test_markup_runtime_surface_and_raw_type_boundary(self):
        static = parse('function CLI { print(<><p>static</p></>) }\n', "static-markup.capy")
        module = CapyModuleCompiler(static, "static-markup.capy", "static-markup.wasm", 10)
        module.compile()
        self.assertEqual(module.host_indices, {"bearer_print_bytes": 0})
        self.assertEqual(module.helper_indices, {})
        self.assertFalse(module.uses_arc_global)
        self.assertEqual(module.data, b"<p>static</p>")

        dynamic = parse(
            'function CLI { var value := clone("<&"); print(<><b><?= value ?></b></>) }\n',
            "dynamic-markup.capy",
        )
        module = CapyModuleCompiler(dynamic, "dynamic-markup.capy", "dynamic-markup.wasm", 10)
        module.compile()
        self.assertEqual(set(module.host_indices), {"bearer_print_bytes", "bearer_alloc", "bearer_free"})
        self.assertEqual(module.used_helpers, {"clone", "release"})
        self.assertTrue(module.uses_arc_global)

        unsafe = parse('function CLI { print(<><?: "raw" ?></>) }\n', "unsafe-markup.capy")
        with self.assertRaisesRegex(CapyError, "raw markup interpolation requires a markup value"):
            compile_bearer_unit(unsafe, "unsafe-markup.capy", "unsafe-markup.wasm", 10)

    def test_compacted_adapter_and_release_indices(self):
        adapters = parse(
            'function CLI { unit_render("/x"); component_render("/x"); '
            'print(dval_string(unit_call("/x", "f", dval("x")))) }\n',
            "adapters.capy",
        )
        module = CapyModuleCompiler(adapters, "adapters.capy", "adapters.wasm", 10)
        module.compile()
        self.assertEqual(
            module.host_indices,
            {
                "bearer_print_bytes": 0,
                "bearer_alloc": 1,
                "bearer_free": 2,
                "bearer_unit_render_bytes": 3,
                "bearer_component_render_bytes": 4,
                "bearer_dv_string_to_brrb": 5,
                "bearer_dv_brrb_to_string": 6,
                "bearer_unit_call_brrb": 7,
            },
        )
        self.assertEqual(module.helper_indices, {"release": 8})
        self.assertEqual(module.definitions[0].function_index, 9)

        release_only = parse(
            'function keep(value : string) { var local := value }\nfunction CLI {}\n',
            "release-only.capy",
        )
        module = CapyModuleCompiler(release_only, "release-only.capy", "release-only.wasm", 10)
        module.compile()
        self.assertEqual(module.host_indices, {"bearer_free": 0})
        self.assertEqual(module.helper_indices, {"retain": 1, "release": 2})
        self.assertEqual(module.first_user_index, 3)

    def test_indirect_table_uses_rebased_function_index(self):
        program = parse(
            'function identity(value : s32) s32 { value }\n'
            'function CLI { var selected := identity; print(selected(7)) }\n',
            "indirect-surface.capy",
        )
        module = CapyModuleCompiler(program, "indirect-surface.capy", "indirect-surface.wasm", 10)
        module.compile()
        identity = next(definition for definition in module.definitions if definition.function.name == "identity")
        self.assertEqual(module.host_indices, {"bearer_print_s32": 0})
        self.assertEqual(module.helper_indices, {})
        self.assertEqual(identity.function_index, 1)
        self.assertEqual(list(module.table_slots.values()), [0])

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

    def test_break_and_continue_reject_arguments_and_operators(self):
        for source in (
            "function CLI { break junk }\n",
            "function CLI { continue() }\n",
            "function CLI { while true { break() } }\n",
        ):
            with self.assertRaisesRegex(CapyError, "does not accept arguments or operators"):
                parse(source, "malformed-loop-control.capy")

    def test_break_and_continue_require_loop_context(self):
        for keyword in ("break", "continue"):
            program = parse(f"function CLI {{ {keyword} }}\n", f"{keyword}.capy")
            with self.assertRaisesRegex(CapyError, f"{keyword} is only valid inside a loop"):
                compile_bearer_unit(program, f"{keyword}.capy", f"{keyword}.wasm", 10)

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
