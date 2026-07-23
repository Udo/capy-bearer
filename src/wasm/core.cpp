// Production WASM W1 core entrypoint.
//
// This file deliberately includes the production BEARER runtime amalgamation with
// __BEARER_WASM_CORE__ enabled. Native-only pieces are carved out in the runtime
// sources, while the workspace-owned DValue ABI and output plumbing are built
// into core.wasm.

#define __BEARER_WASM_CORE__ 1
#include "abi.h"
#include <charconv>
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
extern "C" size_t bearer_host_regex_capy(const char* in, size_t in_len, char* out, size_t cap);

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
static String wasm_component_capture_result;
static String wasm_file_read_result;
static String wasm_file_temp_result;
static String wasm_unit_info_result;
static String wasm_units_list_result;
static String wasm_codec_result;
static String wasm_regex_result;
static String wasm_string_list_result;
static String wasm_dval_merge_result;
static String wasm_sqlite_result;
static std::vector<SQLite*> wasm_capy_sqlite_handles;
static size_t bearer_copy_bytes(const String& value, char* out, size_t cap);
static size_t bearer_copy_staged(String& staged, char* out, size_t cap);
static bool bearer_decode_brrb_span(const char* value, size_t value_len, DValue& decoded);
static String wasm_unit_call_encoded_result;

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
	if(!connection)
	{
		insert_id = 0;
		affected_rows = 0;
		set_error(21, "sqlite query called without an open connection"); // SQLITE_MISUSE
		return(DValue());
	}
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

static SQLite* bearer_sqlite_handle(u64 handle)
{
	if(handle == 0 || handle > wasm_capy_sqlite_handles.size())
		return(0);
	return(wasm_capy_sqlite_handles[(size_t)handle - 1]);
}

extern "C" u64 bearer_sqlite_connect(const char* path, size_t path_len)
{
	SQLite* db = sqlite_connect(String(path ? path : "", path ? path_len : 0));
	if(!db)
		return(0);
	wasm_capy_sqlite_handles.push_back(db);
	return((u64)wasm_capy_sqlite_handles.size());
}

extern "C" s32 bearer_sqlite_disconnect(u64 handle)
{
	SQLite* db = bearer_sqlite_handle(handle);
	if(!db)
		return(0);
	sqlite_disconnect(db);
	wasm_capy_sqlite_handles[(size_t)handle - 1] = 0;
	return(1);
}

extern "C" size_t bearer_sqlite_error(u64 handle, char* out, size_t cap)
{
	if(!out)
	{
		wasm_sqlite_result.clear();
		SQLite* db = bearer_sqlite_handle(handle);
		if(!db)
			return(std::numeric_limits<size_t>::max());
		wasm_sqlite_result = sqlite_error(db);
		return(wasm_sqlite_result.size());
	}
	return(bearer_copy_staged(wasm_sqlite_result, out, cap));
}

extern "C" size_t bearer_sqlite_query(u64 handle, const char* query, size_t query_len, const char* params, size_t params_len, char* out, size_t cap)
{
	if(!out)
	{
		wasm_sqlite_result.clear();
		SQLite* db = bearer_sqlite_handle(handle);
		if(!db)
			return(std::numeric_limits<size_t>::max());
		StringMap parameter_map;
		if(params_len)
		{
			DValue decoded;
			if(!bearer_decode_brrb_span(params, params_len, decoded) || !decoded.is_array())
				return(std::numeric_limits<size_t>::max());
			bool valid = true;
			decoded.each([&](const DValue& value, String key) {
				if(value.deref().type != 'S')
					valid = false;
				else
					parameter_map[key] = value.to_string();
			});
			if(!valid)
				return(std::numeric_limits<size_t>::max());
		}
		wasm_sqlite_result = brb_encode(sqlite_query(db, String(query ? query : "", query ? query_len : 0), parameter_map));
		if(wasm_sqlite_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_sqlite_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_sqlite_result.size());
	}
	return(bearer_copy_staged(wasm_sqlite_result, out, cap));
}

extern "C" u64 bearer_sqlite_insert_id(u64 handle)
{
	SQLite* db = bearer_sqlite_handle(handle);
	return(db ? sqlite_insert_id(db) : std::numeric_limits<u64>::max());
}

