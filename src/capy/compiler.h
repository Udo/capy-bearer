#pragma once

#include "frontend.h"
#include "wasm.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace capy
{

struct ParsedSourceCacheStats
{
	std::size_t hits = 0, misses = 0, entries = 0, charged_bytes = 0, evictions = 0, oversize = 0;
	std::size_t pinned_entries = 0, pinned_source_bytes = 0;
};

namespace detail
{
struct ParsedSourceCacheAccess;
}

// Successful parses are cached even if later validation/lowering fails; parser
// failures and cancellations are not. AST acquisition is compiler-internal: the
// public API cannot expose mutable AST nodes shared with cached compilations.
struct ParsedSourceCache
{
	explicit ParsedSourceCache(std::size_t max_entries = 128, std::size_t max_charged_bytes = 8 * 1024 * 1024,
		std::size_t max_source_bytes = 1024 * 1024);
	~ParsedSourceCache();
	ParsedSourceCache(const ParsedSourceCache&) = delete;
	ParsedSourceCache& operator=(const ParsedSourceCache&) = delete;
	ParsedSourceCacheStats stats() const;
	void clear();

private:
	std::shared_ptr<const Program> acquire(std::string_view source, const std::string& canonical_identity,
		const std::string& diagnostic_identity, const std::string& parser_compiler_identity, unsigned abi_version,
		CancellationCallback cancelled, bool pinned);
	friend struct detail::ParsedSourceCacheAccess;
	struct State;
	mutable std::atomic<std::shared_ptr<State>> state_;
	std::size_t max_entries_, max_charged_bytes_, max_source_bytes_;
};

struct CompileOptions
{
	std::string source_path = "<input>";
	std::string module_name = "module.wasm";
	unsigned abi_version = 0;
	CancellationCallback cancelled;
	// A non-empty canonical identity enables parsed-source reuse. source_path
	// remains diagnostic-only, so distinct diagnostic paths never share ASTs.
	std::string canonical_source_identity;
	ParsedSourceCache* parsed_source_cache = nullptr;
	std::function<std::vector<std::string>(const std::string& path)> import_type_metadata;
};

struct CompileResult
{
	wasm::Bytes wasm;
	std::string source_map;
	std::vector<std::string> custom_exports;
	std::vector<std::string> function_exports;
	std::vector<std::string> type_exports;
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
