#include "zip.h"

#include <cstring>

extern "C" {
#include "../3rdparty/miniz/miniz.c"
#include "../3rdparty/miniz/miniz_tdef.c"
#include "../3rdparty/miniz/miniz_tinfl.c"
#include "../3rdparty/miniz/miniz_zip.c"
}

String zip_error(String api, String detail)
{
	return(api + "(): " + detail);
}

void archive_check_size(String api, String label, u64 size, String config_key, u64 fallback)
{
	u64 limit = to_u64(context->server->config[config_key], fallback);
	if(limit > 0 && size > limit)
		throw std::runtime_error(zip_error(api, label + " exceeds configured limit"));
}

bool zip_entry_name_safe(String name)
{
	if(name == "")
		return(false);
	if(name[0] == '/' || name[0] == '\\')
		return(false);
	if(name.find(":") != String::npos)
		return(false);

	for(char c : name)
	{
		if(c == '\0' || (unsigned char)c < 0x20)
			return(false);
	}

	auto parts = split(replace(name, "\\", "/"), "/");
	for(auto part : parts)
	{
		if(part == "..")
			return(false);
	}
	return(true);
}

String zip_normalize_entry_name(String name)
{
	name = replace(name, "\\", "/");
	while(name.find("//") != String::npos)
		name = replace(name, "//", "/");
	return(name);
}

bool zip_ensure_parent_directory(String path)
{
	String parent = dirname(path);
	if(parent == "" || parent == "." || parent == "/")
		return(true);
	if(file_exists(parent))
		return(true);
	return(mkdir(parent));
}

void zip_add_file_info(DValue& files, mz_zip_archive* archive, mz_uint index)
{
	mz_zip_archive_file_stat stat;
	std::memset(&stat, 0, sizeof(stat));
	if(!mz_zip_reader_file_stat(archive, index, &stat))
		throw std::runtime_error(zip_error("zip_list", "could not read file metadata at index " + std::to_string((u64)index)));

	DValue item;
	item["name"] = String(stat.m_filename);
	item["index"] = (f64)index;
	item["size"] = (f64)stat.m_uncomp_size;
	item["compressed_size"] = (f64)stat.m_comp_size;
	item["is_directory"].set_bool(mz_zip_reader_is_file_a_directory(archive, index));
	item["method"] = (f64)stat.m_method;
	files.push(item);
}

DValue zip_list(String zip_file_name)
{
	mz_zip_archive archive;
	std::memset(&archive, 0, sizeof(archive));
	if(!mz_zip_reader_init_file(&archive, zip_file_name.c_str(), 0))
		throw std::runtime_error(zip_error("zip_list", "could not open " + zip_file_name));

	DValue result;
	try
	{
		mz_uint count = mz_zip_reader_get_num_files(&archive);
		archive_check_size("zip_list", "entry count", count, "ARCHIVE_MAX_ZIP_ENTRIES", 4096);
		result["file"] = zip_file_name;
		result["count"] = (f64)count;
		result["entries"].set_array();
		for(mz_uint i = 0; i < count; i++)
			zip_add_file_info(result["entries"], &archive, i);
		mz_zip_reader_end(&archive);
		return(result);
	}
	catch(...)
	{
		mz_zip_reader_end(&archive);
		throw;
	}
}

String zip_read(String zip_file_name, String entry_name)
{
	entry_name = zip_normalize_entry_name(entry_name);
	if(!zip_entry_name_safe(entry_name))
		throw std::runtime_error(zip_error("zip_read", "unsafe or empty entry name '" + entry_name + "'"));

	mz_zip_archive archive;
	std::memset(&archive, 0, sizeof(archive));
	if(!mz_zip_reader_init_file(&archive, zip_file_name.c_str(), 0))
		throw std::runtime_error(zip_error("zip_read", "could not open " + zip_file_name));

	int file_index = mz_zip_reader_locate_file(&archive, entry_name.c_str(), NULL, 0);
	if(file_index < 0)
	{
		mz_zip_reader_end(&archive);
		throw std::runtime_error(zip_error("zip_read", "entry not found or not readable: " + entry_name));
	}
	mz_zip_archive_file_stat stat;
	std::memset(&stat, 0, sizeof(stat));
	if(!mz_zip_reader_file_stat(&archive, (mz_uint)file_index, &stat))
	{
		mz_zip_reader_end(&archive);
		throw std::runtime_error(zip_error("zip_read", "entry not found or not readable: " + entry_name));
	}
	archive_check_size("zip_read", "uncompressed entry", stat.m_uncomp_size, "ARCHIVE_MAX_OUTPUT_BYTES", 64 * 1024 * 1024);

	size_t size = 0;
	void* data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
	if(!data)
	{
		mz_zip_reader_end(&archive);
		throw std::runtime_error(zip_error("zip_read", "entry not found or not readable: " + entry_name));
	}
	String result((char*)data, size);
	mz_free(data);
	mz_zip_reader_end(&archive);
	return(result);
}