extern "C" u64 bearer_sqlite_affected_rows(u64 handle)
{
	SQLite* db = bearer_sqlite_handle(handle);
	return(db ? sqlite_affected_rows(db) : std::numeric_limits<u64>::max());
}

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
	wasm_unit_call_encoded_result.clear();
	wasm_component_capture_result.clear();
	wasm_file_read_result.clear();
	wasm_file_temp_result.clear();
	wasm_unit_info_result.clear();
	wasm_units_list_result.clear();
	wasm_codec_result.clear();
	wasm_regex_result.clear();
	wasm_string_list_result.clear();
	wasm_dval_merge_result.clear();
	wasm_sqlite_result.clear();
	for(SQLite* db : wasm_capy_sqlite_handles)
		if(db)
			sqlite_disconnect(db);
	wasm_capy_sqlite_handles.clear();
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

static bool bearer_regex_call(const char* operation, const char* pattern, size_t pattern_len, const char* subject, size_t subject_len,
	const char* replacement, size_t replacement_len, const char* flags, size_t flags_len, DValue& response)
{
	DValue request;
	request["op"] = operation;
	request["pattern"] = String(pattern ? pattern : "", pattern ? pattern_len : 0);
	request["subject"] = String(subject ? subject : "", subject ? subject_len : 0);
	request["replacement"] = String(replacement ? replacement : "", replacement ? replacement_len : 0);
	request["flags"] = String(flags ? flags : "", flags ? flags_len : 0);
	String encoded = brb_encode(request);
	size_t need = bearer_host_regex_capy(encoded.data(), encoded.size(), 0, 0);
	if(need == 0)
		return(false);
	String buffer(need, 0);
	size_t got = bearer_host_regex_capy(encoded.data(), encoded.size(), &buffer[0], need);
	String decode_error;
	if(got != need || !brb_decode(String(buffer.data(), got), response, &decode_error))
		return(false);
	if(DValue* error = response.key("error"))
	{
		String message = error->to_string();
		bearer_host_log(3, message.data(), message.size());
		return(false);
	}
	return(true);
}

s32 bearer_regex_match(const char* pattern, size_t pattern_len, const char* subject, size_t subject_len, const char* flags, size_t flags_len)
{
	DValue response;
	if(!bearer_regex_call("match", pattern, pattern_len, subject, subject_len, 0, 0, flags, flags_len, response))
		return(-1);
	return(response["bool"].to_bool() ? 1 : 0);
}

size_t bearer_regex(s32 operation, const char* pattern, size_t pattern_len, const char* subject, size_t subject_len,
	const char* replacement, size_t replacement_len, const char* flags, size_t flags_len, char* out, size_t cap)
{
	if(!out)
	{
		static const char* operations[] = {"search", "search_all", "replace", "split"};
		DValue response;
		if(operation < 0 || operation > 3 || !bearer_regex_call(operations[operation], pattern, pattern_len, subject, subject_len,
			replacement, replacement_len, flags, flags_len, response))
			return(std::numeric_limits<size_t>::max());
		if(operation == 0 || operation == 1)
		{
			DValue* tree = response.key("tree");
			wasm_regex_result = brb_encode(tree ? *tree : DValue());
		}
		else if(operation == 2)
			wasm_regex_result = response["text"].to_string();
		else
		{
			DValue* list = response.key("list");
			wasm_regex_result = brb_encode(list ? *list : DValue());
		}
		if(wasm_regex_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_regex_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_regex_result.size());
	}
	return(bearer_copy_staged(wasm_regex_result, out, cap));
}

size_t bearer_codec(s32 operation, const char* input, size_t input_len, char* out, size_t cap)
{
	if(!out)
	{
		String value(input ? input : "", input ? input_len : 0);
		if(operation == 0)
			wasm_codec_result = base64_encode(value);
		else if(operation == 1)
		{
			bool ok = false;
			wasm_codec_result = base64_decode(value, ok);
			if(!ok) wasm_codec_result.clear();
		}
		else if(operation == 2)
			wasm_codec_result = uri_encode(value);
		else if(operation == 3)
			wasm_codec_result = uri_decode(value);
		else if(operation == 4)
			wasm_codec_result = html_escape(value);
		else if(operation == 5)
		{
			DValue decoded;
			String error;
			if(!brb_decode(value, decoded, &error))
				wasm_codec_result.clear();
			else
				wasm_codec_result = json_encode(decoded);
		}
		else if(operation == 6)
			wasm_codec_result = brb_encode(json_decode(value));
		else
			wasm_codec_result.clear();
		if(wasm_codec_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_codec_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_codec_result.size());
	}
	return(bearer_copy_staged(wasm_codec_result, out, cap));
}

