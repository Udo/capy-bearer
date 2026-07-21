#!/usr/bin/env python3
"""Typed Capy-to-Wasm lowering and Bearer side-module encoding."""

from dataclasses import dataclass
import struct

from capy_frontend import *

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


def managed_type(value_type: str) -> bool:
    return value_type in {"string", "markup", "dval"} or value_type.startswith(("array<", "struct:", "tuple<"))


HANDLER_EXPORTS = {
    "RENDER": "__bearer_render",
    "COMPONENT": "__bearer_component",
    "CLI": "__bearer_cli",
    "WS": "__bearer_websocket",
    "ONCE": "__bearer_once",
    "INIT": "__bearer_init",
    "SERVE_HTTP": "__bearer_serve_http",
}


def handler_export(name: str) -> str | None:
    if name in HANDLER_EXPORTS:
        return HANDLER_EXPORTS[name]
    for prefix, symbol in (("RENDER_", "__bearer_render_"), ("COMPONENT_", "__bearer_component_"), ("SERVE_HTTP_", "__bearer_serve_http_")):
        if name.startswith(prefix) and len(name) > len(prefix):
            return symbol + name[len(prefix):]
    return None


def capy_type(expression: Expr | None, location: Location, allow_void: bool = False) -> str:
    if expression is None:
        if allow_void:
            return "void"
        raise CapyError(location, "function return type cannot be inferred yet; declare it explicitly")
    if isinstance(expression, TupleExpr):
        if len(expression.items) < 2:
            raise CapyError(expression.location, "tuple type requires at least two element types")
        return "tuple<" + ",".join(capy_type(item, item.location) for item in expression.items) + ">"
    if isinstance(expression, ArrayLiteral):
        if len(expression.items) != 1:
            raise CapyError(expression.location, "array type requires exactly one element type")
        element_type = capy_type(expression.items[0], expression.location)
        if element_type not in SCALAR_TYPES | {"string"}:
            raise CapyError(expression.location, f"array element type {element_type!r} is unsupported")
        return f"array<{element_type}>"
    name = type_name(expression)
    if name == "any" or "::type" in name:
        raise CapyError(expression.location, "compile-time any and dependent result types are reserved for phase 3")
    if name in SCALAR_TYPES | {"string", "markup", "dval", "request", "void"}:
        return name
    return f"struct:{name}"