String zip_entry_content(DValue item)
{
	DValue* content = item.key("content");
	if(content)
		return(content->to_string());
	DValue* file_name = item.key("file");
	if(file_name)
		return(file_get_contents(file_name->to_string()));
	return(item.to_string());
}

String zip_entry_name(String key, DValue item)
{
	DValue* name = item.key("name");
	if(name)
		return(name->to_string());
	return(key);
}

void zip_add_entry(mz_zip_archive* archive, String name, String content)
{
	archive_check_size("zip_create", "entry content", content.size(), "ARCHIVE_MAX_INPUT_BYTES", 64 * 1024 * 1024);
	name = zip_normalize_entry_name(name);
	if(!zip_entry_name_safe(name))
		throw std::runtime_error(zip_error("zip_create", "unsafe or empty entry name '" + name + "'"));
	if(!mz_zip_writer_add_mem(archive, name.c_str(), content.data(), content.size(), MZ_BEST_COMPRESSION))
		throw std::runtime_error(zip_error("zip_create", "could not add entry " + name));
}

bool zip_create(String zip_file_name, DValue entries)
{
	if(!zip_ensure_parent_directory(zip_file_name))
		throw std::runtime_error(zip_error("zip_create", "could not create parent directory for " + zip_file_name));

	mz_zip_archive archive;
	std::memset(&archive, 0, sizeof(archive));
	if(!mz_zip_writer_init_file(&archive, zip_file_name.c_str(), 0))
		throw std::runtime_error(zip_error("zip_create", "could not open " + zip_file_name + " for writing"));

	try
	{
		u64 entry_count = 0;
		entries.each([&](DValue item, String key)
		{
			entry_count++;
			archive_check_size("zip_create", "entry count", entry_count, "ARCHIVE_MAX_ZIP_ENTRIES", 4096);
			String name = zip_entry_name(key, item);
			String content = zip_entry_content(item);
			zip_add_entry(&archive, name, content);
		});
		if(!mz_zip_writer_finalize_archive(&archive))
			throw std::runtime_error(zip_error("zip_create", "could not finalize " + zip_file_name));
		mz_zip_writer_end(&archive);
		return(true);
	}
	catch(...)
	{
		mz_zip_writer_end(&archive);
		throw;
	}
}

void gz_append_u32_le(String& out, u32 value)
{
	out.push_back((char)(value & 0xff));
	out.push_back((char)((value >> 8) & 0xff));
	out.push_back((char)((value >> 16) & 0xff));
	out.push_back((char)((value >> 24) & 0xff));
}

u32 gz_read_u32_le(String src, size_t offset)
{
	return(
		((u32)(u8)src[offset]) |
		((u32)(u8)src[offset + 1] << 8) |
		((u32)(u8)src[offset + 2] << 16) |
		((u32)(u8)src[offset + 3] << 24)
	);
}

void gz_skip_zero_terminated(String compressed, size_t& offset)
{
	while(offset < compressed.size() && compressed[offset] != '\0')
		offset++;
	if(offset >= compressed.size())
		throw std::runtime_error("gz_uncompress(): truncated gzip header");
	offset++;
}

size_t gz_deflate_offset(String compressed)
{
	if(compressed.size() < 18)
		throw std::runtime_error("gz_uncompress(): compressed data is too short");
	if((u8)compressed[0] != 0x1f || (u8)compressed[1] != 0x8b)
		throw std::runtime_error("gz_uncompress(): missing gzip header");
	if((u8)compressed[2] != 8)
		throw std::runtime_error("gz_uncompress(): unsupported gzip compression method");

	u8 flags = (u8)compressed[3];
	if(flags & 0xe0)
		throw std::runtime_error("gz_uncompress(): gzip header uses reserved flags");

	size_t offset = 10;
	if(flags & 0x04)
	{
		if(offset + 2 > compressed.size())
			throw std::runtime_error("gz_uncompress(): truncated gzip extra header");
		u16 extra_len = ((u16)(u8)compressed[offset]) | ((u16)(u8)compressed[offset + 1] << 8);
		offset += 2 + extra_len;
		if(offset > compressed.size())
			throw std::runtime_error("gz_uncompress(): truncated gzip extra data");
	}
	if(flags & 0x08)
		gz_skip_zero_terminated(compressed, offset);
	if(flags & 0x10)
		gz_skip_zero_terminated(compressed, offset);
	if(flags & 0x02)
	{
		offset += 2;
		if(offset > compressed.size())
			throw std::runtime_error("gz_uncompress(): truncated gzip header crc");
	}
	if(offset + 8 > compressed.size())
		throw std::runtime_error("gz_uncompress(): missing gzip footer");
	return(offset);
}

