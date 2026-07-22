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
	for (const auto source : {
			 "function square(value : any) value::type { value * value }\nfunction CLI { square(clone(\"x\")) }\n",
			 "function choose(a : any, b : s32) a::type { a }\nfunction choose(a : s32, b : any) b::type { b }\nfunction CLI { choose(1, 1) }\n",
			 "function incomplete(value : bool) s32 { if value { return 1 } }\nfunction CLI { print(incomplete(false)) }\n",
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
