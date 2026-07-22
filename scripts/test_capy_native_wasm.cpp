#include "../src/capy/wasm.h"

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>

using capy::wasm::Bytes;

static Bytes bytes(std::initializer_list<std::uint8_t> value)
{
	return Bytes(value);
}

static void expect(const Bytes& actual, std::initializer_list<std::uint8_t> expected)
{
	assert(actual == Bytes(expected));
}

static void imported(Bytes& payload, const std::string& module, const std::string& name, std::uint8_t kind)
{
	capy::wasm::append_string(payload, module);
	capy::wasm::append_string(payload, name);
	payload.push_back(kind);
	if (kind == 0)
		capy::wasm::append_uleb(payload, 0);
	if (kind == 1)
	{
		payload.push_back(0x70);
		capy::wasm::append_uleb(payload, 0);
		capy::wasm::append_uleb(payload, 0);
	}
	if (kind == 2)
	{
		capy::wasm::append_uleb(payload, 0);
		capy::wasm::append_uleb(payload, 1);
	}
	if (kind == 3)
	{
		payload.push_back(0x7f);
		payload.push_back(0);
	}
}

static Bytes bearer_unit(bool duplicate_export = false, bool unexpected_import = false)
{
	Bytes module = bytes({0, 'a', 's', 'm', 1, 0, 0, 0});
	Bytes dylink;
	capy::wasm::append_string(dylink, "dylink.0");
	Bytes memory_info;
	for (std::uint32_t value : {1u, 0u, 0u, 0u})
		capy::wasm::append_uleb(memory_info, value);
	dylink.push_back(1);
	capy::wasm::append_uleb(dylink, static_cast<std::uint32_t>(memory_info.size()));
	dylink.insert(dylink.end(), memory_info.begin(), memory_info.end());
	capy::wasm::append_section(module, 0, dylink);

	const std::string abi = "format=bearer-wasm-unit-abi-v1\nunit_abi_version=7\ntoolchain=capy\n";
	capy::wasm::append_custom_section(module, "bearer.abi", Bytes(abi.begin(), abi.end()));
	capy::wasm::append_custom_section(module, "bearer.module", bytes({'t'}));

	Bytes imports;
	capy::wasm::append_uleb(imports, unexpected_import ? 3 : 2);
	imported(imports, "env", "memory", 2);
	imported(imports, "env", "__memory_base", 3);
	if (unexpected_import)
		imported(imports, "wasi_snapshot_preview1", "fd_write", 0);
	capy::wasm::append_section(module, 2, imports);

	Bytes exports;
	capy::wasm::append_uleb(exports, duplicate_export ? 2 : 1);
	capy::wasm::append_string(exports, "render");
	exports.push_back(0);
	capy::wasm::append_uleb(exports, 0);
	if (duplicate_export)
	{
		capy::wasm::append_string(exports, "render");
		exports.push_back(0);
		capy::wasm::append_uleb(exports, 1);
	}
	capy::wasm::append_section(module, 7, exports);
	return module;
}

int main()
{
	for (const auto& test : std::initializer_list<std::pair<std::uint32_t, Bytes>>{
			 {0, bytes({0})}, {127, bytes({127})}, {128, bytes({0x80, 1})}, {std::numeric_limits<std::uint32_t>::max(), bytes({0xff, 0xff, 0xff, 0xff, 0x0f})}})
	{
		Bytes encoded;
		capy::wasm::append_uleb(encoded, test.first);
		assert(encoded == test.second);
	}
	for (const auto& test :
		 std::initializer_list<std::pair<std::int32_t, Bytes>>{{0, bytes({0})},
															   {-1, bytes({0x7f})},
															   {63, bytes({0x3f})},
															   {64, bytes({0xc0, 0})},
															   {-64, bytes({0x40})},
															   {-65, bytes({0xbf, 0x7f})},
															   {std::numeric_limits<std::int32_t>::max(), bytes({0xff, 0xff, 0xff, 0xff, 0x07})},
															   {std::numeric_limits<std::int32_t>::min(), bytes({0x80, 0x80, 0x80, 0x80, 0x78})}})
	{
		Bytes encoded;
		capy::wasm::append_sleb32(encoded, test.first);
		assert(encoded == test.second);
	}

	Bytes encoded;
	capy::wasm::append_string(encoded, "x");
	capy::wasm::append_vector(encoded, std::vector<Bytes>{bytes({1}), bytes({2, 3})});
	capy::wasm::append_section(encoded, 1, bytes({4}));
	capy::wasm::append_custom_section(encoded, "x", bytes({5}));
	expect(encoded, {1, 'x', 2, 1, 2, 3, 1, 1, 4, 0, 3, 1, 'x', 5});

	capy::wasm::ValidationOptions options;
	options.bearer_abi_version = "7";
	auto result = capy::wasm::validate_bearer_unit(bearer_unit(), options);
	assert(result.valid && result.dylink.found && result.imports_memory && result.imports_memory_base);
	assert(result.bearer_module == "t" && result.bearer_toolchain == "capy" && result.exports.size() == 1);

	Bytes bad_magic = bearer_unit();
	bad_magic[0] = 1;
	assert(!capy::wasm::validate_bearer_unit(bad_magic, options).valid);
	Bytes missing_dylink = bearer_unit();
	missing_dylink[11] = 'x';
	assert(!capy::wasm::validate_bearer_unit(missing_dylink, options).valid);
	Bytes truncated = bearer_unit();
	truncated.pop_back();
	assert(!capy::wasm::validate_bearer_unit(truncated, options).valid);
	Bytes oversized_leb = bytes({0, 'a', 's', 'm', 1, 0, 0, 0, 1, 0x80, 0x80, 0x80, 0x80, 0x10});
	assert(!capy::wasm::validate_bearer_unit(oversized_leb, options).valid);
	assert(!capy::wasm::validate_bearer_unit(bearer_unit(true), options).valid);
	assert(capy::wasm::validate_bearer_unit(bearer_unit(false, true), options).valid);
	options.bearer_abi_version = "8";
	assert(!capy::wasm::validate_bearer_unit(bearer_unit(), options).valid);
	options.bearer_abi_version = "7";
	options.max_module_bytes = 8;
	assert(!capy::wasm::validate_bearer_unit(bearer_unit(), options).valid);
}
