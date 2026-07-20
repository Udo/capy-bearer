// Production WASM W1 core entrypoint.
//
// This file deliberately includes the production BEARER runtime amalgamation with
// __BEARER_WASM_CORE__ enabled. Native-only pieces are carved out in the runtime
// sources, while the workspace-owned DValue ABI and output plumbing are built
// into core.wasm.

#define __BEARER_WASM_CORE__ 1
#include "abi.h"
#include "../lib/bearer_lib.cpp"

#include "../lib/mysql-connector.h"
#include "../lib/sqlite-connector.h"

// ---- W3 connector membranes -----------------------------------------------
// sqlite/mysql run host-side (the host links the native connectors and owns the
// connections in per-workspace handle tables). BRRB2-marshalled hostcalls carry
// operation requests/responses; `connection` holds the host handle (>0).

static const char* WASM_DB_UNAVAILABLE =
	"database connector is not available in the wasm workspace";

extern "C" size_t bearer_host_mysql(const char* in, size_t in_len, char* out, size_t cap);
extern "C" size_t bearer_host_zip(const char* in, size_t in_len, char* out, size_t cap);
extern "C" size_t bearer_host_units(const char* in, size_t in_len, char* out, size_t cap);

static DValue wasm_sized_hostcall(DValue request, size_t (*hostcall)(const char*, size_t, char*, size_t))
{
	String encoded = brb_encode(request);
	size_t need = hostcall(encoded.data(), encoded.size(), 0, 0);
	if(need == 0)
		return(DValue());
	String buffer(need, 0);
	size_t got = hostcall(encoded.data(), encoded.size(), &buffer[0], need);
	if(got == 0 || got > need)
		return(DValue());
	DValue response;
	String error;
	brb_decode(String(buffer.data(), got), response, &error);
	return(response);
}

static DValue wasm_zip_call(DValue request)
{
	DValue response = wasm_sized_hostcall(request, bearer_host_zip);
	return(response);
}

DValue zip_list(String zip_file_name)
{
	DValue request;
	request["op"] = "list";
	request["path"] = zip_file_name;
	DValue response = wasm_zip_call(request);
	DValue* result = response.key("result");
	return(result ? *result : DValue());
}

String zip_read(String zip_file_name, String entry_name)
{
	DValue request;
	request["op"] = "read";
	request["path"] = zip_file_name;
	request["entry"] = entry_name;
	DValue response = wasm_zip_call(request);
	return(response["result"].to_string());
}

bool zip_create(String zip_file_name, DValue entries)
{
	DValue request;
	request["op"] = "create";
	request["path"] = zip_file_name;
	request["entries"] = entries;
	DValue response = wasm_zip_call(request);
	return(response["ok"].to_bool());
}

bool zip_extract(String zip_file_name, String destination_directory)
{
	DValue request;
	request["op"] = "extract";
	request["path"] = zip_file_name;
	request["destination"] = destination_directory;
	DValue response = wasm_zip_call(request);
	return(response["ok"].to_bool());
}

String gz_compress(String src)
{
	DValue request;
	request["op"] = "gz_compress";
	request["src"] = src;
	DValue response = wasm_zip_call(request);
	return(response["result"].to_string());
}

String gz_uncompress(String compressed)
{
	DValue request;
	request["op"] = "gz_uncompress";
	request["src"] = compressed;
	DValue response = wasm_zip_call(request);
	return(response["result"].to_string());
}

static DValue wasm_units_call(DValue request)
{
	return(wasm_sized_hostcall(request, bearer_host_units));
}

DValue unit_info(String path)
{
	DValue request;
	request["op"] = "info";
	request["path"] = path;
	DValue response = wasm_units_call(request);
	DValue* result = response.key("result");
	return(result ? *result : DValue());
}

StringList units_list()
{
	DValue request;
	request["op"] = "list";
	DValue response = wasm_units_call(request);
	StringList result;
	DValue* items = response.key("result");
	if(items)
		items->each([&](const DValue& value, String) { result.push_back(value.to_string()); });
	return(result);
}

bool unit_compile(String path)
{
	DValue request;
	request["op"] = "compile";
	request["path"] = path;
	DValue response = wasm_units_call(request);
	return(response["ok"].to_bool());
}

static DValue wasm_unit_call_result;

static DValue wasm_mysql_call(DValue request)
{
	return(wasm_sized_hostcall(request, bearer_host_mysql));
}

