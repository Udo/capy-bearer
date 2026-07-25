#pragma once

#include <cstdint>
#include <string>

namespace capy_backtrace
{
constexpr std::uint32_t metadata_header_bytes = 16;
constexpr std::uint32_t max_metadata_record_bytes = 64 * 1024;
constexpr std::size_t max_escaped_field_bytes = 4096;

inline std::uint32_t load_u32_le(const std::string& bytes, std::size_t offset)
{
	return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
		static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8 |
		static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16 |
		static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24;
}

inline std::string escape_field(const std::string& text)
{
	std::string result;
	for(unsigned char byte : text)
	{
		const bool printable = byte >= 0x20 && byte <= 0x7e;
		const std::size_t width = printable ? 1 : 4;
		if(result.size() + width > max_escaped_field_bytes)
		{
			result += "...[truncated]";
			break;
		}
		if(printable)
			result.push_back(static_cast<char>(byte));
		else
		{
			static constexpr char hex[] = "0123456789ABCDEF";
			result += "\\x";
			result.push_back(hex[byte >> 4]);
			result.push_back(hex[byte & 15]);
		}
	}
	return result;
}

inline bool format_record(const std::string& record, std::string& output)
{
	if(record.size() < metadata_header_bytes)
		return false;
	const std::uint32_t function_size = load_u32_le(record, 0), path_size = load_u32_le(record, 4);
	const std::size_t payload_size = record.size() - metadata_header_bytes;
	if(function_size > payload_size || path_size != payload_size - function_size)
		return false;
	const std::string function = record.substr(metadata_header_bytes, function_size);
	const std::string path = record.substr(metadata_header_bytes + function_size, path_size);
	output = escape_field(function) + " at " + escape_field(path) + ":" + std::to_string(load_u32_le(record, 8));
	const std::uint32_t column = load_u32_le(record, 12);
	if(column)
		output += ":" + std::to_string(column);
	return true;
}
}
