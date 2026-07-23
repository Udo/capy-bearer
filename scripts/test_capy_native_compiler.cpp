#include "compiler.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <string_view>

int main()
{
	capy::CompileOptions options;
	options.source_path = "native-test.capy";
	options.module_name = "native-test.wasm";
	options.abi_version = 11;
	auto result = capy::compile_bearer_unit("function CLI { print(1, \"ok\") }\n", options);
	assert(result.wasm.size() >= 4 && result.wasm[0] == 0 && result.wasm[1] == 'a' && result.wasm[2] == 's' && result.wasm[3] == 'm');
	auto validated = capy::wasm::validate_bearer_unit(result.wasm, {.bearer_abi_version = "11"});
	assert(validated.valid);
	assert(validated.bearer_module == "native-test.wasm");
	assert(result.source_map.starts_with("BEARER_SOURCE_MAP_V1\tnative-test.wasm\n"));
	std::ofstream output("/tmp/capy-native.wasm", std::ios::binary);
	output.write(reinterpret_cast<const char*>(result.wasm.data()), result.wasm.size());

	constexpr std::string_view arc_cases[] = {
		"function CLI { var value := clone(\"x\") }\n",
		"function CLI { var value := clone(\"x\"); value = clone(\"y\") }\n",
		"function borrow(x : string) string { return x }\nfunction CLI { var value := borrow(\"x\") }\n",
		"function make() string { return clone(\"x\") }\nfunction CLI { var value := make() }\n",
		"function early(x : string) string { var discarded := clone(\"x\"); if true { return x }; return discarded }\nfunction CLI { var value := early(\"x\") "
		"}\n",
		"function CLI { var values := [clone(\"x\"), clone(\"y\")]; var first : string = values[0] }\n",
		"function CLI { var value := clone(\"x\"); print(arc_live()) }\n",
	};
	for (const auto source : arc_cases)
	{
		const auto compiled = capy::compile_bearer_unit(source, options);
		assert(capy::wasm::validate_bearer_unit(compiled.wasm, {.bearer_abi_version = "11"}).valid);
	}

	constexpr std::string_view generic_source = "function identity(value : any) value::type { value }\n"
												"function choose(value : any) value::type { value }\n"
												"function choose(value : s32) value::type { value + 1 }\n"
												"function countdown(value : any) value::type { if value == 0 { return value }; return countdown(value - 1) }\n"
												"function CLI { print(identity(7), choose(4), countdown(3), identity((1, clone(\"x\")))[1], 2 as bool) }\n";
	const auto generic = capy::compile_bearer_unit(generic_source, options);
	assert(capy::wasm::validate_bearer_unit(generic.wasm, {.bearer_abi_version = "11"}).valid);
	const auto returning = capy::compile_bearer_unit(
		"function choose(value : bool) s32 { if value { return 1 } else { return 2 } }\nfunction CLI { print(choose(true)) }\n", options);
	assert(capy::wasm::validate_bearer_unit(returning.wasm, {.bearer_abi_version = "11"}).valid);
	const auto boundary = capy::compile_bearer_unit("function CLI { print(-2147483648, 2147483647) }\n", options);
	assert(capy::wasm::validate_bearer_unit(boundary.wasm, {.bearer_abi_version = "11"}).valid);
	const auto wide =
		capy::compile_bearer_unit("function next(value : u64) u64 { value + 1u64 }\nfunction half(value : f64) f64 { value / 2.0 }\n"
								  "function CLI { var fn : function(value : u64) u64 = next; print(fn(4u64), half(3.0), -1 as u64, 9.0 as s32) }\n",
								  options);
	assert(capy::wasm::validate_bearer_unit(wide.wasm, {.bearer_abi_version = "11"}).valid);
	const auto marker_collision = capy::compile_bearer_unit("function CLI { 1509949440; print([1][1]) }\n", options);
	assert(capy::wasm::validate_bearer_unit(marker_collision.wasm, {.bearer_abi_version = "11"}).valid);
	assert(marker_collision.source_map.find("\t1\t1\t37\n") != std::string::npos);
	for (const auto source : {
			 "function square(value : any) value::type { value * value }\nfunction CLI { square(clone(\"x\")) }\n",
			 "function choose(a : any, b : s32) a::type { a }\nfunction choose(a : s32, b : any) b::type { b }\nfunction CLI { choose(1, 1) }\n",
			 "function incomplete(value : bool) s32 { if value { return 1 } }\nfunction CLI { print(incomplete(false)) }\n",
			 "function CLI { print(2147483648) }\n",
			 "function CLI { print(-2147483649) }\n",
			 "function CLI { print(999999999999999999999999999999999999) }\n",
			 "function CLI { value := 1; value := 2 }\n",
			 "function CLI { print(1 && true) }\n",
			 "function CLI { false && (hidden := true); print(hidden) }\n",
			 "function CLI { var held := clone(\"old\"); var replace := function() { held = clone(\"new\") } }\n",
		 })
	{
		try
		{
			capy::compile_bearer_unit(source, options);
			assert(false);
		}
		catch (const capy::Error&)
		{
		}
	}
	for (const auto& [source, expected] : {
			 std::pair{"function CLI { (1 + 2) := 3 }\n", "inferred declaration target must be a local name"},
			 std::pair{"function CLI { print(1 && true) }\n", "logical operators require bool operands"},
			 std::pair{"function CLI { var held := clone(\"old\"); var replace := function() { held = clone(\"new\") } }\n", "unknown local 'held'"},
			 std::pair{"function CLI { var values := [1u64, 2u64] }\n", "not yet supported in array layouts"},
			 std::pair{"function CLI { var values := (1s64, 2) }\n", "s64, u64, and f64 are not yet supported in tuple layouts"},
			 std::pair{"struct Wide { value : f64 }\nfunction CLI {}\n", "not yet supported in struct layouts"},
			 std::pair{"function CLI { var value := 1u64; var closure := function() u64 { value } }\n", "not yet supported in captured closure layouts"},
		 })
	{
		try
		{
			capy::compile_bearer_unit(source, options);
			assert(false);
		}
		catch (const capy::Error& error)
		{
			assert(error.message.find(expected) != std::string::npos);
			assert(error.message.find("unsupported operator") == std::string::npos);
		}
	}

	bool cancelled = false;
	options.cancelled = [&] { return cancelled; };
	cancelled = true;
	try
	{
		capy::compile_bearer_unit("function CLI {}", options);
		assert(false);
	}
	catch (const capy::Error&)
	{
	}

	unsigned cancellation_polls = 0;
	options.cancelled = [&] { return ++cancellation_polls == 2; };
	const std::string large_source = "function CLI { print(\"" + std::string(20000, 'x') + "\") }\n";
	try
	{
		capy::compile_bearer_unit(large_source, options);
		assert(false);
	}
	catch (const capy::Error&)
	{
	}
	assert(cancellation_polls == 2);
	std::cout << "native Capy compiler smoke tests passed\n";
}