bool MySQL::connect(String host, String username, String password, String database)
{
	request_host = host;
	request_username = username;
	request_password = password;
	request_database = database;
	DValue request;
	request["op"] = "connect";
	request["host"] = host;
	request["username"] = username;
	request["password"] = password;
	request["database"] = database;
	DValue response = wasm_mysql_call(request);
	u64 handle = response["handle"].to_u64();
	connection = (void*)(uintptr_t)handle;
	_preload_next_error_code = (u32)response["error_code"].to_u64();
	statement_info = response["statement_info"].to_string();
	return(handle != 0 && _preload_next_error_code == 0);
}

void MySQL::disconnect()
{
	if(connection)
	{
		DValue request;
		request["op"] = "disconnect";
		request["handle"] = (f64)(uintptr_t)connection;
		wasm_mysql_call(request);
		connection = 0;
	}
}

String MySQL::error()
{
	String result = statement_info;
	statement_info = "";
	_preload_next_error_code = 0;
	return(result);
}

String MySQL::escape(String raw, char quote_char) { return(mysql_escape(raw, quote_char)); }

String mysql_escape(String raw, char quote_char)
{
	DValue request;
	request["op"] = "escape";
	request["raw"] = raw;
	request["quote_char"] = String(quote_char > 0 ? 1 : 0, quote_char);
	DValue response = wasm_mysql_call(request);
	DValue* result = response.key("result");
	return(result ? result->to_string() : raw);
}

String MySQL::parse_query_parameters(String query, StringMap map)
{
	String result;
	query.append(1, ' ');

	u8 mode = 0;
	char quote = 0;
	String identifier;
	for(u32 i = 0; i < query.length(); i++)
	{
		char c = query[i];
		if(mode == 0)
		{
			if(c == ':')
			{
				mode = 1;
				identifier = "";
			}
			else if(c == '"' || c == '\'')
			{
				result.append(1, c);
				mode = 2;
				quote = c;
			}
			else
				result.append(1, c);
		}
		else if(mode == 1)
		{
			if(isalnum(c) || c == '_')
				identifier.append(1, c);
			else
			{
				result.append(escape(map[identifier]));
				result.append(1, c);
				mode = 0;
			}
		}
		else if(mode == 2)
		{
			if(c == quote)
				mode = 0;
			result.append(1, c);
		}
	}

	return(result);
}

static bool wasm_mysql_has_unquoted_positional_placeholder(String query)
{
	bool quoted = false;
	char quote = 0;
	bool escaped = false;
	for(u32 i = 0; i < query.length(); i++)
	{
		char c = query[i];
		if(quoted)
		{
			if(escaped)
			{
				escaped = false;
				continue;
			}
			if(c == '\\')
			{
				escaped = true;
				continue;
			}
			if(c == quote)
				quoted = false;
			continue;
		}
		if(c == '\'' || c == '"')
		{
			quoted = true;
			quote = c;
			continue;
		}
		if(c == '?')
			return(true);
	}
	return(false);
}

DValue MySQL::query(String q)
{
	affected_rows = 0;
	if(wasm_mysql_has_unquoted_positional_placeholder(q))
	{
		_preload_next_error_code = 2000;
		statement_info = "mysql positional ? placeholders are not supported; use named :name placeholders";
		return(DValue());
	}
	if(!connection)
	{
		if(_preload_next_error_code == 0)
			_preload_next_error_code = 2000;
		if(statement_info == "")
			statement_info = "mysql connection is not open";
		return(DValue());
	}
	DValue request;
	request["op"] = "query";
	request["handle"] = (f64)(uintptr_t)connection;
	request["query"] = q;
	DValue response = wasm_mysql_call(request);
	insert_id = response["insert_id"].to_u64();
	affected_rows = (u32)response["affected"].to_u64();
	_preload_next_error_code = (u32)response["error_code"].to_u64();
	statement_info = response["statement_info"].to_string();
	DValue* result = response.key("result");
	return(result ? *result : DValue());
}

DValue MySQL::query(String q, StringMap params) { return(query(parse_query_parameters(q, params))); }
DValue MySQL::get_pending_result() { return(DValue()); }

// sqlite runs host-side (the host links libsqlite and owns the connections in
// a per-workspace handle table). One BRRB2-marshalled hostcall carries
// {op,handle,path,query,params} in and {handle,result,insert_id,affected,
// error_code,statement_info} out. `connection` holds the host handle (>0).
extern "C" size_t bearer_host_sqlite(const char* in, size_t in_len, char* out, size_t cap);

