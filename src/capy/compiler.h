#pragma once

#include "frontend.h"
#include "wasm.h"

#include <string>
#include <string_view>

namespace capy
{

struct CompileOptions
{
	std::string source_path = "<input>";
	std::string module_name = "module.wasm";
	unsigned abi_version = 0;
	CancellationCallback cancelled;
};

struct CompileResult
{
	wasm::Bytes wasm;
	std::string source_map;
};

// Compile source text to a Bearer direct Wasm side module.  Errors retain their
// Capy source locations; cancellation is reported as an Error at <input>:1:1.
CompileResult compile_bearer_unit(std::string_view source, const CompileOptions& options);

// Convenience entry point for callers which already parsed the input.
CompileResult compile_bearer_unit(const Program& program, const std::string& source_path, const std::string& module_name, unsigned abi_version,
								  CancellationCallback cancelled = {});

// Reads and compiles a Capy source file.  source_path is used in diagnostics and
// source-map records unless overridden in options.
CompileResult compile_bearer_file(const std::string& path, CompileOptions options = {});

} // namespace capy