static size_t bearer_copy_staged(String& staged, char* out, size_t cap)
{
	if(cap < staged.size())
		return(staged.size());
	if(!staged.empty())
		memcpy(out, staged.data(), staged.size());
	size_t size = staged.size();
	staged.clear();
	return(size);
}

size_t bearer_unit_info_brrb(const char* path, size_t path_len, char* out, size_t cap)
{
	if(!out)
	{
		wasm_unit_info_result = brb_encode(unit_info(String(path ? path : "", path ? path_len : 0)));
		return(wasm_unit_info_result.size());
	}
	return(bearer_copy_staged(wasm_unit_info_result, out, cap));
}

size_t bearer_units_list_brrb(char* out, size_t cap)
{
	if(!out)
	{
		DValue result;
		result.set_array();
		for(const String& path : units_list())
		{
			DValue value;
			value.set(path);
			result.push(value);
		}
		wasm_units_list_result = brb_encode(result);
		return(wasm_units_list_result.size());
	}
	return(bearer_copy_staged(wasm_units_list_result, out, cap));
}

s32 bearer_unit_compile(const char* path, size_t path_len)
{
	return(unit_compile(String(path ? path : "", path ? path_len : 0)));
}

u64 bearer_file_open(const char* path, size_t path_len, const char* mode, size_t mode_len)
{
	return(file_open(String(path ? path : "", path ? path_len : 0), String(mode ? mode : "", mode ? mode_len : 0)));
}

size_t bearer_file_read(u64 handle, u64 length, char* out, size_t cap)
{
	if(!out)
	{
		wasm_file_read_result = file_read(handle, length);
		if(wasm_file_read_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_file_read_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_file_read_result.size());
	}
	if(cap < wasm_file_read_result.size())
		return(wasm_file_read_result.size());
	if(!wasm_file_read_result.empty())
		memcpy(out, wasm_file_read_result.data(), wasm_file_read_result.size());
	size_t size = wasm_file_read_result.size();
	wasm_file_read_result.clear();
	return(size);
}

u64 bearer_file_write(u64 handle, const char* data, size_t data_len)
{
	return(file_write(handle, String(data ? data : "", data ? data_len : 0)));
}

s64 bearer_file_seek(u64 handle, s64 offset, s64 whence)
{
	return(file_seek(handle, offset, whence));
}

s64 bearer_file_tell(u64 handle)
{
	return(file_tell(handle));
}

s32 bearer_file_fsync(u64 handle)
{
	return(file_fsync(handle));
}

void bearer_file_close(u64 handle)
{
	file_close(handle);
}

size_t bearer_file_temp(const char* prefix, size_t prefix_len, char* out, size_t cap)
{
	if(!out)
	{
		wasm_file_temp_result = file_temp(String(prefix ? prefix : "", prefix ? prefix_len : 0));
		return(wasm_file_temp_result.size());
	}
	if(cap < wasm_file_temp_result.size())
		return(wasm_file_temp_result.size());
	if(!wasm_file_temp_result.empty())
		memcpy(out, wasm_file_temp_result.data(), wasm_file_temp_result.size());
	size_t size = wasm_file_temp_result.size();
	wasm_file_temp_result.clear();
	return(size);
}

void bearer_file_unlink(const char* path, size_t path_len)
{
	file_unlink(String(path ? path : "", path ? path_len : 0));
}

u64 bearer_time()
{
	return(time());
}

f64 bearer_time_precise()
{
	return(time_precise());
}

void bearer_print_s32(s32 value)
{
	if(context == 0)
		bearer_wasm_core_init();
	print(std::to_string(value));
}

static String bearer_format_f64_value(f64 value)
{
	std::ostringstream output;
	output.imbue(std::locale::classic());
	output << std::setprecision(std::numeric_limits<f64>::max_digits10) << value;
	return(output.str());
}

size_t bearer_format_s64(s64 value, char* out, size_t cap)
{
	return(bearer_copy_bytes(std::to_string(value), out, cap));
}