static DValue wasm_sqlite_call(DValue request)
{
	String encoded = brb_encode(request);
	size_t need = bearer_host_sqlite(encoded.data(), encoded.size(), 0, 0);
	if(need == 0)
		return(DValue());
	String buffer(need, 0);
	size_t got = bearer_host_sqlite(encoded.data(), encoded.size(), &buffer[0], need);
	if(got == 0 || got > need)
		return(DValue());
	DValue response;
	String error;
	brb_decode(String(buffer.data(), got), response, &error);
	return(response);
}

void SQLite::set_error(s32 code, String info) { error_code = code; statement_info = info; }

bool SQLite::connect(String path)
{
	this->path = path;
	DValue request;
	request["op"] = "connect";
	request["path"] = path;
	DValue response = wasm_sqlite_call(request);
	u64 handle = response["handle"].to_u64();
	connection = (void*)(uintptr_t)handle;
	error_code = (s32)response["error_code"].to_s64();
	statement_info = response["statement_info"].to_string();
	return(handle != 0 && error_code == 0);
}

void SQLite::disconnect()
{
	if(connection)
	{
		DValue request;
		request["op"] = "disconnect";
		request["handle"] = (f64)(uintptr_t)connection;
		wasm_sqlite_call(request);
		connection = 0;
	}
}

String SQLite::error()
{
	return(statement_info);
}

DValue SQLite::query(String q, const StringMap& params)
{
	DValue request;
	request["op"] = "query";
	request["handle"] = (f64)(uintptr_t)connection;
	request["query"] = q;
	for(auto& entry : params)
		request["params"][entry.first] = entry.second;
	DValue response = wasm_sqlite_call(request);
	insert_id = response["insert_id"].to_u64();
	affected_rows = (u32)response["affected"].to_u64();
	error_code = (s32)response["error_code"].to_s64();
	statement_info = response["statement_info"].to_string();
	DValue* result = response.key("result");
	return(result ? *result : DValue());
}

DValue SQLite::query(String q) { return(query(q, StringMap())); }
bool SQLite::apply_default_pragmas() { return(true); }
bool SQLite::bind_params(void* statement, const StringMap& params) { (void)statement; (void)params; return(true); }
DValue SQLite::collect_rows(void* statement) { (void)statement; return(DValue()); }

SQLite* sqlite_connect(String path)
{
	SQLite* db = new SQLite();
	db->request_cleanup_delete = true;
	db->connect(path);
	return(db);
}

void sqlite_disconnect(SQLite* db) { if(db) { db->disconnect(); if(db->request_cleanup_delete) delete db; } }
String sqlite_error(SQLite* db) { return(db ? db->error() : String(WASM_DB_UNAVAILABLE)); }
DValue sqlite_query(SQLite* db, String q) { return(db ? db->query(q) : DValue()); }
DValue sqlite_query(SQLite* db, String q, const StringMap& params) { return(db ? db->query(q, params) : DValue()); }
u64 sqlite_insert_id(SQLite* db) { return(db ? db->insert_id : 0); }
u32 sqlite_affected_rows(SQLite* db) { return(db ? db->affected_rows : 0); }
void cleanup_sqlite_connections() { }
void cleanup_mysql_connections() { }

static ServerState wasm_server;
static Request wasm_request;
static String wasm_output;
static String wasm_response_meta;

// ---- vague-linkage link anchors --------------------------------------------
// Units import libc++ template instantiations they use; --export-all only
// exports what the core itself instantiated. Some libc++ internals lack the
// hide-from-ABI attribute (the Phase 0 lambda finding), so units emit them as
// imports rather than binding locally. This function exists purely to make
// the core instantiate — and therefore export — the ones the site tree needs.
// Extend it when the loader reports "unresolved import env.<libc++ symbol>".
extern "C" void bearer_wasm_link_anchors()
{
	StringMap string_map;
	string_map["k"] = "v";
	string_map.erase(String("k"));                       // __tree::__erase_unique<String>
	std::map<String, DValue> dvalue_map;
	dvalue_map["k"] = DValue();
	dvalue_map.erase(String("k"));
	std::vector<String> string_list = { "a", "b" };
	string_list.erase(string_list.begin());
	std::set<String> string_set;
	string_set.insert("k");
	string_set.erase(String("k"));

	// libc functions units may call that the core itself never references —
	// taking their address forces them into the link (and --export-all)
	static void* volatile libc_anchors[] = {
		(void*)&atof, (void*)&atoi, (void*)&atol, (void*)&atoll,
		(void*)&strtol, (void*)&strtoul, (void*)&strtoll, (void*)&strtoull,
		(void*)&strtod, (void*)&strtof,
		(void*)&qsort, (void*)&bsearch,
		(void*)&snprintf, (void*)&sscanf,
		(void*)&memmove, (void*)&strncmp, (void*)&strncpy,
		// memchr/strchr/strrchr/strstr are C++-overloaded; cast to the C shape
		(void*)(const void* (*)(const void*, int, size_t))&memchr,
		(void*)(const char* (*)(const char*, int))&strchr,
		(void*)(const char* (*)(const char*, int))&strrchr,
		(void*)(const char* (*)(const char*, const char*))&strstr,
		// ctype family (int(int)); units use these directly
		(void*)&isalnum, (void*)&isalpha, (void*)&isblank, (void*)&iscntrl,
		(void*)&isdigit, (void*)&isgraph, (void*)&islower, (void*)&isprint,
		(void*)&ispunct, (void*)&isspace, (void*)&isupper, (void*)&isxdigit,
		(void*)&tolower, (void*)&toupper,
		// math functions used by side modules but not otherwise retained by core
		(void*)(double (*)(double))&cos,
		(void*)(double (*)(double))&sin,
		(void*)(double (*)(double))&round,
	};
	(void)libc_anchors;
}

