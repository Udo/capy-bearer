#pragma once

#include <string>
#include <map>
#include <list>
#include <vector>
#include <functional>
#include <sstream>
#include <atomic>
#include <set>
#include <utility>

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned int u32;
typedef signed int s32;
typedef float f32;
typedef double f64;
typedef unsigned long long u64;
typedef long long s64;

typedef std::string String;

// inline: this header is included by every runtime object (core/wasm/main); a
// non-inline definition here would be a duplicate symbol across them.
inline String operator+(String lhs, u64 rhs) {
	return(lhs + std::to_string(rhs));
}

inline String operator+(String lhs, u32 rhs) {
	return(lhs + std::to_string(rhs));
}

inline String operator+(String lhs, s64 rhs) {
	return(lhs + std::to_string(rhs));
}

inline String operator+(String lhs, s32 rhs) {
	return(lhs + std::to_string(rhs));
}

inline String operator+(String lhs, f64 rhs) {
	return(lhs + std::to_string(rhs));
}

inline String operator+(String lhs, f32 rhs) {
	return(lhs + std::to_string(rhs));
}

#define DEBUG_MEMORY_OFF

typedef std::map<String, String> StringMap;

struct StringList : std::vector<String> {
	using std::vector<String>::vector;
	using std::vector<String>::operator=;

	StringList() = default;
	StringList(const StringList&) = default;
	StringList(StringList&&) = default;
	StringList& operator=(const StringList&) = default;
	StringList& operator=(StringList&&) = default;
	StringList(const std::vector<String>& source) : std::vector<String>(source) {}
	StringList(std::vector<String>&& source) : std::vector<String>(std::move(source)) {}
	StringList& operator=(const std::vector<String>& source) { std::vector<String>::operator=(source); return(*this); }
	StringList& operator=(std::vector<String>&& source) { std::vector<String>::operator=(std::move(source)); return(*this); }

	template<typename F>
	StringList filter(F f) const
	{
		StringList result;
		for(const auto& item : *this)
		{
			if(f(item))
				result.push_back(item);
		}
		return(result);
	}

	template<typename F>
	StringList map(F f) const
	{
		StringList result;
		for(const auto& item : *this)
			result.push_back(f(item));
		return(result);
	}

	StringList unique() const
	{
		StringList result;
		for(const auto& item : *this)
		{
			bool seen = false;
			for(const auto& existing : result)
			{
				if(existing == item)
				{
					seen = true;
					break;
				}
			}
			if(!seen)
				result.push_back(item);
		}
		return(result);
	}

	StringList sort() const
	{
		StringList result = *this;
		for(size_t i = 1; i < result.size(); i++)
		{
			String value = result[i];
			size_t j = i;
			while(j > 0 && value < result[j - 1])
			{
				result[j] = result[j - 1];
				j--;
			}
			result[j] = value;
		}
		return(result);
	}

	template<typename F>
	bool some(F f) const
	{
		for(const auto& item : *this)
		{
			if(f(item))
				return(true);
		}
		return(false);
	}

	template<typename F>
	bool every(F f) const
	{
		for(const auto& item : *this)
		{
			if(!f(item))
				return(false);
		}
		return(true);
	}

	template<typename F>
	String find(F f, String fallback = "") const
	{
		for(const auto& item : *this)
		{
			if(f(item))
				return(item);
		}
		return(fallback);
	}

	StringList keys() const
	{
		StringList result;
		for(size_t i = 0; i < size(); i++)
			result.push_back(std::to_string(i));
		return(result);
	}

	template<typename F>
	void each(F f) const
	{
		for(const auto& item : *this)
			f(item);
	}
};

typedef std::ostringstream ByteStream;

struct Request;
struct DValue;

typedef void (*WasmRequestHandler)(Request& request);
typedef DValue* (*WasmDValueCallHandler)(DValue* call_param);

inline String to_string(s64 v) { return(std::to_string(v)); }

struct SharedUnit {

	String file_name;
	String wasm_name;
	String wasm_check_file_name;
	String api_file_name;
	String meta_file_name;
	String compile_output_file_name;
	String setup_file_name;
	StringList api_declarations;

	String src_path;
	String bin_path;
	String pre_path;
	String src_file_name;
	String wasm_file_name;
	String pre_file_name;

	String compiler_messages;
	String compile_status = "unknown";
	String compile_error_status = "";
	String runtime_error_status = "";
	time_t last_compiled = 0;
	time_t observed_compiled_time = 0;
	time_t last_rendered = 0;
	time_t last_error = 0;
	String observed_metadata_content = "";

	u64 request_count = 0;
	u64 invoke_count = 0;
	u64 runtime_error_count = 0;

	u64 compile_count = 0;
	u64 compile_success_count = 0;
	u64 compile_failure_count = 0;