size_t bearer_format_u64(u64 value, char* out, size_t cap)
{
	return(bearer_copy_bytes(std::to_string(value), out, cap));
}

size_t bearer_format_f64(f64 value, char* out, size_t cap)
{
	return(bearer_copy_bytes(bearer_format_f64_value(value), out, cap));
}

void bearer_print_s64(s64 value)
{
	if(context == 0)
		bearer_wasm_core_init();
	print(std::to_string(value));
}

void bearer_print_u64(u64 value)
{
	if(context == 0)
		bearer_wasm_core_init();
	print(std::to_string(value));
}

void bearer_print_f64(f64 value)
{
	if(context == 0)
		bearer_wasm_core_init();
	print(bearer_format_f64_value(value));
}

void bearer_unit_render_bytes(const char* target, size_t target_len)
{
	unit_render(String(target ? target : "", target ? target_len : 0));
}

s32 bearer_component_exists(const char* target, size_t target_len)
{
	return(component_exists(String(target ? target : "", target ? target_len : 0)));
}

size_t bearer_component_resolve(const char* target, size_t target_len, char* out, size_t cap)
{
	return(bearer_copy_bytes(component_resolve(String(target ? target : "", target ? target_len : 0)), out, cap));
}

void bearer_component_render_bytes(const char* target, size_t target_len)
{
	component_render(String(target ? target : "", target ? target_len : 0));
}

s32 bearer_component_render_props_brrb(const char* target, size_t target_len, const char* props, size_t props_len)
{
	DValue value;
	String error;
	if(!props || !brb_decode(String(props, props_len), value, &error))
		return(0);
	component_render(String(target ? target : "", target ? target_len : 0), value);
	return(1);
}

static size_t bearer_component_capture_impl(const char* target, size_t target_len, const char* props, size_t props_len, char* out, size_t cap)
{
	if(!out)
	{
		ob_start();
		if(props)
		{
			DValue value;
			String error;
			if(!brb_decode(String(props, props_len), value, &error))
			{
				ob_get_close();
				return(0);
			}
			component_render(String(target ? target : "", target ? target_len : 0), value);
		}
		else
			component_render(String(target ? target : "", target ? target_len : 0));
		wasm_component_capture_result = ob_get_close();
		if(wasm_component_capture_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_component_capture_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_component_capture_result.size());
	}
	if(cap < wasm_component_capture_result.size())
		return(wasm_component_capture_result.size());
	if(!wasm_component_capture_result.empty())
		memcpy(out, wasm_component_capture_result.data(), wasm_component_capture_result.size());
	size_t size = wasm_component_capture_result.size();
	wasm_component_capture_result.clear();
	return(size);
}

size_t bearer_component_capture(const char* target, size_t target_len, char* out, size_t cap)
{
	return(bearer_component_capture_impl(target, target_len, 0, 0, out, cap));
}

size_t bearer_component_capture_props_brrb(const char* target, size_t target_len, const char* props, size_t props_len, char* out, size_t cap)
{
	return(bearer_component_capture_impl(target, target_len, props, props_len, out, cap));
}

size_t bearer_dv_string_to_brrb(const char* value, size_t value_len, char* out, size_t cap)
{
	DValue encoded_value;
	encoded_value.set(String(value ? value : "", value ? value_len : 0));
	String encoded = brb_encode(encoded_value);
	if(out && cap >= encoded.size())
		memcpy(out, encoded.data(), encoded.size());
	return(encoded.size());
}

size_t bearer_dv_brrb_to_string(const char* value, size_t value_len, char* out, size_t cap)
{
	DValue decoded;
	String error;
	if(!brb_decode(String(value ? value : "", value ? value_len : 0), decoded, &error))
		return(0);
	String result = decoded.to_string();
	if(out && cap >= result.size())
		memcpy(out, result.data(), result.size());
	return(result.size());
}

struct BearerDValueEntry
{
	const char* key;
	u32 key_len;
	const char* value;
	u32 value_len;
};

static bool bearer_decode_brrb_span(const char* value, size_t value_len, DValue& decoded)
{
	String error;
	return(brb_decode(String(value ? value : "", value ? value_len : 0), decoded, &error));
}