String gz_compress(String src)
{
	archive_check_size("gz_compress", "input", src.size(), "ARCHIVE_MAX_INPUT_BYTES", 64 * 1024 * 1024);
	mz_stream stream;
	std::memset(&stream, 0, sizeof(stream));
	int status = mz_deflateInit2(&stream, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY);
	if(status != MZ_OK)
		throw std::runtime_error("gz_compress(): could not initialize compressor");

	mz_ulong bound = mz_deflateBound(&stream, src.size());
	String deflated;
	deflated.resize(bound);
	stream.next_in = (const unsigned char*)src.data();
	stream.avail_in = src.size();
	stream.next_out = (unsigned char*)deflated.data();
	stream.avail_out = deflated.size();
	status = mz_deflate(&stream, MZ_FINISH);
	if(status != MZ_STREAM_END)
	{
		mz_deflateEnd(&stream);
		throw std::runtime_error("gz_compress(): compression failed");
	}
	deflated.resize(stream.total_out);
	mz_deflateEnd(&stream);

	String result;
	result.reserve(10 + deflated.size() + 8);
	result.push_back((char)0x1f);
	result.push_back((char)0x8b);
	result.push_back((char)0x08);
	result.push_back((char)0x00);
	gz_append_u32_le(result, 0);
	result.push_back((char)0x00);
	result.push_back((char)0xff);
	result.append(deflated);
	gz_append_u32_le(result, (u32)mz_crc32(MZ_CRC32_INIT, (const unsigned char*)src.data(), src.size()));
	gz_append_u32_le(result, (u32)src.size());
	return(result);
}

String gz_uncompress(String compressed)
{
	archive_check_size("gz_uncompress", "input", compressed.size(), "ARCHIVE_MAX_INPUT_BYTES", 64 * 1024 * 1024);
	size_t deflate_offset = gz_deflate_offset(compressed);
	size_t footer_offset = compressed.size() - 8;
	size_t deflate_size = footer_offset - deflate_offset;
	u32 expected_size = gz_read_u32_le(compressed, footer_offset + 4);
	archive_check_size("gz_uncompress", "declared output", expected_size, "ARCHIVE_MAX_OUTPUT_BYTES", 64 * 1024 * 1024);
	size_t out_len = 0;
	void* out = tinfl_decompress_mem_to_heap(compressed.data() + deflate_offset, deflate_size, &out_len, 0);
	if(!out)
		throw std::runtime_error("gz_uncompress(): decompression failed");

	u64 output_limit = to_u64(context->server->config["ARCHIVE_MAX_OUTPUT_BYTES"], 64 * 1024 * 1024);
	if(output_limit > 0 && out_len > output_limit)
	{
		mz_free(out);
		throw std::runtime_error("gz_uncompress(): output exceeds configured limit");
	}
	String result((char*)out, out_len);
	mz_free(out);

	u32 expected_crc = gz_read_u32_le(compressed, footer_offset);
	u32 actual_crc = (u32)mz_crc32(MZ_CRC32_INIT, (const unsigned char*)result.data(), result.size());
	if(actual_crc != expected_crc)
		throw std::runtime_error("gz_uncompress(): crc check failed");
	if((u32)result.size() != expected_size)
		throw std::runtime_error("gz_uncompress(): size check failed");
	return(result);
}

bool zip_extract(String zip_file_name, String destination_directory)
{
	if(destination_directory == "")
		throw std::runtime_error(zip_error("zip_extract", "destination directory is empty"));
	if(!file_exists(destination_directory) && !mkdir(destination_directory))
		throw std::runtime_error(zip_error("zip_extract", "could not create destination directory " + destination_directory));

	mz_zip_archive archive;
	std::memset(&archive, 0, sizeof(archive));
	if(!mz_zip_reader_init_file(&archive, zip_file_name.c_str(), 0))
		throw std::runtime_error(zip_error("zip_extract", "could not open " + zip_file_name));

	try
	{
		mz_uint count = mz_zip_reader_get_num_files(&archive);
		archive_check_size("zip_extract", "entry count", count, "ARCHIVE_MAX_ZIP_ENTRIES", 4096);
		for(mz_uint i = 0; i < count; i++)
		{
			mz_zip_archive_file_stat stat;
			std::memset(&stat, 0, sizeof(stat));
			if(!mz_zip_reader_file_stat(&archive, i, &stat))
				throw std::runtime_error(zip_error("zip_extract", "could not read file metadata at index " + std::to_string((u64)i)));
			archive_check_size("zip_extract", "uncompressed entry", stat.m_uncomp_size, "ARCHIVE_MAX_OUTPUT_BYTES", 64 * 1024 * 1024);
			String name = zip_normalize_entry_name(stat.m_filename);
			if(!zip_entry_name_safe(name))
				throw std::runtime_error(zip_error("zip_extract", "refusing unsafe entry name '" + name + "'"));

			String target = path_join(destination_directory, name);
			if(mz_zip_reader_is_file_a_directory(&archive, i))
			{
				if(!file_exists(target) && !mkdir(target))
					throw std::runtime_error(zip_error("zip_extract", "could not create directory " + target));
				continue;
			}
			if(!zip_ensure_parent_directory(target))
				throw std::runtime_error(zip_error("zip_extract", "could not create parent directory for " + target));
			if(!mz_zip_reader_extract_to_file(&archive, i, target.c_str(), 0))
				throw std::runtime_error(zip_error("zip_extract", "could not extract " + name));
		}
		mz_zip_reader_end(&archive);
		return(true);
	}
	catch(...)
	{
		mz_zip_reader_end(&archive);
		throw;
	}
}
