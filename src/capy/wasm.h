#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace capy::wasm
{

using Bytes = std::vector<std::uint8_t>;

void append_uleb(Bytes& out, std::uint32_t value);
void append_sleb32(Bytes& out, std::int32_t value);
void append_string(Bytes& out, const std::string& value);
void append_vector(Bytes& out, const std::vector<Bytes>& values);
void append_section(Bytes& out, std::uint8_t id, const Bytes& payload);
void append_custom_section(Bytes& out, const std::string& name, const Bytes& content);

template <typename T, typename Encode> void append_vector(Bytes& out, const std::vector<T>& values, Encode encode)
{
	append_uleb(out, static_cast<std::uint32_t>(values.size()));
	for (const T& value : values)
		encode(out, value);
}

struct Import
{
	std::string module;
	std::string name;
	std::uint8_t kind = 0;
};

struct Export
{
	std::string name;
	std::uint8_t kind = 0;
};

struct DylinkMemoryInfo
{
	bool found = false;
	std::uint32_t memory_size = 0;
	std::uint32_t memory_alignment = 0;
	std::uint32_t table_size = 0;
	std::uint32_t table_alignment = 0;
};

struct ValidationOptions
{
	std::string bearer_abi_version;
	std::size_t max_module_bytes = 16 * 1024 * 1024;
	std::size_t max_metadata_bytes = 1024 * 1024;
};

struct ValidationResult
{
	bool valid = false;
	std::string error;
	DylinkMemoryInfo dylink;
	std::string bearer_abi_version;
	std::string bearer_toolchain;
	std::string bearer_module;
	std::vector<Import> imports;
	std::vector<Export> exports;
	bool imports_memory = false;
	bool imports_memory_base = false;
	bool imports_indirect_function_table = false;
	bool imports_stack_pointer = false;
	bool imports_table_base = false;
	bool has_forbidden_allocator_export = false;
};

ValidationResult validate_bearer_unit(const Bytes& module, const ValidationOptions& options = {});

} // namespace capy::wasm