static size_t bearer_copy_bytes(const String& value, char* out, size_t cap)
{
	if(out && cap >= value.size())
		memcpy(out, value.data(), value.size());
	return(value.size());
}

s32 bearer_string_find(const char* value, size_t value_len, const char* needle, size_t needle_len)
{
	size_t found = String(value ? value : "", value ? value_len : 0).find(String(needle ? needle : "", needle ? needle_len : 0));
	return(found == String::npos || found > (size_t)std::numeric_limits<s32>::max() ? -1 : (s32)found);
}

size_t bearer_string_replace(const char* value, size_t value_len, const char* from, size_t from_len, const char* to, size_t to_len, char* out, size_t cap)
{
	String result(value ? value : "", value ? value_len : 0);
	String source(from ? from : "", from ? from_len : 0), replacement(to ? to : "", to ? to_len : 0);
	if(!source.empty())
	{
		size_t position = 0;
		while((position = result.find(source, position)) != String::npos)
		{
			if(replacement.size() > source.size())
			{
				size_t growth = replacement.size() - source.size(), limit = (size_t)std::numeric_limits<s32>::max() - 20;
				if(growth > limit || result.size() > limit - growth)
					return(std::numeric_limits<size_t>::max());
			}
			result.replace(position, source.size(), replacement);
			position += replacement.size();
		}
	}
	return(bearer_copy_bytes(result, out, cap));
}

size_t bearer_string_lower(const char* value, size_t value_len, char* out, size_t cap)
{
	return(bearer_copy_bytes(to_lower(String(value ? value : "", value ? value_len : 0)), out, cap));
}

size_t bearer_string_upper(const char* value, size_t value_len, char* out, size_t cap)
{
	return(bearer_copy_bytes(to_upper(String(value ? value : "", value ? value_len : 0)), out, cap));
}

s32 bearer_string_nonblank(const char* value, size_t value_len)
{
	return(trim(String(value ? value : "", value ? value_len : 0)).empty() ? 0 : 1);
}

size_t bearer_string_list(s32 operation, const char* input, size_t input_len, const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(!out)
	{
		wasm_string_list_result.clear();
		String argument_value(argument ? argument : "", argument ? argument_len : 0);
		if(operation == 0)
		{
			DValue result;
			for(auto& part : split(String(input ? input : "", input ? input_len : 0), argument_value))
			{
				DValue value;
				value = part;
				result.push(value);
			}
			wasm_string_list_result = brb_encode(result);
		}
		else if(operation == 1)
		{
			DValue decoded;
			String error;
			if(!brb_decode(String(input ? input : "", input ? input_len : 0), decoded, &error) || !decoded.is_list() || !decoded.deref()._list_mode)
				return(std::numeric_limits<size_t>::max());
			StringList values;
			bool valid = true;
			decoded.each([&](const DValue& item, String) {
				if(item.deref().type != 'S')
					valid = false;
				else
					values.push_back(item.to_string());
			});
			if(!valid)
				return(std::numeric_limits<size_t>::max());
			wasm_string_list_result = join(values, argument_value);
		}
		else
			return(std::numeric_limits<size_t>::max());
		if(wasm_string_list_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_string_list_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_string_list_result.size());
	}
	return(bearer_copy_staged(wasm_string_list_result, out, cap));
}

size_t bearer_string_substr(const char* value, size_t value_len, s32 start, s32 length, char* out, size_t cap)
{
	return(bearer_copy_bytes(substr(String(value ? value : "", value ? value_len : 0), start, length), out, cap));
}

s32 bearer_session_start(const char* name, size_t name_len)
{
	if(context == 0)
		bearer_wasm_core_init();
	return(session_start(String(name ? name : "", name ? name_len : 0)) != "");
}

s32 bearer_session_set(const char* key, size_t key_len, const char* value, size_t value_len)
{
	if(context == 0)
		bearer_wasm_core_init();
	if(context->session_id == "")
		return(0);
	context->session[String(key ? key : "", key ? key_len : 0)] = String(value ? value : "", value ? value_len : 0);
	return(1);
}

s32 bearer_session_remove(const char* key, size_t key_len)
{
	if(context == 0)
		bearer_wasm_core_init();
	if(context->session_id == "")
		return(0);
	context->session.erase(String(key ? key : "", key ? key_len : 0));
	return(1);
}