// W3 membrane: the host resolves component/render targets to funcref-table
// slots (loading units lazily) and writes the resolved unit path back so
// nested relative component resolution keeps working.
extern "C" int32_t bearer_host_component_resolve(
	const char* target, size_t target_len,
	const char* handler, size_t handler_len,
	const char* current_unit, size_t current_unit_len,
	char* resolved_buf, size_t resolved_cap,
	int32_t* once_slot_out);

// target → table slot, reset per request (workspaces die with the request,
// but a single workspace can render the same component many times)
static std::map<String, s32> wasm_component_slots;
static std::map<String, String> wasm_component_paths;
static std::map<String, String> wasm_component_errors;

// These mirror small page-runtime pieces that cannot include compiler.cpp in
// the wasm core (compiler.cpp owns parser/clang/cache bookkeeping for the host
// build). Kept byte-identical where possible.
String component_normalize_path(String name)
{
	name = trim(name);
	if((name.length() >= 4 && name.substr(name.length() - 4) == ".uce") ||
		(name.length() >= 5 && name.substr(name.length() - 5) == ".capy"))
		return(name);
	return(name + ".uce");
}

void component_parse_target(String target, String& file_name, String& render_name)
{
	target = trim(target);
	render_name = "";
	auto render_split_pos = target.find(":");
	if(render_split_pos != String::npos)
	{
		render_name = trim(target.substr(render_split_pos + 1));
		target = trim(target.substr(0, render_split_pos));
	}
	file_name = target;
}

String component_error_banner(String message)
{
	return("<div class=\"banner\">" + html_escape(message) + "</div>");
}

struct RequestPropsScope
{
	Request* context = 0;
	DValue previous_props;

	static void swap_value(DValue& left, DValue& right)
	{
		std::swap(left.type, right.type);
		left._String.swap(right._String);
		std::swap(left._float, right._float);
		std::swap(left._array_index, right._array_index);
		std::swap(left._bool, right._bool);
		std::swap(left._list_mode, right._list_mode);
		std::swap(left._ptr, right._ptr);
		left._map.swap(right._map);
	}

	RequestPropsScope(Request* context, DValue& props)
	{
		this->context = context;
		if(this->context)
		{
			swap_value(previous_props, this->context->props);
			swap_value(this->context->props, props);
		}
	}

	~RequestPropsScope()
	{
		if(context)
			swap_value(context->props, previous_props);
	}
};

