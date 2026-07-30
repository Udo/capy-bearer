#include "compiler.h"
#include "../src/wasm/capy_backtrace.h"

#include <cassert>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

int main()
{
	capy::CompileOptions options;
	options.source_path = "native-test.capy";
	options.module_name = "native-test.wasm";
	options.abi_version = 11;
	capy::ParsedSourceCache parsed_cache;
	capy::CompileOptions cached_options = options;
	cached_options.canonical_source_identity = "capy-test://cached-source";
	cached_options.parsed_source_cache = &parsed_cache;
	const std::string cached_source = "function CLI { print(request_body()) }\n";
	const auto cached_first = capy::compile_bearer_unit(cached_source, cached_options);
	const auto cached_second = capy::compile_bearer_unit(cached_source, cached_options);
	const auto uncached = capy::compile_bearer_unit(cached_source, options);
	assert(cached_first.wasm == cached_second.wasm && cached_first.source_map == cached_second.source_map);
	assert(cached_first.wasm == uncached.wasm && cached_first.source_map == uncached.source_map);
	const auto after_hit = parsed_cache.stats();
	assert(after_hit.hits >= 2 && after_hit.entries == 1 && after_hit.pinned_entries == 1 && after_hit.pinned_source_bytes > 0);
	capy::compile_bearer_unit("function CLI { print(request_body(), 2) }\n", cached_options);
	const auto after_content_change = parsed_cache.stats();
	assert(after_content_change.misses > after_hit.misses);
	cached_options.source_path = "other-diagnostic.capy";
	cached_options.canonical_source_identity = "capy-test://cached-source";
	capy::compile_bearer_unit(cached_source, cached_options);
	assert(parsed_cache.stats().misses > after_content_change.misses);
	const auto entries_before_failure = parsed_cache.stats().entries;
	cached_options.source_path = "bad.capy";
	cached_options.canonical_source_identity = "capy-test://bad";
	try { capy::compile_bearer_unit("function", cached_options); assert(false); }
	catch (const capy::Error&) {}
	assert(parsed_cache.stats().entries == entries_before_failure);

	// These identities produced the same old newline-concatenated key.
	cached_options.source_path = "path-c";
	cached_options.canonical_source_identity = "identity\npath-b";
	const auto newline_first = capy::compile_bearer_unit(cached_source, cached_options);
	const auto misses_before_newline_second = parsed_cache.stats().misses;
	cached_options.source_path = "path-b\npath-c";
	cached_options.canonical_source_identity = "identity";
	const auto newline_second = capy::compile_bearer_unit(cached_source, cached_options);
	assert(parsed_cache.stats().misses > misses_before_newline_second && newline_first.source_map != newline_second.source_map);

	unsigned hit_polls = 0;
	cached_options.source_path = "native-test.capy";
	cached_options.canonical_source_identity = "capy-test://cached-source";
	cached_options.cancelled = [&] { return ++hit_polls == 4; };
	try { capy::compile_bearer_unit(cached_source, cached_options); assert(false); }
	catch (const capy::Error&) {}
	assert(hit_polls == 4); // initial, hash start/end, then final cache-hit return poll
	cached_options.cancelled = {};

	std::mutex contention_mutex;
	std::condition_variable contention_ready, contention_release;
	bool first_parsing = false, second_finished = false, first_cancelled = false;
	const std::string contention_source = "function CLI {}\n" + std::string(4097, ' ');
	capy::CompileOptions contention_options = options;
	contention_options.source_path = "contention.capy";
	contention_options.canonical_source_identity = "capy-test://contention";
	contention_options.parsed_source_cache = &parsed_cache;
	std::thread first([&] {
		unsigned polls = 0;
		contention_options.cancelled = [&] {
			if (++polls == 5)
			{
				std::unique_lock lock(contention_mutex);
				first_parsing = true;
				contention_ready.notify_one();
				contention_release.wait(lock, [&] { return second_finished; });
			}
			return polls == 8;
		};
		try { capy::compile_bearer_unit(contention_source, contention_options); }
		catch (const capy::Error&) { first_cancelled = true; }
	});
	{
		std::unique_lock lock(contention_mutex);
		contention_ready.wait(lock, [&] { return first_parsing; });
	}
	std::thread second([&] {
		contention_options.cancelled = {};
		capy::compile_bearer_unit(contention_source, contention_options);
		std::lock_guard lock(contention_mutex);
		second_finished = true;
		contention_release.notify_one();
	});
	first.join();
	second.join();
	assert(first_cancelled);

	capy::ParsedSourceCache bounded(2, 256, 96);
	capy::CompileOptions bounded_options = options;
	bounded_options.parsed_source_cache = &bounded;
	for (const auto& identity : {"a", "b", "c"})
	{
		bounded_options.canonical_source_identity = identity;
		capy::compile_bearer_unit("function CLI {}", bounded_options);
	}
	assert(bounded.stats().entries == 2 && bounded.stats().charged_bytes <= 256 && bounded.stats().evictions == 1);
	bounded_options.canonical_source_identity = "large";
	capy::compile_bearer_unit("function CLI { print(\"" + std::string(97, ' ') + "\") }", bounded_options);
	assert(bounded.stats().oversize == 1);
	if (const pid_t child = fork(); child == 0)
	{
		if (bounded.stats().entries != 0)
			_exit(1);
		bounded_options.canonical_source_identity = "child";
		capy::compile_bearer_unit("function CLI {}", bounded_options);
		_exit(bounded.stats().entries == 1 ? 0 : 1);
	}
	else
	{
		int status = 0;
		assert(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	bounded.clear();
	assert(bounded.stats().entries == 0 && bounded.stats().pinned_entries == 0);
	options.cancelled = {};
	options.source_path = "native-test.capy";
	options.canonical_source_identity.clear();
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
	try { capy::compile_bearer_unit("function CLI { if var wrong : bool = 1 {} }\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("expected bool, found s32") != std::string::npos); }

	constexpr std::string_view generic_source = "function identity(value : any) value::type { value }\n"
												"function choose(value : any) value::type { value }\n"
												"function choose(value : s32) value::type { value + 1 }\n"
												"function countdown(value : any) value::type { if value == 0 { return value }; return countdown(value - 1) }\n"
												"function CLI { print(identity(7), choose(4), countdown(3), identity((1, clone(\"x\")))[1], 2 as bool) }\n";
	const auto generic = capy::compile_bearer_unit(generic_source, options);
	assert(capy::wasm::validate_bearer_unit(generic.wasm, {.bearer_abi_version = "11"}).valid);
	const auto generic_method = capy::compile_bearer_unit(
		"function same(value : any) value::type { value }\nfunction CLI { print((7).same()) }\n", options);
	assert(capy::wasm::validate_bearer_unit(generic_method.wasm, {.bearer_abi_version = "11"}).valid);
	assert(generic_method.source_map.find("F\t1\tnative-test.capy\n") != std::string::npos);
	const auto module_exports = capy::compile_bearer_unit(
		"EXPORTS invoke\nEXPORTS other\nfunction invoke(value : dval) dval { value }\nfunction other(value : dval) dval { value }\n", options);
	assert(module_exports.custom_exports == std::vector<std::string>({"invoke", "other"}));
	assert(module_exports.source_map.find("F\t1\tnative-test.capy\n") != std::string::npos);
	assert(capy::wasm::validate_bearer_unit(module_exports.wasm, {.bearer_abi_version = "11"}).valid);
	const auto module_call = capy::compile_bearer_unit(
		"function CLI { var loaded : module = unit_load(\"child.capy\"); loaded.call(\"invoke\") }\n", options);
	const std::string module_call_bytes(module_call.wasm.begin(), module_call.wasm.end());
	assert(module_call_bytes.find("bearer_unit_load") != std::string::npos && module_call_bytes.find("bearer_module_call_brrb") != std::string::npos);
	assert(capy::wasm::validate_bearer_unit(module_call.wasm, {.bearer_abi_version = "11"}).valid);
	assert(module_call.source_map.find("F\t2\tcapy://stdlib.capy\n") != std::string::npos);
	const auto module_passthrough = capy::compile_bearer_unit(
		"function pass(value : module) module { value }\n"
		"function identity(value : any) value::type { value }\n"
		"function CLI { var loaded : module = unit_load(\"child.capy\"); var returned : module = pass(loaded); identity(returned).call(\"invoke\") }\n", options);
	assert(capy::wasm::validate_bearer_unit(module_passthrough.wasm, {.bearer_abi_version = "11"}).valid);
	for (const auto& [source, expected] : {
			 std::pair{"EXPORTS missing\nfunction CLI {}\n", "unknown local function 'missing'"},
			 std::pair{"EXPORTS wrong\nfunction wrong(value : string) string { value }\nfunction CLI {}\n", "must have signature (dval) dval"},
			 std::pair{"EXPORTS generic\nfunction generic(value : any) value::type { value }\nfunction CLI {}\n", "must name a non-generic local function"},
			 std::pair{"EXPORTS invoke\nfunction EXPORT_invoke(value : dval) dval { value }\nfunction CLI {}\n", "already declared"},
			 std::pair{"function CLI { var loaded : module = unit_load(\"x\"); print(loaded + loaded) }\n", "unsupported operator + for module"},
			 std::pair{"function CLI { var loaded : module = unit_load(\"x\"); if loaded {} }\n", "module is opaque and cannot be used as a condition"},
			 std::pair{"function CLI { var loaded : module = unit_load(\"x\"); loaded as s32 }\n", "no explicit conversion from module to s32"},
			 std::pair{"function CLI { var loaded : module = unit_load(\"x\"); var values := [loaded] }\n", "module is opaque and cannot be stored in array layouts"},
			 std::pair{"function CLI { var loaded : module = unit_load(\"x\"); var pair := (loaded, loaded) }\n", "module is opaque and cannot be stored in tuple layouts"},
			 std::pair{"struct Box { handle : module }\nfunction CLI {}\n", "module is opaque and cannot be stored in struct layouts"},
			 std::pair{"function CLI { var loaded : module = unit_load(\"x\"); var f := function() module { loaded } }\n", "module is opaque and cannot be captured by a closure"},
		 })
	{
		try { capy::compile_bearer_unit(source, options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find(expected) != std::string::npos); }
	}
	const auto returning = capy::compile_bearer_unit(
		"function choose(value : bool) s32 { if value { return 1 } else { return 2 } }\nfunction CLI { print(choose(true)) }\n", options);
	assert(capy::wasm::validate_bearer_unit(returning.wasm, {.bearer_abi_version = "11"}).valid);
	const auto constants = capy::compile_bearer_unit("const answer : s32 = 42\nfunction CLI { print(answer) }\n", options);
	const auto constants_repeat = capy::compile_bearer_unit("const answer : s32 = 42\nfunction CLI { print(answer) }\n", options);
	const auto constant_baseline = capy::compile_bearer_unit("function CLI { print(42) }\n", options);
	assert(constants.wasm == constants_repeat.wasm && constants.source_map == constants_repeat.source_map);
	assert(constants.wasm == constant_baseline.wasm);
	const auto shadowed_constant = capy::compile_bearer_unit("const answer : s32 = 42\nfunction CLI { var answer : s32 = 7; print(answer) }\n", options);
	assert(shadowed_constant.wasm == capy::compile_bearer_unit("function CLI { var answer : s32 = 7; print(answer) }\n", options).wasm);
	try { capy::compile_bearer_unit("const x : s32 = 1\nconst x : s32 = 2\nfunction CLI { print(x) }\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("already declared") != std::string::npos); }
	try { capy::compile_bearer_unit("const x : s32 = 1\nfunction x() s32 { 1 }\nfunction CLI { print(x) }\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("conflicts with constant") != std::string::npos); }
	try { capy::compile_bearer_unit("function x() s32 { 1 }\nconst x : s32 = 1\nfunction CLI {}\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("conflicts with constant") != std::string::npos); }
	try { capy::compile_bearer_unit("struct x {}\nconst x : s32 = 1\nfunction CLI {}\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("conflicts with constant") != std::string::npos); }
	try { capy::compile_bearer_unit("const answer : s32 = 42\nfunction CLI { answer() }\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.location.line == 2 && error.message.find("no overload answer()") != std::string::npos); }
	try { capy::compile_bearer_unit("const x : string = \"x\"\nfunction CLI { print(x) }\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("s32 literal") != std::string::npos); }
	try { capy::compile_bearer_unit("const __bearer_x : s32 = 1\nfunction CLI { print(1) }\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("reserved") != std::string::npos); }
	const auto boundary = capy::compile_bearer_unit("function CLI { print(-2147483648, 2147483647) }\n", options);
	assert(capy::wasm::validate_bearer_unit(boundary.wasm, {.bearer_abi_version = "11"}).valid);
	const auto ordinary_sqlite = capy::compile_bearer_unit("function CLI { var db := sqlite_connect(\":memory:\"); print(sqlite_error(db)); sqlite_disconnect(db) }\n", options);
	assert(capy::wasm::validate_bearer_unit(ordinary_sqlite.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string sqlite_bytes(ordinary_sqlite.wasm.begin(), ordinary_sqlite.wasm.end());
	assert(sqlite_bytes.find("bearer_sqlite_connect") != std::string::npos);
	assert(sqlite_bytes.find("bearer_sqlite_error") != std::string::npos);
	assert(sqlite_bytes.find("bearer_capy_backtrace") == std::string::npos);
	assert(ordinary_sqlite.source_map.find("F\t2\tcapy://stdlib.capy\n") != std::string::npos);
	bool sqlite_stdlib_marker = false;
	std::istringstream sqlite_map(ordinary_sqlite.source_map);
	for (std::string row; std::getline(sqlite_map, row);)
	{
		std::istringstream fields(row);
		std::string kind, address;
		unsigned source_id = 0;
		if (std::getline(fields, kind, '\t') && kind == "L" && std::getline(fields, address, '\t') && (fields >> source_id) && source_id == 2)
			sqlite_stdlib_marker = true;
	}
	assert(sqlite_stdlib_marker);
	const auto no_stdlib_demand = capy::compile_bearer_unit("function CLI { print(1) }\n", options);
	const std::string no_stdlib_bytes(no_stdlib_demand.wasm.begin(), no_stdlib_demand.wasm.end());
	assert(no_stdlib_bytes.find("bearer_sqlite_") == std::string::npos && no_stdlib_bytes.find("bearer_mysql_") == std::string::npos && no_stdlib_bytes.find("bearer_capy_backtrace") == std::string::npos);
	const auto f64_print = capy::compile_bearer_unit("function emit(value : f64) { print(value) }\nfunction CLI { emit(1.5) }\n", options);
	const std::string f64_print_bytes(f64_print.wasm.begin(), f64_print.wasm.end());
	assert(f64_print_bytes.find("bearer_print_f64") != std::string::npos);
	assert(f64_print_bytes.find("bearer_print_s64") == std::string::npos && f64_print_bytes.find("bearer_print_u64") == std::string::npos);
	const auto ordinary_backtrace = capy::compile_bearer_unit(
		"function outer() string { inner() }\nfunction inner() string { var capture := function() string { backtrace_get_frames(2, 1) }; capture() }\nfunction CLI { print(outer(), backtrace_get_frames()) }\n", options);
	assert(capy::wasm::validate_bearer_unit(ordinary_backtrace.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string backtrace_bytes(ordinary_backtrace.wasm.begin(), ordinary_backtrace.wasm.end());
	assert(backtrace_bytes.find("bearer_capy_backtrace") != std::string::npos);
	assert(backtrace_bytes.find("void*") == std::string::npos);
	assert(ordinary_backtrace.source_map.find("F\t2\tcapy://stdlib.capy\n") != std::string::npos);
	capy::CompileOptions control_path_options = options;
	control_path_options.source_path = "tab\tline\n\033.capy";
	const auto control_path_backtrace = capy::compile_bearer_unit("function CLI { print(backtrace_get_frames()) }\n", control_path_options);
	const std::string control_path_bytes(control_path_backtrace.wasm.begin(), control_path_backtrace.wasm.end());
	const std::string control_record = std::string("\003\0\0\0\017\0\0\0\001\0\0\0\001\0\0\0CLI", 19) + control_path_options.source_path;
	assert(control_path_bytes.find(control_record) != std::string::npos);
	std::string control_frame;
	assert(capy_backtrace::format_record(control_record, control_frame));
	assert(control_frame == "CLI at tab\\x09line\\x0A\\x1B.capy:1:1");
	std::string oversized_frame;
	const std::string oversized_path(5000, 'p');
	std::string oversized_record("\003\0\0\0", 4);
	oversized_record.append("\210\023\0\0", 4);
	oversized_record.append("\001\0\0\0\001\0\0\0CLI", 11);
	oversized_record += oversized_path;
	assert(capy_backtrace::format_record(oversized_record, oversized_frame));
	assert(oversized_frame.find("...[truncated]") != std::string::npos);
	capy::CompileOptions long_path_options = options;
	long_path_options.source_path = std::string(5000, 'p') + ".capy";
	const auto long_path_backtrace = capy::compile_bearer_unit("function CLI { print(backtrace_get_frames()) }\n", long_path_options);
	assert(capy::wasm::validate_bearer_unit(long_path_backtrace.wasm, {.bearer_abi_version = "11"}).valid);
	assert(std::string(long_path_backtrace.wasm.begin(), long_path_backtrace.wasm.end()).find(long_path_options.source_path) != std::string::npos);
	const auto ordinary_boundary = capy::compile_bearer_unit(
		"function CLI { print(request_body(), ws_message(), component_resolve(\"card\"), regex_replace(\"x\", \"y\", \"x\"), base64_encode(\"x\"), file_temp(\"x\"), time()) }\n",
		options);
	assert(ordinary_boundary.source_map.find("F\t2\tcapy://stdlib.capy\n") != std::string::npos);
	const std::string boundary_bytes(ordinary_boundary.wasm.begin(), ordinary_boundary.wasm.end());
	for (const auto& import : {"bearer_request_body", "bearer_ws_message", "bearer_component_resolve", "bearer_regex", "bearer_codec", "bearer_file_temp", "bearer_time"})
		assert(boundary_bytes.find(import) != std::string::npos);
	const auto narrow_boundary = capy::compile_bearer_unit("function CLI { print(request_body()) }\n", options);
	const std::string narrow_bytes(narrow_boundary.wasm.begin(), narrow_boundary.wasm.end());
	assert(narrow_bytes.find("bearer_request_body") != std::string::npos);
	for (const auto& absent : {"bearer_request_value", "bearer_ws_", "bearer_component_", "bearer_regex", "bearer_codec", "bearer_file_", "bearer_time", "bearer_unit_load", "bearer_module_call_brrb"})
		assert(narrow_bytes.find(absent) == std::string::npos);
	std::ifstream compiler_source("src/capy/compiler.cpp");
	const std::string compiler_text((std::istreambuf_iterator<char>(compiler_source)), {});
	assert(compiler_source && compiler_text.find("bearer_string" "_list") == std::string::npos && compiler_text.find("__legacy" "_") == std::string::npos);
	assert(compiler_text.find("named->value == \"__bearer_") == std::string::npos);
	assert(compiler_text.find("named->value == \"__bearer_mysql") == std::string::npos);
	assert(compiler_text.find("named->value == \"__bearer_sqlite") == std::string::npos);
	assert(compiler_text.find("named->value == \"__bearer_regex") == std::string::npos);
	assert(compiler_text.find("named->value == \"__bearer_unit_call") == std::string::npos);
	assert(compiler_text.find("named->value == \"__bearer_codec") == std::string::npos);
	assert(compiler_text.find("named->value == \"unit_load\"") == std::string::npos);
	assert(compiler_text.find("named->value == \"call\"") == std::string::npos);
	for (const auto& public_name : {"array_merge", "dval_set", "dval_assign", "dval_push", "dval_pop", "dval_remove", "dval_clear", "dval_get_by_path", "dval_get_or_create",
			 "dval_set_array", "dval_set_bool", "dval_set_type", "dval_get_type_name", "dval_key", "dval_keys", "dval_values", "dval_to_json", "dval_to_stringmap", "dval_put",
			 "dval_to_s64", "dval_to_u64", "request_param", "request_get", "request_post", "request_cookie", "request_session", "request_body", "response_header", "request_context",
			 "session_start", "session_set", "session_remove", "session_destroy", "session_id_create", "response_cookie", "redirect", "csrf_token", "csrf_valid", "csrf_rotate", "csrf_field",
			 "ws_message", "ws_connection_id", "ws_scope", "ws_opcode", "ws_is_binary", "ws_send", "ws_send_to", "ws_close", "component", "component_capture", "component_render",
			 "component_exists", "component_resolve", "unit_render", "unit_call", "unit_info", "units_list", "unit_compile", "regex_match", "regex_search", "regex_search_all", "regex_replace",
			 "regex_split", "base64_encode", "base64_decode", "uri_encode", "uri_decode", "json_encode", "json_decode", "html_escape", "strpos", "file_open", "file_read", "file_write",
			 "file_seek", "file_tell", "file_fsync", "file_close", "file_temp", "file_unlink", "time", "time_precise"})
		assert(compiler_text.find("named->value == \"" + std::string(public_name) + "\"") == std::string::npos);
	const auto ordinary_mysql = capy::compile_bearer_unit("function CLI { print(mysql_escape(\"Ada\")) }\n", options);
	assert(capy::wasm::validate_bearer_unit(ordinary_mysql.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string mysql_bytes(ordinary_mysql.wasm.begin(), ordinary_mysql.wasm.end());
	assert(mysql_bytes.find("bearer_mysql_escape") != std::string::npos);
	const auto typed_sized_hosts = capy::compile_bearer_unit(
		"function CLI { var params := dval({:}); mysql_query(0u64, \"select 1\", params); sqlite_query(0u64, \"select 1\", params); regex_search(\"x\", \"x\"); regex_replace(\"x\", \"y\", \"x\"); unit_call(\"/x\", \"CLI\", params); json_decode(\"{}\"); base64_encode(\"x\") }\n", options);
	assert(capy::wasm::validate_bearer_unit(typed_sized_hosts.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string typed_sized_host_bytes(typed_sized_hosts.wasm.begin(), typed_sized_hosts.wasm.end());
	for (const auto& import : {"bearer_mysql_query", "bearer_sqlite_query", "bearer_regex_dval", "bearer_regex_text", "bearer_unit_call_brrb", "bearer_codec_dval", "bearer_codec_text"})
		assert(typed_sized_host_bytes.find(import) != std::string::npos);
	const auto owned_sized_host = capy::compile_bearer_unit(
		"function CLI { sqlite_query(0u64, clone(\"select 1\"), dval({\"owned\": clone(\"value\")})) }\n", options);
	assert(capy::wasm::validate_bearer_unit(owned_sized_host.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string sizing_release = "release_inputs(); append(code, module_.marker(value->location));";
	const std::size_t first_sizing_release = compiler_text.find(sizing_release);
	assert(first_sizing_release != std::string::npos && compiler_text.find(sizing_release, first_sizing_release + 1) != std::string::npos);
	const std::string function_value_source = "function CLI { var escape : function(raw : string) string = mysql_escape; print(escape(\"Ada\")) }\n";
	const auto function_value = capy::compile_bearer_unit(function_value_source, options);
	assert(capy::wasm::validate_bearer_unit(function_value.wasm, {.bearer_abi_version = "11"}).valid);
	assert(std::string(function_value.wasm.begin(), function_value.wasm.end()).find("bearer_mysql_escape") != std::string::npos);
	const auto parsed_function_value = capy::parse(function_value_source, options.source_path);
	const auto parsed_result = capy::compile_bearer_unit(parsed_function_value, options.source_path, options.module_name, options.abi_version);
	assert(function_value.wasm == parsed_result.wasm && function_value.source_map == parsed_result.source_map);
	const auto shadowed_stdlib = capy::compile_bearer_unit(
		"function CLI { var sqlite_connect : function(path : string) u64 = function(path : string) u64 { 1u64 }; print(sqlite_connect(\"x\")) }\n", options);
	assert(std::string(shadowed_stdlib.wasm.begin(), shadowed_stdlib.wasm.end()).find("bearer_sqlite_") == std::string::npos);
	for (const auto& [source, expected] : {
			 std::pair{"host function __bearer_trace() string\nfunction CLI {}\n", "host declarations are available only in the embedded Capy standard library"},
			 std::pair{"trace host function __bearer_trace() string\nfunction CLI {}\n", "host declarations are available only in the embedded Capy standard library"},
			 std::pair{"function __bearer_sqlite_connect(path : string) u64 { 0u64 }\nfunction CLI {}\n", "reserved for the Capy standard library"},
			 std::pair{"function CLI { __bearer_sqlite_connect(\":memory:\") }\n", "reserved for the Capy standard library"},
			 std::pair{"function sqlite_connect(path : string) u64 { 0u64 }\nfunction CLI {}\n", "reserved by the Capy standard library"},
			 std::pair{"function CLI { var callback : function(__bearer_value : s32) s32 = function(__bearer_value : s32) s32 { __bearer_value } }\n", "__bearer_* names are reserved for the Capy standard library"},
			 std::pair{"function CLI { for __bearer_key, value = dval({:}) { value } }\n", "__bearer_* names are reserved for the Capy standard library"},
			 std::pair{"function duplicate(value : any) value::type { value }\nfunction duplicate(value : any) value::type { value }\nfunction CLI {}\n", "duplicate overload duplicate(any)"},
		 })
	{
		try
		{
			capy::compile_bearer_unit(source, options);
			assert(false);
		}
		catch (const capy::Error& error)
		{
			assert(error.message.find(expected) != std::string::npos);
		}
	}
	const auto wide =
		capy::compile_bearer_unit("function next(value : u64) u64 { value + 1u64 }\nfunction half(value : f64) f64 { value / 2.0 }\n"
								  "function CLI { var fn : function(value : u64) u64 = next; print(fn(4u64), half(3.0), -1 as u64, 9.0 as s32) }\n",
								  options);
	assert(capy::wasm::validate_bearer_unit(wide.wasm, {.bearer_abi_version = "11"}).valid);
	const auto marker_collision = capy::compile_bearer_unit("function CLI { 1509949440; print([1][1]) }\n", options);
	assert(capy::wasm::validate_bearer_unit(marker_collision.wasm, {.bearer_abi_version = "11"}).valid);
	assert(marker_collision.source_map.find("\t1\t1\t37\n") != std::string::npos);
	const auto merge_marker = capy::compile_bearer_unit(
		"function CLI {\n    array_merge(dval({\"left\": \"x\"}), dval({\"right\": \"y\"}))\n}\n", options);
	assert(capy::wasm::validate_bearer_unit(merge_marker.wasm, {.bearer_abi_version = "11"}).valid);
	assert(merge_marker.source_map.find("\t2\t5\n") != std::string::npos);
	for (const auto source : {
			 "function square(value : any) value::type { value * value }\nfunction CLI { square(clone(\"x\")) }\n",
			 "function choose(a : any, b : s32) a::type { a }\nfunction choose(a : s32, b : any) b::type { b }\nfunction CLI { choose(1, 1) }\n",
			 "function incomplete(value : bool) s32 { if value { return 1 } }\nfunction CLI { print(incomplete(false)) }\n",
			 "function CLI { print(2147483648) }\n",
			 "function CLI { print(-2147483649) }\n",
			 "function CLI { print(999999999999999999999999999999999999) }\n",
			 "function CLI { value := 1; value := 2 }\n",
			 "function CLI { print(1 && true) }\n",
			 "function CLI { false && (hidden := true); print(hidden) }\n",
			 "function CLI { var held := clone(\"old\"); var replace := function() { held = clone(\"new\") } }\n",
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
	for (const auto& [source, expected] : {
			 std::pair{"function CLI { (1 + 2) := 3 }\n", "inferred declaration target must be a local name"},
			 std::pair{"function CLI { print(1 && true) }\n", "logical operators require bool operands"},
			 std::pair{"function CLI { var held := clone(\"old\"); var replace := function() { held = clone(\"new\") } }\n", "unknown local 'held'"},
			 std::pair{"function CLI { var values := [1u64, 2u64] }\n", "not yet supported in array layouts"},
			 std::pair{"function CLI { var values := (1s64, 2) }\n", "s64, u64, and f64 are not yet supported in tuple layouts"},
			 std::pair{"struct Wide { value : f64 }\nfunction CLI {}\n", "not yet supported in struct layouts"},
			 std::pair{"function CLI { print(first(\"ok\", 1)) }\n", "no overload first(string, s32)"},
			 std::pair{"function CLI { array_merge(dval({\"x\": \"y\"}), \"bad\") }\n", "no overload array_merge(dval, string)"},
			 std::pair{"function CLI { array_merge(dval({\"x\": \"y\"})) }\n", "no overload array_merge(dval)"},
			 std::pair{"function CLI { var value := 1u64; var closure := function() u64 { value } }\n", "not yet supported in captured closure layouts"},
		 })
	{
		try
		{
			capy::compile_bearer_unit(source, options);
			assert(false);
		}
		catch (const capy::Error& error)
		{
			assert(error.message.find(expected) != std::string::npos);
			assert(error.message.find("unsupported operator") == std::string::npos);
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
