#include "wasm.h"

#include <algorithm>

namespace capy::wasm
{
namespace
{

struct Reader
{
	Reader(const Bytes& bytes, std::size_t begin, std::size_t end) : bytes_(bytes), pos_(begin), end_(end) {}

	bool byte(std::uint8_t& value)
	{
		if (pos_ == end_)
			return false;
		value = bytes_[pos_++];
		return true;
	}

	bool uleb(std::uint32_t& value)
	{
		value = 0;
		for (std::uint32_t i = 0; i != 5; ++i)
		{
			std::uint8_t byte_value;
			if (!byte(byte_value))
				return false;
			if (i == 4 && (byte_value & 0x70) != 0)
				return false;
			value |= std::uint32_t(byte_value & 0x7f) << (i * 7);
			if ((byte_value & 0x80) == 0)
				return true;
		}
		return false;
	}

	bool string(std::string& value)
	{
		std::uint32_t length;
		if (!uleb(length) || length > remaining())
			return false;
		value.assign(reinterpret_cast<const char*>(bytes_.data() + pos_), length);
		pos_ += length;
		return true;
	}

	std::size_t pos() const
	{
		return pos_;
	}
	std::size_t end() const
	{
		return end_;
	}
	std::size_t remaining() const
	{
		return end_ - pos_;
	}
	void seek(std::size_t position)
	{
		pos_ = position;
	}