s32 bearer_session_destroy(const char* name, size_t name_len)
{
	if(context == 0)
		bearer_wasm_core_init();
	session_destroy(String(name ? name : "", name ? name_len : 0));
	return(1);
}

s32 bearer_response_cookie(const char* name, size_t name_len, const char* value, size_t value_len)
{
	if(context == 0)
		bearer_wasm_core_init();
	set_cookie(String(name ? name : "", name ? name_len : 0), String(value ? value : "", value ? value_len : 0));
	return(1);
}

size_t bearer_ws_message(char* out, size_t cap)
{
	return(bearer_copy_bytes(ws_message(), out, cap));
}

size_t bearer_ws_connection_id(char* out, size_t cap)
{
	return(bearer_copy_bytes(ws_connection_id(), out, cap));
}

size_t bearer_ws_scope(char* out, size_t cap)
{
	return(bearer_copy_bytes(ws_scope(), out, cap));
}

s32 bearer_ws_opcode()
{
	return((s32)ws_opcode());
}

s32 bearer_ws_is_binary()
{
	return(ws_is_binary());
}

s32 bearer_ws_send(const char* message, size_t message_len, s32 binary)
{
	return(ws_send(String(message ? message : "", message ? message_len : 0), binary != 0));
}

s32 bearer_ws_send_to(const char* connection_id, size_t connection_id_len, const char* message, size_t message_len, s32 binary)
{
	return(ws_send_to(String(connection_id ? connection_id : "", connection_id ? connection_id_len : 0),
		String(message ? message : "", message ? message_len : 0), binary != 0));
}

s32 bearer_ws_close(const char* connection_id, size_t connection_id_len)
{
	return(ws_close(String(connection_id ? connection_id : "", connection_id ? connection_id_len : 0)));
}

size_t bearer_csrf_token(const char* session_name, size_t session_name_len, const char* token_name, size_t token_name_len, char* out, size_t cap)
{
	String token = csrf_token(String(session_name ? session_name : "", session_name ? session_name_len : 0),
		String(token_name ? token_name : "", token_name ? token_name_len : 0));
	return(bearer_copy_bytes(token, out, cap));
}

s32 bearer_csrf_valid(const char* submitted, size_t submitted_len, const char* session_name, size_t session_name_len,
	const char* token_name, size_t token_name_len)
{
	return(csrf_valid(String(submitted ? submitted : "", submitted ? submitted_len : 0),
		String(session_name ? session_name : "", session_name ? session_name_len : 0),
		String(token_name ? token_name : "", token_name ? token_name_len : 0)));
}

s32 bearer_csrf_rotate(const char* session_name, size_t session_name_len, const char* token_name, size_t token_name_len)
{
	csrf_rotate(String(session_name ? session_name : "", session_name ? session_name_len : 0),
		String(token_name ? token_name : "", token_name ? token_name_len : 0));
	return(1);
}

s32 bearer_redirect(const char* url, size_t url_len, s32 status)
{
	if(context == 0)
		bearer_wasm_core_init();
	if(status < 300 || status > 399)
		return(0);
	redirect(String(url ? url : "", url ? url_len : 0), status);
	return(1);
}

s32 bearer_response_set_status(s32 status)
{
	if(context == 0)
		bearer_wasm_core_init();
	if(status < 100 || status > 999)
		return(0);
	context->set_status(status);
	return(1);
}

s32 bearer_response_set_header(const char* name, size_t name_len, const char* value, size_t value_len)
{
	if(context == 0)
		bearer_wasm_core_init();
	String header_name(name ? name : "", name ? name_len : 0);
	if(!http_header_name_valid(header_name))
		return(0);
	context->header[header_name] = http_header_value_clean(String(value ? value : "", value ? value_len : 0));
	return(1);
}

size_t bearer_request_value(s32 kind, const char* key, size_t key_len, char* out, size_t cap)
{
	if(context == 0)
		bearer_wasm_core_init();
	String lookup(key ? key : "", key ? key_len : 0);
	const StringMap* values = kind == 0 ? &context->params : kind == 1 ? &context->get : kind == 2 ? &context->post : kind == 3 ? &context->cookies : &context->session;
	auto found = values->find(lookup);
	return(bearer_copy_bytes(found == values->end() ? String("") : found->second, out, cap));
}