// A unit is a bag of exported handlers; invoking any of them is one operation —
// the host resolves __bearer_<handler> in the loaded module to a funcref slot. The
// handler is just a string: "render", "component:CARD", "render:VARIANT",
// "once", "cli", "websocket", "serve_http:named" — or "exists" (an existence
// probe that loads nothing). No per-mode kinds.
static s32 wasm_resolve_target(String unit_target, String handler, String* resolved_out = 0, String* error_out = 0)
{
	String current = context ? context->resources.current_unit_file : "";
	String cache_key = current + "\t" + handler + "\t" + unit_target;
	bool is_exists = (handler == "exists");
	auto cached = wasm_component_slots.find(cache_key);
	if(cached != wasm_component_slots.end() && !is_exists)
	{
		if(resolved_out)
			*resolved_out = wasm_component_paths[cache_key];
		if(error_out)
			*error_out = wasm_component_errors[cache_key];
		return(cached->second);
	}
	char resolved[4096];
	s32 once_slot = 0;
	s32 slot = bearer_host_component_resolve(
		unit_target.data(), unit_target.size(), handler.data(), handler.size(),
		current.data(), current.size(),
		resolved, sizeof(resolved), &once_slot);
	String response(resolved, strnlen(resolved, sizeof(resolved)));
	String resolve_error = slot < 0 ? response : String("");
	if(slot < 0)
		slot = 0;
	String resolved_path = slot ? response : String("");
	if(resolved_out && slot)
		*resolved_out = resolved_path;
	if(error_out)
		*error_out = resolve_error;
	if(!is_exists)
	{
		wasm_component_slots[cache_key] = slot;
		wasm_component_paths[cache_key] = resolved_path;
		wasm_component_errors[cache_key] = resolve_error;
		bool runs_once = handler == "render" || handler.rfind("render:", 0) == 0 ||
			handler == "component" || handler.rfind("component:", 0) == 0;
		if(slot && runs_once)
		{
			String once_key = current + "\t" + "once" + "\t" + resolved_path;
			wasm_component_slots[once_key] = once_slot;
			wasm_component_paths[once_key] = once_slot ? resolved_path : String("");
		}
	}
	return(slot);
}

String component_resolve(String name)
{
	String file_name, render_name;
	component_parse_target(trim(name), file_name, render_name);
	String resolved;
	if(wasm_resolve_target(file_name, "exists", &resolved))
		return(resolved);
	return("");
}

bool component_exists(String name)
{
	return(component_resolve(name) != "");
}

// Run a unit's ONCE() handler at most once per request (native
// compiler_run_unit_once_if_needed semantics): dedup on the resolved unit
// path via request.once_units. The handler emits head assets, etc.
static void wasm_run_once(const String& resolved, Request& request)
{
	if(resolved == "")
		return;
	if(request.once_units.find(resolved) != request.once_units.end())
		return;
	request.once_units.insert(resolved);
	s32 once_slot = wasm_resolve_target(resolved, "once");
	if(once_slot == 0)
		return;
	String previous_unit = request.resources.current_unit_file;
	request.resources.current_unit_file = resolved;
	WasmRequestHandler once_handler = (WasmRequestHandler)(uintptr_t)once_slot;
	once_handler(request);
	request.resources.current_unit_file = previous_unit;
}

DValue* unit_call(String file_name, String function_name, DValue* call_param)
{
	String macro = to_upper(trim(function_name));
	String handler = "";
	if(macro == "RENDER" || macro.rfind("RENDER:", 0) == 0)
		handler = "render" + (macro.length() > 7 ? ":" + trim(function_name.substr(function_name.find(":") + 1)) : String(""));
	else if(macro == "COMPONENT" || macro.rfind("COMPONENT:", 0) == 0)
		handler = "component" + (macro.length() > 10 ? ":" + trim(function_name.substr(function_name.find(":") + 1)) : String(""));
	else if(macro == "ONCE")
		handler = "once";
	else if(macro == "INIT")
		handler = "init";

	if(handler != "")
	{
		String resolved;
		String resolve_error;
		s32 slot = wasm_resolve_target(file_name, handler, &resolved, &resolve_error);
		if(!slot)
		{
			if(resolve_error != "")
				print("Error: ", resolve_error);
			else
				print("Error: unit_call() function '", function_name, "' not found");
			return(0);
		}
		DValue props = call_param ? *call_param : DValue();
		RequestPropsScope props_scope(context, props);
		if((handler == "render" || handler.rfind("render:", 0) == 0 || handler == "component" || handler.rfind("component:", 0) == 0) && resolved != "")
			wasm_run_once(resolved, *context);
		String previous_unit = context->resources.current_unit_file;
		if(resolved != "")
			context->resources.current_unit_file = resolved;
		WasmRequestHandler handler_fn = (WasmRequestHandler)(uintptr_t)slot;
		handler_fn(*context);
		context->resources.current_unit_file = previous_unit;
		return(0);
	}

	String resolved;
	String resolve_error;
	s32 slot = wasm_resolve_target(file_name, "export:" + function_name, &resolved, &resolve_error);
	if(!slot)
	{
		if(resolve_error != "")
			print("Error: ", resolve_error);
		else
			print("Error: unit_call() function '", function_name, "' not found");
		return(0);
	}

	String previous_unit = context->resources.current_unit_file;
	if(resolved != "")
		context->resources.current_unit_file = resolved;
	WasmDValueCallHandler handler_fn = (WasmDValueCallHandler)(uintptr_t)slot;
	DValue* result = handler_fn(call_param);
	context->resources.current_unit_file = previous_unit;
	if(result)
	{
		wasm_unit_call_result = *result;
		return(&wasm_unit_call_result);
	}
	return(0);
}