	f64 last_compile_duration = 0;
	f64 total_compile_duration = 0;
	f64 best_compile_duration = 0;
	f64 worst_compile_duration = 0;

	f64 last_render_duration = 0;
	f64 total_render_duration = 0;
	f64 best_render_duration = 0;
	f64 worst_render_duration = 0;

	~SharedUnit();
};

struct UploadedFile {
	String file_name;
	String tmp_name;
	u32 size;
};

struct ServerState {

	std::map<String, SharedUnit*> units;
	StringMap config;
	u32 request_count = 0;

};

struct URI {

	StringMap query;
	StringMap parts;

};

String nibble(String div, String& haystack);

#include "dvalue.h"

struct Request {

	ServerState* server = 0;

	StringMap params;
	StringMap get;
	StringMap post;
	StringMap cookies;
	StringMap session;
	String session_loaded_hash = "";
	std::set<String> once_units;

	DValue call;
	DValue cfg;
	DValue props;
	DValue connection;

	String session_id = "";
	String session_name = "";
	std::vector<UploadedFile> uploaded_files;

	String response_code = "HTTP/1.1 200 OK";
	StringMap header;
	StringList set_cookies;

	u64 random_seed = 0;
	u64 random_index = 0;

	std::vector<ByteStream*> ob_stack;
	ByteStream* ob = 0;

	String in;
	String out;
	String err;

	struct Flags {
		bool log_request = true;
		bool is_finished = false;
		int status = 0;
		bool output_closed = false;
		bool params_closed = false;
		bool input_closed = false;
	} flags;

	struct Stats {
		u32 bytes_written = 0;
		f64 time_init = 0;
		f64 time_params = 0;
		f64 time_input = 0;
		f64 time_start = 0;
		f64 time_end = 0;
		f64 wasm_handler_ready = 0;
		f64 wasm_backend_started = 0;
		f64 wasm_backend_finished = 0;
		u64 wasm_dispatch_us = 0;
		u64 wasm_workspace_complete_us = 0;
		u64 wasm_entry_invoke_us = 0;
		u64 wasm_output_collect_us = 0;
		u64 wasm_ready_normalize_us = 0;
		u64 wasm_ready_mutation_check_us = 0;
		u64 wasm_ready_artifact_stat_us = 0;
		u64 wasm_ready_freshness_us = 0;
		u64 wasm_ready_source_generation_us = 0;
		u64 wasm_ready_freshness_full_check_us = 0;
		u64 wasm_ready_worker_us = 0;
		u32 wasm_ready_check_count = 0;
		u32 wasm_ready_freshness_cache_hit_count = 0;
		u64 mem_high = 0;
		u64 mem_alloc = 0;
		u32 invoke_count = 0;
	} stats;

	// OS-related resources
	struct Resources {
		std::vector<u64> sockets;
		std::vector<void*> mysql_connections;
		std::vector<void*> sqlite_connections;
		u64 client_socket = 0;
		u64 server_socket = 0;
		bool is_websocket = false;
		bool is_cli = false;
		String websocket_connection_id = "";
		String websocket_scope = "";
		DValue* websocket_connection_state = 0;
		StringList websocket_scope_connection_ids;
		DValue websocket_connection_state_before;
		DValue websocket_dispatch_commands;
		bool websocket_dispatch_capture = false;
		u8 websocket_opcode = 0;
		bool websocket_is_binary = false;
		bool websocket_is_text = false;
		String current_unit_file = "";
		String params_buffer;
		// True while a configured error page (page_compiling /
		// page_compiler_error / page_runtime_error) is rendering, so a failing
		// error page can never recurse into another error page.
		bool error_page_active = false;
	} resources;

	void ob_start();
	void set_status(s32 code, String reason = "");

	~Request();

};

typedef Request FastCGIRequest;

// Native runtime is split across objects (core/wasm/main), so context is
// declared here and defined once in types.cpp. The wasm core is a single TU and
// wasm units resolve context through the loader's GOT, so both keep the
// in-place definition (unchanged ABI).
#if defined(__BEARER_WASM_CORE__) || defined(__BEARER_WASM_UNIT__)
Request* context;
#else
extern Request* context;
#endif

#include <iostream>

// NB: header templates must be inline — wasm units rely on
// -fvisibility-inlines-hidden binding instantiations locally (§6)
template <typename... Ts>
inline void print(Ts... args)
{
    ((*context->ob << args), ...);
}

template <typename... Ts>
inline void out(Ts... args)
{
    ((*context->ob << args), ...);
}

template <typename... Ts>
inline String concat(Ts... args)
{
	ByteStream out;
    ((out << args), ...);
    return(out.str());
}

// The global allocator override (request memory accounting) is defined once in
// types.cpp so it lives in a single runtime object; a header definition would
// duplicate it across core/wasm/main. Units (__BEARER_WASM_UNIT__) import it.