	const Bytes& bytes_;
	std::size_t pos_;
	std::size_t end_;
};

bool skip_limits(Reader& reader)
{
	std::uint32_t flags, minimum, maximum;
	return reader.uleb(flags) && (flags & ~1u) == 0 && reader.uleb(minimum) && (!(flags & 1) || reader.uleb(maximum));
}

bool parse_imports(Reader& reader, ValidationResult& result)
{
	std::uint32_t count;
	if (!reader.uleb(count))
		return false;
	for (std::uint32_t i = 0; i != count; ++i)
	{
		Import imported;
		if (!reader.string(imported.module) || !reader.string(imported.name) || !reader.byte(imported.kind))
			return false;
		switch (imported.kind)
		{
		case 0:
		{
			std::uint32_t type;
			if (!reader.uleb(type))
				return false;
			break;
		}
		case 1:
		{
			std::uint8_t type;
			if (!reader.byte(type) || !skip_limits(reader))
				return false;
			break;
		}
		case 2:
			if (!skip_limits(reader))
				return false;
			break;
		case 3:
		{
			std::uint8_t type, mutability;
			if (!reader.byte(type) || !reader.byte(mutability))
				return false;
			break;
		}
		default:
			return false;
		}
		if (imported.module == "env" && imported.name == "memory")
			result.imports_memory = imported.kind == 2;
		if (imported.module == "env" && imported.name == "__memory_base")
			result.imports_memory_base = imported.kind == 3;
		if (imported.module == "env" && imported.name == "__indirect_function_table")
			result.imports_indirect_function_table = imported.kind == 1;
		if (imported.module == "env" && imported.name == "__stack_pointer")
			result.imports_stack_pointer = imported.kind == 3;
		if (imported.module == "env" && imported.name == "__table_base")
			result.imports_table_base = imported.kind == 3;
		result.imports.push_back(std::move(imported));
	}
	return reader.pos() == reader.end();
}

bool parse_exports(Reader& reader, ValidationResult& result)
{
	std::uint32_t count;
	if (!reader.uleb(count))
		return false;
	for (std::uint32_t i = 0; i != count; ++i)
	{
		Export exported;
		std::uint32_t index;
		if (!reader.string(exported.name) || !reader.byte(exported.kind) || exported.kind > 3 || !reader.uleb(index))
			return false;
		for (const Export& previous : result.exports)
			if (previous.name == exported.name)
				return false;
		if (exported.name == "bearer_alloc" || exported.name == "bearer_free")
			result.has_forbidden_allocator_export = true;
		result.exports.push_back(std::move(exported));
	}
	return reader.pos() == reader.end();
}

bool parse_dylink(Reader& reader, DylinkMemoryInfo& info)
{
	while (reader.pos() != reader.end())
	{
		std::uint8_t id;
		std::uint32_t size;
		if (!reader.byte(id) || !reader.uleb(size) || size > reader.remaining())
			return false;
		const std::size_t end = reader.pos() + size;
		if (id == 1)
		{
			if (info.found || !reader.uleb(info.memory_size) || !reader.uleb(info.memory_alignment) || !reader.uleb(info.table_size) ||
				!reader.uleb(info.table_alignment) || reader.pos() != end || info.memory_alignment >= 32 || info.table_alignment >= 32 ||
				info.memory_size >= (1u << 31) || info.table_size >= (1u << 31))
				return false;
			info.found = true;
		}
		reader.seek(end);
	}
	return info.found;
}

bool abi_value(const std::string& text, const std::string& key, std::string& value)
{
	const std::string prefix = key + "=";
	std::size_t start = 0;
	while (start < text.size())
	{
		const std::size_t end = text.find('\n', start);
		const std::size_t line_end = end == std::string::npos ? text.size() : end;
		if (text.compare(start, prefix.size(), prefix) == 0)
		{
			value.assign(text, start + prefix.size(), line_end - start - prefix.size());
			return true;
		}
		start = line_end + 1;
	}
	return false;
}

ValidationResult fail(ValidationResult result, const char* error)
{
	result.error = error;
	return result;
}

} // namespace

void append_uleb(Bytes& out, std::uint32_t value)
{
	do
	{
		std::uint8_t byte = value & 0x7f;
		value >>= 7;
		out.push_back(value ? byte | 0x80 : byte);
	} while (value);
}

void append_sleb32(Bytes& out, std::int32_t value)
{
	std::int64_t remaining = value;
	bool more;
	do
	{
		std::uint8_t byte = static_cast<std::uint8_t>(remaining) & 0x7f;
		remaining = remaining >= 0 ? remaining / 128 : -((-remaining + 127) / 128);
		more = !((remaining == 0 && (byte & 0x40) == 0) || (remaining == -1 && (byte & 0x40) != 0));
		out.push_back(more ? byte | 0x80 : byte);
	} while (more);
}

void append_string(Bytes& out, const std::string& value)
{
	append_uleb(out, static_cast<std::uint32_t>(value.size()));
	out.insert(out.end(), value.begin(), value.end());
}

void append_vector(Bytes& out, const std::vector<Bytes>& values)
{
	append_uleb(out, static_cast<std::uint32_t>(values.size()));
	for (const Bytes& value : values)
		out.insert(out.end(), value.begin(), value.end());
}

void append_section(Bytes& out, std::uint8_t id, const Bytes& payload)
{
	out.push_back(id);
	append_uleb(out, static_cast<std::uint32_t>(payload.size()));
	out.insert(out.end(), payload.begin(), payload.end());
}

void append_custom_section(Bytes& out, const std::string& name, const Bytes& content)
{
	Bytes payload;
	append_string(payload, name);
	payload.insert(payload.end(), content.begin(), content.end());
	append_section(out, 0, payload);
}

ValidationResult validate_bearer_unit(const Bytes& module, const ValidationOptions& options)
{
	ValidationResult result;
	if (module.size() > options.max_module_bytes)
		return fail(std::move(result), "module exceeds byte limit");
	if (module.size() < 8 || !std::equal(module.begin(), module.begin() + 4, "\0asm") || module[4] != 1 || module[5] != 0 || module[6] != 0 || module[7] != 0)
		return fail(std::move(result), "not a wasm v1 module");

	bool dylink_seen = false, abi_seen = false, module_seen = false;
	std::uint8_t last_standard_section = 0;
	Reader sections(module, 8, module.size());
	while (sections.pos() != sections.end())
	{
		std::uint8_t id;
		std::uint32_t size;
		if (!sections.byte(id) || !sections.uleb(size) || size > sections.remaining())
			return fail(std::move(result), "malformed wasm section header");
		const std::size_t end = sections.pos() + size;
		if (id != 0 && (id > 12 || id <= last_standard_section))
			return fail(std::move(result), "invalid wasm section order");
		if (id != 0)
			last_standard_section = id;
		Reader payload(module, sections.pos(), end);
		if (id == 0)
		{
			std::string name;
			if (!payload.string(name))
				return fail(std::move(result), "malformed custom section name");
			if ((name == "dylink.0" || name == "bearer.abi" || name == "bearer.module") && size > options.max_metadata_bytes)
				return fail(std::move(result), "metadata section exceeds byte limit");
			if (name == "dylink.0")
			{
				if (dylink_seen || !parse_dylink(payload, result.dylink))
					return fail(std::move(result), "invalid dylink.0 mem_info");
				dylink_seen = true;
			}
			else if (name == "bearer.abi")
			{
				if (abi_seen)
					return fail(std::move(result), "duplicate bearer.abi custom section");
				abi_seen = true;
				const std::string text(reinterpret_cast<const char*>(module.data() + payload.pos()), payload.remaining());
				std::string format;
				if (!abi_value(text, "format", format) || format != "bearer-wasm-unit-abi-v1" ||
					!abi_value(text, "unit_abi_version", result.bearer_abi_version) || !abi_value(text, "toolchain", result.bearer_toolchain))
					return fail(std::move(result), "invalid bearer.abi custom section");
			}
			else if (name == "bearer.module")
			{
				if (module_seen || payload.remaining() == 0)
					return fail(std::move(result), "invalid bearer.module custom section");
				module_seen = true;
				result.bearer_module.assign(reinterpret_cast<const char*>(module.data() + payload.pos()), payload.remaining());
			}
		}
		else if (id == 2 && !parse_imports(payload, result))
			return fail(std::move(result), "malformed import section");
		else if (id == 7 && !parse_exports(payload, result))
			return fail(std::move(result), "malformed export section");
		sections.seek(end);
	}
	if (!dylink_seen || !result.dylink.found)
		return fail(std::move(result), "missing dylink.0 mem_info");
	if (!abi_seen)
		return fail(std::move(result), "missing bearer.abi custom section");
	if (!options.bearer_abi_version.empty() && result.bearer_abi_version != options.bearer_abi_version)
		return fail(std::move(result), "unexpected bearer ABI version");
	if (!module_seen)
		return fail(std::move(result), "missing bearer.module custom section");
	if (result.has_forbidden_allocator_export)
		return fail(std::move(result), "forbidden allocator export");
	if (!result.imports_memory || !result.imports_memory_base)
		return fail(std::move(result), "missing required env import");
	for (const Import& imported : result.imports)
	{
		if (imported.module == "env" && imported.name == "__indirect_function_table" && imported.kind != 1)
			return fail(std::move(result), "invalid indirect function table import");
		if (imported.module == "env" && (imported.name == "__stack_pointer" || imported.name == "__table_base") && imported.kind != 3)
			return fail(std::move(result), "invalid runtime global import");
		if (imported.module.rfind("GOT.", 0) == 0 && imported.kind != 3)
			return fail(std::move(result), "invalid GOT import");
	}
	result.valid = true;
	return result;
}

} // namespace capy::wasm
