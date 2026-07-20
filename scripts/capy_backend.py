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
        self.owned_scopes: list[list[tuple[int, str]]] = [[]]
        self.parameter_count = 1 if definition.export_name else len(definition.parameter_types)
        self.local_count = 0
        self.borrowed_managed_parameters: set[int] = set()
        if definition.export_name:
            if len(definition.function.parameters) == 1:
                parameter = definition.function.parameters[0]
                self.scopes[0][parameter.name] = (0, "request")
        else:
            for index, parameter in enumerate(definition.function.parameters):
                self.scopes[0][parameter.name] = (index, definition.parameter_types[index])
                if definition.parameter_types[index] == "string":
                    self.borrowed_managed_parameters.add(index)

    def lookup(self, name: Name) -> tuple[int, str]:
        for scope in reversed(self.scopes):
            if name.value in scope:
                return scope[name.value]
        raise CapyError(name.location, f"unknown local {name.value!r}")

    def allocate_local(self, value_type: str, location: Location) -> int:
        if value_type not in SCALAR_TYPES | {"string"}:
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
            if result_type == "string" and is_last and expected_result == "string":
                result_local = self.allocate_local("string", expression.location)
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
                        if result_type == "string" and self.expression_is_owned(expression):
                            code.extend(b"\x10" + uleb(self.module.release_index))
                        else:
                            code.append(0x1A)  # drop
        if expected_result != "void" and not produced_result and not self.block_guarantees_return(block):
            raise CapyError(block.location, f"not all paths produce {expected_result}")
        if not (expected_result == "string" and block.items):
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

    def cleanup_scope(self) -> bytes:
        code = bytearray()
        for local, value_type in reversed(self.owned_scopes[-1]):
            if value_type == "string":
                code.extend(b"\x20" + uleb(local) + b"\x10" + uleb(self.module.release_index))
        return bytes(code)

    def cleanup_all_scopes(self) -> bytes:
        code = bytearray()
        for scope in reversed(self.owned_scopes):
            for local, value_type in reversed(scope):
                if value_type == "string":
                    code.extend(b"\x20" + uleb(local) + b"\x10" + uleb(self.module.release_index))
        return bytes(code)

    def expression_is_owned(self, expression: Expr) -> bool:
        if isinstance(expression, Call) and isinstance(expression.function, Name):
            if expression.function.value == "clone":
                return True
            argument_types = tuple(self.infer_expr_type(argument) for argument in expression.arguments)
            target = self.module.functions.get(FunctionKey(expression.function.value, argument_types))
            return bool(target and target.result_type == "string")
        return False

    def infer_expr_type(self, expression: Expr) -> str:
        if isinstance(expression, String):
            return "string"
        if isinstance(expression, Integer):
            return "s32"
        if isinstance(expression, Name):
            if expression.value in {"true", "false"}:
                return "bool"
            return self.lookup(expression)[1]
        if isinstance(expression, Binary):
            if expression.operator in {"==", "!=", "<", ">", "<=", ">="}:
                return "bool"
            return self.infer_expr_type(expression.right if expression.operator == "=" else expression.left)
        if isinstance(expression, Call) and isinstance(expression.function, Name):
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
        if isinstance(expression, (Variable, Return, If, While, For, Block)):
            return "void"
        raise CapyError(expression.location, f"cannot infer type of {expression.__class__.__name__}")

    def require_type(self, location: Location, expected: str, actual: str) -> None:
        if expected != actual:
            raise CapyError(location, f"expected {expected}, found {actual}")

    def compile_expr(self, expression: Expr) -> tuple[bytes, str]:
        if isinstance(expression, Integer):
            return b"\x41" + sleb32(expression.value), "s32"
        if isinstance(expression, String):
            offset = self.module.add_static_string(expression.value.encode("utf-8"))
            return b"\x23\x00\x41" + sleb32(offset) + b"\x6a", "string"
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
            code = bytearray(value_code + b"\x21" + uleb(local))
            if declared == "string":
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
                if target_type == "string":
                    if local in self.borrowed_managed_parameters:
                        raise CapyError(expression.left.location, "cannot assign to a borrowed managed parameter; copy it into a local first")
                    temporary = self.allocate_local("string", expression.location)
                    code = bytearray(value_code + b"\x21" + uleb(temporary))
                    if not self.expression_is_owned(expression.right):
                        code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.retain_index))
                    code.extend(b"\x20" + uleb(local) + b"\x10" + uleb(self.module.release_index))
                    code.extend(b"\x20" + uleb(temporary) + b"\x22" + uleb(local))
                    return bytes(code), "string"
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
                    argument_code, argument_type = self.compile_expr(argument)
                    if argument_type == "string":
                        temporary = self.allocate_local("string", argument.location)
                        code.extend(argument_code + b"\x21" + uleb(temporary))
                        code.extend(b"\x20" + uleb(temporary) + b"\x41\x14\x6a")
                        code.extend(b"\x20" + uleb(temporary) + b"\x28\x02\x10")
                        code.extend(b"\x10\x00")
                        if self.expression_is_owned(argument):
                            code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.release_index))
                    elif argument_type in SCALAR_TYPES:
                        code.extend(argument_code + b"\x10\x01")
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
                return b"\x23\x01", "s32"
            if expression.function.value == "trap":
                if expression.arguments:
                    raise CapyError(expression.location, "trap expects no arguments")
                return b"\x00", "void"
            arguments = [self.compile_expr(argument) for argument in expression.arguments]
            parameter_types = tuple(value_type for _, value_type in arguments)
            target = self.module.resolve_function(expression.function.value, parameter_types, expression.location)
            code = bytearray()
            owned_argument_locals: list[int] = []
            for source_expression, (argument_code, argument_type) in zip(expression.arguments, arguments):
                if argument_type == "string" and self.expression_is_owned(source_expression):
                    temporary = self.allocate_local("string", source_expression.location)
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
            if value_type == "string":
                temporary = self.allocate_local("string", expression.location)
                code = bytearray(value_code + b"\x21" + uleb(temporary))
                if not self.expression_is_owned(expression.value):
                    code.extend(b"\x20" + uleb(temporary) + b"\x10" + uleb(self.module.retain_index))
                code.extend(self.cleanup_all_scopes())
                code.extend(b"\x20" + uleb(temporary) + b"\x0f")
                return bytes(code), "void"
            return value_code + self.cleanup_all_scopes() + b"\x0f", "void"
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
        self.retain_index = 4
        self.release_index = 5
        self.clone_index = 6

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
        release += b"\x20\x01\x45\x04\x40\x23\x01\x41\x01\x6b\x24\x01\x20\x00\x10\x03\x0b"

        clone = bytearray()
        clone.extend(b"\x20\x00\x28\x02\x10\x21\x02")  # len = source.length
        clone.extend(b"\x20\x02\x41\x14\x6a\x10\x02\x21\x01")
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
        return [
            body(b"\x00", retain),
            body(b"\x01\x01\x7f", release),
            body(b"\x01\x02\x7f", bytes(clone)),
        ]

    def compile(self) -> tuple[bytes, str]:
        self.collect()
        # Stable direct-language imports are function indices 0..3.
        print_bytes_type = self.wasm_type(("s32", "s32"), "void")
        print_s32_type = self.wasm_type(("s32",), "void")
        allocator_type = self.wasm_type(("s32",), "s32")
        retain_type = self.wasm_type(("s32",), "void")
        clone_type = self.wasm_type(("s32",), "s32")
        for index, definition in enumerate(self.definitions):
            definition.function_index = 7 + index
            wasm_parameters = ("s32",) if definition.export_name else definition.parameter_types
            definition.type_index = self.wasm_type(wasm_parameters, definition.result_type)
        bodies = self.runtime_bodies() + [WasmFunctionCompiler(self, definition).compile_body() for definition in self.definitions]

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
            wasm_string("env") + wasm_string("bearer_alloc") + b"\x00" + uleb(allocator_type),
            wasm_string("env") + wasm_string("bearer_free") + b"\x00" + uleb(retain_type),
        ]
        exports = [
            wasm_string(definition.export_name) + b"\x00" + uleb(definition.function_index)
            for definition in self.definitions if definition.export_name
        ]
        mem_info = uleb(len(self.data)) + uleb(3) + uleb(0) + uleb(0)
        data_segment = b"\x00\x23\x00\x0b" + uleb(len(self.data)) + bytes(self.data)
        abi = (
            "format=bearer-wasm-unit-abi-v1\n"
            f"unit_abi_version={self.abi_version}\n"
            "toolchain=capyc-direct-wasm-phase1\n"
            f"source={self.source}\n"
        ).encode("utf-8")
        module_name_payload = wasm_string(self.module_name)
        name_section = b"\x00" + uleb(len(module_name_payload)) + module_name_payload
        wasm = b"\x00asm\x01\x00\x00\x00" + b"".join([
            wasm_custom("dylink.0", b"\x01" + uleb(len(mem_info)) + mem_info),
            wasm_section(1, wasm_vector([encode_type(signature) for signature in self.types])),
            wasm_section(2, wasm_vector(imports)),
            wasm_section(3, wasm_vector([
                uleb(retain_type), uleb(retain_type), uleb(clone_type),
                *[uleb(definition.type_index) for definition in self.definitions],
            ])),
            wasm_section(6, wasm_vector([b"\x7f\x01\x41\x00\x0b"])),
            wasm_section(7, wasm_vector(exports)),
            wasm_section(10, wasm_vector(bodies)),
            wasm_section(11, wasm_vector([data_segment])),
            wasm_custom("name", name_section),
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
