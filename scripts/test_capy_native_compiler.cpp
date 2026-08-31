#include "compiler.h"
#include "../src/wasm/capy_backtrace.h"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
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
	const std::string cached_source = "function CLI(request : dval) { print(string(request.body)) }\n";
	const auto cached_first = capy::compile_bearer_unit(cached_source, cached_options);
	const auto cached_second = capy::compile_bearer_unit(cached_source, cached_options);
	const auto uncached = capy::compile_bearer_unit(cached_source, options);
	assert(cached_first.wasm == cached_second.wasm && cached_first.source_map == cached_second.source_map);
	assert(cached_first.wasm == uncached.wasm && cached_first.source_map == uncached.source_map);
	const auto after_hit = parsed_cache.stats();
	assert(after_hit.hits >= 2 && after_hit.entries == 1 && after_hit.pinned_entries == 1 && after_hit.pinned_source_bytes > 0);
	capy::compile_bearer_unit("function CLI(request : dval) { print(string(request.body), 2) }\n", cached_options);
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
	const std::string contention_source = "function CLI(request : dval) {}\n" + std::string(4097, ' ');
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
		capy::compile_bearer_unit("function CLI(request : dval) {}", bounded_options);
	}
	assert(bounded.stats().entries == 2 && bounded.stats().charged_bytes <= 256 && bounded.stats().evictions == 1);
	bounded_options.canonical_source_identity = "large";
	capy::compile_bearer_unit("function CLI(request : dval) { print(\"" + std::string(97, ' ') + "\") }", bounded_options);
	assert(bounded.stats().oversize == 1);
	if (const pid_t child = fork(); child == 0)
	{
		if (bounded.stats().entries != 0)
			_exit(1);
		bounded_options.canonical_source_identity = "child";
		capy::compile_bearer_unit("function CLI(request : dval) {}", bounded_options);
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
	auto result = capy::compile_bearer_unit("function CLI(request : dval) { print(1, \"ok\") }\n", options);
	assert(result.wasm.size() >= 4 && result.wasm[0] == 0 && result.wasm[1] == 'a' && result.wasm[2] == 's' && result.wasm[3] == 'm');
	auto validated = capy::wasm::validate_bearer_unit(result.wasm, {.bearer_abi_version = "11"});
	assert(validated.valid);
	assert(validated.bearer_module == "native-test.wasm");
	assert(result.source_map.starts_with("BEARER_SOURCE_MAP_V1\tnative-test.wasm\n"));
	const auto raw_markup = capy::compile_bearer_unit("function CLI(request : dval) { print(<><p><?: clone(\"<Ada>\") ?></p></>) }\n", options);
	assert(capy::wasm::validate_bearer_unit(raw_markup.wasm, {.bearer_abi_version = "11"}).valid);
	const auto markup_strings = capy::compile_bearer_unit(
		"function literal() string { -> <><p>literal</p></> }\n"
		"function joined() string { -> literal() + <><p><?= clone(\"<&>\") ?></p></> }\n"
		"function CLI(request : dval) { print(joined()) }\n", options);
	assert(capy::wasm::validate_bearer_unit(markup_strings.wasm, {.bearer_abi_version = "11"}).valid);
	const auto empty_if_body = capy::compile_bearer_unit("function CLI(request : dval) { if true {} }\n", options);
	assert(capy::wasm::validate_bearer_unit(empty_if_body.wasm, {.bearer_abi_version = "11"}).valid);
	const auto defaults = capy::compile_bearer_unit(
		"type Count = s64\n"
		"function add(left : s32, right : s32 = 2) s32 { -> left + right }\n"
		"function suffix(value : string, tail : string = \"!\") string { -> value + tail }\n"
		"function text(value : as string, suffix : string = \"!\") string { -> value + suffix }\n"
		"function s64(value : s32, extra : s32 = 0) s64 { -> s64(3) }\n"
		"function literals(a : s32 = 1, b : s64 = 2, c : u64 = 3, d : f64 = 4.5, e : bool = false, f : string = \"x\") {}\n"
		"function next(value : s32) s32 { -> value }\n"
		"function ordered(first : s32, second : s32 = 2) s32 { -> first + second }\n"
		"function CLI(request : dval) { var first := add(1); var second := \"x\".suffix(); var converted := text(1); var count : Count = Count(3); literals(); print(ordered(next(1)), first, second, converted, count) }\n", options);
	assert(capy::wasm::validate_bearer_unit(defaults.wasm, {.bearer_abi_version = "11"}).valid);
	assert(defaults.source_map.find("\t5\t") != std::string::npos);
	const auto contextual_integers = capy::compile_bearer_unit(
		"struct WideValue { value : u64 }\n"
		"function parameter(value : s64) s64 { -> value }\n"
		"function returned() u64 { return 18446744073709551615 }\n"
		"function implicit() s64 { -> 8 }\n"
		"function pick(value : s32) s32 { -> 1 }\nfunction pick(value : s64) s32 { -> 2 }\n"
		"function CLI(request : dval) { var a : s64 = 8; var b := s64(8); var c : u64 = 9; var d := u64(9); "
		"var minimum : s64 = -9223372036854775808; var minimum_inferred := s64(-9223372036854775808); "
		"var maximum : u64 = 18446744073709551615; var maximum_inferred := u64(18446744073709551615); "
		"var values : [u64] = [18446744073709551615]; values.push(1); values.insert(0, 2); values.resize(4, 3); var field := WideValue(18446744073709551615); "
		"var callback : function(value : s64) s64 = parameter; "
		"a = 9223372036854775807; print(callback(8) + s64(1), parameter(8), returned(), implicit(), maximum == 18446744073709551615, pick(8), "
		"a, b, c, d, minimum, minimum_inferred, maximum, maximum_inferred, values[0], field.value) }\n", options);
	assert(capy::wasm::validate_bearer_unit(contextual_integers.wasm, {.bearer_abi_version = "11"}).valid);
	for (const auto& [source, expected] : {
			 std::pair{"function CLI(request : dval) { var value := 9223372036854775808 }\n", "outside the s64 range"},
			 std::pair{"function CLI(request : dval) { var value : s64 = 9223372036854775808 }\n", "outside the s64 range"},
			 std::pair{"function CLI(request : dval) { var value : u64 = -1 }\n", "outside the u64 range"},
			 std::pair{"function CLI(request : dval) { var value := u64(18446744073709551616) }\n", "outside the u64 range"},
			 std::pair{"function CLI(request : dval) { var value := s64(-9223372036854775809) }\n", "outside the s64 range"},
				 std::pair{"function CLI(request : dval) { var value := 1s64 }\n", "numeric suffixes were removed"},
			 std::pair{"function CLI(request : dval) { <><script>const value = <?= 1.5 ?>;</script></> }\n", "f64 markup interpolation is not supported"},
			 std::pair{"function CLI(request : dval) { <><script>const value = \"<?= \"x\" ?>\";</script></> }\n", "JavaScript string"},
			 std::pair{"function CLI(request : dval) { <><div title=<?= \"x\" ?>></div></> }\n", "requires a quoted attribute value"},
			 std::pair{"function CLI(request : dval) { var value := dval(\"x\"); <><p><?= value ?></p></> }\n", "markup interpolation does not support dval"},
			 std::pair{"function legacy() markup { -> \"x\" }\nfunction CLI(request : dval) {}\n", "unknown type 'markup'"},
		 })
	{
		try { capy::compile_bearer_unit(source, options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find(expected) != std::string::npos); }
	}
	const auto default_string_arc = capy::compile_bearer_unit(
		"function echo(value : string = \"x\") string { -> value }\nfunction CLI(request : dval) { var value := echo(); print(arc_live()) }\n", options);
	assert(capy::wasm::validate_bearer_unit(default_string_arc.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string dval_default_source =
		"function fresh(opt : dval = {nested: {items: [1, false, \"x\"]}}) dval { -> opt }\n"
		"function CLI(request : dval) { var first := fresh(); first.nested.items[0] = 2; var second := fresh(); print(s32(first.nested.items[0]), s32(second.nested.items[0]), arc_live()) }\n";
	const auto dval_defaults = capy::compile_bearer_unit(dval_default_source, options);
	const auto dval_defaults_repeat = capy::compile_bearer_unit(dval_default_source, options);
	assert(capy::wasm::validate_bearer_unit(dval_defaults.wasm, {.bearer_abi_version = "11"}).valid);
	assert(dval_defaults.wasm == dval_defaults_repeat.wasm && dval_defaults.source_map == dval_defaults_repeat.source_map);
	assert(dval_defaults.source_map.find("\t2\t") != std::string::npos);
	const std::string dval_default_bytes(dval_defaults.wasm.begin(), dval_defaults.wasm.end());
	assert(dval_default_bytes.find("bearer_dv_build_brrb") != std::string::npos);
	const auto exact_before_default = capy::compile_bearer_unit(
		"function select(value : s32) s32 { -> 1 }\nfunction select(value : s32, suffix : string = \"x\") s32 { -> 2 }\nfunction CLI(request : dval) { print(select(1)) }\n", options);
	assert(capy::wasm::validate_bearer_unit(exact_before_default.wasm, {.bearer_abi_version = "11"}).valid);
	const auto dval_constructors = capy::compile_bearer_unit(
		"type Text = string\n"
		"function show(value : as Text) string { -> value }\n"
		"function next_value() dval { -> dval(\"7\") }\n"
		"function next_fallback() string { -> \"fallback\" }\n"
		"function CLI(request : dval) { var value := next_value(); print(string(value), string(value, {fallback: next_fallback()}), bool(value), bool(value, {fallback: true}), s32(value), s32(value, {fallback: 7}), s64(value), s64(value, {fallback: s64(7)}), u64(value), u64(value, {fallback: u64(7)}), f64(value), f64(value, {fallback: 7.0}), show(value), arc_live()) }\n", options);
	assert(capy::wasm::validate_bearer_unit(dval_constructors.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string dval_constructor_bytes(dval_constructors.wasm.begin(), dval_constructors.wasm.end());
	for (const char* import : {"bearer_dv_extract_string", "bearer_dv_extract_bool", "bearer_dv_extract_s32", "bearer_dv_extract_s64", "bearer_dv_extract_u64", "bearer_dv_extract_f64"})
		assert(dval_constructor_bytes.find(import) != std::string::npos);
	const auto unrelated_dval_imports = capy::compile_bearer_unit("function CLI(request : dval) { print(\"plain\") }\n", options);
	const std::string unrelated_dval_bytes(unrelated_dval_imports.wasm.begin(), unrelated_dval_imports.wasm.end());
	assert(unrelated_dval_bytes.find("bearer_format_s64") == std::string::npos && unrelated_dval_bytes.find("bearer_format_u64") == std::string::npos);
	const auto plain_comparison_condition = capy::compile_bearer_unit("function CLI(request : dval) { if 1 == 1 { print(\"x\") } }\n", options);
	const std::string plain_comparison_condition_bytes(plain_comparison_condition.wasm.begin(), plain_comparison_condition.wasm.end());
	assert(plain_comparison_condition_bytes.find("bearer_format_s64") == std::string::npos && plain_comparison_condition_bytes.find("bearer_format_u64") == std::string::npos);
	const auto condition_coercions = capy::compile_bearer_unit(
		"function CLI(request : dval) { "
		"var nonzero_u64 := u64(7); var zero_u64 := u64(0); var nonzero_s32 := 5; var zero_s32 := 0; "
		"var nonzero_dval := dval(u64(7)); var zero_dval := dval(u64(0)); var bool_dval := dval(true); var run := u64(1); var n := u64(0); "
		"if nonzero_u64 { print(\"u64-nonzero\") } if zero_u64 { print(\"u64-zero\") } "
		"if nonzero_s32 { print(\"s32-nonzero\") } if zero_s32 { print(\"s32-zero\") } "
		"if nonzero_dval { print(\"dval-nonzero\") } if zero_dval { print(\"dval-zero\") } if bool_dval { print(\"dval-bool\") } "
		"if true { print(\"bool\") } if dval(\"true\") { print(\"string-true\") } if dval(\"1\") { print(\"string-one\") } if dval(\"yes\") { print(\"string-yes\") } "
		"if dval(\"hello\") { print(\"string-hello\") } if dval(\"0\") { print(\"string-zero\") } if dval(\"\") { print(\"string-empty\") } "
		"while run { run = u64(0) } while n < u64(2) { n = u64(2) } "
		"}\n", options);
	assert(capy::wasm::validate_bearer_unit(condition_coercions.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string condition_coercion_bytes(condition_coercions.wasm.begin(), condition_coercions.wasm.end());
	assert(condition_coercion_bytes.find("bearer_dv_extract_bool") != std::string::npos);
	const auto coercive_parameter_reproducer = capy::compile_bearer_unit(
		"function truth(flag : as bool) bool { -> flag }\n"
		"function CLI(request : dval) { print(truth(dval(\"hello\"))) }\n", options);
	assert(capy::wasm::validate_bearer_unit(coercive_parameter_reproducer.wasm, {.bearer_abi_version = "11"}).valid);
	for (const char* old_name : {"dval_string", "dval_bool", "dval_s32", "dval_f64", "dval_to_string", "dval_to_bool", "dval_to_f64", "dval_to_s64", "dval_to_u64"})
		try { capy::compile_bearer_unit(std::string("function CLI(request : dval) { ") + old_name + "(dval(\"x\")) }\n", options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find("no overload") != std::string::npos); }
	for (const char* old_name : {"request_context", "request_param", "request_get", "request_post", "request_cookie", "request_session", "request_body", "request_base_url", "request_script_url", "request_query_path", "request_query_route", "cli_input", "cli_arg", "ws_message", "ws_connection_id", "ws_scope", "ws_opcode", "ws_is_binary", "ws_connections", "ws_connection_count", "to_bool", "to_f64", "to_s64", "to_u64", "to_lower", "to_upper", "dval_to_json", "dval_to_stringmap", "password_needs_rehash", "ascii_safe_name", "component_exists", "memcache_escape_key", "sha256_hex", "hmac_sha256_hex", "shell_spawn"})
		try { capy::compile_bearer_unit(std::string("function CLI(request : dval) { ") + old_name + "() }\n", options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find("was removed") != std::string::npos); }
	try { capy::compile_bearer_unit("function CLI(request : dval) { request_route_from_raw_path(\"path\") }\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("no overload") != std::string::npos); }
	try { capy::compile_bearer_unit("function CLI(request : dval) { request_perf() }\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("was renamed") != std::string::npos); }
	try { capy::compile_bearer_unit("function CLI(request : request) {}\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("type request was removed") != std::string::npos); }
	for (const auto& [source, expected] : {
			 std::pair{"function ambiguous(value : s32, suffix : string = \"x\") {}\nfunction ambiguous(value : s32, enabled : bool = true) {}\nfunction CLI(request : dval) { ambiguous(1) }\n", "ambiguous default overload"},
			 std::pair{"function sum(left : s32, right : s32 = 1) s32 { -> left + right }\nfunction CLI(request : dval) { var value : function(left : s32, right : s32) s32 = sum; value(1) }\n", "function value argument count"},
			 std::pair{"host function __bearer_bad(value : s32 = 1)\nfunction CLI(request : dval) {}\n", "host declarations are available only"},
			 std::pair{"function RENDER(value : s32 = 1) {}\n", "Bearer handlers cannot use default parameters"},
			 std::pair{"function wrong(value : s32 = 1.5) {}\nfunction CLI(request : dval) {}\n", "default parameter literal must have type s32"},
			 std::pair{"function wrong(value : s32 = {}) {}\nfunction CLI(request : dval) {}\n", "default parameter literal must have type s32"},
			 std::pair{"function generic(value : any = 1) value::type { -> value }\nfunction CLI(request : dval) {}\n", "generic functions cannot use default parameters"},
			 std::pair{"function values(...value : s32 = 1) {}\nfunction CLI(request : dval) {}\n", "variadic parameter cannot have a default value"},
			 std::pair{"function CLI(request : dval) { var value := function(input : s32 = 1) s32 { -> input } }\n", "anonymous functions cannot use default parameters"},
			 std::pair{"function CLI(request : dval) { var value : function(input : s32 = 1) s32 = function(input : s32) s32 { -> input } }\n", "function types cannot use default parameters"},
		 })
	{
		try { capy::compile_bearer_unit(source, options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find(expected) != std::string::npos); }
	}
	const auto tasks = capy::compile_bearer_unit(
		"function TASK(request : dval) {}\nfunction TASK:NAME(request : dval) {}\n", options);
	const auto tasks_repeat = capy::compile_bearer_unit(
		"function TASK(request : dval) {}\nfunction TASK:NAME(request : dval) {}\n", options);
	assert(tasks.wasm == tasks_repeat.wasm && tasks.source_map == tasks_repeat.source_map);
	const auto task_validation = capy::wasm::validate_bearer_unit(tasks.wasm, {.bearer_abi_version = "11"});
	assert(task_validation.valid && task_validation.exports.size() == 2);
	assert(task_validation.exports[0].name == "__bearer_task" && task_validation.exports[0].kind == 0);
	assert(task_validation.exports[1].name == "__bearer_task_NAME" && task_validation.exports[1].kind == 0);
	assert(std::none_of(task_validation.imports.begin(), task_validation.imports.end(),
		[](const auto& imported) { return imported.module == "env" && imported.name == "bearer_handler_input_brrb"; }));
	assert(std::string(tasks.wasm.begin(), tasks.wasm.end()).find("bearer_request") == std::string::npos);
	const auto named_handlers = capy::compile_bearer_unit(
		"function RENDER:NAME(request : dval) {}\nfunction COMPONENT:NAME(request : dval) {}\nfunction SERVE_HTTP:NAME(request : dval) {}\n", options);
	const auto named_validation = capy::wasm::validate_bearer_unit(named_handlers.wasm, {.bearer_abi_version = "11"});
	assert(named_validation.valid);
	for (const std::string name : {"__bearer_render_NAME", "__bearer_component_NAME", "__bearer_serve_http_NAME"})
		assert(std::any_of(named_validation.exports.begin(), named_validation.exports.end(),
						   [&](const auto& exported) { return exported.name == name && exported.kind == 0; }));
	const auto empty_handler = capy::compile_bearer_unit("function RENDER(request : dval) {}\n", options);
	const auto empty_validation = capy::wasm::validate_bearer_unit(empty_handler.wasm, {.bearer_abi_version = "11"});
	assert(empty_validation.valid);
	assert(std::none_of(empty_validation.imports.begin(), empty_validation.imports.end(),
		[](const auto& imported) { return imported.module == "env" && (imported.name == "bearer_format_s64" || imported.name == "bearer_format_u64"); }));
	for (const auto& [source, expected] : {
			 std::pair{"function TASK() {}\n", "exactly one dval parameter"},
			 std::pair{"function TASK(request : dval, extra : dval) {}\n", "exactly one dval parameter"},
			 std::pair{"function TASK(value : s32) {}\n", "exactly one dval parameter"},
			 std::pair{"function TASK(request : dval) s32 { -> 1 }\n", "must return void"},
			 std::pair{"function TASK:NAME() {}\n", "exactly one dval parameter"},
			 std::pair{"function TASK:NAME(request : dval, extra : dval) {}\n", "exactly one dval parameter"},
			 std::pair{"function TASK:NAME(value : s32) {}\n", "exactly one dval parameter"},
			 std::pair{"function TASK:NAME(request : dval) s32 { -> 1 }\n", "must return void"},
			 std::pair{"function TASK:NAME(request : dval) {}\nfunction TASK:NAME(request : dval) {}\n", "duplicate"},
			 std::pair{"function TASK_NAME(request : dval) {}\n", "use ':'"},
			 std::pair{"function CLI:NAME {}\n", "named handler syntax applies only"},
			 std::pair{"function EXPORT___bearer_task(value : dval) dval { -> value }\nfunction TASK(request : dval) {}\n", "collides with custom DValue export"},
		 })
	{
		try { capy::compile_bearer_unit(source, options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find(expected) != std::string::npos); }
	}
	std::ofstream output("/tmp/capy-native.wasm", std::ios::binary);
	output.write(reinterpret_cast<const char*>(result.wasm.data()), result.wasm.size());

	constexpr std::string_view arc_cases[] = {
		"function CLI(request : dval) { var value := clone(\"x\") }\n",
		"function CLI(request : dval) { var value := clone(\"x\"); value = clone(\"y\") }\n",
		"function borrow(x : string) string { return x }\nfunction CLI(request : dval) { var value := borrow(\"x\") }\n",
		"function make() string { return clone(\"x\") }\nfunction CLI(request : dval) { var value := make() }\n",
		"function early(x : string) string { var discarded := clone(\"x\"); if true { return x }; return discarded }\nfunction CLI(request : dval) { var value := early(\"x\") "
		"}\n",
		"function CLI(request : dval) { var values := [clone(\"x\"), clone(\"y\")]; var first : string = values[0] }\n",
		"function CLI(request : dval) { var value := clone(\"x\"); print(arc_live()) }\n",
	};
	for (const auto source : arc_cases)
	{
		const auto compiled = capy::compile_bearer_unit(source, options);
		assert(capy::wasm::validate_bearer_unit(compiled.wasm, {.bearer_abi_version = "11"}).valid);
	}
	const auto scalar_parameter_reassignment = capy::compile_bearer_unit(
		"function change(value : s32) s32 { value = 2; -> value }\nfunction CLI(request : dval) { print(change(1)) }\n", options);
	assert(capy::wasm::validate_bearer_unit(scalar_parameter_reassignment.wasm, {.bearer_abi_version = "11"}).valid);
	const auto managed_parameter_reassignment = capy::compile_bearer_unit(
		"function change_text(value : string) string { value = value + \"!\"; return value }\n"
		"function change_array(values : [s32]) [s32] { values = [2]; return values }\n"
		"function CLI(request : dval) { var text := \"x\"; var values : [s32] = [1]; print(change_text(text), text, change_array(values)[0], values[0]) }\n", options);
	assert(capy::wasm::validate_bearer_unit(managed_parameter_reassignment.wasm, {.bearer_abi_version = "11"}).valid);
	for (const auto& [source, expected] : {
			 std::pair{"function CLI(request : dval) { var value := 1; var closure := function() void { value = 2 } }\n", "cannot assign to captured binding 'value'. Captures are immutable"},
			 std::pair{"function CLI(request : dval) { var value := clone(\"x\"); var closure := function() void { value = clone(\"y\") } }\n", "cannot assign to captured binding 'value'. Captures are immutable"},
		 })
	{
		try { capy::compile_bearer_unit(source, options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find(expected) != std::string::npos); }
	}
	const std::string bare_dval_source =
		"function CLI(request : dval) {\n"
		"var value := {name: clone(\"Ada\"), nested: {answer: 42}, values: dval([3])}\n"
		"print(value.name, s32(value.nested.answer), s32(value.values[0]), arc_live())\n"
		"}\n";
	const auto bare_dval = capy::compile_bearer_unit(bare_dval_source, options);
	const auto bare_dval_repeat = capy::compile_bearer_unit(bare_dval_source, options);
	assert(capy::wasm::validate_bearer_unit(bare_dval.wasm, {.bearer_abi_version = "11"}).valid);
	assert(bare_dval.wasm == bare_dval_repeat.wasm && bare_dval.source_map == bare_dval_repeat.source_map);
	const std::string bare_dval_bytes(bare_dval.wasm.begin(), bare_dval.wasm.end());
	assert(bare_dval_bytes.find("bearer_dv_build_brrb") != std::string::npos);
	assert(bare_dval_bytes.find("bearer_dv_read_brrb") != std::string::npos);
	assert(bare_dval_bytes.find("bearer_dv_get_brrb") != std::string::npos);
	assert(bare_dval.source_map.find("\t3\t19\n") != std::string::npos);
	const auto dval_has_only = capy::compile_bearer_unit("function CLI(request : dval) { var value := {}; print(has(value, \"key\")) }\n", options);
	const std::string dval_has_only_bytes(dval_has_only.wasm.begin(), dval_has_only.wasm.end());
	assert(dval_has_only_bytes.find("bearer_dv_get_brrb") != std::string::npos);
	assert(dval_has_only_bytes.find("bearer_dv_read_brrb") == std::string::npos);
	const auto dval_none_and_paths = capy::compile_bearer_unit(
		"function selector() string { -> \"key\" }\nfunction replacement() string { -> \"value\" }\n"
		"function CLI(request : dval) { var value := {items: [{}]}; var missing := value.missing; var absent := missing?; value.items[0][selector()].name = replacement(); print(value.items[0][\"key\"].name, absent, arc_live()) }\n", options);
	const auto dval_none_and_paths_repeat = capy::compile_bearer_unit(
		"function selector() string { -> \"key\" }\nfunction replacement() string { -> \"value\" }\n"
		"function CLI(request : dval) { var value := {items: [{}]}; var missing := value.missing; var absent := missing?; value.items[0][selector()].name = replacement(); print(value.items[0][\"key\"].name, absent, arc_live()) }\n", options);
	const auto dval_none_and_paths_validation = capy::wasm::validate_bearer_unit(dval_none_and_paths.wasm, {.bearer_abi_version = "11"});
	assert(dval_none_and_paths_validation.valid && dval_none_and_paths.wasm == dval_none_and_paths_repeat.wasm && dval_none_and_paths.source_map == dval_none_and_paths_repeat.source_map);
	std::set<std::string> dval_path_imports;
	for (const auto& imported : dval_none_and_paths_validation.imports) dval_path_imports.insert(imported.name);
	for (const char* name : {"bearer_dv_read_brrb", "bearer_dv_is_none_brrb", "bearer_dv_set_path_brrb", "bearer_dv_string_to_brrb", "bearer_dv_s32_to_brrb"}) assert(dval_path_imports.contains(name));
	assert(!dval_path_imports.contains("bearer_dv_s64_to_brrb"));
	const auto none_literal = capy::compile_bearer_unit("function CLI(request : dval) { var value := none; print(value?) }\n", options);
	assert(capy::wasm::validate_bearer_unit(none_literal.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string none_literal_bytes(none_literal.wasm.begin(), none_literal.wasm.end());
	assert(none_literal_bytes.find("bearer_dv_none_brrb") != std::string::npos);
	for (const char* unrelated : {"bearer_dv_get_brrb", "bearer_dv_read_brrb", "bearer_dv_string_to_brrb", "bearer_dv_s32_to_brrb"})
		assert(none_literal_bytes.find(unrelated) == std::string::npos);
	const auto mutable_dval_roots = capy::compile_bearer_unit(
		"function write(value : dval) { value.key = 1; var closure := function() void { value.captured = 2 }; closure() }\n"
		"function CLI(request : dval) { var value := {}; write(value); set(value, \"mutated\", true) }\n", options);
	assert(capy::wasm::validate_bearer_unit(mutable_dval_roots.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string mutable_dval_root_bytes(mutable_dval_roots.wasm.begin(), mutable_dval_roots.wasm.end());
	assert(mutable_dval_root_bytes.find("bearer_dval_replace") == std::string::npos);
	for (const auto& [source, expected] : {
			std::pair{"function CLI(request : dval) { dval({}).key = 1 }\n", "requires a local, parameter, or captured dval root"},
			std::pair{"function make() dval { -> {} }\nfunction CLI(request : dval) { make().key = 1 }\n", "requires a local, parameter, or captured dval root"},
			std::pair{"struct Holder { value : dval }\nfunction CLI(request : dval) { var holder := Holder({}); holder.value.key = 1 }\n", "requires a local, parameter, or captured dval root"},
			std::pair{"function CLI(request : dval) { var values : [dval] = [dval({})]; values[0].key = 1 }\n", "requires a local, parameter, or captured dval root"},
			std::pair{"function CLI(request : dval) { var values := dval([{}]); for item := values { item.key = 1 } }\n", "requires a local, parameter, or captured dval root"},
			std::pair{"function CLI(request : dval) { var value := {}; value.key = 1; value[true] = 2 }\n", "dval index must be string or s64"},
		})
		try { capy::compile_bearer_unit(source, options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find(expected) != std::string::npos); }
	const auto empty_bare_dval = capy::compile_bearer_unit("function CLI(request : dval) { var value := {}; dval({}); {} }\n", options);
	assert(capy::wasm::validate_bearer_unit(empty_bare_dval.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string empty_bare_dval_bytes(empty_bare_dval.wasm.begin(), empty_bare_dval.wasm.end());
	assert(empty_bare_dval_bytes.find("bearer_dv_build_brrb") != std::string::npos);
	const auto dval_member_method = capy::compile_bearer_unit(
		"function take(receiver : dval) dval { -> receiver }\nfunction CLI(request : dval) { var value := {}; value.take() }\n", options);
	assert(capy::wasm::validate_bearer_unit(dval_member_method.wasm, {.bearer_abi_version = "11"}).valid);
	const auto missing_member = capy::compile_bearer_unit("function CLI(request : dval) {\nvar value := {}\nstring(value.missing)\n}\n", options);
	assert(missing_member.source_map.find("\t3\t13\n") != std::string::npos);
	try { capy::compile_bearer_unit("function CLI(request : dval) { var value := (1).missing }\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("member access requires a struct") != std::string::npos); }
	const auto converted_dvals = capy::compile_bearer_unit(
		"function accept(value : as dval) dval { -> value }\n"
		"function CLI(request : dval) { var a := accept(\"x\"); var b := accept(2); var c := accept(s64(-3)); var d := accept(u64(4)); var e := accept(5.5); var f := accept(true) }\n", options);
	const std::string converted_dval_bytes(converted_dvals.wasm.begin(), converted_dvals.wasm.end());
	for (const std::string& import : {"bearer_dv_string_to_brrb", "bearer_dv_s64_to_brrb", "bearer_dv_u64_to_brrb", "bearer_dv_f64_to_brrb", "bearer_dv_bool_to_brrb"})
		assert(converted_dval_bytes.find(import) != std::string::npos);
	const auto stdlib_converted_dvals = capy::compile_bearer_unit(
		"function CLI(request : dval) { var a := dval(\"x\"); var b := dval(1); var c := push(dval([]), u64(2)); var d := set({}, \"ok\", true); "
		"var task_id := task(\"/task\", 3.5); var output := component(\"/component\", false); component_render(\"/component\", \"props\"); "
		"var loaded := unit_load(\"/unit\"); var called := loaded.call(\"echo\", 4); var direct := unit_call(\"/unit\", \"echo\", \"input\") }\n", options);
	assert(capy::wasm::validate_bearer_unit(stdlib_converted_dvals.wasm, {.bearer_abi_version = "11"}).valid);
	const auto component_render_result = capy::compile_bearer_unit(
		"function CLI(request : dval) { var plain := component_render(\"/component\"); var props := component_render(\"/component\", \"props\"); if plain && props {} component_render(\"/component\"); component_render(\"/component\", \"props\") }\n", options);
	assert(capy::wasm::validate_bearer_unit(component_render_result.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string component_render_result_bytes(component_render_result.wasm.begin(), component_render_result.wasm.end());
	assert(component_render_result_bytes.find("bearer_component_render_bytes") != std::string::npos);
	assert(component_render_result_bytes.find("bearer_component_render_props_brrb") != std::string::npos);
	const auto generic_before_dval = capy::compile_bearer_unit(
		"function choose(value : any) string { -> \"generic\" }\nfunction choose(value : as dval) string { -> \"dval\" }\nfunction CLI(request : dval) { print(choose(1)) }\n", options);
	const std::string generic_before_dval_bytes(generic_before_dval.wasm.begin(), generic_before_dval.wasm.end());
	assert(generic_before_dval_bytes.find("bearer_dv_s32_to_brrb") == std::string::npos);
	for (const char* source : {
		"function accept(value : as dval) dval { -> value }\nfunction CLI(request : dval) { accept([1]) }\n",
		"struct Value { item : s32 }\nfunction accept(value : as dval) dval { -> value }\nfunction CLI(request : dval) { accept(Value(1)) }\n",
	})
	{
		try { capy::compile_bearer_unit(source, options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find("no overload accept") != std::string::npos); }
	}
	try { capy::compile_bearer_unit("function CLI(request : dval) { if var wrong : bool = 1 {} }\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("expected bool, found s64") != std::string::npos); }

	constexpr std::string_view generic_source = "function identity(value : any) value::type { -> value }\n"
												"function choose(value : any) value::type { -> value }\n"
												"function choose(value : s32) value::type { -> value + 1 }\n"
												"function countdown(value : any) value::type { if value == 0 { return value }; return countdown(value - 1) }\n"
												"function CLI(request : dval) { print(identity(7), choose(4), countdown(3), identity([clone(\"first\"), clone(\"x\")])[1], bool(2)) }\n";
	const auto generic = capy::compile_bearer_unit(generic_source, options);
	assert(capy::wasm::validate_bearer_unit(generic.wasm, {.bearer_abi_version = "11"}).valid);
	const auto generic_method = capy::compile_bearer_unit(
		"function same(value : any) value::type { -> value }\nfunction CLI(request : dval) { print((7).same()) }\n", options);
	assert(capy::wasm::validate_bearer_unit(generic_method.wasm, {.bearer_abi_version = "11"}).valid);
	assert(generic_method.source_map.find("F\t1\tnative-test.capy\n") != std::string::npos);
	const auto converted_parameters = capy::compile_bearer_unit(
		"function text(value : as string) string { -> value }\nfunction wide(value : as s64) s64 { -> value }\n"
		"function CLI(request : dval) { print(text(42), text(false), text([1][0]), wide(u64(7)), wide(1e300), string(8)) }\n", options);
	const auto converted_parameters_validation = capy::wasm::validate_bearer_unit(converted_parameters.wasm, {.bearer_abi_version = "11"});
	assert(converted_parameters_validation.valid);
	assert(std::none_of(converted_parameters_validation.imports.begin(), converted_parameters_validation.imports.end(),
		[](const auto& imported) { return imported.name == "bearer_format_f64"; }));
	const auto converted_dval_parameters = capy::compile_bearer_unit(
		"type Text = string\nfunction text(value : as Text) string { -> value }\nfunction truth(value : as bool) bool { -> value }\n"
		"function narrow(value : as s32) s32 { -> value }\nfunction signed(value : as s64) s64 { -> value }\n"
		"function unsigned(value : as u64) u64 { -> value }\nfunction decimal(value : as f64) f64 { -> value }\n"
		"function CLI(request : dval) { var value := dval(\"1\"); print(text(value), truth(value), narrow(value), signed(value), unsigned(value), decimal(value)) }\n", options);
	const auto converted_dval_validation = capy::wasm::validate_bearer_unit(converted_dval_parameters.wasm, {.bearer_abi_version = "11"});
	assert(converted_dval_validation.valid);
	std::set<std::string> converted_dval_imports;
	for (const auto& imported : converted_dval_validation.imports)
		converted_dval_imports.insert(imported.name);
	for (const std::string& target : {"string", "bool", "s32", "s64", "u64", "f64"})
		assert(converted_dval_imports.contains("bearer_dv_extract_" + target));
	const auto direct_dval_print = capy::compile_bearer_unit(
		"function CLI(request : dval) { var profile := {nickname: clone(\"Capy\")}; print(profile[\"nickname\"]); "
		"var printer : function(...values : as string) void = print; printer(dval(clone(\"again\"))) }\n", options);
	const auto direct_dval_print_validation = capy::wasm::validate_bearer_unit(direct_dval_print.wasm, {.bearer_abi_version = "11"});
	assert(direct_dval_print_validation.valid);
	assert(std::any_of(direct_dval_print_validation.imports.begin(), direct_dval_print_validation.imports.end(),
		[](const auto& imported) { return imported.name == "bearer_dv_extract_string"; }));
	const auto mixed_dval_print = capy::compile_bearer_unit(
		"function CLI(request : dval) { var profile := {name: \"Ada\"}; print(\"x\", profile.name) }\n", options);
	assert(capy::wasm::validate_bearer_unit(mixed_dval_print.wasm, {.bearer_abi_version = "11"}).valid);
	const auto scalar_print = capy::compile_bearer_unit("function CLI(request : dval) { print(\"ok\", 1, true) }\n", options);
	const auto scalar_print_validation = capy::wasm::validate_bearer_unit(scalar_print.wasm, {.bearer_abi_version = "11"});
	assert(scalar_print_validation.valid);
	for (const auto& imported : scalar_print_validation.imports)
		assert(imported.name.rfind("bearer_dv_extract_", 0) != 0);
	const auto constructors_and_variadics = capy::compile_bearer_unit(
		"struct Token { value : string }\n"
		"struct Pair { left : s32; right : string }\n"
		"function Token(value : s32) Token { -> Token(string(value)) }\n"
		"function token_text(value : Token) string { -> value.value }\n"
		"function collect(...values : as string) [string] { -> values }\n"
		"function CLI(request : dval) { var values : [string] = []; values.push(token_text(Token(7))); var copy := values; values[0] = \"x\"; print(...copy, ...collect(1, 2), Pair(s32(1), \"p\").right) }\n", options);
	assert(capy::wasm::validate_bearer_unit(constructors_and_variadics.wasm, {.bearer_abi_version = "11"}).valid);
	const auto variadic_function_field = capy::compile_bearer_unit(
		"function compose(...values : as string) string { -> values[0] }\n"
		"struct Collector { invoke : function(...values : as string) string }\n"
		"function CLI(request : dval) { var collector := Collector(compose); print(collector.invoke(...[1, 2])) }\n", options);
	const auto variadic_field_validation = capy::wasm::validate_bearer_unit(variadic_function_field.wasm, {.bearer_abi_version = "11"});
	assert(variadic_field_validation.valid);
	for (const auto& imported : variadic_field_validation.imports)
		assert(imported.name != "bearer_format_s64" && imported.name != "bearer_format_u64");
	const auto inferred_managed_local = capy::compile_bearer_unit(
		"function CLI(request : dval) { var greeting := \"Hello\" }\n", options);
	assert(capy::wasm::validate_bearer_unit(inferred_managed_local.wasm, {.bearer_abi_version = "11"}).valid);
	const auto standalone_struct = capy::compile_bearer_unit(
		"struct Point { x : s32; y : s32 }\nfunction CLI(request : dval) { var point := Point(3, 4); print(point.x) }\n", options);
	const auto standalone_struct_validation = capy::wasm::validate_bearer_unit(standalone_struct.wasm, {.bearer_abi_version = "11"});
	assert(standalone_struct_validation.valid);
	assert(std::any_of(standalone_struct_validation.imports.begin(), standalone_struct_validation.imports.end(),
		[](const auto& imported) { return imported.name == "bearer_alloc"; }));
	const auto unused_conversion = capy::compile_bearer_unit(
		"function text(value : as string) string { -> value }\nfunction CLI(request : dval) {}\n", options);
	const auto unused_conversion_validation = capy::wasm::validate_bearer_unit(unused_conversion.wasm, {.bearer_abi_version = "11"});
	assert(unused_conversion_validation.valid);
	for (const auto& imported : unused_conversion_validation.imports)
		assert(imported.name.rfind("bearer_format_", 0) != 0);
	const auto module_exports = capy::compile_bearer_unit(
		"#exports invoke\n#exports other\nfunction invoke(value : dval) dval { -> value }\nfunction other(value : dval) dval { -> value }\n", options);
	assert(module_exports.custom_exports == std::vector<std::string>({"invoke", "other"}));
	assert(module_exports.function_exports == std::vector<std::string>({"capy function invoke(dval):dval", "capy function other(dval):dval"}));
	assert(module_exports.source_map.find("F\t1\tnative-test.capy\n") != std::string::npos);
	assert(capy::wasm::validate_bearer_unit(module_exports.wasm, {.bearer_abi_version = "11"}).valid);
	const auto type_exports = capy::compile_bearer_unit(
		"#exports echo, MyType, Alias\nstruct MyType { value : s32 }\ntype Alias = MyType\nfunction echo(value : dval) dval { -> value }\nfunction MyType(value : dval) MyType { -> MyType(1) }\n", options);
	assert(type_exports.custom_exports == std::vector<std::string>({"echo"}));
	assert(type_exports.function_exports == std::vector<std::string>({"capy function echo(dval):dval", "capy function MyType(dval):struct:MyType"}));
	assert(type_exports.type_exports == std::vector<std::string>({"capy type struct MyType{value:s32}", "capy type alias Alias=struct:MyType"}));
	const auto module_call = capy::compile_bearer_unit(
		"function CLI(request : dval) { var loaded : module = unit_load(\"child.capy\"); loaded.call(\"invoke\") }\n", options);
	const std::string module_call_bytes(module_call.wasm.begin(), module_call.wasm.end());
	assert(module_call_bytes.find("bearer_unit_load") != std::string::npos && module_call_bytes.find("bearer_module_call_brrb") != std::string::npos);
	assert(capy::wasm::validate_bearer_unit(module_call.wasm, {.bearer_abi_version = "11"}).valid);
	const auto module_member_call = capy::compile_bearer_unit(
		"function CLI(request : dval) { var loaded : module = unit_load(\"child.capy\"); loaded.invoke(); loaded.invoke({value: 1}) }\n", options);
	const std::string module_member_call_bytes(module_member_call.wasm.begin(), module_member_call.wasm.end());
	assert(module_member_call_bytes.find("bearer_unit_load") != std::string::npos && module_member_call_bytes.find("bearer_module_call_brrb") != std::string::npos);
	assert(capy::wasm::validate_bearer_unit(module_member_call.wasm, {.bearer_abi_version = "11"}).valid);
	assert(module_call.source_map.find("F\t2\tcapy://stdlib.capy\n") != std::string::npos);
	const auto module_passthrough = capy::compile_bearer_unit(
		"function pass(value : module) module { -> value }\n"
		"function identity(value : any) value::type { -> value }\n"
		"function CLI(request : dval) { var loaded : module = unit_load(\"child.capy\"); var returned : module = pass(loaded); identity(returned).call(\"invoke\") }\n", options);
	assert(capy::wasm::validate_bearer_unit(module_passthrough.wasm, {.bearer_abi_version = "11"}).valid);
	capy::CompileOptions import_options = options;
	import_options.import_type_metadata = [](const std::string& path) {
		assert(path == "child.capy");
		return std::vector<std::string>({"capy type struct Shape{value:s32}", "capy type alias Other=struct:Shape"});
	};
	const auto imported_types = capy::compile_bearer_unit(
		"#import \"child.capy\" as child\ntype LocalShape = child.Shape\ntype LocalOther = child.Other\nfunction CLI(request : dval) {}\n", import_options);
	assert(capy::wasm::validate_bearer_unit(imported_types.wasm, {.bearer_abi_version = "11"}).valid);
	for (const auto& [source, expected] : {
			 std::pair{"#exports missing\nfunction CLI(request : dval) {}\n", "unknown local function or type 'missing'"},
			 std::pair{"#exports invoke\nfunction EXPORT_invoke(value : dval) dval { -> value }\nfunction CLI(request : dval) {}\n", "already declared"},
			 std::pair{"function CLI(request : dval) { var loaded : module = unit_load(\"x\"); print(loaded + loaded) }\n", "unsupported operator + for module"},
			 std::pair{"function CLI(request : dval) { var loaded : module = unit_load(\"x\"); if loaded {} }\n", "module is opaque and cannot be used as a condition"},
			 std::pair{"function CLI(request : dval) { var loaded : module = unit_load(\"x\"); s32(loaded) }\n", "no overload s32(module)"},
			 std::pair{"function CLI(request : dval) { var loaded : module = unit_load(\"x\"); loaded.echo({a: 1}, {b: 2}) }\n", "dynamic module member call accepts at most one dval input"},
			 std::pair{"function CLI(request : dval) { var loaded : module = unit_load(\"x\"); var name := loaded.echo }\n", "module member access must be called"},
			 std::pair{"function bad(value : as [s32]) {}\n", "requires a concrete named type constructor"},
				 std::pair{"function CLI(request : dval) { var callback : function(value : as s64) s64 = function(value : s64) s64 { -> value } }\n", "only variadic function-type parameters may request conversion"},
			 std::pair{"function CLI(request : dval) { var value := 1 as s64 }\n", "call the target type constructor instead"},
			 std::pair{"struct Point { x : s32 }\nfunction string(value : any) string { -> value + \"!\" }\nfunction show(value : as string) {}\nfunction CLI(request : dval) { show(Point(1)) }\n", "string operators require string operands"},
			 std::pair{"function s32(value : string) string { -> value }\nfunction CLI(request : dval) {}\n", "constructor 's32' must return s32"},
			 std::pair{"struct Point { x : s32 }\nfunction Point(x : s32) Point { -> Point(x) }\nfunction CLI(request : dval) {}\n", "duplicates the generated Point field constructor"},
			 std::pair{"function CLI(request : dval) { var loaded : module = unit_load(\"x\"); var values := [loaded] }\n", "module is opaque and cannot be stored in array layouts"},
			 std::pair{"struct Box { handle : module }\nfunction CLI(request : dval) {}\n", "module is opaque and cannot be stored in struct layouts"},
			 std::pair{"function CLI(request : dval) { var loaded : module = unit_load(\"x\"); var f := function() module { -> loaded } }\n", "module is opaque and cannot be captured by a closure"},
		 })
	{
		try { capy::compile_bearer_unit(source, options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find(expected) != std::string::npos); }
	}
	const auto returning = capy::compile_bearer_unit(
		"function choose(value : bool) s32 { if value { return 1 } else { return 2 } }\nfunction CLI(request : dval) { print(choose(true)) }\n", options);
	assert(capy::wasm::validate_bearer_unit(returning.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string aliases_source =
		"type Count = s64\ntype Counts = [Count]\ntype Pair = CountText\ntype Printer = function(...values : as string) void\n"
		"type User = Person\ntype Json = dval\nstruct CountText { count : Count; text : string }\nstruct Person { value : s32 }\n"
		"function take(value : Count) Count { -> value }\nfunction CLI(request : dval) { var values : Counts = [Count(4)]; var pair : Pair = Pair(values[0], \"x\"); "
		"var user : User = User(7); var json : Json = Json({value: user.value}); var output : Printer = print; "
		"output(take(pair.count), pair.text, s32(json[\"value\"])) }\n";
	const auto aliases = capy::compile_bearer_unit(aliases_source, options);
	const auto aliases_repeat = capy::compile_bearer_unit(aliases_source, options);
	assert(capy::wasm::validate_bearer_unit(aliases.wasm, {.bearer_abi_version = "11"}).valid);
	assert(aliases.wasm == aliases_repeat.wasm && aliases.source_map == aliases_repeat.source_map);
	for (const auto& [source, expected] : {
			 std::pair{"const x : s32 = 1\nfunction CLI(request : dval) {}\n", "const declarations were removed"},
			 std::pair{"function CLI(request : dval) { var none := dval(\"x\") }\n", "'none' is a reserved literal"},
			 std::pair{"type A = B\ntype B = A\nfunction CLI(request : dval) {}\n", "cyclic type alias"},
			 std::pair{"type MissingAlias = Missing\nfunction CLI(request : dval) {}\n", "unknown type 'Missing'"},
			 std::pair{"type s32 = u64\nfunction CLI(request : dval) {}\n", "is reserved"},
			 std::pair{"type Thing = s32\nstruct Thing { value : s32 }\nfunction CLI(request : dval) {}\n", "conflicts with type alias"},
			 std::pair{"type Id = s32\nfunction use(value : Id) {}\nfunction use(value : s32) {}\nfunction CLI(request : dval) {}\n", "return type does not distinguish overloads"},
		 })
	{
		try { capy::compile_bearer_unit(source, options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find(expected) != std::string::npos); }
	}
	const auto boundary = capy::compile_bearer_unit("function CLI(request : dval) { print(-2147483648, 2147483647) }\n", options);
	assert(capy::wasm::validate_bearer_unit(boundary.wasm, {.bearer_abi_version = "11"}).valid);
	const auto ordinary_sqlite = capy::compile_bearer_unit("function CLI(request : dval) { var db := sqlite_connect(\":memory:\"); print(sqlite_info(db, \"error\")); sqlite_disconnect(db) }\n", options);
	assert(capy::wasm::validate_bearer_unit(ordinary_sqlite.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string sqlite_bytes(ordinary_sqlite.wasm.begin(), ordinary_sqlite.wasm.end());
	assert(sqlite_bytes.find("bearer_sqlite_connect") != std::string::npos);
	assert(sqlite_bytes.find("bearer_sqlite_error") != std::string::npos);
	assert(sqlite_bytes.find("bearer_hard_error") != std::string::npos);
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
	const auto flush_output = capy::compile_bearer_unit("function RENDER(request : dval) { print(\"first\"); flush_output(); print(\"second\") }\n", options);
	assert(capy::wasm::validate_bearer_unit(flush_output.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string flush_output_bytes(flush_output.wasm.begin(), flush_output.wasm.end());
	assert(flush_output_bytes.find("bearer_flush_output") != std::string::npos);
	const auto no_stdlib_demand = capy::compile_bearer_unit("function CLI(request : dval) { print(1) }\n", options);
	const std::string no_stdlib_bytes(no_stdlib_demand.wasm.begin(), no_stdlib_demand.wasm.end());
	assert(no_stdlib_bytes.find("bearer_sqlite_") == std::string::npos && no_stdlib_bytes.find("bearer_mysql_") == std::string::npos && no_stdlib_bytes.find("bearer_capy_backtrace") == std::string::npos);
	assert(no_stdlib_bytes.find("bearer_format_s64") == std::string::npos && no_stdlib_bytes.find("bearer_format_u64") == std::string::npos && no_stdlib_bytes.find("bearer_print_bytes") != std::string::npos);
	assert(no_stdlib_bytes.find("bearer_alloc") != std::string::npos && no_stdlib_bytes.find("bearer_free") != std::string::npos && no_stdlib_bytes.find("bearer_handler_input_brrb") == std::string::npos);
	assert(no_stdlib_demand.source_map.find("\t1\t1\t32\n") != std::string::npos);
	const auto literal_print = capy::compile_bearer_unit("function CLI(request : dval) { print(\"ok\") }\n", options);
	assert(literal_print.wasm.size() < empty_handler.wasm.size() + 600);
	const auto f64_print = capy::compile_bearer_unit("function emit(value : f64) { print(value) }\nfunction CLI(request : dval) { emit(1.5) }\n", options);
	const std::string f64_print_bytes(f64_print.wasm.begin(), f64_print.wasm.end());
	assert(f64_print_bytes.find("bearer_format_f64") == std::string::npos && f64_print_bytes.find("bearer_print_bytes") != std::string::npos);
	assert(f64_print_bytes.find("bearer_print_f64") == std::string::npos && f64_print_bytes.find("bearer_print_s64") == std::string::npos &&
		f64_print_bytes.find("bearer_print_u64") == std::string::npos);
	assert(f64_print_bytes.find("bearer_handler_input_brrb") == std::string::npos);
	const auto contextual_host = capy::compile_bearer_unit("function CLI(request : dval) { print(gen_noise64(1, 2)) }\n", options);
	assert(std::string(contextual_host.wasm.begin(), contextual_host.wasm.end()).find("bearer_noise_u64") != std::string::npos);
	const auto contextual_f32 = capy::compile_bearer_unit("function take(value : f32) {}\nfunction CLI(request : dval) { take(1.5) }\n", options);
	const std::string contextual_f32_bytes(contextual_f32.wasm.begin(), contextual_f32.wasm.end());
	assert(contextual_f32_bytes.find(std::string("\x43\x00\x00\xc0\x3f", 5)) != std::string::npos);
	assert(contextual_f32_bytes.find(std::string("\x44\x00\x00\x00\x00\x00\x00\xf8\x3f", 9)) == std::string::npos);
	try { capy::compile_bearer_unit("function take(value : f32) {}\nfunction CLI(request : dval) { take(1e100) }\n", options); assert(false); }
	catch (const capy::Error& error) { assert(error.message.find("outside the f32 range") != std::string::npos); }
	const auto print_value = capy::compile_bearer_unit(
		"function CLI(request : dval) { var output : function(...values : as string) void = print; output(1, \"x\") }\n", options);
	assert(capy::wasm::validate_bearer_unit(print_value.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string print_value_bytes(print_value.wasm.begin(), print_value.wasm.end());
	assert(print_value_bytes.find("bearer_alloc") != std::string::npos && print_value_bytes.find("bearer_free") != std::string::npos);
	const auto ordinary_backtrace = capy::compile_bearer_unit(
		"function outer() string { -> inner() }\nfunction inner() string { var capture := function() string { -> backtrace_get_frames(2, 1) }; -> capture() }\nfunction CLI(request : dval) { print(outer(), backtrace_get_frames()) }\n", options);
	assert(capy::wasm::validate_bearer_unit(ordinary_backtrace.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string backtrace_bytes(ordinary_backtrace.wasm.begin(), ordinary_backtrace.wasm.end());
	assert(backtrace_bytes.find("bearer_capy_backtrace") != std::string::npos);
	assert(backtrace_bytes.find("void*") == std::string::npos);
	assert(ordinary_backtrace.source_map.find("F\t2\tcapy://stdlib.capy\n") != std::string::npos);
	capy::CompileOptions control_path_options = options;
	control_path_options.source_path = "tab\tline\n\033.capy";
	const auto control_path_backtrace = capy::compile_bearer_unit("function CLI(request : dval) { print(backtrace_get_frames()) }\n", control_path_options);
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
	const auto long_path_backtrace = capy::compile_bearer_unit("function CLI(request : dval) { print(backtrace_get_frames()) }\n", long_path_options);
	assert(capy::wasm::validate_bearer_unit(long_path_backtrace.wasm, {.bearer_abi_version = "11"}).valid);
	assert(std::string(long_path_backtrace.wasm.begin(), long_path_backtrace.wasm.end()).find(long_path_options.source_path) != std::string::npos);
	const auto ordinary_boundary = capy::compile_bearer_unit(
		"function CLI(request : dval) { print(string(request.body), string(request.websocket.connection_id), component_resolve(\"card\"), regex_replace(\"x\", \"y\", \"x\"), base64_encode(\"x\"), file_temp(\"x\"), time()) }\n",
		options);
	assert(ordinary_boundary.source_map.find("F\t2\tcapy://stdlib.capy\n") != std::string::npos);
	const std::string boundary_bytes(ordinary_boundary.wasm.begin(), ordinary_boundary.wasm.end());
	for (const auto& import : {"bearer_handler_input_brrb", "bearer_component_resolve", "bearer_regex", "bearer_codec", "bearer_file_temp", "bearer_time"})
		assert(boundary_bytes.find(import) != std::string::npos);
	const auto narrow_boundary = capy::compile_bearer_unit("function CLI(request : dval) { print(string(request.body)) }\n", options);
	const std::string narrow_bytes(narrow_boundary.wasm.begin(), narrow_boundary.wasm.end());
	assert(narrow_bytes.find("bearer_handler_input_brrb") != std::string::npos);
	for (const auto& absent : {"bearer_request_value", "bearer_request_body", "bearer_ws_", "bearer_component_", "bearer_regex", "bearer_codec", "bearer_file_", "bearer_time", "bearer_unit_load", "bearer_module_call_brrb"})
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
	for (const char* old_name : {"dval_string", "dval_bool", "dval_s32", "dval_f64", "dval_to_string", "dval_to_bool", "dval_to_f64", "dval_to_s64", "dval_to_u64"})
		assert(compiler_text.find("\"" + std::string(old_name) + "\"") == std::string::npos);
	for (const auto& public_name : {"array_merge", "set", "push", "pop", "delete", "clear", "get_by_path", "get_or_create",
			 "get_type_name", "get", "keys", "values", "has",
			 "request_param", "request_get", "request_post", "request_cookie", "request_session", "request_body", "request_context",
			 "session_start", "session_set", "session_remove", "session_destroy", "session_id_create", "redirect", "csrf_token", "csrf_valid", "csrf_rotate", "csrf_field",
			 "ws_message", "ws_connection_id", "ws_scope", "ws_opcode", "ws_is_binary", "ws_send", "ws_send_to", "ws_close", "component", "component_render",
			 "component_resolve", "unit_render", "unit_call", "unit_info", "units_list", "unit_compile", "regex_match", "regex_search", "regex_search_all", "regex_replace",
			 "regex_split", "base64_encode", "base64_decode", "uri_encode", "uri_decode", "json_encode", "json_decode", "html_escape", "strpos", "file_open", "file_read", "file_write",
			 "file_seek", "file_tell", "file_fsync", "file_close", "file_temp", "file_unlink", "time", "time_precise"})
		assert(compiler_text.find("named->value == \"" + std::string(public_name) + "\"") == std::string::npos);
	const auto ordinary_mysql = capy::compile_bearer_unit("function CLI(request : dval) { print(mysql_escape(\"Ada\")) }\n", options);
	assert(capy::wasm::validate_bearer_unit(ordinary_mysql.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string mysql_bytes(ordinary_mysql.wasm.begin(), ordinary_mysql.wasm.end());
	assert(mysql_bytes.find("bearer_mysql_escape") != std::string::npos);
	const auto typed_sized_hosts = capy::compile_bearer_unit(
		"function CLI(request : dval) { var params := dval({}); mysql_query(u64(0), \"select 1\", params); sqlite_query(u64(0), \"select 1\", params); regex_search(\"x\", \"x\"); regex_replace(\"x\", \"y\", \"x\"); unit_call(\"/x\", \"CLI\", params); json_decode(\"{}\"); base64_encode(\"x\") }\n", options);
	assert(capy::wasm::validate_bearer_unit(typed_sized_hosts.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string typed_sized_host_bytes(typed_sized_hosts.wasm.begin(), typed_sized_hosts.wasm.end());
	for (const auto& import : {"bearer_mysql_query", "bearer_sqlite_query", "bearer_regex_dval", "bearer_regex_text", "bearer_unit_call_brrb", "bearer_codec_dval", "bearer_codec_text"})
		assert(typed_sized_host_bytes.find(import) != std::string::npos);
	const auto owned_sized_host = capy::compile_bearer_unit(
		"function CLI(request : dval) { sqlite_query(u64(0), clone(\"select 1\"), dval({\"owned\": clone(\"value\")})) }\n", options);
	assert(capy::wasm::validate_bearer_unit(owned_sized_host.wasm, {.bearer_abi_version = "11"}).valid);
	const std::string sizing_release = "release_inputs(); append(code, module_.marker(value->location));";
	const std::size_t first_sizing_release = compiler_text.find(sizing_release);
	assert(first_sizing_release != std::string::npos && compiler_text.find(sizing_release, first_sizing_release + 1) != std::string::npos);
	const std::string function_value_source = "function CLI(request : dval) { var escape : function(raw : string) string = mysql_escape; print(escape(\"Ada\")) }\n";
	const auto function_value = capy::compile_bearer_unit(function_value_source, options);
	assert(capy::wasm::validate_bearer_unit(function_value.wasm, {.bearer_abi_version = "11"}).valid);
	assert(std::string(function_value.wasm.begin(), function_value.wasm.end()).find("bearer_mysql_escape") != std::string::npos);
	const auto parsed_function_value = capy::parse(function_value_source, options.source_path);
	const auto parsed_result = capy::compile_bearer_unit(parsed_function_value, options.source_path, options.module_name, options.abi_version);
	assert(function_value.wasm == parsed_result.wasm && function_value.source_map == parsed_result.source_map);
	const auto scoped_stdlib_parameter = capy::compile_bearer_unit(
		"function CLI(request : dval) { print(encode_query(dval({\"name\": \"Ada\"}))) }\n", options);
	const auto bare_stdlib_function_value = capy::compile_bearer_unit("function CLI(request : dval) { map(dval([\"X\"]), lower) }\n", options);
	assert(capy::wasm::validate_bearer_unit(bare_stdlib_function_value.wasm, {.bearer_abi_version = "11"}).valid);
	assert(std::string(bare_stdlib_function_value.wasm.begin(), bare_stdlib_function_value.wasm.end()).find("bearer_string_lower") != std::string::npos);
	const auto overloaded_stdlib_function_value = capy::compile_bearer_unit(
		"function CLI(request : dval) { var slice : function(value : string, start : s64) string = substr; print(slice(\"Ada\", s64(1))) }\n", options);
	assert(capy::wasm::validate_bearer_unit(overloaded_stdlib_function_value.wasm, {.bearer_abi_version = "11"}).valid);
	const auto returned_stdlib_function_value = capy::compile_bearer_unit(
		"function lower_callback() (function(value : string) string) { return lower }\nfunction CLI(request : dval) { map(dval([\"X\"]), lower_callback()) }\n", options);
	assert(std::string(returned_stdlib_function_value.wasm.begin(), returned_stdlib_function_value.wasm.end()).find("bearer_string_lower") != std::string::npos);
	const auto shadowed_stdlib = capy::compile_bearer_unit(
		"function CLI(request : dval) { var sqlite_connect : function(path : string) u64 = function(path : string) u64 { -> u64(1) }; print(sqlite_connect(\"x\")) }\n", options);
	assert(std::string(shadowed_stdlib.wasm.begin(), shadowed_stdlib.wasm.end()).find("bearer_sqlite_") == std::string::npos);
	const auto shadowed_function_value = capy::compile_bearer_unit(
		"function CLI(request : dval) { var lower : function(value : string) string = function(value : string) string { -> value }; map(dval([\"X\"]), lower) }\n", options);
	assert(std::string(shadowed_function_value.wasm.begin(), shadowed_function_value.wasm.end()).find("bearer_string_lower") == std::string::npos);
	for (const auto& [source, expected] : {
			 std::pair{"host function __bearer_trace() string\nfunction CLI(request : dval) {}\n", "host declarations are available only in the embedded Capy standard library"},
			 std::pair{"trace host function __bearer_trace() string\nfunction CLI(request : dval) {}\n", "host declarations are available only in the embedded Capy standard library"},
			 std::pair{"function __bearer_sqlite_connect(path : string) u64 { -> u64(0) }\nfunction CLI(request : dval) {}\n", "reserved for the Capy standard library"},
			 std::pair{"function CLI(request : dval) { __bearer_sqlite_connect(\":memory:\") }\n", "reserved for the Capy standard library"},
			 std::pair{"function sqlite_connect(path : string) u64 { -> u64(0) }\nfunction CLI(request : dval) {}\n", "reserved by the Capy standard library"},
			 std::pair{"function CLI(request : dval) { var callback : function(__bearer_value : s32) s32 = function(__bearer_value : s32) s32 { -> __bearer_value } }\n", "__bearer_* names are reserved for the Capy standard library"},
			 std::pair{"function CLI(request : dval) { for value, __bearer_key := dval({}) { value } }\n", "__bearer_* names are reserved for the Capy standard library"},
			 std::pair{"function duplicate(value : any) value::type { -> value }\nfunction duplicate(value : any) value::type { -> value }\nfunction CLI(request : dval) {}\n", "duplicate overload duplicate(any)"},
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
		capy::compile_bearer_unit("function next(value : u64) u64 { -> value + u64(1) }\nfunction half(value : f64) f64 { -> value / 2.0 }\n"
								  "function CLI(request : dval) { var fn : function(value : u64) u64 = next; print(fn(u64(4)), half(3.0), u64(1), s32(9.0)) }\n",
								  options);
	assert(capy::wasm::validate_bearer_unit(wide.wasm, {.bearer_abi_version = "11"}).valid);
	const auto marker_collision = capy::compile_bearer_unit("function CLI(request : dval) { 1509949440; print([1][1]) }\n", options);
	assert(capy::wasm::validate_bearer_unit(marker_collision.wasm, {.bearer_abi_version = "11"}).valid);
	assert(marker_collision.source_map.find("\t1\t1\t53\n") != std::string::npos);
	const auto merge_marker = capy::compile_bearer_unit(
		"function CLI(request : dval) {\n    array_merge(dval({\"left\": \"x\"}), dval({\"right\": \"y\"}))\n}\n", options);
	assert(capy::wasm::validate_bearer_unit(merge_marker.wasm, {.bearer_abi_version = "11"}).valid);
	assert(merge_marker.source_map.find("\t2\t5\n") != std::string::npos);
	for (const auto& [source, expected] : {
			 std::pair{"function implicit() s32 { 1 }\nfunction CLI(request : dval) { print(implicit()) }\n", "not all paths produce s32"},
			 std::pair{"function misplaced() s32 { -> 1; 2 }\nfunction CLI(request : dval) { print(misplaced()) }\n", "block yield must be the final item"},
			 std::pair{"function CLI(request : dval) { print(-> 1) }\n", "block yield is only valid as a block item"},
			 std::pair{"function CLI(request : dval) { var value := { 1 } }\n", "value-producing block must end with '-> expression'"},
			 std::pair{"function wrong() string { -> 1 }\nfunction CLI(request : dval) { print(wrong()) }\n", "expected string, found s64"},
		})
	{
		try { capy::compile_bearer_unit(source, options); assert(false); }
		catch (const capy::Error& error) { assert(error.message.find(expected) != std::string::npos); }
	}
	for (const auto source : {
			 "function square(value : any) value::type { -> value * value }\nfunction CLI(request : dval) { square(clone(\"x\")) }\n",
			 "function choose(a : any, b : s32) a::type { -> a }\nfunction choose(a : s32, b : any) b::type { -> b }\nfunction CLI(request : dval) { choose(1, 1) }\n",
			 "function incomplete(value : bool) s32 { if value { return 1 } }\nfunction CLI(request : dval) { print(incomplete(false)) }\n",
			 "function CLI(request : dval) { print(9223372036854775808) }\n",
			 "function CLI(request : dval) { print(-9223372036854775809) }\n",
			 "function CLI(request : dval) { print(999999999999999999999999999999999999) }\n",
			 "function CLI(request : dval) { value := 1; value := 2 }\n",
			 "function CLI(request : dval) { print(1 && true) }\n",
			 "function CLI(request : dval) { false && (hidden := true); print(hidden) }\n",
			 "function CLI(request : dval) { var held := clone(\"old\"); var replace := function() { held = clone(\"new\") } }\n",
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
	const auto wide_aggregates = capy::compile_bearer_unit(
		"struct Wide { narrow : s32; signed : s64; unsigned : u64; decimal : f64; text : string }\n"
		"function capture(value : u64, decimal : f64, text : string) (function(add : u64) f64) { -> function(add : u64) f64 { -> f64(value + add) + decimal + f64(length(text)) } }\n"
		"function CLI(request : dval) { var wide := Wide(s32(1), s64(-2), u64(3), 4.5, clone(\"x\")); var fn := capture(wide.unsigned, wide.decimal, wide.text); print(wide.signed, fn(u64(2)), arc_live()) }\n", options);
	assert(capy::wasm::validate_bearer_unit(wide_aggregates.wasm, {.bearer_abi_version = "11"}).valid);
	const auto assignment_contexts = capy::compile_bearer_unit(
		"struct Inner { signed : s64; unsigned : u64; decimal : f64; text : string }\n"
		"struct Holder { inner : Inner; callback : function(value : s64) s64 }\n"
		"function identity(value : s64) s64 { -> value }\nfunction identity(value : u64) u64 { -> value }\nfunction identity(value : f64) f64 { -> value }\n"
		"function select(value : s64) s32 { -> 1 }\nfunction select(value : u64) s32 { -> 2 }\nfunction select(value : f64) s32 { -> 3 }\n"
		"function return_signed(inner : Inner) s64 { -> inner.signed = 9 }\nfunction return_unsigned(inner : Inner) u64 { -> inner.unsigned = 10 }\nfunction return_decimal(inner : Inner) f64 { -> inner.decimal = 2.5 }\n"
		"function borrowed(inner : Inner, value : string) string { -> inner.text = value }\n"
		"function owned(inner : Inner) string { -> inner.text = clone(\"owned\") }\n"
		"function CLI(request : dval) { var holder := Holder(Inner(s64(0), u64(0), 0.0, clone(\"old\")), function(value : s64) s64 { -> value }); "
		"var inferred_signed := (holder.inner.signed = 9223372036854775807); var inferred_unsigned := (holder.inner.unsigned = 18446744073709551615); var inferred_decimal := (holder.inner.decimal = 1.25); "
		"print(identity(holder.inner.signed = 7), identity(holder.inner.unsigned = 8), identity(holder.inner.decimal = 1.5), select(holder.inner.signed = 8), "
		"select(holder.inner.unsigned = 9), select(holder.inner.decimal = 2.0), inferred_signed, inferred_unsigned, inferred_decimal, "
		"return_signed(holder.inner), return_unsigned(holder.inner), return_decimal(holder.inner)); "
		"holder.inner.decimal = 1.5; "
		"holder.callback = function(value : s64) s64 { -> value + s64(1) }; print(holder.callback(s64(2)), borrowed(holder.inner, clone(\"borrowed\")), owned(holder.inner), arc_live()) }\n", options);
	assert(capy::wasm::validate_bearer_unit(assignment_contexts.wasm, {.bearer_abi_version = "11"}).valid);
	const auto cycle_characterization = capy::compile_bearer_unit(
		"struct Cycle { children : [Cycle] }\nfunction empty_cycles() [Cycle] { var values : [Cycle] = []; -> values }\nfunction CLI(request : dval) { var cycle := Cycle(empty_cycles()); cycle.children.push(cycle); print(arc_live() > 0) }\n", options);
	assert(capy::wasm::validate_bearer_unit(cycle_characterization.wasm, {.bearer_abi_version = "11"}).valid);
	const auto value_expressions = capy::compile_bearer_unit(
		"function choose(flag : bool) string { -> { var prefix := clone(\"p\"); -> if flag { -> prefix + \"a\" } else { return clone(\"b\") } } }\n"
		"function CLI(request : dval) { var scalar := { var base := 2; -> if true { -> base + 1 } else { -> base + 2 } }; print(scalar, choose(true), choose(false), arc_live()) }\n", options);
	assert(capy::wasm::validate_bearer_unit(value_expressions.wasm, {.bearer_abi_version = "11"}).valid);
	for (const auto& [source, expected] : {
			 std::pair{"function CLI(request : dval) { (1 + 2) := 3 }\n", "inferred declaration target must be a local name"},
			 std::pair{"function CLI(request : dval) { print(1 && true) }\n", "logical operators require bool operands"},
			 std::pair{"function CLI(request : dval) { var held := clone(\"old\"); var replace := function() { held = clone(\"new\") } }\n", "cannot assign to captured binding 'held'. Captures are immutable"},
			 std::pair{"struct Item { wide : s64; text : string }\nfunction CLI(request : dval) { var item := Item(s64(0), clone(\"x\")); item.wide = \"bad\" }\n", "expected s64, found string"},
			 std::pair{"struct Item { wide : s64 }\nfunction CLI(request : dval) { var item := Item(s64(0)); item.missing = 1 }\n", "struct has no member 'missing'"},
			 std::pair{"struct Item { wide : s64 }\nfunction CLI(request : dval) { var item := Item(s64(0)); item.wide = 9223372036854775808 }\n", "outside the s64 range"},
			 std::pair{"struct Item { wide : u64 }\nfunction CLI(request : dval) { var item := Item(u64(0)); item.wide = -1 }\n", "outside the u64 range"},
			 std::pair{"function CLI(request : dval) { var value := if true { -> 1 } else { -> \"x\" } }\n", "if branches produce s64 and string"},
			 std::pair{"function CLI(request : dval) { print(first(\"ok\", 1)) }\n", "no overload first(string, s64)"},
			 std::pair{"function CLI(request : dval) { array_merge(dval({\"x\": \"y\"}), \"bad\") }\n", "no overload array_merge(dval, string)"},
			 std::pair{"function CLI(request : dval) { array_merge(dval({\"x\": \"y\"})) }\n", "no overload array_merge(dval)"},
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
		capy::compile_bearer_unit("function CLI(request : dval) {}", options);
		assert(false);
	}
	catch (const capy::Error&)
	{
	}

	unsigned cancellation_polls = 0;
	options.cancelled = [&] { return ++cancellation_polls == 2; };
	const std::string large_source = "function CLI(request : dval) { print(\"" + std::string(20000, 'x') + "\") }\n";
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