static void component_render_with_props(String name, DValue& props, Request& request)
{
	String file_name, render_name;
	component_parse_target(trim(name), file_name, render_name);
	String handler = render_name == "" ? String("component") : "component:" + render_name;
	String resolved;
	String resolve_error;
	s32 slot = wasm_resolve_target(file_name, handler, &resolved, &resolve_error);
	if(!slot)
	{
		request.set_status(500, "Internal Server Error");
		print(component_error_banner(resolve_error != "" ? resolve_error : "component not found: " + trim(name)));
		return;
	}
	wasm_run_once(resolved, request);
	RequestPropsScope props_scope(&request, props);
	String previous_unit = request.resources.current_unit_file;
	if(resolved != "")
		request.resources.current_unit_file = resolved;
	// a wasm function pointer is its index in the shared funcref table; the
	// host returned the handler's slot, so this is a plain call_indirect
	WasmRequestHandler handler_fn = (WasmRequestHandler)(uintptr_t)slot;
	handler_fn(request);
	request.resources.current_unit_file = previous_unit;
}

void component_render(String name, DValue props, Request& request) { component_render_with_props(name, props, request); }
void component_render(String name) { DValue props; component_render_with_props(name, props, *context); }
void component_render(String name, Request& request) { DValue props; component_render_with_props(name, props, request); }
void component_render(String name, DValue props) { component_render_with_props(name, props, *context); }

String component(String name, DValue props, Request& request)
{
	ob_start();
	component_render_with_props(name, props, request);
	return(ob_get_close());
}

String component(String name) { DValue props; return(component(name, props, *context)); }
String component(String name, Request& request) { DValue props; return(component(name, props, request)); }
String component(String name, DValue props) { return(component(name, props, *context)); }

void unit_render(String file_name, Request& request)
{
	String unit_name, render_name;
	component_parse_target(trim(file_name), unit_name, render_name);
	String handler = render_name == "" ? String("render") : "render:" + render_name;
	String resolved;
	String resolve_error;
	s32 slot = wasm_resolve_target(unit_name, handler, &resolved, &resolve_error);
	if(!slot)
	{
		request.set_status(500, "Internal Server Error");
		print(component_error_banner(resolve_error != "" ? resolve_error : "unit not found: " + trim(file_name)));
		return;
	}
	wasm_run_once(resolved, request);
	String previous_unit = request.resources.current_unit_file;
	if(resolved != "")
		request.resources.current_unit_file = resolved;
	WasmRequestHandler handler_fn = (WasmRequestHandler)(uintptr_t)slot;
	handler_fn(request);
	request.resources.current_unit_file = previous_unit;
}

void unit_render(String file_name) { unit_render(file_name, *context); }

struct WasmRequestEnvelopeSegment
{
	const char* data = 0;
	size_t size = 0;
};

static bool wasm_decode_request_envelope(const char* encoded, size_t encoded_size,
	WasmRequestEnvelopeSegment (&segments)[12], String& error)
{
	if(encoded_size < 6 || memcmp(encoded, "BRRQ", 4) != 0)
	{
		error = "missing BEARER request-envelope header";
		return(false);
	}
	if((u8)encoded[4] != 1 || (u8)encoded[5] != 12)
	{
		error = "unsupported BEARER request-envelope version or segment count";
		return(false);
	}
	size_t offset = 6;
	for(u32 i = 0; i < 12; i++)
	{
		u64 segment_size = 0;
		if(!brb_read_varint(encoded, encoded_size, offset, segment_size) || segment_size > encoded_size - offset)
		{
			error = "invalid BEARER request-envelope segment " + std::to_string(i);
			return(false);
		}
		segments[i].data = encoded + offset;
		segments[i].size = (size_t)segment_size;
		offset += (size_t)segment_size;
	}
	if(offset != encoded_size)
	{
		error = "trailing bytes after BEARER request envelope";
		return(false);
	}
	return(true);
}

