#include "compiler.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace
{

void usage(std::ostream& output)
{
	output << "usage: capyc SOURCE -o UNIT.wasm --source-map UNIT.wasm.source-map --abi-version VERSION\n";
}

void write_atomic(const std::filesystem::path& path, const char* data, std::size_t size)
{
	const std::filesystem::path temporary = path.string() + ".tmp." + std::to_string(static_cast<unsigned long long>(::getpid()));
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output || !output.write(data, static_cast<std::streamsize>(size)))
			throw std::runtime_error("could not write " + temporary.string());
		output.close();
		if (!output)
			throw std::runtime_error("could not close " + temporary.string());
	}
	const int file = ::open(temporary.c_str(), O_RDONLY);
	if (file < 0 || ::fsync(file) != 0)
	{
		const int error_number = errno;
		if (file >= 0)
			::close(file);
		std::filesystem::remove(temporary);
		throw std::runtime_error("could not sync " + temporary.string() + ": " + std::strerror(error_number));
	}
	::close(file);
	std::error_code error;
	std::filesystem::rename(temporary, path, error);
	if (error)
	{
		std::filesystem::remove(temporary);
		throw std::runtime_error("could not publish " + path.string() + ": " + error.message());
	}
	const std::filesystem::path directory = path.parent_path().empty() ? "." : path.parent_path();
	const int parent = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
	if (parent < 0 || ::fsync(parent) != 0)
	{
		const int error_number = errno;
		if (parent >= 0)
			::close(parent);
		throw std::runtime_error("could not sync output directory " + directory.string() + ": " + std::strerror(error_number));
	}
	::close(parent);
}

} // namespace

int main(int argc, char** argv)
{
	std::string source, output, source_map;
	unsigned abi_version = 0;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		auto value = [&](const char* option)
		{
			if (++index == argc)
				throw std::runtime_error(std::string(option) + " requires a value");
			return std::string(argv[index]);
		};
		try
		{
			if (argument == "-h" || argument == "--help")
			{
				usage(std::cout);
				return 0;
			}
			if (argument == "-o" || argument == "--output")
				output = value(argument.c_str());
			else if (argument == "--source-map")
				source_map = value(argument.c_str());
			else if (argument == "--abi-version")
				abi_version = static_cast<unsigned>(std::stoul(value(argument.c_str())));
			// Retained for compatibility with scripts/compile_wasm_unit; capyc always emits Bearer units.
			else if (argument == "--bearer-unit")
			{
			}
			else if (!argument.empty() && argument[0] == '-')
				throw std::runtime_error("unknown option " + argument);
			else if (source.empty())
				source = argument;
			else
				throw std::runtime_error("multiple source files are not supported");
		}
		catch (const std::exception& error)
		{
			std::cerr << "capyc: " << error.what() << '\n';
			usage(std::cerr);
			return 2;
		}
	}
	if (source.empty() || output.empty() || source_map.empty() || abi_version == 0)
	{
		usage(std::cerr);
		return 2;
	}

	try
	{
		capy::CompileOptions options;
		options.source_path = std::filesystem::absolute(source).string();
		options.module_name = std::filesystem::path(output).filename().string();
		options.abi_version = abi_version;
		const capy::CompileResult result = capy::compile_bearer_file(source, std::move(options));
		const auto validation = capy::wasm::validate_bearer_unit(result.wasm, {.bearer_abi_version = std::to_string(abi_version)});
		if (!validation.valid)
			throw std::runtime_error("native Capy compiler emitted invalid Wasm: " + validation.error);
		write_atomic(output, reinterpret_cast<const char*>(result.wasm.data()), result.wasm.size());
		write_atomic(source_map, result.source_map.data(), result.source_map.size());
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