@dataclass
class GenericDefinition:
    function: Function
    parameter_patterns: tuple[str | None, ...]
    dependent_result: int


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
        self.owned_scopes: list[list[tuple[int, str]]] = [[]]
        self.parameter_count = 1 if definition.export_name else len(definition.parameter_types)
        self.local_count = 0
        self.borrowed_managed_slots: set[int] = set()
        self.control_depth = 0
        self.loop_contexts: list[tuple[int, int, int, bytes, bytes]] = []  # targets, owned boundary, edge cleanup
        if definition.export_name:
            if len(definition.function.parameters) == 1:
                parameter = definition.function.parameters[0]
                self.scopes[0][parameter.name] = (0, "request")
        else:
            for index, parameter in enumerate(definition.function.parameters):
                self.scopes[0][parameter.name] = (index, definition.parameter_types[index])
                if managed_type(definition.parameter_types[index]):
                    self.borrowed_managed_slots.add(index)

    def lookup(self, name: Name) -> tuple[int, str]:
        for scope in reversed(self.scopes):
            if name.value in scope:
                return scope[name.value]
        raise CapyError(name.location, f"unknown local {name.value!r}")

    def lookup_optional(self, name: str) -> tuple[int, str] | None:
        for scope in reversed(self.scopes):
            if name in scope:
                return scope[name]
        return None

    def allocate_local(self, value_type: str, location: Location) -> int:
        if value_type not in SCALAR_TYPES | {"string", "markup", "dval"} and not value_type.startswith(("array<", "struct:", "tuple<", "function#")):
            raise CapyError(location, f"phase-1 local type {value_type!r} is unsupported")
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
        self.owned_scopes.append([])
        code = bytearray()
        produced_result = False
        for index, expression in enumerate(block.items):
            is_last = index == len(block.items) - 1
            expression_code, result_type = self.compile_expr(expression)
            if managed_type(result_type) and is_last and expected_result == result_type:
                result_local = self.allocate_local(result_type, expression.location)
                code.extend(expression_code + b"\x21" + uleb(result_local))
                if not self.expression_is_owned(expression):
                    code.extend(b"\x20" + uleb(result_local) + b"\x10" + uleb(self.module.retain_index))
                code.extend(self.cleanup_scope())
                code.extend(b"\x20" + uleb(result_local))
                produced_result = True
            else:
                code.extend(expression_code)
                if result_type != "void":
                    if is_last and expected_result != "void":
                        self.require_type(expression.location, expected_result, result_type)
                        produced_result = True
                    else:
                        if managed_type(result_type) and self.expression_is_owned(expression):
                            code.extend(b"\x10" + uleb(self.module.release_index))
                        else:
                            code.append(0x1A)  # drop
        if expected_result != "void" and not produced_result and not self.block_guarantees_return(block):
            raise CapyError(block.location, f"not all paths produce {expected_result}")
        if not (managed_type(expected_result) and block.items):
            code.extend(self.cleanup_scope())
        self.owned_scopes.pop()
        self.scopes.pop()
        return bytes(code)

    def expression_guarantees_return(self, expression: Expr) -> bool:
        if isinstance(expression, Return):
            return True
        if isinstance(expression, Block):
            return self.block_guarantees_return(expression)
        if isinstance(expression, If):
            return bool(
                expression.else_body and
                self.block_guarantees_return(expression.then_body) and
                self.block_guarantees_return(expression.else_body)
            )
        return False

    def block_guarantees_return(self, block: Block) -> bool:
        return any(self.expression_guarantees_return(expression) for expression in block.items)

    def allocate_blob(self, value_type: str, type_id: int, length_local: int, location: Location) -> tuple[bytes, int]:
        pointer = self.allocate_local(value_type, location)
        code = bytearray(b"\x20" + uleb(length_local) + b"\x41\x14\x6a" + self.module.host_call("bearer_alloc") + b"\x21" + uleb(pointer))
        code.extend(b"\x20" + uleb(pointer) + b"\x45\x04\x40" + self.module.source_marker(location) + b"\x00\x0b")
        for value, offset in [(1, 0), (1, 4), (type_id, 8)]:
            code.extend(b"\x20" + uleb(pointer) + b"\x41" + sleb32(value) + b"\x36\x02" + uleb(offset))
        code.extend(b"\x20" + uleb(pointer) + b"\x20" + uleb(length_local) + b"\x41\x14\x6a\x36\x02\x0c")
        code.extend(b"\x20" + uleb(pointer) + b"\x20" + uleb(length_local) + b"\x36\x02\x10")
        code.extend(b"\x23\x01\x41\x01\x6a\x24\x01")
        return bytes(code), pointer

    def cleanup_scope(self) -> bytes:
        code = bytearray()
        for local, value_type in reversed(self.owned_scopes[-1]):
            if managed_type(value_type):
                code.extend(b"\x20" + uleb(local) + b"\x10" + uleb(self.module.release_index))
        return bytes(code)

    def cleanup_all_scopes(self) -> bytes:
        return self.cleanup_scopes_from(0)

    def cleanup_scopes_from(self, boundary: int) -> bytes:
        code = bytearray()
        for scope in reversed(self.owned_scopes[boundary:]):
            for local, value_type in reversed(scope):
                if managed_type(value_type):
                    code.extend(b"\x20" + uleb(local) + b"\x10" + uleb(self.module.release_index))
        return bytes(code)

    def expression_is_owned(self, expression: Expr) -> bool:
        if isinstance(expression, Markup):
            return any(isinstance(part, MarkupField) for part in expression.parts)
        if isinstance(expression, (ArrayLiteral, TupleExpr)):
            return True
        if isinstance(expression, Index):
            if self.infer_expr_type(expression.value) == "dval":
                return True
            return managed_type(self.infer_expr_type(expression)) and self.expression_is_owned(expression.value)
        if isinstance(expression, Member):
            return managed_type(self.infer_expr_type(expression)) and self.expression_is_owned(expression.value)
        if isinstance(expression, Call) and isinstance(expression.function, Name):
            local_function = self.lookup_optional(expression.function.value)
            if local_function and local_function[1].startswith("function#"):
                return managed_type(self.module.function_value_signatures[local_function[1]][1])
            if expression.function.value in self.module.structs:
                return True
            if expression.function.value == "trusted_markup":
                return self.expression_is_owned(expression.arguments[0]) if expression.arguments else False
            if expression.function.value in {"clone", "dval", "dval_string", "unit_call"}:
                return True
            argument_types = tuple(self.infer_expr_type(argument) for argument in expression.arguments)
            target = self.module.functions.get(FunctionKey(expression.function.value, argument_types))
            return bool(target and managed_type(target.result_type))
        return False

    def infer_expr_type(self, expression: Expr) -> str:
        if isinstance(expression, String):
            return "string"
        if isinstance(expression, Markup):
            return "markup"
        if isinstance(expression, Integer):
            return "s32"
        if isinstance(expression, TupleExpr):
            if len(expression.items) < 2:
                raise CapyError(expression.location, "tuple value requires at least two elements")
            return self.module.register_tuple(tuple(self.infer_expr_type(item) for item in expression.items))
        if isinstance(expression, MapLiteral):
            raise CapyError(expression.location, "map literals must be wrapped in dval(...)")
        if isinstance(expression, ArrayLiteral):
            if not expression.items:
                raise CapyError(expression.location, "empty array literal needs an explicit element type")
            element_types = [self.infer_expr_type(item) for item in expression.items]
            if any(item != element_types[0] for item in element_types[1:]):
                raise CapyError(expression.location, "array literal elements must have one type")
            if element_types[0] not in SCALAR_TYPES | {"string"}:
                raise CapyError(expression.location, "array elements must be scalar or string")
            return f"array<{element_types[0]}>"
        if isinstance(expression, Index):
            value_type = self.infer_expr_type(expression.value)
            if value_type == "dval":
                index_type = self.infer_expr_type(expression.index)
                if index_type not in {"string", "s32"}:
                    raise CapyError(expression.index.location, "dval index must be string or s32")
                return "dval"
            if value_type.startswith("array<"):
                return value_type[6:-1]
            if value_type.startswith("tuple<"):
                if not isinstance(expression.index, Integer):
                    raise CapyError(expression.index.location, "tuple index must be a compile-time integer")
                _, element_types = self.module.tuples[value_type]
                if expression.index.value < 0 or expression.index.value >= len(element_types):
                    raise CapyError(expression.index.location, "tuple index is out of bounds")
                return element_types[expression.index.value]
            raise CapyError(expression.location, "indexing requires an array or tuple")
        if isinstance(expression, Member):
            value_type = self.infer_expr_type(expression.value)
            if not value_type.startswith("struct:"):
                raise CapyError(expression.location, "member access requires a struct")
            _, members = self.module.structs[value_type[7:]]
            for member_name, member_type in members:
                if member_name == expression.member:
                    return member_type
            raise CapyError(expression.location, f"struct {value_type[7:]!r} has no member {expression.member!r}")
        if isinstance(expression, Cast):
            source_type = self.infer_expr_type(expression.value)
            target_type = capy_type(expression.target_type, expression.location)
            if source_type not in SCALAR_TYPES or target_type not in SCALAR_TYPES:
                raise CapyError(expression.location, f"no explicit conversion from {source_type} to {target_type}")
            return target_type
        if isinstance(expression, Name):
            if expression.value in {"true", "false"}:
                return "bool"
            local = self.lookup_optional(expression.value)
            if local:
                return local[1]
            return self.module.reference_function(expression.value, expression.location)[0]
        if isinstance(expression, Binary):
            if expression.operator in {"==", "!=", "<", ">", "<=", ">="}:
                return "bool"
            return self.infer_expr_type(expression.right if expression.operator == "=" else expression.left)
        if isinstance(expression, Call) and isinstance(expression.function, Name):
            local_function = self.lookup_optional(expression.function.value)
            if local_function and local_function[1].startswith("function#"):
                parameters, result_type = self.module.function_value_signatures[local_function[1]]
                actual = tuple(self.infer_expr_type(argument) for argument in expression.arguments)
                if actual != parameters:
                    raise CapyError(expression.location, f"function value expects ({', '.join(parameters)}), found ({', '.join(actual)})")
                return result_type
            if expression.function.value in self.module.structs:
                return f"struct:{expression.function.value}"
            if expression.function.value == "trusted_markup":
                if len(expression.arguments) != 1:
                    raise CapyError(expression.location, "trusted_markup expects one string")
                self.require_type(expression.arguments[0].location, "string", self.infer_expr_type(expression.arguments[0]))
                return "markup"
            if expression.function.value == "dval":
                if len(expression.arguments) != 1:
                    raise CapyError(expression.location, "dval expects one scalar, map, or list")
                argument = expression.arguments[0]
                if isinstance(argument, (MapLiteral, ArrayLiteral)):
                    return "dval"
                argument_type = self.infer_expr_type(argument)
                if argument_type not in {"string", "s32", "bool", "dval"}:
                    raise CapyError(argument.location, f"cannot construct dval from {argument_type}")
                return "dval"
            if expression.function.value == "dval_has":
                if len(expression.arguments) != 2:
                    raise CapyError(expression.location, "dval_has expects dval and string/s32 key")
                self.require_type(expression.arguments[0].location, "dval", self.infer_expr_type(expression.arguments[0]))
                if self.infer_expr_type(expression.arguments[1]) not in {"string", "s32"}:
                    raise CapyError(expression.arguments[1].location, "dval key must be string or s32")
                return "bool"
            if expression.function.value in {"dval_s32", "dval_bool"}:
                if len(expression.arguments) != 1:
                    raise CapyError(expression.location, f"{expression.function.value} expects one dval")
                self.require_type(expression.arguments[0].location, "dval", self.infer_expr_type(expression.arguments[0]))
                return "s32" if expression.function.value == "dval_s32" else "bool"
            if expression.function.value == "dval_string":
                if len(expression.arguments) != 1:
                    raise CapyError(expression.location, "dval_string expects one dval")
                self.require_type(expression.arguments[0].location, "dval", self.infer_expr_type(expression.arguments[0]))
                return "string"
            if expression.function.value == "unit_call":
                if len(expression.arguments) != 3:
                    raise CapyError(expression.location, "unit_call expects target, function, and dval")
                for argument, value_type in zip(expression.arguments, ("string", "string", "dval")):
                    self.require_type(argument.location, value_type, self.infer_expr_type(argument))
                return "dval"
            if expression.function.value in {"unit_render", "component_render"}:
                if len(expression.arguments) != 1:
                    raise CapyError(expression.location, f"{expression.function.value} expects one string target")
                self.require_type(expression.arguments[0].location, "string", self.infer_expr_type(expression.arguments[0]))
                return "void"
            if expression.function.value == "print":
                return "void"
            if expression.function.value == "clone":
                return "string"
            if expression.function.value == "arc_live":
                return "s32"
            if expression.function.value == "trap":
                return "void"
            parameters = tuple(self.infer_expr_type(argument) for argument in expression.arguments)
            return self.module.resolve_function(expression.function.value, parameters, expression.location).result_type
        if isinstance(expression, (Variable, Return, Break, Continue, If, While, For, Block)):
            return "void"
        raise CapyError(expression.location, f"cannot infer type of {expression.__class__.__name__}")

    def require_type(self, location: Location, expected: str, actual: str) -> None:
        if expected != actual:
            raise CapyError(location, f"expected {expected}, found {actual}")

    def compile_blob_conversion(self, expression: Call, input_type: str, output_type: str, import_name: str, type_id: int) -> tuple[bytes, str]:
        if len(expression.arguments) != 1:
            raise CapyError(expression.location, f"{expression.function.value} expects one {input_type}")
        source = expression.arguments[0]
        source_code, actual_type = self.compile_expr(source)
        self.require_type(source.location, input_type, actual_type)
        source_local = self.allocate_local(input_type, source.location)
        length_local = self.allocate_local("s32", expression.location)
        code = bytearray(source_code + b"\x21" + uleb(source_local))
        code.extend(b"\x20" + uleb(source_local) + b"\x41\x14\x6a")
        code.extend(b"\x20" + uleb(source_local) + b"\x28\x02\x10\x41\x00\x41\x00")
        code.extend(self.module.host_call(import_name) + b"\x21" + uleb(length_local))
        allocation, pointer = self.allocate_blob(output_type, type_id, length_local, expression.location)
        code.extend(allocation)
        code.extend(b"\x20" + uleb(source_local) + b"\x41\x14\x6a")
        code.extend(b"\x20" + uleb(source_local) + b"\x28\x02\x10")
        code.extend(b"\x20" + uleb(pointer) + b"\x41\x14\x6a\x20" + uleb(length_local))
        code.extend(self.module.host_call(import_name) + b"\x20" + uleb(length_local) + b"\x47\x04\x40\x00\x0b")
        if self.expression_is_owned(source):
            code.extend(b"\x20" + uleb(source_local) + b"\x10" + uleb(self.module.release_index))
        code.extend(b"\x20" + uleb(pointer))
        return bytes(code), output_type

    def compile_scalar_dval(self, source: Expr, value_type: str) -> tuple[bytes, str]:
        value_code, actual_type = self.compile_expr(source)
        self.require_type(source.location, value_type, actual_type)
        value_local = self.allocate_local(value_type, source.location)
        length_local = self.allocate_local("s32", source.location)
        import_name = "bearer_dv_s32_to_brrb" if value_type == "s32" else "bearer_dv_bool_to_brrb"
        code = bytearray(value_code + b"\x21" + uleb(value_local))
        code.extend(b"\x20" + uleb(value_local) + b"\x41\x00\x41\x00" + self.module.host_call(import_name))
        code.extend(b"\x21" + uleb(length_local))
        allocation, pointer = self.allocate_blob("dval", 4, length_local, source.location)
        code.extend(allocation)
        code.extend(b"\x20" + uleb(value_local) + b"\x20" + uleb(pointer) + b"\x41\x14\x6a\x20" + uleb(length_local))
        code.extend(self.module.host_call(import_name) + b"\x20" + uleb(length_local) + b"\x47\x04\x40\x00\x0b")
        code.extend(b"\x20" + uleb(pointer))
        return bytes(code), "dval"

    def compile_dval_value(self, expression: Expr) -> tuple[bytes, str]:
        if isinstance(expression, MapLiteral):
            return self.compile_dval_container(expression.entries, False, expression.location)
        if isinstance(expression, ArrayLiteral):
            return self.compile_dval_container([(str(index), item) for index, item in enumerate(expression.items)], True, expression.location)
        value_type = self.infer_expr_type(expression)
        if value_type == "dval":
            return self.compile_expr(expression)
        if value_type == "string":
            synthetic = Call(expression.location, Name(expression.location, "dval"), [expression])
            return self.compile_blob_conversion(synthetic, "string", "dval", "bearer_dv_string_to_brrb", 4)
        if value_type in {"s32", "bool"}:
            return self.compile_scalar_dval(expression, value_type)
        raise CapyError(expression.location, f"cannot construct dval from {value_type}")

    def compile_dval_container(self, entries: list[tuple[str, Expr]], list_mode: bool, location: Location) -> tuple[bytes, str]:
        if not list_mode:
            keys = [key for key, _ in entries]
            if len(set(keys)) != len(keys):
                raise CapyError(location, "dval map literal contains a duplicate key")
        code = bytearray()
        values: list[tuple[int, bool]] = []
        for _, expression in entries:
            source_is_dval = not isinstance(expression, (MapLiteral, ArrayLiteral)) and self.infer_expr_type(expression) == "dval"
            value_code, value_type = self.compile_dval_value(expression)
            self.require_type(expression.location, "dval", value_type)
            local = self.allocate_local("dval", expression.location)
            code.extend(value_code + b"\x21" + uleb(local))
            values.append((local, self.expression_is_owned(expression) if source_is_dval else True))
        descriptor = self.allocate_local("s32", location)
        descriptor_size = len(entries) * 16
        if descriptor_size:
            code.extend(b"\x41" + sleb32(descriptor_size) + self.module.host_call("bearer_alloc") + b"\x21" + uleb(descriptor))
            code.extend(b"\x20" + uleb(descriptor) + b"\x45\x04\x40" + self.module.source_marker(location) + b"\x00\x0b")
        else:
            code.extend(b"\x41\x00\x21" + uleb(descriptor))
        for index, ((key, _), (value_local, _)) in enumerate(zip(entries, values)):
            key_bytes = key.encode("utf-8")
            key_offset = self.module.add_static_bytes(key_bytes)
            base = index * 16
            code.extend(b"\x20" + uleb(descriptor) + b"\x23\x00\x41" + sleb32(key_offset) + b"\x6a\x36\x02" + uleb(base))
            code.extend(b"\x20" + uleb(descriptor) + b"\x41" + sleb32(len(key_bytes)) + b"\x36\x02" + uleb(base + 4))
            code.extend(b"\x20" + uleb(descriptor) + b"\x20" + uleb(value_local) + b"\x41\x14\x6a\x36\x02" + uleb(base + 8))
            code.extend(b"\x20" + uleb(descriptor) + b"\x20" + uleb(value_local) + b"\x28\x02\x10\x36\x02" + uleb(base + 12))
        length = self.allocate_local("s32", location)
        code.extend(b"\x41" + sleb32(1 if list_mode else 0) + b"\x20" + uleb(descriptor) + b"\x41" + sleb32(len(entries)) + b"\x41\x00\x41\x00")
        code.extend(self.module.host_call("bearer_dv_build_brrb") + b"\x21" + uleb(length))
        allocation, pointer = self.allocate_blob("dval", 4, length, location)
        code.extend(allocation)
        code.extend(b"\x41" + sleb32(1 if list_mode else 0) + b"\x20" + uleb(descriptor) + b"\x41" + sleb32(len(entries)))
        code.extend(b"\x20" + uleb(pointer) + b"\x41\x14\x6a\x20" + uleb(length) + self.module.host_call("bearer_dv_build_brrb"))
        code.extend(b"\x20" + uleb(length) + b"\x47\x04\x40\x00\x0b")
        if descriptor_size:
            code.extend(b"\x20" + uleb(descriptor) + self.module.host_call("bearer_free"))
        for value_local, owned in reversed(values):
            if owned:
                code.extend(b"\x20" + uleb(value_local) + b"\x10" + uleb(self.module.release_index))
        code.extend(b"\x20" + uleb(pointer))
        return bytes(code), "dval"

    def compile_dval_lookup(self, value: Expr, key: Expr, require_present: bool) -> tuple[bytes, str]:
        value_code, value_type = self.compile_expr(value)
        self.require_type(value.location, "dval", value_type)
        key_code, key_type = self.compile_expr(key)
        if key_type not in {"string", "s32"}:
            raise CapyError(key.location, "dval index must be string or s32")
        value_local = self.allocate_local("dval", value.location)
        key_local = self.allocate_local(key_type, key.location)
        length = self.allocate_local("s32", key.location)
        code = bytearray(value_code + b"\x21" + uleb(value_local) + key_code + b"\x21" + uleb(key_local))

        def append_call(out: bytes, cap: bytes) -> None:
            code.extend(b"\x20" + uleb(value_local) + b"\x41\x14\x6a\x20" + uleb(value_local) + b"\x28\x02\x10")
            if key_type == "string":
                code.extend(b"\x41\x00\x20" + uleb(key_local) + b"\x41\x14\x6a\x20" + uleb(key_local) + b"\x28\x02\x10\x41\x00")
            else:
                code.extend(b"\x41\x01\x41\x00\x41\x00\x20" + uleb(key_local))
            code.extend(out + cap + self.module.host_call("bearer_dv_get_brrb"))

        append_call(b"\x41\x00", b"\x41\x00")
        code.extend(b"\x21" + uleb(length))
        if not require_present:
            code.extend(b"\x20" + uleb(length) + b"\x41\x00\x4e")
            result = self.allocate_local("bool", key.location)
            code.extend(b"\x21" + uleb(result))
            if self.expression_is_owned(key):
                code.extend(b"\x20" + uleb(key_local) + b"\x10" + uleb(self.module.release_index))
            if self.expression_is_owned(value):
                code.extend(b"\x20" + uleb(value_local) + b"\x10" + uleb(self.module.release_index))
            code.extend(b"\x20" + uleb(result))
            return bytes(code), "bool"
        code.extend(b"\x20" + uleb(length) + b"\x41\x00\x48\x04\x40" + self.module.source_marker(key.location) + b"\x00\x0b")
        allocation, pointer = self.allocate_blob("dval", 4, length, key.location)
        code.extend(allocation)
        append_call(b"\x20" + uleb(pointer) + b"\x41\x14\x6a", b"\x20" + uleb(length))
        code.extend(b"\x20" + uleb(length) + b"\x47\x04\x40\x00\x0b")
        if self.expression_is_owned(key):
            code.extend(b"\x20" + uleb(key_local) + b"\x10" + uleb(self.module.release_index))
        if self.expression_is_owned(value):
            code.extend(b"\x20" + uleb(value_local) + b"\x10" + uleb(self.module.release_index))
        code.extend(b"\x20" + uleb(pointer))
        return bytes(code), "dval"

    def compile_dval_scalar(self, expression: Call, result_type: str) -> tuple[bytes, str]:
        if len(expression.arguments) != 1:
            raise CapyError(expression.location, f"{expression.function.value} expects one dval")
        source = expression.arguments[0]
        source_code, source_type = self.compile_expr(source)
        self.require_type(source.location, "dval", source_type)
        source_local = self.allocate_local("dval", source.location)
        code = bytearray(source_code + b"\x21" + uleb(source_local))
        if result_type == "string":
            code.extend(b"\x20" + uleb(source_local) + b"\x41\x14\x6a\x20" + uleb(source_local) + b"\x28\x02\x10")
            code.extend(self.module.host_call("bearer_dv_scalar_type_brrb") + b"\x41" + sleb32(ord("S")) + b"\x47\x04\x40" + self.module.source_marker(expression.location) + b"\x00\x0b")
            length = self.allocate_local("s32", expression.location)
            code.extend(b"\x20" + uleb(source_local) + b"\x41\x14\x6a\x20" + uleb(source_local) + b"\x28\x02\x10\x41\x00\x41\x00")
            code.extend(self.module.host_call("bearer_dv_brrb_to_string") + b"\x21" + uleb(length))
            allocation, pointer = self.allocate_blob("string", 1, length, expression.location)
            code.extend(allocation)
            code.extend(b"\x20" + uleb(source_local) + b"\x41\x14\x6a\x20" + uleb(source_local) + b"\x28\x02\x10")
            code.extend(b"\x20" + uleb(pointer) + b"\x41\x14\x6a\x20" + uleb(length) + self.module.host_call("bearer_dv_brrb_to_string"))
            code.extend(b"\x20" + uleb(length) + b"\x47\x04\x40\x00\x0b")
            result_local = pointer
        else:
            result_local = self.allocate_local(result_type, expression.location)
            result_pointer = self.allocate_local("s32", expression.location)
            import_name = "bearer_dv_s32_brrb" if result_type == "s32" else "bearer_dv_bool_brrb"
            code.extend(b"\x41\x04" + self.module.host_call("bearer_alloc") + b"\x21" + uleb(result_pointer))
            code.extend(b"\x20" + uleb(result_pointer) + b"\x45\x04\x40" + self.module.source_marker(expression.location) + b"\x00\x0b")
            code.extend(b"\x20" + uleb(source_local) + b"\x41\x14\x6a\x20" + uleb(source_local) + b"\x28\x02\x10\x20" + uleb(result_pointer))
            code.extend(self.module.host_call(import_name) + b"\x45\x04\x40" + self.module.source_marker(expression.location) + b"\x00\x0b")
            code.extend(b"\x20" + uleb(result_pointer) + b"\x28\x02\x00\x21" + uleb(result_local))
            code.extend(b"\x20" + uleb(result_pointer) + self.module.host_call("bearer_free"))
        if self.expression_is_owned(source):
            code.extend(b"\x20" + uleb(source_local) + b"\x10" + uleb(self.module.release_index))
        code.extend(b"\x20" + uleb(result_local))
        return bytes(code), result_type

    def markup_escape_length(self, source: int, total: int, location: Location) -> bytes:
        index = self.allocate_local("s32", location)
        length = self.allocate_local("s32", location)
        byte = self.allocate_local("s32", location)
        code = bytearray(b"\x20" + uleb(source) + b"\x28\x02\x10\x21" + uleb(length))
        code.extend(b"\x41\x00\x21" + uleb(index) + b"\x02\x40\x03\x40")
        code.extend(b"\x20" + uleb(index) + b"\x20" + uleb(length) + b"\x4f\x0d\x01")
        code.extend(b"\x20" + uleb(source) + b"\x41\x14\x6a\x20" + uleb(index) + b"\x6a\x2d\x00\x00\x21" + uleb(byte))
        code.extend(b"\x20" + uleb(total) + b"\x41\x01\x6a\x21" + uleb(total))
        for character, extra in ((38, 4), (60, 3), (62, 3), (34, 5), (39, 4)):
            code.extend(b"\x20" + uleb(byte) + b"\x41" + sleb32(character) + b"\x46\x04\x40")
            code.extend(b"\x20" + uleb(total) + b"\x41" + sleb32(extra) + b"\x6a\x21" + uleb(total) + b"\x0b")
        code.extend(b"\x20" + uleb(index) + b"\x41\x01\x6a\x21" + uleb(index) + b"\x0c\x00\x0b\x0b")
        return bytes(code)

    def markup_write_bytes(self, cursor: int, value: bytes) -> bytes:
        code = bytearray()
        for byte in value:
            code.extend(b"\x20" + uleb(cursor) + b"\x41" + sleb32(byte) + b"\x3a\x00\x00")
            code.extend(b"\x20" + uleb(cursor) + b"\x41\x01\x6a\x21" + uleb(cursor))
        return bytes(code)

    def markup_escape_write(self, source: int, cursor: int, location: Location) -> bytes:
        index = self.allocate_local("s32", location)
        length = self.allocate_local("s32", location)
        byte = self.allocate_local("s32", location)
        code = bytearray(b"\x20" + uleb(source) + b"\x28\x02\x10\x21" + uleb(length))
        code.extend(b"\x41\x00\x21" + uleb(index) + b"\x02\x40\x03\x40")
        code.extend(b"\x20" + uleb(index) + b"\x20" + uleb(length) + b"\x4f\x0d\x01")
        code.extend(b"\x20" + uleb(source) + b"\x41\x14\x6a\x20" + uleb(index) + b"\x6a\x2d\x00\x00\x21" + uleb(byte))
        code.extend(b"\x02\x40")
        for character, escaped in ((38, b"&amp;"), (60, b"&lt;"), (62, b"&gt;"), (34, b"&quot;"), (39, b"&#39;")):
            code.extend(b"\x20" + uleb(byte) + b"\x41" + sleb32(character) + b"\x46\x04\x40")
            code.extend(self.markup_write_bytes(cursor, escaped) + b"\x0c\x01\x0b")
        code.extend(b"\x20" + uleb(cursor) + b"\x20" + uleb(byte) + b"\x3a\x00\x00")
        code.extend(b"\x20" + uleb(cursor) + b"\x41\x01\x6a\x21" + uleb(cursor) + b"\x0b")
        code.extend(b"\x20" + uleb(index) + b"\x41\x01\x6a\x21" + uleb(index) + b"\x0c\x00\x0b\x0b")
        return bytes(code)

    def markup_s32_length(self, source: int, total: int, location: Location) -> bytes:
        value = self.allocate_local("s32", location)
        digits = self.allocate_local("s32", location)
        code = bytearray(b"\x20" + uleb(source) + b"\x21" + uleb(value))
        code.extend(b"\x20" + uleb(source) + b"\x41\x00\x48\x04\x40")
        code.extend(b"\x20" + uleb(total) + b"\x41\x01\x6a\x21" + uleb(total) + b"\x0b")
        code.extend(b"\x41\x00\x21" + uleb(digits) + b"\x02\x40\x03\x40")
        code.extend(b"\x20" + uleb(digits) + b"\x41\x01\x6a\x21" + uleb(digits))
        code.extend(b"\x20" + uleb(value) + b"\x41\x0a\x6d\x22" + uleb(value) + b"\x0d\x00\x0b\x0b")
        code.extend(b"\x20" + uleb(total) + b"\x20" + uleb(digits) + b"\x6a\x21" + uleb(total))
        return bytes(code)

    def markup_s32_write(self, source: int, cursor: int, location: Location) -> bytes:
        value = self.allocate_local("s32", location)
        divisor = self.allocate_local("s32", location)
        digit = self.allocate_local("s32", location)
        code = bytearray(b"\x20" + uleb(source) + b"\x21" + uleb(value))
        code.extend(b"\x20" + uleb(value) + b"\x41\x00\x48\x04\x40")
        code.extend(self.markup_write_bytes(cursor, b"-") + b"\x05")
        code.extend(b"\x41\x00\x20" + uleb(value) + b"\x6b\x21" + uleb(value) + b"\x0b")
        code.extend(b"\x41\x01\x21" + uleb(divisor) + b"\x02\x40\x03\x40")
        code.extend(b"\x20" + uleb(value) + b"\x20" + uleb(divisor) + b"\x6d\x41\x76\x4c\x45\x0d\x01")
        code.extend(b"\x20" + uleb(divisor) + b"\x41\x0a\x6c\x21" + uleb(divisor) + b"\x0c\x00\x0b\x0b")
        code.extend(b"\x02\x40\x03\x40")
        code.extend(b"\x41\x00\x20" + uleb(value) + b"\x20" + uleb(divisor) + b"\x6d\x6b\x21" + uleb(digit))
        code.extend(b"\x20" + uleb(cursor) + b"\x20" + uleb(digit) + b"\x41\x30\x6a\x3a\x00\x00")
        code.extend(b"\x20" + uleb(cursor) + b"\x41\x01\x6a\x21" + uleb(cursor))
        code.extend(b"\x20" + uleb(value) + b"\x20" + uleb(divisor) + b"\x6f\x21" + uleb(value))
        code.extend(b"\x20" + uleb(divisor) + b"\x41\x0a\x6d\x22" + uleb(divisor) + b"\x0d\x00\x0b\x0b")
        return bytes(code)

    def compile_markup(self, expression: Markup) -> tuple[bytes, str]:
        fields = [part for part in expression.parts if isinstance(part, MarkupField)]
        if not fields:
            value = "".join(part.value for part in expression.parts if isinstance(part, MarkupText)).encode("utf-8")
            offset = self.module.add_static_string(value)
            return b"\x23\x00\x41" + sleb32(offset) + b"\x6a", "markup"

        code = bytearray()
        compiled_fields: dict[int, tuple[int, str, bool]] = {}
        for field in fields:
            value_code, value_type = self.compile_expr(field.value)
            if value_type not in {"string", "markup", "s32", "bool"}:
                raise CapyError(field.location, f"markup interpolation does not support {value_type}")
            if not field.escaped and value_type != "markup":
                raise CapyError(field.location, "raw markup interpolation requires a markup value")
            local = self.allocate_local(value_type, field.location)
            code.extend(value_code + b"\x21" + uleb(local))
            compiled_fields[id(field)] = (local, value_type, self.expression_is_owned(field.value))

        total = self.allocate_local("s32", expression.location)
        static_length = sum(len(part.value.encode("utf-8")) for part in expression.parts if isinstance(part, MarkupText))
        code.extend(b"\x41" + sleb32(static_length) + b"\x21" + uleb(total))
        for field in fields:
            local, value_type, _ = compiled_fields[id(field)]
            if field.escaped and value_type == "string":
                code.extend(self.markup_escape_length(local, total, field.location))
            elif value_type == "s32":
                code.extend(self.markup_s32_length(local, total, field.location))
            elif value_type == "bool":
                code.extend(b"\x20" + uleb(total) + b"\x41\x04\x6a\x21" + uleb(total))
                code.extend(b"\x20" + uleb(local) + b"\x45\x04\x40\x20" + uleb(total) + b"\x41\x01\x6a\x21" + uleb(total) + b"\x0b")
            else:
                code.extend(b"\x20" + uleb(total) + b"\x20" + uleb(local) + b"\x28\x02\x10\x6a\x21" + uleb(total))

        allocation, pointer = self.allocate_blob("markup", 1, total, expression.location)
        code.extend(allocation)
        cursor = self.allocate_local("s32", expression.location)
        code.extend(b"\x20" + uleb(pointer) + b"\x41\x14\x6a\x21" + uleb(cursor))
        for part in expression.parts:
            if isinstance(part, MarkupText):
                value = part.value.encode("utf-8")
                if value:
                    offset = self.module.add_static_bytes(value)
                    code.extend(b"\x20" + uleb(cursor) + b"\x23\x00\x41" + sleb32(offset) + b"\x6a\x41" + sleb32(len(value)) + b"\xfc\x0a\x00\x00")
                    code.extend(b"\x20" + uleb(cursor) + b"\x41" + sleb32(len(value)) + b"\x6a\x21" + uleb(cursor))
                continue
            local, value_type, owned = compiled_fields[id(part)]
            if part.escaped and value_type == "string":
                code.extend(self.markup_escape_write(local, cursor, part.location))
            elif value_type == "s32":
                code.extend(self.markup_s32_write(local, cursor, part.location))
            elif value_type == "bool":
                code.extend(b"\x20" + uleb(local) + b"\x04\x40")
                code.extend(self.markup_write_bytes(cursor, b"true") + b"\x05")
                code.extend(self.markup_write_bytes(cursor, b"false") + b"\x0b")
            else:
                code.extend(b"\x20" + uleb(cursor) + b"\x20" + uleb(local) + b"\x41\x14\x6a\x20" + uleb(local) + b"\x28\x02\x10\xfc\x0a\x00\x00")
                code.extend(b"\x20" + uleb(cursor) + b"\x20" + uleb(local) + b"\x28\x02\x10\x6a\x21" + uleb(cursor))
            if owned:
                code.extend(b"\x20" + uleb(local) + b"\x10" + uleb(self.module.release_index))
        code.extend(b"\x20" + uleb(pointer))
        return bytes(code), "markup"

    def compile_expr(self, expression: Expr) -> tuple[bytes, str]:
        if isinstance(expression, Integer):
            return b"\x41" + sleb32(expression.value), "s32"
        if isinstance(expression, String):
            offset = self.module.add_static_string(expression.value.encode("utf-8"))
            return b"\x23\x00\x41" + sleb32(offset) + b"\x6a", "string"
        if isinstance(expression, Markup):
            return self.compile_markup(expression)
        if isinstance(expression, TupleExpr):
            value_type = self.infer_expr_type(expression)
            type_id, element_types = self.module.tuples[value_type]
            size = 16 + 4 * len(expression.items)
            pointer = self.allocate_local(value_type, expression.location)
            code = bytearray(b"\x41" + sleb32(size) + self.module.host_call("bearer_alloc") + b"\x21" + uleb(pointer))
            code.extend(b"\x20" + uleb(pointer) + b"\x45\x04\x40" + self.module.source_marker(expression.location) + b"\x00\x0b")
            for value, offset in [(1, 0), (1, 4), (type_id, 8), (size, 12)]:
                code.extend(b"\x20" + uleb(pointer) + b"\x41" + sleb32(value) + b"\x36\x02" + uleb(offset))
            code.extend(b"\x23\x01\x41\x01\x6a\x24\x01")
            for index, (item, item_type) in enumerate(zip(expression.items, element_types)):
                item_code, actual_type = self.compile_expr(item)
                self.require_type(item.location, item_type, actual_type)
                temporary = self.allocate_local(item_type, item.location)
                code.extend(item_code + b"\x21" + uleb(temporary))
                if managed_type(item_type) and not self.expression_is_owned(item):
                    code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.retain_index))
                code.extend(b"\x20" + uleb(pointer) + b"\x20" + uleb(temporary))
                code.extend(b"\x36\x02" + uleb(16 + 4 * index))
            code.extend(b"\x20" + uleb(pointer))
            return bytes(code), value_type
        if isinstance(expression, ArrayLiteral):
            value_type = self.infer_expr_type(expression)
            size = 20 + 4 * len(expression.items)
            pointer = self.allocate_local(value_type, expression.location)
            code = bytearray(b"\x41" + sleb32(size) + self.module.host_call("bearer_alloc") + b"\x21" + uleb(pointer))
            code.extend(b"\x20" + uleb(pointer) + b"\x45\x04\x40" + self.module.source_marker(expression.location) + b"\x00\x0b")
            element_type = value_type[6:-1]
            type_id = 3 if element_type == "string" else 2
            for value, offset in [(1, 0), (1, 4), (type_id, 8), (size, 12), (len(expression.items), 16)]:
                code.extend(b"\x20" + uleb(pointer) + b"\x41" + sleb32(value))
                code.extend(b"\x36\x02" + uleb(offset))
            code.extend(b"\x23\x01\x41\x01\x6a\x24\x01")
            for index, item in enumerate(expression.items):
                item_code, item_type = self.compile_expr(item)
                self.require_type(item.location, element_type, item_type)
                item_local = self.allocate_local(item_type, item.location)
                code.extend(item_code + b"\x21" + uleb(item_local))
                if managed_type(item_type) and not self.expression_is_owned(item):
                    code.extend(b"\x20" + uleb(item_local) + b"\x10" + uleb(self.module.retain_index))
                code.extend(b"\x20" + uleb(pointer) + b"\x20" + uleb(item_local))
                code.extend(b"\x36\x02" + uleb(20 + 4 * index))
            code.extend(b"\x20" + uleb(pointer))
            return bytes(code), value_type
        if isinstance(expression, Index):
            if self.infer_expr_type(expression.value) == "dval":
                return self.compile_dval_lookup(expression.value, expression.index, True)
            value_code, value_type = self.compile_expr(expression.value)
            if value_type.startswith("tuple<"):
                if not isinstance(expression.index, Integer):
                    raise CapyError(expression.index.location, "tuple index must be a compile-time integer")
                _, element_types = self.module.tuples[value_type]
                index = expression.index.value
                if index < 0 or index >= len(element_types):
                    raise CapyError(expression.index.location, "tuple index is out of bounds")
                result_type = element_types[index]
                object_local = self.allocate_local(value_type, expression.value.location)
                result_local = self.allocate_local(result_type, expression.location)
                code = bytearray(value_code + b"\x21" + uleb(object_local))
                code.extend(b"\x20" + uleb(object_local) + b"\x28\x02" + uleb(16 + 4 * index))
                if self.expression_is_owned(expression.value):
                    code.extend(b"\x21" + uleb(result_local))
                    if managed_type(result_type):
                        code.extend(b"\x20" + uleb(result_local) + b"\x10" + uleb(self.module.retain_index))
                    code.extend(b"\x20" + uleb(object_local) + b"\x10" + uleb(self.module.release_index))
                    code.extend(b"\x20" + uleb(result_local))
                return bytes(code), result_type
            if not value_type.startswith("array<"):
                raise CapyError(expression.location, "indexing requires an array or tuple")
            index_code, index_type = self.compile_expr(expression.index)
            self.require_type(expression.index.location, "s32", index_type)
            array_local = self.allocate_local(value_type, expression.value.location)
            index_local = self.allocate_local("s32", expression.index.location)
            code = bytearray(value_code + b"\x21" + uleb(array_local))
            code.extend(index_code + b"\x21" + uleb(index_local))
            code.extend(b"\x20" + uleb(index_local) + b"\x41\x00\x48\x04\x40" + self.module.source_marker(expression.location) + b"\x00\x0b")
            code.extend(b"\x20" + uleb(index_local) + b"\x20" + uleb(array_local))
            code.extend(b"\x28\x02\x10\x4f\x04\x40" + self.module.source_marker(expression.location) + b"\x00\x0b")
            code.extend(b"\x20" + uleb(array_local) + b"\x20" + uleb(index_local))
            code.extend(b"\x41\x04\x6c\x6a\x28\x02\x14")
            if self.expression_is_owned(expression.value):
                result_type = value_type[6:-1]
                result_local = self.allocate_local(result_type, expression.location)
                code.extend(b"\x21" + uleb(result_local))
                if managed_type(result_type):
                    code.extend(b"\x20" + uleb(result_local) + b"\x10" + uleb(self.module.retain_index))
                code.extend(b"\x20" + uleb(array_local) + b"\x10" + uleb(self.module.release_index))
                code.extend(b"\x20" + uleb(result_local))
            return bytes(code), value_type[6:-1]
        if isinstance(expression, Member):
            value_code, value_type = self.compile_expr(expression.value)
            if not value_type.startswith("struct:"):
                raise CapyError(expression.location, "member access requires a struct")
            _, members = self.module.structs[value_type[7:]]
            try:
                member_index, (_, member_type) = next(
                    (index, member) for index, member in enumerate(members) if member[0] == expression.member
                )
            except StopIteration:
                raise CapyError(expression.location, f"struct {value_type[7:]!r} has no member {expression.member!r}")
            object_local = self.allocate_local(value_type, expression.value.location)
            code = bytearray(value_code + b"\x21" + uleb(object_local))
            code.extend(b"\x20" + uleb(object_local) + b"\x28\x02" + uleb(16 + 4 * member_index))
            if self.expression_is_owned(expression.value):
                result_local = self.allocate_local(member_type, expression.location)
                code.extend(b"\x21" + uleb(result_local))
                if managed_type(member_type):
                    code.extend(b"\x20" + uleb(result_local) + b"\x10" + uleb(self.module.retain_index))
                code.extend(b"\x20" + uleb(object_local) + b"\x10" + uleb(self.module.release_index))
                code.extend(b"\x20" + uleb(result_local))
            return bytes(code), member_type
        if isinstance(expression, Cast):
            value_code, source_type = self.compile_expr(expression.value)
            target_type = capy_type(expression.target_type, expression.location)
            if source_type not in SCALAR_TYPES or target_type not in SCALAR_TYPES:
                raise CapyError(expression.location, f"no explicit conversion from {source_type} to {target_type}")
            if target_type == "bool" and source_type != "bool":
                value_code += b"\x45\x45"
            return value_code, target_type
        if isinstance(expression, Name):
            if expression.value in {"true", "false"}:
                return b"\x41" + sleb32(1 if expression.value == "true" else 0), "bool"
            local = self.lookup_optional(expression.value)
            if local:
                index, value_type = local
                return b"\x20" + uleb(index), value_type
            value_type, table_slot = self.module.reference_function(expression.value, expression.location)
            return b"\x41" + sleb32(table_slot), value_type
        if isinstance(expression, Variable):
            value_code, value_type = self.compile_expr(expression.value)
            declared = capy_type(expression.annotation, expression.location) if expression.annotation else value_type
            self.require_type(expression.location, declared, value_type)
            local = self.add_local(expression.name, declared, expression.location)
            code = bytearray(value_code + b"\x21" + uleb(local))
            if managed_type(declared):
                if not self.expression_is_owned(expression.value):
                    code.extend(b"\x20" + uleb(local) + b"\x10" + uleb(self.module.retain_index))
                self.owned_scopes[-1].append((local, declared))
            return bytes(code), "void"
        if isinstance(expression, Binary):
            if expression.operator == "=":
                if not isinstance(expression.left, Name):
                    raise CapyError(expression.left.location, "assignment target must be a local name")
                local, target_type = self.lookup(expression.left)
                value_code, value_type = self.compile_expr(expression.right)
                self.require_type(expression.location, target_type, value_type)
                if managed_type(target_type):
                    if local in self.borrowed_managed_slots:
                        raise CapyError(expression.left.location, "cannot assign to a borrowed managed value; copy it into a local first")
                    temporary = self.allocate_local(target_type, expression.location)
                    code = bytearray(value_code + b"\x21" + uleb(temporary))
                    if not self.expression_is_owned(expression.right):
                        code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.retain_index))
                    code.extend(b"\x20" + uleb(local) + b"\x10" + uleb(self.module.release_index))
                    code.extend(b"\x20" + uleb(temporary) + b"\x22" + uleb(local))
                    return bytes(code), target_type
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
            local_function = self.lookup_optional(expression.function.value)
            if local_function and local_function[1].startswith("function#"):
                parameters, result_type = self.module.function_value_signatures[local_function[1]]
                arguments = [self.compile_expr(argument) for argument in expression.arguments]
                actual = tuple(value_type for _, value_type in arguments)
                if actual != parameters:
                    raise CapyError(expression.location, f"function value expects ({', '.join(parameters)}), found ({', '.join(actual)})")
                code = bytearray()
                owned_argument_locals: list[int] = []
                for source_expression, (argument_code, argument_type) in zip(expression.arguments, arguments):
                    if managed_type(argument_type) and self.expression_is_owned(source_expression):
                        temporary = self.allocate_local(argument_type, source_expression.location)
                        code.extend(argument_code + b"\x21" + uleb(temporary) + b"\x20" + uleb(temporary))
                        owned_argument_locals.append(temporary)
                    else:
                        code.extend(argument_code)
                code.extend(b"\x20" + uleb(local_function[0]))
                type_index = int(local_function[1][9:])
                code.extend(b"\x11" + uleb(type_index) + b"\x00")
                for temporary in reversed(owned_argument_locals):
                    code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.release_index))
                return bytes(code), result_type
            if expression.function.value in self.module.structs:
                type_id, members = self.module.structs[expression.function.value]
                if len(expression.arguments) != len(members):
                    raise CapyError(expression.location, f"struct {expression.function.value} expects {len(members)} fields")
                value_type = f"struct:{expression.function.value}"
                size = 16 + 4 * len(members)
                pointer = self.allocate_local(value_type, expression.location)
                code = bytearray(b"\x41" + sleb32(size) + self.module.host_call("bearer_alloc") + b"\x21" + uleb(pointer))
                code.extend(b"\x20" + uleb(pointer) + b"\x45\x04\x40" + self.module.source_marker(expression.location) + b"\x00\x0b")
                for value, offset in [(1, 0), (1, 4), (type_id, 8), (size, 12)]:
                    code.extend(b"\x20" + uleb(pointer) + b"\x41" + sleb32(value) + b"\x36\x02" + uleb(offset))
                code.extend(b"\x23\x01\x41\x01\x6a\x24\x01")
                for index, (argument, (_, member_type)) in enumerate(zip(expression.arguments, members)):
                    argument_code, argument_type = self.compile_expr(argument)
                    self.require_type(argument.location, member_type, argument_type)
                    temporary = self.allocate_local(argument_type, argument.location)
                    code.extend(argument_code + b"\x21" + uleb(temporary))
                    if managed_type(argument_type) and not self.expression_is_owned(argument):
                        code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.retain_index))
                    code.extend(b"\x20" + uleb(pointer) + b"\x20" + uleb(temporary))
                    code.extend(b"\x36\x02" + uleb(16 + 4 * index))
                code.extend(b"\x20" + uleb(pointer))
                return bytes(code), value_type
            if expression.function.value == "trusted_markup":
                if len(expression.arguments) != 1:
                    raise CapyError(expression.location, "trusted_markup expects one string")
                value_code, value_type = self.compile_expr(expression.arguments[0])
                self.require_type(expression.arguments[0].location, "string", value_type)
                return value_code, "markup"
            if expression.function.value == "dval":
                if len(expression.arguments) != 1:
                    raise CapyError(expression.location, "dval expects one scalar, map, or list")
                return self.compile_dval_value(expression.arguments[0])
            if expression.function.value == "dval_has":
                if len(expression.arguments) != 2:
                    raise CapyError(expression.location, "dval_has expects dval and string/s32 key")
                return self.compile_dval_lookup(expression.arguments[0], expression.arguments[1], False)
            if expression.function.value == "dval_string":
                return self.compile_dval_scalar(expression, "string")
            if expression.function.value == "dval_s32":
                return self.compile_dval_scalar(expression, "s32")
            if expression.function.value == "dval_bool":
                return self.compile_dval_scalar(expression, "bool")
            if expression.function.value == "unit_call":
                if len(expression.arguments) != 3:
                    raise CapyError(expression.location, "unit_call expects target, function, and dval")
                locals_and_sources: list[tuple[int, Expr]] = []
                code = bytearray()
                for argument, expected_type in zip(expression.arguments, ("string", "string", "dval")):
                    argument_code, argument_type = self.compile_expr(argument)
                    self.require_type(argument.location, expected_type, argument_type)
                    temporary = self.allocate_local(expected_type, argument.location)
                    code.extend(argument_code + b"\x21" + uleb(temporary))
                    locals_and_sources.append((temporary, argument))
                def append_inputs() -> None:
                    for temporary, _ in locals_and_sources:
                        code.extend(b"\x20" + uleb(temporary) + b"\x41\x14\x6a")
                        code.extend(b"\x20" + uleb(temporary) + b"\x28\x02\x10")
                append_inputs()
                code.extend(b"\x41\x00\x41\x00" + self.module.host_call("bearer_unit_call_brrb"))
                length_local = self.allocate_local("s32", expression.location)
                code.extend(b"\x21" + uleb(length_local))
                allocation, pointer = self.allocate_blob("dval", 4, length_local, expression.location)
                code.extend(allocation)
                append_inputs()
                code.extend(b"\x20" + uleb(pointer) + b"\x41\x14\x6a\x20" + uleb(length_local) + self.module.host_call("bearer_unit_call_brrb"))
                code.extend(b"\x20" + uleb(length_local) + b"\x47\x04\x40\x00\x0b")
                for temporary, source in reversed(locals_and_sources):
                    if self.expression_is_owned(source):
                        code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.release_index))
                code.extend(b"\x20" + uleb(pointer))
                return bytes(code), "dval"
            if expression.function.value in {"unit_render", "component_render"}:
                if len(expression.arguments) != 1:
                    raise CapyError(expression.location, f"{expression.function.value} expects one string target")
                target_expression = expression.arguments[0]
                target_code, target_type = self.compile_expr(target_expression)
                self.require_type(target_expression.location, "string", target_type)
                temporary = self.allocate_local("string", target_expression.location)
                code = bytearray(target_code + b"\x21" + uleb(temporary))
                code.extend(b"\x20" + uleb(temporary) + b"\x41\x14\x6a")
                code.extend(b"\x20" + uleb(temporary) + b"\x28\x02\x10")
                import_name = "bearer_unit_render_bytes" if expression.function.value == "unit_render" else "bearer_component_render_bytes"
                code.extend(self.module.host_call(import_name))
                if self.expression_is_owned(target_expression):
                    code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.release_index))
                return bytes(code), "void"
            if expression.function.value == "print":
                code = bytearray()
                for argument in expression.arguments:
                    if isinstance(argument, Markup) and not any(isinstance(part, MarkupField) for part in argument.parts):
                        value = "".join(part.value for part in argument.parts if isinstance(part, MarkupText)).encode("utf-8")
                        offset = self.module.add_static_bytes(value)
                        code.extend(b"\x23\x00\x41" + sleb32(offset) + b"\x6a\x41" + sleb32(len(value)))
                        code.extend(self.module.host_call("bearer_print_bytes"))
                        continue
                    if isinstance(argument, String):
                        value = argument.value.encode("utf-8")
                        offset = self.module.add_static_bytes(value)
                        code.extend(b"\x23\x00\x41" + sleb32(offset) + b"\x6a\x41" + sleb32(len(value)))
                        code.extend(self.module.host_call("bearer_print_bytes"))
                        continue
                    argument_code, argument_type = self.compile_expr(argument)
                    if argument_type in {"string", "markup"}:
                        temporary = self.allocate_local(argument_type, argument.location)
                        code.extend(argument_code + b"\x21" + uleb(temporary))
                        code.extend(b"\x20" + uleb(temporary) + b"\x41\x14\x6a")
                        code.extend(b"\x20" + uleb(temporary) + b"\x28\x02\x10")
                        code.extend(self.module.host_call("bearer_print_bytes"))
                        if self.expression_is_owned(argument):
                            code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.release_index))
                    elif argument_type in SCALAR_TYPES:
                        code.extend(argument_code + self.module.host_call("bearer_print_s32"))
                    else:
                        raise CapyError(argument.location, f"print does not yet support {argument_type}")
                return bytes(code), "void"
            if expression.function.value == "clone":
                if len(expression.arguments) != 1:
                    raise CapyError(expression.location, "clone expects one string")
                source = expression.arguments[0]
                value_code, value_type = self.compile_expr(source)
                self.require_type(expression.location, "string", value_type)
                if self.expression_is_owned(source):
                    temporary = self.allocate_local("string", source.location)
                    code = value_code + b"\x21" + uleb(temporary) + b"\x20" + uleb(temporary)
                    code += b"\x10" + uleb(self.module.clone_index)
                    code += b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.release_index)
                    return code, "string"
                return value_code + b"\x10" + uleb(self.module.clone_index), "string"
            if expression.function.value == "arc_live":
                if expression.arguments:
                    raise CapyError(expression.location, "arc_live expects no arguments")
                self.module.uses_arc_global = True
                return b"\x23\x01", "s32"
            if expression.function.value == "trap":
                if expression.arguments:
                    raise CapyError(expression.location, "trap expects no arguments")
                return self.module.source_marker(expression.location) + b"\x00", "void"
            arguments = [self.compile_expr(argument) for argument in expression.arguments]
            parameter_types = tuple(value_type for _, value_type in arguments)
            target = self.module.resolve_function(expression.function.value, parameter_types, expression.location)
            code = bytearray()
            owned_argument_locals: list[int] = []
            for source_expression, (argument_code, argument_type) in zip(expression.arguments, arguments):
                if managed_type(argument_type) and self.expression_is_owned(source_expression):
                    temporary = self.allocate_local(argument_type, source_expression.location)
                    code.extend(argument_code + b"\x21" + uleb(temporary) + b"\x20" + uleb(temporary))
                    owned_argument_locals.append(temporary)
                else:
                    code.extend(argument_code)
            code.extend(b"\x10" + uleb(target.function_index))
            for temporary in reversed(owned_argument_locals):
                code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.release_index))
            return bytes(code), target.result_type
        if isinstance(expression, Return):
            expected = self.definition.result_type
            if expression.value is None:
                self.require_type(expression.location, "void", expected)
                return self.cleanup_all_scopes() + b"\x0f", "void"
            value_code, value_type = self.compile_expr(expression.value)
            self.require_type(expression.location, expected, value_type)
            if managed_type(value_type):
                temporary = self.allocate_local(value_type, expression.location)
                code = bytearray(value_code + b"\x21" + uleb(temporary))
                if not self.expression_is_owned(expression.value):
                    code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.retain_index))
                code.extend(self.cleanup_all_scopes())
                code.extend(b"\x20" + uleb(temporary) + b"\x0f")
                return bytes(code), "void"
            return value_code + self.cleanup_all_scopes() + b"\x0f", "void"
        if isinstance(expression, (Break, Continue)):
            if not self.loop_contexts:
                keyword = "break" if isinstance(expression, Break) else "continue"
                raise CapyError(expression.location, f"{keyword} is only valid inside a loop")
            break_target, continue_target, ownership_boundary, break_cleanup, continue_cleanup = self.loop_contexts[-1]
            target_depth = break_target if isinstance(expression, Break) else continue_target
            edge_cleanup = break_cleanup if isinstance(expression, Break) else continue_cleanup
            label_depth = self.control_depth - target_depth
            if label_depth < 0:
                raise CapyError(expression.location, "invalid loop control nesting")
            return self.cleanup_scopes_from(ownership_boundary) + edge_cleanup + b"\x0c" + uleb(label_depth), "void"
        if isinstance(expression, If):
            condition_code, condition_type = self.compile_expr(expression.condition)
            if condition_type not in SCALAR_TYPES:
                raise CapyError(expression.condition.location, "if condition must be scalar")
            self.control_depth += 1
            then_code = self.compile_block(expression.then_body)
            self.control_depth -= 1
            code = bytearray(condition_code + b"\x04\x40" + then_code)
            if expression.else_body:
                self.control_depth += 1
                else_code = self.compile_block(expression.else_body)
                self.control_depth -= 1
                code.extend(b"\x05" + else_code)
            code.append(0x0B)
            return bytes(code), "void"
        if isinstance(expression, While):
            condition_code, condition_type = self.compile_expr(expression.condition)
            if condition_type not in SCALAR_TYPES:
                raise CapyError(expression.condition.location, "while condition must be scalar")
            outer_depth = self.control_depth
            ownership_boundary = len(self.owned_scopes)
            self.control_depth += 2
            self.loop_contexts.append((outer_depth + 1, outer_depth + 2, ownership_boundary, b"", b""))
            body = self.compile_block(expression.body)
            self.loop_contexts.pop()
            self.control_depth -= 2
            return b"\x02\x40\x03\x40" + condition_code + b"\x45\x0d\x01" + body + b"\x0c\x00\x0b\x0b", "void"
        if isinstance(expression, For):
            iterable_type = self.infer_expr_type(expression.iterable)
            if iterable_type == "dval":
                if len(expression.names) not in {1, 2}:
                    raise CapyError(expression.location, "dval iteration accepts value or key,value bindings")
                iterable_code, _ = self.compile_expr(expression.iterable)
                iterable_local = self.allocate_local("dval", expression.iterable.location)
                count_local = self.allocate_local("s32", expression.location)
                index_local = self.allocate_local("s32", expression.location)
                value_local = self.allocate_local("dval", expression.location)
                key_local = self.allocate_local("string", expression.location) if len(expression.names) == 2 else -1
                self.borrowed_managed_slots.add(value_local)
                if key_local >= 0:
                    self.borrowed_managed_slots.add(key_local)
                code = bytearray(iterable_code + b"\x21" + uleb(iterable_local))
                code.extend(b"\x20" + uleb(iterable_local) + b"\x41\x14\x6a\x20" + uleb(iterable_local) + b"\x28\x02\x10")
                code.extend(self.module.host_call("bearer_dv_count_brrb") + b"\x21" + uleb(count_local))
                code.extend(b"\x20" + uleb(count_local) + b"\x41\x00\x48\x04\x40" + self.module.source_marker(expression.iterable.location) + b"\x00\x0b\x41\x00\x21" + uleb(index_local))
                code.extend(b"\x02\x40\x03\x40\x20" + uleb(index_local) + b"\x20" + uleb(count_local) + b"\x4f\x0d\x01")

                def append_entry(import_name: str, value_type: str, target_local: int) -> None:
                    length = self.allocate_local("s32", expression.location)
                    code.extend(b"\x20" + uleb(iterable_local) + b"\x41\x14\x6a\x20" + uleb(iterable_local) + b"\x28\x02\x10\x20" + uleb(index_local) + b"\x41\x00\x41\x00")
                    code.extend(self.module.host_call(import_name) + b"\x21" + uleb(length))
                    code.extend(b"\x20" + uleb(length) + b"\x41\x00\x48\x04\x40" + self.module.source_marker(expression.location) + b"\x00\x0b")
                    allocation, pointer = self.allocate_blob(value_type, 1 if value_type == "string" else 4, length, expression.location)
                    code.extend(allocation)
                    code.extend(b"\x20" + uleb(iterable_local) + b"\x41\x14\x6a\x20" + uleb(iterable_local) + b"\x28\x02\x10\x20" + uleb(index_local))
                    code.extend(b"\x20" + uleb(pointer) + b"\x41\x14\x6a\x20" + uleb(length) + self.module.host_call(import_name))
                    code.extend(b"\x20" + uleb(length) + b"\x47\x04\x40\x00\x0b\x20" + uleb(pointer) + b"\x21" + uleb(target_local))

                if key_local >= 0:
                    append_entry("bearer_dv_entry_key_brrb", "string", key_local)
                append_entry("bearer_dv_entry_value_brrb", "dval", value_local)
                scope = {expression.names[-1]: (value_local, "dval")}
                if key_local >= 0:
                    scope[expression.names[0]] = (key_local, "string")
                self.scopes.append(scope)
                release_items = bytearray()
                release_items.extend(b"\x20" + uleb(value_local) + b"\x10" + uleb(self.module.release_index))
                if key_local >= 0:
                    release_items.extend(b"\x20" + uleb(key_local) + b"\x10" + uleb(self.module.release_index))
                increment = b"\x20" + uleb(index_local) + b"\x41\x01\x6a\x21" + uleb(index_local)
                outer_depth = self.control_depth
                ownership_boundary = len(self.owned_scopes)
                self.control_depth += 2
                self.loop_contexts.append((outer_depth + 1, outer_depth + 2, ownership_boundary, bytes(release_items), bytes(release_items) + increment))
                body = self.compile_block(expression.body)
                self.loop_contexts.pop()
                self.control_depth -= 2
                self.scopes.pop()
                code.extend(body + release_items + increment + b"\x0c\x00\x0b\x0b")
                if self.expression_is_owned(expression.iterable):
                    code.extend(b"\x20" + uleb(iterable_local) + b"\x10" + uleb(self.module.release_index))
                return bytes(code), "void"
            if len(expression.names) != 1:
                raise CapyError(expression.location, "two-variable iteration requires a dval map or list")
            loop_name = expression.names[0]
            if isinstance(expression.iterable, Binary) and expression.iterable.operator == "..":
                start_code, start_type = self.compile_expr(expression.iterable.left)
                end_code, end_type = self.compile_expr(expression.iterable.right)
                self.require_type(expression.location, "s32", start_type)
                self.require_type(expression.location, "s32", end_type)
                loop_local = self.allocate_local("s32", expression.location)
                end_local = self.allocate_local("s32", expression.location)
                self.scopes.append({loop_name: (loop_local, "s32")})
                outer_depth = self.control_depth
                ownership_boundary = len(self.owned_scopes)
                self.control_depth += 3
                self.loop_contexts.append((outer_depth + 1, outer_depth + 3, ownership_boundary, b"", b""))
                body = self.compile_block(expression.body)
                self.loop_contexts.pop()
                self.control_depth -= 3
                self.scopes.pop()
                code = start_code + b"\x21" + uleb(loop_local) + end_code + b"\x21" + uleb(end_local)
                code += b"\x02\x40\x03\x40\x20" + uleb(loop_local) + b"\x20" + uleb(end_local) + b"\x4e\x0d\x01\x02\x40"
                code += body + b"\x0b\x20" + uleb(loop_local) + b"\x41\x01\x6a\x21" + uleb(loop_local) + b"\x0c\x00\x0b\x0b"
                return code, "void"
            iterable_code, iterable_type = self.compile_expr(expression.iterable)
            if not iterable_type.startswith("array<"):
                raise CapyError(expression.iterable.location, "for loop requires an exclusive range or array")
            element_type = iterable_type[6:-1]
            array_local = self.allocate_local(iterable_type, expression.iterable.location)
            index_local = self.allocate_local("s32", expression.location)
            length_local = self.allocate_local("s32", expression.location)
            item_local = self.allocate_local(element_type, expression.location)
            if managed_type(element_type):
                self.borrowed_managed_slots.add(item_local)
            owns_iterable = self.expression_is_owned(expression.iterable)
            self.scopes.append({loop_name: (item_local, element_type)})
            self.owned_scopes.append([(array_local, iterable_type)] if owns_iterable else [])
            outer_depth = self.control_depth
            ownership_boundary = len(self.owned_scopes)
            self.control_depth += 3
            self.loop_contexts.append((outer_depth + 1, outer_depth + 3, ownership_boundary, b"", b""))
            body = self.compile_block(expression.body)
            self.loop_contexts.pop()
            self.control_depth -= 3
            self.owned_scopes.pop()
            self.scopes.pop()
            code = bytearray(iterable_code + b"\x21" + uleb(array_local) + b"\x41\x00\x21" + uleb(index_local))
            code.extend(b"\x20" + uleb(array_local) + b"\x28\x02\x10\x21" + uleb(length_local))
            code.extend(b"\x02\x40\x03\x40\x20" + uleb(index_local) + b"\x20" + uleb(length_local) + b"\x4f\x0d\x01")
            code.extend(b"\x20" + uleb(array_local) + b"\x20" + uleb(index_local) + b"\x41\x04\x6c\x6a\x28\x02\x14")
            code.extend(b"\x21" + uleb(item_local) + b"\x02\x40" + body + b"\x0b")
            code.extend(b"\x20" + uleb(index_local) + b"\x41\x01\x6a\x21" + uleb(index_local) + b"\x0c\x00\x0b\x0b")
            if owns_iterable:
                code.extend(b"\x20" + uleb(array_local) + b"\x10" + uleb(self.module.release_index))
            return bytes(code), "void"
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
        self.generics: dict[str, list[GenericDefinition]] = {}
        self.structs: dict[str, tuple[int, list[tuple[str, str]]]] = {}
        self.tuples: dict[str, tuple[int, tuple[str, ...]]] = {}
        self.function_value_signatures: dict[str, tuple[tuple[str, ...], str]] = {}
        self.custom_exports: list[tuple[str, CompiledDefinition]] = []
        self.table_slots: dict[FunctionKey, int] = {}
        self.types: list[tuple[tuple[str, ...], str]] = []
        self.type_indices: dict[tuple[tuple[str, ...], str], int] = {}
        self.host_import_order = (
            "bearer_print_bytes", "bearer_print_s32", "bearer_alloc", "bearer_free",
            "bearer_unit_render_bytes", "bearer_component_render_bytes",
            "bearer_dv_string_to_brrb", "bearer_dv_s32_to_brrb", "bearer_dv_bool_to_brrb",
            "bearer_dv_build_brrb", "bearer_dv_get_brrb", "bearer_dv_count_brrb",
            "bearer_dv_entry_key_brrb", "bearer_dv_entry_value_brrb", "bearer_dv_scalar_type_brrb",
            "bearer_dv_s32_brrb", "bearer_dv_bool_brrb", "bearer_dv_ptr_to_brrb",
            "bearer_dv_brrb_to_ptr", "bearer_dv_brrb_to_string", "bearer_unit_call_brrb",
        )
        self.host_indices = {name: index for index, name in enumerate(self.host_import_order)}
        self.helper_indices = {"retain": 9, "release": 10, "clone": 11}
        self.used_host_imports: set[str] = set()
        self.used_helpers: set[str] = set()
        self.uses_arc_global = False
        self.first_user_index = 12
        self.source_markers: list[tuple[bytes, Location]] = []

    def source_marker(self, location: Location) -> bytes:
        marker = b"\x41" + sleb32(0x5A000000 + len(self.source_markers)) + b"\x1a"
        self.source_markers.append((marker, location))
        return marker

    def encoded_host_call(self, name: str) -> bytes:
        return b"\x10" + uleb(self.host_indices[name])

    def host_call(self, name: str) -> bytes:
        self.used_host_imports.add(name)
        if name == "bearer_alloc":
            self.uses_arc_global = True
        return self.encoded_host_call(name)

    def helper_index(self, name: str) -> int:
        self.used_helpers.add(name)
        if name in {"release", "clone"}:
            self.uses_arc_global = True
        return self.helper_indices[name]

    @property
    def retain_index(self) -> int:
        return self.helper_index("retain")

    @property
    def release_index(self) -> int:
        return self.helper_index("release")

    @property
    def clone_index(self) -> int:
        return self.helper_index("clone")

    def align_data(self, alignment: int = 4) -> None:
        while len(self.data) % alignment:
            self.data.append(0)

    def add_static_string(self, value: bytes) -> int:
        self.align_data(8)
        offset = len(self.data)
        # Immortal ARC header: strong, weak, type-id, allocation bytes, length.
        self.data.extend(struct.pack("<IIIII", 0xFFFFFFFF, 0xFFFFFFFF, 1, 20 + len(value), len(value)))
        self.data.extend(value)
        return offset

    def add_static_bytes(self, value: bytes) -> int:
        offset = len(self.data)
        self.data.extend(value)
        return offset

    def register_tuple(self, element_types: tuple[str, ...]) -> str:
        value_type = "tuple<" + ",".join(element_types) + ">"
        if value_type not in self.tuples:
            self.tuples[value_type] = (5 + len(self.structs) + len(self.tuples), element_types)
        return value_type

    def register_type_expression(self, expression: Expr | None) -> None:
        if isinstance(expression, TupleExpr):
            element_types = tuple(capy_type(item, item.location) for item in expression.items)
            self.register_tuple(element_types)
            for item in expression.items:
                self.register_type_expression(item)

    def reference_function(self, name: str, location: Location) -> tuple[str, int]:
        if name in self.generics:
            raise CapyError(location, f"generic function value {name!r} requires an explicit concrete function type")
        candidates = [(key, definition) for key, definition in self.functions.items() if key.name == name]
        if not candidates:
            raise CapyError(location, f"unknown local {name!r}")
        if len(candidates) != 1:
            raise CapyError(location, f"function value {name!r} requires exactly one concrete overload; found more than one overload")
        key, definition = candidates[0]
        value_type = f"function#{definition.type_index}"
        self.function_value_signatures[value_type] = (definition.parameter_types, definition.result_type)
        if key not in self.table_slots:
            self.table_slots[key] = len(self.table_slots)
        return value_type, self.table_slots[key]

    def wasm_type(self, parameters: tuple[str, ...], result: str) -> int:
        key = (parameters, result)
        if key not in self.type_indices:
            self.type_indices[key] = len(self.types)
            self.types.append(key)
        return self.type_indices[key]

    def collect(self) -> None:
        struct_nodes: list[Struct] = []
        for item in self.program.items:
            if not isinstance(item, Struct):
                continue
            if item.name in self.structs:
                raise CapyError(item.location, f"struct {item.name!r} is already declared")
            self.structs[item.name] = (5 + len(self.structs), [])
            struct_nodes.append(item)
        for item in struct_nodes:
            members: list[tuple[str, str]] = []
            seen_members: set[str] = set()
            for member in item.members:
                if not isinstance(member, Annotation) or not isinstance(member.value, Name):
                    raise CapyError(member.location, "struct members must be name:type annotations")
                if member.value.value in seen_members:
                    raise CapyError(member.location, f"struct member {member.value.value!r} is already declared")
                seen_members.add(member.value.value)
                self.register_type_expression(member.type_expr)
                members.append((member.value.value, capy_type(member.type_expr, member.location)))
            type_id, _ = self.structs[item.name]
            self.structs[item.name] = (type_id, members)

        def validate_type(value_type: str, location: Location) -> None:
            if value_type.startswith("struct:") and value_type[7:] not in self.structs:
                raise CapyError(location, f"unknown type {value_type[7:]!r}")
            if value_type.startswith("tuple<"):
                for element_type in self.tuples[value_type][1]:
                    validate_type(element_type, location)

        for _, members in self.structs.values():
            for _, member_type in members:
                validate_type(member_type, Location(self.source, 1, 1, 0))

        seen_handlers: dict[str, Location] = {}
        top_level: list[Expr] = []
        for item in self.program.items:
            if isinstance(item, Function):
                if item.name in self.structs:
                    raise CapyError(item.location, f"function name {item.name!r} conflicts with a struct constructor")
                for parameter in item.parameters:
                    self.register_type_expression(parameter.type_expr)
                self.register_type_expression(item.return_type)
                parameter_patterns = tuple(
                    None if isinstance(parameter.type_expr, Name) and parameter.type_expr.value == "any"
                    else capy_type(parameter.type_expr, item.location)
                    for parameter in item.parameters
                )
                concrete_parameters = tuple(value_type for value_type in parameter_patterns if value_type is not None)
                for parameter_type in concrete_parameters:
                    validate_type(parameter_type, item.location)
                    if parameter_type == "void":
                        raise CapyError(item.location, "function parameters cannot have type void")
                is_generic = len(concrete_parameters) != len(parameter_patterns)
                export_name = handler_export(item.name)
                if export_name is not None:
                    if is_generic:
                        raise CapyError(item.location, "Bearer handlers cannot use any parameters")
                    parameter_types = tuple(concrete_parameters)
                    if len(item.parameters) > 1:
                        raise CapyError(item.location, "Bearer handler accepts zero parameters or one request parameter")
                    if parameter_types and parameter_types != ("request",):
                        raise CapyError(item.location, "Bearer handler parameter must have type request")
                    if export_name in seen_handlers:
                        first = seen_handlers[export_name]
                        raise CapyError(item.location, f"Bearer handler {item.name} is already declared at {first.line}:{first.column}; handlers cannot be overloaded")
                    seen_handlers[export_name] = item.location
                    definition = CompiledDefinition(item, parameter_types, "void", export_name)
                    self.definitions.append(definition)
                elif is_generic:
                    dependent_result = -1
                    if item.return_type is None:
                        result_type = "void"
                    elif isinstance(item.return_type, ScopeLookup) and isinstance(item.return_type.value, Name) and item.return_type.member == "type":
                        names = [parameter.name for parameter in item.parameters]
                        if item.return_type.value.value not in names:
                            raise CapyError(item.return_type.location, "dependent result names an unknown parameter")
                        dependent_result = names.index(item.return_type.value.value)
                        result_type = "void"
                    else:
                        result_type = capy_type(item.return_type, item.location)
                        if result_type != "void":
                            raise CapyError(item.location, "generic results must be void or use x::type")
                    self.generics.setdefault(item.name, []).append(GenericDefinition(item, parameter_patterns, dependent_result))
                else:
                    parameter_types = tuple(concrete_parameters)
                    if item.return_type is None:
                        result_type = "void"
                    elif isinstance(item.return_type, ScopeLookup) and isinstance(item.return_type.value, Name) and item.return_type.member == "type":
                        names = [parameter.name for parameter in item.parameters]
                        if item.return_type.value.value not in names:
                            raise CapyError(item.return_type.location, "dependent result names an unknown parameter")
                        result_type = parameter_types[names.index(item.return_type.value.value)]
                    else:
                        result_type = capy_type(item.return_type, item.location)
                    validate_type(result_type, item.location)
                    definition = CompiledDefinition(item, parameter_types, result_type, None)
                    key = FunctionKey(item.name, parameter_types)
                    self.functions[key] = definition
                    self.definitions.append(definition)
                    if item.name.startswith("EXPORT_"):
                        export_name = item.name[7:]
                        if not export_name:
                            raise CapyError(item.location, "custom DValue export requires a name after EXPORT_")
                        if parameter_types != ("dval",) or result_type != "dval":
                            raise CapyError(item.location, "custom DValue export must have signature (dval) dval")
                        if any(existing == export_name for existing, _ in self.custom_exports):
                            raise CapyError(item.location, f"custom DValue export {export_name!r} is already declared")
                        self.custom_exports.append((export_name, definition))
            elif isinstance(item, Struct):
                continue
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
        if key in self.functions:
            return self.functions[key]
        candidates = [
            generic for generic in self.generics.get(name, [])
            if len(generic.parameter_patterns) == len(parameters) and all(
                pattern is None or pattern == actual
                for pattern, actual in zip(generic.parameter_patterns, parameters)
            )
        ]
        if not candidates:
            rendered = ", ".join(parameters)
            raise CapyError(location, f"no overload {name}({rendered})")
        best_rank = max(sum(pattern is not None for pattern in candidate.parameter_patterns) for candidate in candidates)
        best = [candidate for candidate in candidates if sum(pattern is not None for pattern in candidate.parameter_patterns) == best_rank]
        if len(best) != 1:
            rendered = ", ".join(parameters)
            raise CapyError(location, f"ambiguous generic overload {name}({rendered})")
        generic = best[0]
        result_type = parameters[generic.dependent_result] if generic.dependent_result >= 0 else "void"
        definition = CompiledDefinition(generic.function, parameters, result_type, None)
        definition.function_index = self.first_user_index + len(self.definitions)
        definition.type_index = self.wasm_type(parameters, result_type)
        self.functions[key] = definition
        self.definitions.append(definition)
        return definition

    def runtime_bodies(self) -> list[bytes]:
        def body(locals_prefix: bytes, code: bytes) -> bytes:
            content = locals_prefix + code + b"\x0b"
            return uleb(len(content)) + content

        null_return = b"\x20\x00\x45\x04\x40\x0f\x0b"
        immortal_return = b"\x20\x00\x28\x02\x00\x41\x7f\x46\x04\x40\x0f\x0b"
        retain = null_return + immortal_return
        retain += b"\x20\x00\x28\x02\x00\x41\x7e\x46\x04\x40\x00\x0b"  # overflow guard
        retain += b"\x20\x00\x20\x00\x28\x02\x00\x41\x01\x6a\x36\x02\x00"

        release = null_return + immortal_return
        release += b"\x20\x00\x28\x02\x00\x45\x04\x40\x00\x0b"  # double-release guard
        release += b"\x20\x00\x20\x00\x28\x02\x00\x41\x01\x6b\x22\x01\x36\x02\x00"
        release += b"\x20\x01\x45\x04\x40"
        release += b"\x20\x00\x28\x02\x08\x41\x03\x46\x04\x40"  # string-array drop glue
        release += b"\x20\x00\x28\x02\x10\x21\x02\x41\x00\x21\x01"
        release += b"\x02\x40\x03\x40\x20\x01\x20\x02\x4f\x0d\x01"
        release += b"\x20\x00\x20\x01\x41\x04\x6c\x6a\x28\x02\x14\x10" + uleb(self.helper_indices.get("release", 0))
        release += b"\x20\x01\x41\x01\x6a\x21\x01\x0c\x00\x0b\x0b\x0b"
        for _, (type_id, members) in self.structs.items():
            managed_members = [(index, value_type) for index, (_, value_type) in enumerate(members) if managed_type(value_type)]
            if not managed_members:
                continue
            release += b"\x20\x00\x28\x02\x08\x41" + sleb32(type_id) + b"\x46\x04\x40"
            for index, _ in managed_members:
                release += b"\x20\x00\x28\x02" + uleb(16 + 4 * index) + b"\x10" + uleb(self.helper_indices.get("release", 0))
            release += b"\x0b"
        for type_id, element_types in self.tuples.values():
            managed_elements = [index for index, value_type in enumerate(element_types) if managed_type(value_type)]
            if not managed_elements:
                continue
            release += b"\x20\x00\x28\x02\x08\x41" + sleb32(type_id) + b"\x46\x04\x40"
            for index in managed_elements:
                release += b"\x20\x00\x28\x02" + uleb(16 + 4 * index) + b"\x10" + uleb(self.helper_indices.get("release", 0))
            release += b"\x0b"
        release += b"\x23\x01\x41\x01\x6b\x24\x01\x20\x00\x10" + uleb(self.host_indices.get("bearer_free", 0)) + b"\x0b"

        clone = bytearray()
        clone.extend(b"\x20\x00\x28\x02\x10\x21\x02")  # len = source.length
        clone.extend(b"\x20\x02\x41\x14\x6a\x10" + uleb(self.host_indices.get("bearer_alloc", 0)) + b"\x21\x01")
        clone.extend(b"\x20\x01\x45\x04\x40\x00\x0b")  # deterministic OOM trap before writes
        for value_code, offset in [
            (b"\x41\x01", 0), (b"\x41\x01", 4), (b"\x41\x01", 8),
        ]:
            clone.extend(b"\x20\x01" + value_code + b"\x36\x02" + uleb(offset))
        clone.extend(b"\x20\x01\x20\x02\x41\x14\x6a\x36\x02\x0c")
        clone.extend(b"\x20\x01\x20\x02\x36\x02\x10")
        clone.extend(b"\x20\x01\x41\x14\x6a\x20\x00\x41\x14\x6a\x20\x02\xfc\x0a\x00\x00")
        clone.extend(b"\x23\x01\x41\x01\x6a\x24\x01")
        clone.extend(b"\x20\x01")
        bodies = {
            "retain": body(b"\x00", retain),
            "release": body(b"\x01\x02\x7f", release),
            "clone": body(b"\x01\x02\x7f", bytes(clone)),
        }
        return [bodies[name] for name in ("retain", "release", "clone") if name in self.used_helpers]

    def custom_export_body(self, target: CompiledDefinition) -> bytes:
        length, input_value, result_value, output_pointer = 1, 2, 3, 4
        code = bytearray(b"\x20\x00\x41\x00\x41\x00" + self.encoded_host_call("bearer_dv_ptr_to_brrb") + b"\x21" + uleb(length))
        code.extend(b"\x20" + uleb(length) + b"\x41\x14\x6a" + self.encoded_host_call("bearer_alloc") + b"\x21" + uleb(input_value))
        code.extend(b"\x20" + uleb(input_value) + b"\x45\x04\x40\x00\x0b")
        for value, offset in ((1, 0), (1, 4), (4, 8)):
            code.extend(b"\x20" + uleb(input_value) + b"\x41" + sleb32(value) + b"\x36\x02" + uleb(offset))
        code.extend(b"\x20" + uleb(input_value) + b"\x20" + uleb(length) + b"\x41\x14\x6a\x36\x02\x0c")
        code.extend(b"\x20" + uleb(input_value) + b"\x20" + uleb(length) + b"\x36\x02\x10")
        code.extend(b"\x23\x01\x41\x01\x6a\x24\x01")
        code.extend(b"\x20\x00\x20" + uleb(input_value) + b"\x41\x14\x6a\x20" + uleb(length) + self.encoded_host_call("bearer_dv_ptr_to_brrb"))
        code.extend(b"\x20" + uleb(length) + b"\x47\x04\x40\x00\x0b")
        code.extend(b"\x20" + uleb(input_value) + b"\x10" + uleb(target.function_index) + b"\x21" + uleb(result_value))
        code.extend(b"\x20" + uleb(result_value) + b"\x41\x14\x6a\x20" + uleb(result_value) + b"\x28\x02\x10")
        code.extend(self.encoded_host_call("bearer_dv_brrb_to_ptr") + b"\x21" + uleb(output_pointer))
        code.extend(b"\x20" + uleb(input_value) + b"\x10" + uleb(self.helper_indices["release"]))
        code.extend(b"\x20" + uleb(result_value) + b"\x10" + uleb(self.helper_indices["release"]))
        code.extend(b"\x20" + uleb(output_pointer) + b"\x0b")
        content = b"\x01\x04\x7f" + bytes(code)
        return uleb(len(content)) + content

    def compile(self) -> tuple[bytes, str]:
        self.collect()
        # Stable direct-language imports are function indices 0..8.
        print_bytes_type = self.wasm_type(("s32", "s32"), "void")
        print_s32_type = self.wasm_type(("s32",), "void")
        allocator_type = self.wasm_type(("s32",), "s32")
        blob_adapter_type = self.wasm_type(("s32", "s32", "s32", "s32"), "s32")
        scalar_adapter_type = self.wasm_type(("s32", "s32", "s32"), "s32")
        build_adapter_type = self.wasm_type(("s32", "s32", "s32", "s32", "s32"), "s32")
        get_adapter_type = self.wasm_type(("s32", "s32", "s32", "s32", "s32", "s32", "s32", "s32"), "s32")
        entry_adapter_type = self.wasm_type(("s32", "s32", "s32", "s32", "s32"), "s32")
        unit_call_type = self.wasm_type(("s32", "s32", "s32", "s32", "s32", "s32", "s32", "s32"), "s32")
        retain_type = self.wasm_type(("s32",), "void")
        clone_type = self.wasm_type(("s32",), "s32")
        custom_export_type = self.wasm_type(("s32",), "s32")
        for index, definition in enumerate(self.definitions):
            definition.function_index = self.first_user_index + index
            wasm_parameters = ("s32",) if definition.export_name else definition.parameter_types
            definition.type_index = self.wasm_type(wasm_parameters, definition.result_type)

        # Lower once against stable provisional indexes to discover the exact runtime surface.
        definition_index = 0
        while definition_index < len(self.definitions):
            WasmFunctionCompiler(self, self.definitions[definition_index]).compile_body()
            definition_index += 1
        if self.custom_exports:
            self.used_host_imports.update({"bearer_alloc", "bearer_free", "bearer_dv_ptr_to_brrb", "bearer_dv_brrb_to_ptr"})
            self.used_helpers.add("release")
            self.uses_arc_global = True
        required_helpers = tuple(name for name in ("retain", "release", "clone") if name in self.used_helpers)
        if "clone" in required_helpers:
            self.used_host_imports.add("bearer_alloc")
        if "release" in required_helpers:
            self.used_host_imports.add("bearer_free")
        required_imports = tuple(name for name in self.host_import_order if name in self.used_host_imports)
        requires_arc_global = self.uses_arc_global

        self.host_indices = {name: index for index, name in enumerate(required_imports)}
        self.helper_indices = {
            name: len(required_imports) + index for index, name in enumerate(required_helpers)
        }
        self.first_user_index = len(required_imports) + len(required_helpers)
        for index, definition in enumerate(self.definitions):
            definition.function_index = self.first_user_index + index

        self.data.clear()
        self.source_markers.clear()
        self.used_host_imports.clear()
        self.used_helpers.clear()
        self.uses_arc_global = False
        user_bodies: list[bytes] = []
        for definition in self.definitions:
            user_bodies.append(WasmFunctionCompiler(self, definition).compile_body())
        if self.custom_exports:
            self.used_host_imports.update({"bearer_alloc", "bearer_free", "bearer_dv_ptr_to_brrb", "bearer_dv_brrb_to_ptr"})
            self.used_helpers.add("release")
            self.uses_arc_global = True
        if (
            not self.used_host_imports.issubset(required_imports)
            or not self.used_helpers.issubset(required_helpers)
            or self.uses_arc_global != requires_arc_global
        ):
            raise RuntimeError("Capy runtime-surface discovery changed during final lowering")
        self.used_helpers.update(required_helpers)
        runtime_bodies = self.runtime_bodies()
        custom_bodies = [self.custom_export_body(target) for _, target in self.custom_exports]
        bodies = runtime_bodies + user_bodies + custom_bodies

        def encode_type(signature: tuple[tuple[str, ...], str]) -> bytes:
            parameters, result = signature
            wasm_parameters = [b"\x7f" for _ in parameters]
            wasm_results = [] if result == "void" else [b"\x7f"]
            return b"\x60" + wasm_vector(wasm_parameters) + wasm_vector(wasm_results)

        import_types = {
            "bearer_print_bytes": print_bytes_type,
            "bearer_print_s32": print_s32_type,
            "bearer_alloc": allocator_type,
            "bearer_free": retain_type,
            "bearer_unit_render_bytes": print_bytes_type,
            "bearer_component_render_bytes": print_bytes_type,
            "bearer_dv_string_to_brrb": blob_adapter_type,
            "bearer_dv_s32_to_brrb": scalar_adapter_type,
            "bearer_dv_bool_to_brrb": scalar_adapter_type,
            "bearer_dv_build_brrb": build_adapter_type,
            "bearer_dv_get_brrb": get_adapter_type,
            "bearer_dv_count_brrb": self.wasm_type(("s32", "s32"), "s32"),
            "bearer_dv_entry_key_brrb": entry_adapter_type,
            "bearer_dv_entry_value_brrb": entry_adapter_type,
            "bearer_dv_scalar_type_brrb": self.wasm_type(("s32", "s32"), "s32"),
            "bearer_dv_s32_brrb": scalar_adapter_type,
            "bearer_dv_bool_brrb": scalar_adapter_type,
            "bearer_dv_ptr_to_brrb": scalar_adapter_type,
            "bearer_dv_brrb_to_ptr": self.wasm_type(("s32", "s32"), "s32"),
            "bearer_dv_brrb_to_string": blob_adapter_type,
            "bearer_unit_call_brrb": unit_call_type,
        }
        imports = [
            wasm_string("env") + wasm_string("memory") + b"\x02\x00\x01",
            wasm_string("env") + wasm_string("__memory_base") + b"\x03\x7f\x00",
            *[
                wasm_string("env") + wasm_string(name) + b"\x00" + uleb(import_types[name])
                for name in required_imports
            ],
        ]
        exports = [
            wasm_string(definition.export_name) + b"\x00" + uleb(definition.function_index)
            for definition in self.definitions if definition.export_name
        ]
        exports.extend(
            wasm_string(name) + b"\x00" + uleb(self.first_user_index + len(self.definitions) + index)
            for index, (name, _) in enumerate(self.custom_exports)
        )
        mem_info = uleb(len(self.data)) + uleb(3) + uleb(0) + uleb(0)
        data_segment = b"\x00\x23\x00\x0b" + uleb(len(self.data)) + bytes(self.data)
        abi = (
            "format=bearer-wasm-unit-abi-v1\n"
            f"unit_abi_version={self.abi_version}\n"
            "toolchain=capyc-direct-wasm-phase2\n"
            f"source={self.source}\n"
        ).encode("utf-8")
        module_name_payload = wasm_string(self.module_name)
        name_section = b"\x00" + uleb(len(module_name_payload)) + module_name_payload
        table_sections: list[bytes] = []
        if self.table_slots:
            table_sections.append(wasm_section(4, wasm_vector([b"\x70\x00" + uleb(len(self.table_slots))])))
            ordered_keys = [key for key, _ in sorted(self.table_slots.items(), key=lambda item: item[1])]
            element = b"\x00\x41\x00\x0b" + wasm_vector([uleb(self.functions[key].function_index) for key in ordered_keys])
            table_sections.append(wasm_section(9, wasm_vector([element])))
        code_payload = wasm_vector(bodies)
        code_section = wasm_section(10, code_payload)
        wasm = b"\x00asm\x01\x00\x00\x00" + b"".join([
            wasm_custom("dylink.0", b"\x01" + uleb(len(mem_info)) + mem_info),
            wasm_section(1, wasm_vector([encode_type(signature) for signature in self.types])),
            wasm_section(2, wasm_vector(imports)),
            wasm_section(3, wasm_vector([
                *[
                    uleb(clone_type if name == "clone" else retain_type)
                    for name in required_helpers
                ],
                *[uleb(definition.type_index) for definition in self.definitions],
                *[uleb(custom_export_type) for _ in self.custom_exports],
            ])),
            *table_sections[:1],
            *([wasm_section(6, wasm_vector([b"\x7f\x01\x41\x00\x0b"]))] if requires_arc_global else []),
            wasm_section(7, wasm_vector(exports)),
            *table_sections[1:],
            code_section,
            wasm_section(11, wasm_vector([data_segment])),
            wasm_custom("name", name_section),
            wasm_custom("bearer.abi", abi),
            wasm_custom("bearer.module", self.module_name.encode("utf-8")),
        ])
        def decode_uleb_at(data: bytes, offset: int) -> tuple[int, int]:
            value = 0
            shift = 0
            while True:
                byte = data[offset]
                offset += 1
                value |= (byte & 0x7F) << shift
                if not (byte & 0x80):
                    return value, offset
                shift += 7

        code_address = wasm.find(code_section)
        if code_address < 0 or wasm.find(code_section, code_address + 1) >= 0:
            raise RuntimeError("Capy code section is missing or ambiguous in final Wasm")
        body_address = code_address + 1 + len(uleb(len(code_payload))) + len(uleb(len(bodies)))
        body_entries: list[int] = []
        cursor = body_address
        for body in bodies:
            _, content = decode_uleb_at(body, 0)
            local_groups, instruction = decode_uleb_at(body, content)
            for _ in range(local_groups):
                _, instruction = decode_uleb_at(body, instruction)
                instruction += 1
            body_entries.append(cursor + instruction)
            cursor += len(body)

        source_rows: list[tuple[int, Location]] = [
            (body_entries[len(runtime_bodies) + index], definition.function.location)
            for index, definition in enumerate(self.definitions)
        ]
        source_rows.extend(
            (body_entries[len(runtime_bodies) + len(self.definitions) + index], target.function.location)
            for index, (_, target) in enumerate(self.custom_exports)
        )
        for marker, location in self.source_markers:
            address = wasm.find(marker)
            if address < 0 or wasm.find(marker, address + 1) >= 0:
                raise RuntimeError("Capy source marker is missing or ambiguous in final Wasm")
            source_rows.append((address, location))
        source_rows.sort(key=lambda row: row[0])
        source_map = "\n".join([
            f"BEARER_SOURCE_MAP_V1\t{self.module_name}",
            f"F\t1\t{self.source}",
            *[f"L\t{address:x}\t1\t{location.line}\t{location.column}" for address, location in source_rows],
            "",
        ])
        return wasm, source_map


def compile_bearer_unit(program: Program, source: str, module_name: str, abi_version: int) -> tuple[bytes, str]:
    return CapyModuleCompiler(program, source, module_name, abi_version).compile()