size_t bearer_request_body(char* out, size_t cap)
{
	if(context == 0)
		bearer_wasm_core_init();
	return(bearer_copy_bytes(context->in, out, cap));
}

static size_t bearer_request_context_encode(Request* request, char* out, size_t cap)
{
	if(!request)
		return(0);
	DValue snapshot;
	auto copy_map = [&](String key, const StringMap& values) {
		snapshot[key].set_type('M');
		for(const auto& entry : values)
			snapshot[key][entry.first] = entry.second;
	};
	copy_map("params", request->params);
	copy_map("get", request->get);
	copy_map("post", request->post);
	copy_map("cookies", request->cookies);
	copy_map("session", request->session);
	snapshot["call"] = request->call;
	snapshot["cfg"] = request->cfg;
	snapshot["props"] = request->props;
	snapshot["connection"] = request->connection;
	snapshot["input"] = request->in;
	snapshot["session_id"] = request->session_id;
	snapshot["session_name"] = request->session_name;
	snapshot["current_unit"] = request->resources.current_unit_file;
	return(bearer_copy_bytes(brb_encode(snapshot), out, cap));
}

size_t bearer_request_context_brrb(char* out, size_t cap)
{
	if(context == 0)
		bearer_wasm_core_init();
	return(bearer_request_context_encode(context, out, cap));
}

size_t bearer_request_context_for_brrb(Request* request, char* out, size_t cap)
{
	return(bearer_request_context_encode(request, out, cap));
}

size_t bearer_dv_s32_to_brrb(s32 value, char* out, size_t cap)
{
	DValue encoded;
	encoded = (f64)value;
	return(bearer_copy_bytes(brb_encode(encoded), out, cap));
}

size_t bearer_dv_f64_to_brrb(f64 value, char* out, size_t cap)
{
	DValue encoded;
	encoded = value;
	return(bearer_copy_bytes(brb_encode(encoded), out, cap));
}

size_t bearer_dv_bool_to_brrb(s32 value, char* out, size_t cap)
{
	DValue encoded;
	encoded.set_bool(value != 0);
	return(bearer_copy_bytes(brb_encode(encoded), out, cap));
}

size_t bearer_dv_build_brrb(s32 list_mode, const BearerDValueEntry* entries, size_t count, char* out, size_t cap)
{
	DValue result;
	if(list_mode)
		result.set_array();
	else
		result.set_type('M');
	for(size_t i = 0; i < count; i++)
	{
		DValue child;
		if(!bearer_decode_brrb_span(entries[i].value, entries[i].value_len, child))
			return(0);
		if(list_mode)
			result.push(child);
		else
			result[String(entries[i].key ? entries[i].key : "", entries[i].key ? entries[i].key_len : 0)] = child;
	}
	return(bearer_copy_bytes(brb_encode(result), out, cap));
}

size_t bearer_dv_merge_brrb(const char* left, size_t left_len, const char* right, size_t right_len, char* out, size_t cap)
{
	if(!out)
	{
		wasm_dval_merge_result.clear();
		DValue left_value;
		DValue right_value;
		if(!bearer_decode_brrb_span(left, left_len, left_value) || !bearer_decode_brrb_span(right, right_len, right_value))
			return(std::numeric_limits<size_t>::max());
		wasm_dval_merge_result = brb_encode(array_merge(left_value, right_value));
		if(wasm_dval_merge_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_dval_merge_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_dval_merge_result.size());
	}
	return(bearer_copy_staged(wasm_dval_merge_result, out, cap));
}

s32 bearer_dv_get_brrb(const char* value, size_t value_len, s32 index_mode,
	const char* key, size_t key_len, s32 index, char* out, size_t cap)
{
	DValue decoded;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !decoded.is_array())
		return(-2);
	String lookup = index_mode ? std::to_string(index) : String(key ? key : "", key ? key_len : 0);
	const DValue* child = decoded.key(lookup);
	if(!child)
		return(-1);
	String encoded = brb_encode(*child);
	bearer_copy_bytes(encoded, out, cap);
	return((s32)encoded.size());
}

s32 bearer_dv_count_brrb(const char* value, size_t value_len)
{
	DValue decoded;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !decoded.is_array())
		return(-1);
	return((s32)decoded._map.size());
}