extern "C" {

// The host has already loaded the request's entry unit and can place its
// exports directly in the shared table. This avoids resolving that same unit
// back through a hostcall while preserving per-request ONCE deduplication.
void bearer_wasm_invoke_loaded_entry(s32 handler_slot, s32 once_slot)
{
	String resolved = context->resources.current_unit_file;
	if(once_slot && resolved != "" && context->once_units.find(resolved) == context->once_units.end())
	{
		context->once_units.insert(resolved);
		WasmRequestHandler once_handler = (WasmRequestHandler)(uintptr_t)once_slot;
		once_handler(*context);
	}
	WasmRequestHandler handler_fn = (WasmRequestHandler)(uintptr_t)handler_slot;
	handler_fn(*context);
}

void* bearer_alloc(size_t len)
{
	return(malloc(len));
}

void bearer_free(void* ptr)
{
	free(ptr);
}

u32 bearer_wasm_core_abi_version()
{
	return(BEARER_WASM_CORE_ABI_VERSION);
}

int bearer_wasm_core_init()
{
	wasm_server.config = default_config();
	wasm_request.server = &wasm_server;
	// the primary output stream must live ON ob_stack (native semantics):
	// ob_get_close()/ob_close() pop and rebalance against the stack, so a
	// stream outside it would be orphaned by the first component() capture
	if(wasm_request.ob_stack.empty())
		wasm_request.ob_start();
	context = &wasm_request;
	return(0);
}

void bearer_wasm_core_reset_request()
{
	if(context == 0)
		bearer_wasm_core_init();
	wasm_request.call = DValue();
	wasm_request.props = DValue();
	wasm_request.params.clear();
	wasm_request.get.clear();
	wasm_request.post.clear();
	wasm_request.header.clear();
	wasm_request.set_cookies.clear();
	wasm_request.response_code = "HTTP/1.1 200 OK";
	wasm_request.flags = Request::Flags();
	wasm_request.stats = Request::Stats();
	for(auto* stream : wasm_request.ob_stack)
		delete stream;
	wasm_request.ob_stack.clear();
	wasm_request.ob_start();
	wasm_output = "";
	wasm_response_meta = "";
	wasm_request.cookies.clear();
	wasm_request.session.clear();
	wasm_request.session_id = "";
	wasm_request.session_name = "";
	wasm_request.session_loaded_hash = "";
	wasm_request.out = "";
	wasm_request.resources.current_unit_file = "";
	wasm_component_slots.clear();
	wasm_component_paths.clear();
	wasm_component_errors.clear();
}

// Host pushes the worker-cached immutable configuration followed by the
// dynamic BRRB2 request context into one guest buffer.
int bearer_wasm_apply_context(const char* config_buf, size_t config_len, const char* context_buf, size_t context_len)
{
	if(context == 0)
		bearer_wasm_core_init();
	StringMap decoded_config;
	StringMap decoded_params;
	StringMap decoded_get;
	StringMap decoded_post;
	StringMap decoded_cookies;
	StringMap decoded_session;
	DValue decoded_call;
	DValue decoded_ws;
	WasmRequestEnvelopeSegment segments[12];
	String error;
	if(!brb_decode_flat_string_map(config_buf, config_len, decoded_config, &error))
	{
		bearer_host_log(3, error.data(), error.size());
		return(1);
	}
	if(!wasm_decode_request_envelope(context_buf, context_len, segments, error))
	{
		bearer_host_log(3, error.data(), error.size());
		return(2);
	}
	auto decode_tree = [&](u32 index, DValue& target, const char* name) {
		if(brb_decode(String(segments[index].data, segments[index].size), target, &error))
			return(true);
		error = String(name) + ": " + error;
		return(false);
	};
	auto decode_map = [&](u32 index, StringMap& target, const char* name) {
		if(brb_decode_flat_string_map(segments[index].data, segments[index].size, target, &error))
			return(true);
		error = String(name) + ": " + error;
		return(false);
	};
	if(!decode_tree(0, decoded_call, "request call") ||
		!decode_map(1, decoded_params, "request params") ||
		!decode_map(2, decoded_get, "request get") ||
		!decode_map(3, decoded_post, "request post") ||
		!decode_map(4, decoded_cookies, "request cookies") ||
		!decode_map(5, decoded_session, "request session"))
	{
		bearer_host_log(3, error.data(), error.size());
		return(3);
	}
	if(segments[11].size && !decode_tree(11, decoded_ws, "request websocket"))
	{
		bearer_host_log(3, error.data(), error.size());
		return(4);
	}
	wasm_server.config = std::move(decoded_config);
	wasm_request.call = std::move(decoded_call);
	wasm_request.params = std::move(decoded_params);
	wasm_request.get = std::move(decoded_get);
	wasm_request.post = std::move(decoded_post);
	wasm_request.cookies = std::move(decoded_cookies);
	wasm_request.session = std::move(decoded_session);
	wasm_request.response_code = wasm_request.params["GATEWAY_INTERFACE"] != "" ?
		"Status: 200 OK" : "HTTP/1.1 200 OK";
	wasm_request.session_id.assign(segments[6].data, segments[6].size);
	wasm_request.session_name.assign(segments[7].data, segments[7].size);
	wasm_request.session_loaded_hash.assign(segments[8].data, segments[8].size);
	wasm_request.resources.current_unit_file.assign(segments[9].data, segments[9].size);
	wasm_request.in.assign(segments[10].data, segments[10].size);
	// websocket event context: ws_send()/ws_close() capture into the dispatch
	// list (the workspace owns no connections), which collect() carries back to
	// the broker. Reset per invocation.
	wasm_request.connection = DValue();
	wasm_request.resources.websocket_connection_state_before = DValue();
	wasm_request.resources.websocket_dispatch_commands = DValue();
	wasm_request.resources.websocket_dispatch_capture = false;
	if(segments[11].size)
	{
		wasm_request.resources.websocket_connection_id = decoded_ws["connection_id"].to_string();
		wasm_request.resources.websocket_scope = decoded_ws["scope"].to_string();
		wasm_request.resources.websocket_opcode = (u8)decoded_ws["opcode"].to_u64();
		wasm_request.resources.websocket_is_binary = decoded_ws["binary"].to_bool();
		wasm_request.resources.websocket_scope_connection_ids.clear();
		if(DValue* conns = decoded_ws.key("connections"))
			conns->each([&](const DValue& v, String) {
				wasm_request.resources.websocket_scope_connection_ids.push_back(v.to_string());
			});
		wasm_request.resources.websocket_dispatch_capture = true;
		if(DValue* cstate = decoded_ws.key("connection_state"))
			wasm_request.connection = *cstate;
		wasm_request.resources.websocket_connection_state_before = wasm_request.connection;
	}
	return(0);
}

Request* bearer_wasm_request()
{
	if(context == 0)
		bearer_wasm_core_init();
	return(&wasm_request);
}

// After render: response metadata (status line, headers, cookies, session)
// goes back to the host as BRRB2.
void bearer_wasm_finish_response_meta()
{
	DValue meta;
	meta["status"] = wasm_request.response_code;
	for(auto& header : wasm_request.header)
		meta["headers"][header.first] = header.second;
	for(auto& cookie : wasm_request.set_cookies)
	{
		DValue cookie_value;
		cookie_value = cookie;
		meta["cookies"].push(cookie_value);
	}
	for(auto& entry : wasm_request.session)
		meta["session"][entry.first] = entry.second;
	meta["session_id"] = wasm_request.session_id;
	meta["session_name"] = wasm_request.session_name;
	meta["session_loaded_hash"] = wasm_request.session_loaded_hash;
	bool ws_has_commands = !wasm_request.resources.websocket_dispatch_commands._map.empty();
	bool ws_state_changed = false;
	if(wasm_request.resources.websocket_dispatch_capture)
	{
		String prior_state = brb_encode(wasm_request.resources.websocket_connection_state_before);
		String current_state = brb_encode(wasm_request.connection);
		ws_state_changed = (prior_state != current_state);
	}
	// Any unit code (not just WS handlers) may call ws_send/ws_close; whenever the
	// dispatch list is non-empty, carry it back so the worker can flush it to the
	// broker. If only connection state changed, flush a command-less state-only
	// batch so the broker can persist the connection mutation.
	if(ws_has_commands)
		meta["ws_commands"] = wasm_request.resources.websocket_dispatch_commands;
	if(ws_state_changed)
		meta["ws_connection_state"] = wasm_request.connection;
	wasm_response_meta = brb_encode(meta);
}

const char* bearer_wasm_response_meta_data()
{
	return(wasm_response_meta.data());
}

size_t bearer_wasm_response_meta_size()
{
	return(wasm_response_meta.size());
}

void bearer_print_bytes(const char* data, size_t len)
{
	if(context == 0)
		bearer_wasm_core_init();
	if(context->ob && data && len)
		context->ob->write(data, len);
}

void bearer_print_s32(s32 value)
{
	if(context == 0)
		bearer_wasm_core_init();
	print(std::to_string(value));
}

void bearer_wasm_finish_output()
{
	// ob_stack[0] is the request's primary stream; nested captures above it
	// belong to unbalanced ob_start() calls and are intentionally ignored
	wasm_output = wasm_request.ob_stack.empty() ? String("") : wasm_request.ob_stack[0]->str();
}

const char* bearer_wasm_output_data()
{
	return(wasm_output.data());
}

size_t bearer_wasm_output_size()
{
	return(wasm_output.size());
}

}