static bool bearer_dv_entry_at(const DValue& decoded, size_t ordinal, String& key, const DValue*& child)
{
	size_t position = 0;
	bool found = false;
	decoded.each([&](const DValue& item, String item_key) {
		if(!found && position++ == ordinal)
		{
			key = item_key;
			child = &item;
			found = true;
		}
	});
	return(found);
}

s32 bearer_dv_entry_key_brrb(const char* value, size_t value_len, size_t ordinal, char* out, size_t cap)
{
	DValue decoded;
	String key;
	const DValue* child = 0;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !decoded.is_array() || !bearer_dv_entry_at(decoded, ordinal, key, child))
		return(-1);
	bearer_copy_bytes(key, out, cap);
	return((s32)key.size());
}

s32 bearer_dv_entry_value_brrb(const char* value, size_t value_len, size_t ordinal, char* out, size_t cap)
{
	DValue decoded;
	String key;
	const DValue* child = 0;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !decoded.is_array() || !bearer_dv_entry_at(decoded, ordinal, key, child))
		return(-1);
	String encoded = brb_encode(*child);
	bearer_copy_bytes(encoded, out, cap);
	return((s32)encoded.size());
}

s32 bearer_dv_scalar_type_brrb(const char* value, size_t value_len)
{
	DValue decoded;
	if(!bearer_decode_brrb_span(value, value_len, decoded))
		return(-1);
	return((s32)decoded.deref().type);
}

s32 bearer_dv_s32_brrb(const char* value, size_t value_len, s32* out)
{
	DValue decoded;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || decoded.deref().type != 'F' || !out)
		return(0);
	f64 number = decoded.deref()._float;
	if(number < (f64)std::numeric_limits<s32>::min() || number > (f64)std::numeric_limits<s32>::max() || std::floor(number) != number)
		return(0);
	*out = (s32)number;
	return(1);
}

s32 bearer_dv_f64_brrb(const char* value, size_t value_len, f64* out)
{
	DValue decoded;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !out)
		return(0);
	const DValue& scalar = decoded.deref();
	if(scalar.type == 'F')
	{
		*out = scalar._float;
		return(1);
	}
	if(scalar.type != 'S' || scalar._String.empty())
		return(0);
	f64 parsed = 0;
	const char* begin = scalar._String.data();
	const char* end = begin + scalar._String.size();
	auto converted = std::from_chars(begin, end, parsed, std::chars_format::general);
	if(converted.ec != std::errc() || converted.ptr != end || !std::isfinite(parsed))
		return(0);
	*out = parsed;
	return(1);
}

s32 bearer_dv_bool_brrb(const char* value, size_t value_len, s32* out)
{
	DValue decoded;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || decoded.deref().type != 'B' || !out)
		return(0);
	*out = decoded.to_bool() ? 1 : 0;
	return(1);
}

size_t bearer_dv_ptr_to_brrb(DValue* value, char* out, size_t cap)
{
	return(bearer_copy_bytes(brb_encode(value ? *value : DValue()), out, cap));
}

DValue* bearer_dv_brrb_to_ptr(const char* value, size_t value_len)
{
	String error;
	if(!brb_decode(String(value ? value : "", value ? value_len : 0), wasm_unit_call_result, &error))
		return(0);
	return(&wasm_unit_call_result);
}

size_t bearer_unit_call_brrb(const char* target, size_t target_len,
	const char* function_name, size_t function_len,
	const char* input, size_t input_len, char* out, size_t cap)
{
	if(out == 0)
	{
		wasm_unit_call_encoded_result.clear();
		DValue call_value;
		String error;
		if(input_len && !brb_decode(String(input ? input : "", input ? input_len : 0), call_value, &error))
			return(0);
		DValue* result = unit_call(
			String(target ? target : "", target ? target_len : 0),
			String(function_name ? function_name : "", function_name ? function_len : 0),
			input_len ? &call_value : 0);
		wasm_unit_call_encoded_result = brb_encode(result ? *result : DValue());
	}
	size_t result_size = wasm_unit_call_encoded_result.size();
	if(out)
	{
		if(cap < result_size)
		{
			wasm_unit_call_encoded_result.clear();
			return(result_size);
		}
		memcpy(out, wasm_unit_call_encoded_result.data(), result_size);
		wasm_unit_call_encoded_result.clear();
	}
	return(result_size);
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
