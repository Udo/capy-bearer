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
extern "C" size_t bearer_host_sqlite(const char* in, size_t in_len, char* out, size_t cap);
extern "C" size_t bearer_host_zip(const char* in, size_t in_len, char* out, size_t cap);
extern "C" size_t bearer_host_units(const char* in, size_t in_len, char* out, size_t cap);
extern "C" size_t bearer_host_regex_capy(const char* in, size_t in_len, char* out, size_t cap);
extern "C" size_t bearer_host_capy_backtrace(s32 max_frames, s32 skip_frames, const char* stack, size_t depth, char* out, size_t cap);
extern "C" void bearer_host_hard_error(const char* message, size_t message_len);

extern "C" void bearer_hard_error(const char* message, size_t message_len)
{
	bearer_host_hard_error(message, message_len);
}

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

static DValue wasm_zip_call(DValue request) { return(wasm_sized_hostcall(request, bearer_host_zip)); }
static DValue wasm_units_call(DValue request) { return(wasm_sized_hostcall(request, bearer_host_units)); }
static DValue wasm_mysql_call(DValue request) { return(wasm_sized_hostcall(request, bearer_host_mysql)); }
static DValue wasm_sqlite_call(DValue request) { return(wasm_sized_hostcall(request, bearer_host_sqlite)); }

static bool wasm_zip_apply(DValue request, DValue& response)
{
	response = wasm_zip_call(request);
	DValue* error = response.key("error");
	return(!error || error->to_string() == "");
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

DValue unit_info(String path)
{
	DValue request;
	request["op"] = "info";
	request["path"] = path;
	DValue response = wasm_units_call(request);
	DValue* result = response.key("result");
	return(result ? *result : DValue());
}

DValue units_list()
{
	DValue request;
	request["op"] = "list";
	DValue response = wasm_units_call(request);
	DValue* result = response.key("result");
	return(result ? *result : DValue());
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
static String wasm_crypto_result;
static String wasm_regex_result;
static String wasm_string_list_result;
static String wasm_dval_merge_result;
static String wasm_dval_extract_string_result;
static String wasm_dval_read_result;
static String wasm_dval_require_result;
static String wasm_dval_path_result;
static String wasm_sqlite_result;
static std::vector<SQLite*> wasm_capy_sqlite_handles;
static String wasm_mysql_result;
static std::vector<MySQL*> wasm_capy_mysql_handles;
static size_t bearer_copy_bytes(const String& value, char* out, size_t cap);
static size_t bearer_copy_staged(String& staged, char* out, size_t cap);
static bool bearer_decode_brrb_span(const char* value, size_t value_len, DValue& decoded);
static String wasm_unit_call_encoded_result;

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
	u64 handle = to_u64(response["handle"].to_string(), 0);
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
		request["handle"] = std::to_string((u64)(uintptr_t)connection);
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

String MySQL::escape(String raw, char quote_char)
{
	if(!connection)
		return(mysql_escape(raw, quote_char));
	DValue request;
	request["op"] = "escape";
	request["handle"] = std::to_string((u64)(uintptr_t)connection);
	request["raw"] = raw;
	request["quote_char"] = String(quote_char > 0 ? 1 : 0, quote_char);
	DValue response = wasm_mysql_call(request);
	_preload_next_error_code = (u32)response["error_code"].to_u64();
	statement_info = response["statement_info"].to_string();
	parameter_error = _preload_next_error_code != 0;
	DValue* result = response.key("result");
	return(result ? result->to_string() : String(""));
}

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
			if(isalnum((unsigned char)c) || c == '_')
				identifier.append(1, c);
			else if(c == '!' && query[i + 1] != '=')
			{
				auto parameter = map.find(identifier);
				String value = parameter == map.end() ? String("") : parameter->second;
				bool valid = identifier != "" && value != "";
				for(char digit : value)
					if(!isdigit((unsigned char)digit)) valid = false;
				if(!valid)
				{
					parameter_error = true;
					statement_info = "mysql unsigned parameter :" + identifier + "! must contain only decimal digits";
					return("");
				}
				result.append(value);
				mode = 0;
			}
			else
			{
				auto parameter = map.find(identifier);
				if(identifier == "" || parameter == map.end())
				{
					parameter_error = true;
					statement_info = "mysql named parameter :" + identifier + " is missing";
					return("");
				}
				result.append(escape(parameter->second));
				if(parameter_error) return("");
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

DValue MySQL::query(String q)
{
	affected_rows = 0;
	if(!connection && mysql_has_unquoted_positional_placeholder(q, false))
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
	request["handle"] = std::to_string((u64)(uintptr_t)connection);
	request["query"] = q;
	DValue response = wasm_mysql_call(request);
	insert_id = response["insert_id"].to_u64();
	affected_rows = (u32)response["affected"].to_u64();
	_preload_next_error_code = (u32)response["error_code"].to_u64();
	statement_info = response["statement_info"].to_string();
	DValue* result = response.key("result");
	return(result ? *result : DValue());
}

DValue MySQL::query(String q, StringMap params)
{
	if(!connection && mysql_has_unquoted_positional_placeholder(q, false))
	{
		_preload_next_error_code = 2000;
		statement_info = "mysql positional ? placeholders are not supported; use named :name placeholders";
		return(DValue());
	}
	if(!connection)
	{
		_preload_next_error_code = 2000;
		statement_info = "mysql connection is not open";
		return(DValue());
	}
	DValue request;
	request["op"] = "query";
	request["handle"] = std::to_string((u64)(uintptr_t)connection);
	request["query"] = q;
	for(auto& parameter : params)
		request["params"][parameter.first] = parameter.second;
	DValue response = wasm_mysql_call(request);
	insert_id = response["insert_id"].to_u64();
	affected_rows = (u32)response["affected"].to_u64();
	_preload_next_error_code = (u32)response["error_code"].to_u64();
	statement_info = response["statement_info"].to_string();
	DValue* result = response.key("result");
	return(result ? *result : DValue());
}
DValue MySQL::get_pending_result() { return(DValue()); }

// sqlite runs host-side (the host links libsqlite and owns the connections in
// a per-workspace handle table). One BRRB2-marshalled hostcall carries
// {op,handle,path,query,params} in and {handle,result,insert_id,affected,
// error_code,statement_info} out. `connection` holds the host handle (>0).
void SQLite::set_error(s32 code, String info) { error_code = code; statement_info = info; }

bool SQLite::connect(String path)
{
	this->path = path;
	DValue request;
	request["op"] = "connect";
	request["path"] = path;
	DValue response = wasm_sqlite_call(request);
	u64 handle = to_u64(response["handle"].to_string(), 0);
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
		request["handle"] = std::to_string((u64)(uintptr_t)connection);
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
	request["handle"] = std::to_string((u64)(uintptr_t)connection);
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

static MySQL* bearer_mysql_handle(u64 handle)
{
	if(handle == 0 || handle > wasm_capy_mysql_handles.size())
		return(0);
	return(wasm_capy_mysql_handles[(size_t)handle - 1]);
}

extern "C" u64 bearer_mysql_connect(const char* host, size_t host_len, const char* username, size_t username_len,
	const char* password, size_t password_len, const char* database, size_t database_len)
{
	MySQL* db = mysql_connect(String(host ? host : "", host ? host_len : 0), String(username ? username : "", username ? username_len : 0),
		String(password ? password : "", password ? password_len : 0), String(database ? database : "", database ? database_len : 0));
	if(!db)
		return(0);
	wasm_capy_mysql_handles.push_back(db);
	return((u64)wasm_capy_mysql_handles.size());
}

extern "C" s32 bearer_mysql_connected(u64 handle)
{
	MySQL* db = bearer_mysql_handle(handle);
	return(db ? (mysql_connected(db) ? 1 : 0) : -1);
}

extern "C" s32 bearer_mysql_disconnect(u64 handle)
{
	MySQL* db = bearer_mysql_handle(handle);
	if(!db)
		return(0);
	mysql_disconnect(db);
	wasm_capy_mysql_handles[(size_t)handle - 1] = 0;
	return(1);
}

extern "C" size_t bearer_mysql_error(u64 handle, char* out, size_t cap)
{
	if(!out)
	{
		wasm_mysql_result.clear();
		MySQL* db = bearer_mysql_handle(handle);
		if(!db)
			return(std::numeric_limits<size_t>::max());
		wasm_mysql_result = mysql_error(db);
		return(wasm_mysql_result.size());
	}
	return(bearer_copy_staged(wasm_mysql_result, out, cap));
}

extern "C" size_t bearer_mysql_escape(const char* raw, size_t raw_len, const char* quote, size_t quote_len, char* out, size_t cap)
{
	if(!out)
	{
		wasm_mysql_result = mysql_escape(String(raw ? raw : "", raw ? raw_len : 0), quote && quote_len ? quote[0] : 0);
		return(wasm_mysql_result.size());
	}
	return(bearer_copy_staged(wasm_mysql_result, out, cap));
}

extern "C" size_t bearer_mysql_query(u64 handle, const char* query, size_t query_len, const char* params, size_t params_len, char* out, size_t cap)
{
	if(!out)
	{
		wasm_mysql_result.clear();
		MySQL* db = bearer_mysql_handle(handle);
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
		DValue result = params_len ? mysql_query(db, String(query ? query : "", query ? query_len : 0), parameter_map)
			: mysql_query(db, String(query ? query : "", query ? query_len : 0));
		wasm_mysql_result = brb_encode(result);
		if(wasm_mysql_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_mysql_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_mysql_result.size());
	}
	return(bearer_copy_staged(wasm_mysql_result, out, cap));
}

extern "C" u64 bearer_mysql_insert_id(u64 handle)
{
	MySQL* db = bearer_mysql_handle(handle);
	return(db ? mysql_insert_id(db) : std::numeric_limits<u64>::max());
}

extern "C" u64 bearer_mysql_affected_rows(u64 handle)
{
	MySQL* db = bearer_mysql_handle(handle);
	return(db ? mysql_affected_rows(db) : std::numeric_limits<u64>::max());
}

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
extern "C" s32 bearer_host_unit_load(const char* target, size_t target_len,
	const char* current_unit, size_t current_unit_len);
extern "C" void bearer_host_module_enter(s32 capability);
extern "C" void bearer_host_module_leave();
extern "C" s32 bearer_host_module_resolve(s32 capability, const char* name, size_t name_len);
extern "C" s32 bearer_host_module_staged_size(s32 capability, const char* name, size_t name_len,
	const char* input, size_t input_len);
extern "C" s32 bearer_host_module_stage(s32 capability, const char* name, size_t name_len,
	const char* input, size_t input_len, const char* result, size_t result_len);
extern "C" s32 bearer_host_module_copy(s32 capability, const char* name, size_t name_len,
	const char* input, size_t input_len, char* out, size_t cap);
extern "C" void bearer_host_module_reset();

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
	if(name.length() >= 5 && name.substr(name.length() - 5) == ".capy")
		return(name);
	return(name + ".capy");
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

// Run a unit's ONCE() handler at most once per request (native
// compiler_run_unit_once_if_needed semantics): dedup on the resolved unit
// path via request.once_units. The handler emits head assets, etc.
struct WasmHandlerFrame {
	DValue initial_out;
	DValue written_out;
	void* last_allocated_dvalue = 0;
	void* input_dvalue = 0;
};

static std::vector<WasmHandlerFrame> wasm_handler_frames;

static void wasm_invoke_handler(WasmRequestHandler handler, Request& request);
static void wasm_handler_input_release(void* value);

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
	wasm_invoke_handler(once_handler, request);
	request.resources.current_unit_file = previous_unit;
}

static String wasm_unit_call_handler(const String& function_name)
{
	String macro = to_upper(trim(function_name));
	if(macro == "RENDER" || macro.rfind("RENDER:", 0) == 0)
		return("render" + (macro.length() > 7 ? ":" + trim(function_name.substr(function_name.find(":") + 1)) : String("")));
	if(macro == "COMPONENT" || macro.rfind("COMPONENT:", 0) == 0)
		return("component" + (macro.length() > 10 ? ":" + trim(function_name.substr(function_name.find(":") + 1)) : String("")));
	if(macro == "ONCE")
		return("once");
	if(macro == "INIT")
		return("init");
	return("");
}

DValue* unit_call(String file_name, String function_name, DValue* call_param)
{
	String handler = wasm_unit_call_handler(function_name);

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
		wasm_invoke_handler(handler_fn, *context);
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

static bool component_render_with_props(String name, DValue& props, Request& request)
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
		return(false);
	}
	wasm_run_once(resolved, request);
	RequestPropsScope props_scope(&request, props);
	String previous_unit = request.resources.current_unit_file;
	if(resolved != "")
		request.resources.current_unit_file = resolved;
	// a wasm function pointer is its index in the shared funcref table; the
	// host returned the handler's slot, so this is a plain call_indirect
	WasmRequestHandler handler_fn = (WasmRequestHandler)(uintptr_t)slot;
	wasm_invoke_handler(handler_fn, request);
	request.resources.current_unit_file = previous_unit;
	return(true);
}

bool component_render(String name, DValue props, Request& request) { return(component_render_with_props(name, props, request)); }
bool component_render(String name) { DValue props; return(component_render_with_props(name, props, *context)); }
bool component_render(String name, Request& request) { DValue props; return(component_render_with_props(name, props, request)); }
bool component_render(String name, DValue props) { return(component_render_with_props(name, props, *context)); }

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
	wasm_invoke_handler(handler_fn, request);
	request.resources.current_unit_file = previous_unit;
}

void unit_render(String file_name) { unit_render(file_name, *context); }

struct WasmRequestEnvelopeSegment
{
	const char* data = 0;
	size_t size = 0;
};

static bool wasm_decode_request_envelope(const char* encoded, size_t encoded_size,
	WasmRequestEnvelopeSegment (&segments)[15], String& error)
{
	if(encoded_size < 6 || memcmp(encoded, "BRRQ", 4) != 0)
	{
		error = "missing BEARER request-envelope header";
		return(false);
	}
	if((u8)encoded[4] != 3 || (u8)encoded[5] != 15)
	{
		error = "unsupported BEARER request-envelope version or segment count";
		return(false);
	}
	size_t offset = 6;
	for(u32 i = 0; i < 15; i++)
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
		wasm_invoke_handler(once_handler, *context);
	}
	WasmRequestHandler handler_fn = (WasmRequestHandler)(uintptr_t)handler_slot;
	wasm_invoke_handler(handler_fn, *context);
}

void* bearer_alloc(size_t len)
{
	void* value = malloc(len);
	if(len == 28 && !wasm_handler_frames.empty())
		wasm_handler_frames.back().last_allocated_dvalue = value;
	return(value);
}

void bearer_free(void* ptr)
{
	wasm_handler_input_release(ptr);
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
	wasm_request.cfg = DValue();
	wasm_request.props = DValue();
	wasm_request.connection = DValue();
	wasm_request.uploaded_files.clear();
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
	wasm_request.resources.websocket_connection_id = "";
	wasm_request.resources.websocket_scope = "";
	wasm_request.resources.websocket_opcode = 0;
	wasm_request.resources.websocket_is_binary = false;
	wasm_request.resources.websocket_scope_connection_ids.clear();
	wasm_component_slots.clear();
	wasm_component_paths.clear();
	wasm_component_errors.clear();
	wasm_handler_frames.clear();
	bearer_host_module_reset();
	wasm_unit_call_encoded_result.clear();
	wasm_component_capture_result.clear();
	wasm_file_read_result.clear();
	wasm_file_temp_result.clear();
	wasm_unit_info_result.clear();
	wasm_units_list_result.clear();
	wasm_codec_result.clear();
	wasm_crypto_result.clear();
	wasm_regex_result.clear();
	wasm_string_list_result.clear();
	wasm_dval_merge_result.clear();
	wasm_dval_extract_string_result.clear();
	wasm_dval_read_result.clear();
	wasm_dval_require_result.clear();
	wasm_dval_path_result.clear();
	wasm_sqlite_result.clear();
	wasm_mysql_result.clear();
	for(MySQL* db : wasm_capy_mysql_handles)
		if(db)
			mysql_disconnect(db);
	wasm_capy_mysql_handles.clear();
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
	DValue decoded_props;
	DValue decoded_cfg;
	DValue decoded_files;
	WasmRequestEnvelopeSegment segments[15];
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
	if(!decode_tree(12, decoded_props, "request props") ||
		!decode_tree(13, decoded_cfg, "request config") ||
		!decode_tree(14, decoded_files, "request files") || !decoded_files.is_list())
	{
		bearer_host_log(3, error.data(), error.size());
		return(5);
	}
	std::vector<UploadedFile> uploaded_files;
	bool valid_files = true;
	decoded_files.each([&](const DValue& value, String) {
		const DValue& item = value.deref();
		const DValue* name = item.key("name");
		const DValue* temporary_path = item.key("temporary_path");
		const DValue* encoded_size = item.key("size");
		if(item.type != 'M' || item.is_list() || !name || !temporary_path || !encoded_size)
		{
			valid_files = false;
			return;
		}
		u64 size = encoded_size->to_u64();
		if(size > std::numeric_limits<u32>::max())
		{
			valid_files = false;
			return;
		}
		uploaded_files.push_back({name->to_string(), temporary_path->to_string(), (u32)size});
	});
	if(!valid_files)
	{
		error = "invalid request files";
		bearer_host_log(3, error.data(), error.size());
		return(6);
	}
	wasm_server.config = std::move(decoded_config);
	wasm_request.call = std::move(decoded_call);
	wasm_request.cfg = std::move(decoded_cfg);
	wasm_request.props = std::move(decoded_props);
	wasm_request.uploaded_files = std::move(uploaded_files);
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
	wasm_request.resources.websocket_connection_id = "";
	wasm_request.resources.websocket_scope = "";
	wasm_request.resources.websocket_opcode = 0;
	wasm_request.resources.websocket_is_binary = false;
	wasm_request.resources.websocket_scope_connection_ids.clear();
	if(segments[11].size)
	{
		wasm_request.resources.websocket_connection_id = decoded_ws["connection_id"].to_string();
		wasm_request.resources.websocket_scope = decoded_ws["scope"].to_string();
		wasm_request.resources.websocket_opcode = (u8)decoded_ws["opcode"].to_u64();
		wasm_request.resources.websocket_is_binary = decoded_ws["binary"].to_bool();
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

size_t bearer_regex_dval(s32 operation, const char* pattern, size_t pattern_len, const char* subject, size_t subject_len,
	const char* replacement, size_t replacement_len, const char* flags, size_t flags_len, char* out, size_t cap)
{
	if(!out)
	{
		enum class RegexOperation { regex_regex_search = 0, regex_regex_search_all = 1, regex_regex_replace = 2, regex_regex_split = 3 };
		static const char* operations[] = {"search", "search_all", "replace", "split_strings"};
		DValue response;
		const RegexOperation selected = static_cast<RegexOperation>(operation);
		if((selected != RegexOperation::regex_regex_search && selected != RegexOperation::regex_regex_search_all && selected != RegexOperation::regex_regex_split) ||
			!bearer_regex_call(operations[operation], pattern, pattern_len, subject, subject_len, replacement, replacement_len, flags, flags_len, response))
			return(std::numeric_limits<size_t>::max());
		if(selected == RegexOperation::regex_regex_search || selected == RegexOperation::regex_regex_search_all)
		{
			DValue* tree = response.key("tree");
			wasm_regex_result = brb_encode(tree ? *tree : DValue());
		}
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

size_t bearer_regex_text(s32 operation, const char* pattern, size_t pattern_len, const char* subject, size_t subject_len,
	const char* replacement, size_t replacement_len, const char* flags, size_t flags_len, char* out, size_t cap)
{
	if(!out)
	{
		enum class RegexOperation { regex_regex_search = 0, regex_regex_search_all = 1, regex_regex_replace = 2, regex_regex_split = 3 };
		DValue response;
		if(static_cast<RegexOperation>(operation) != RegexOperation::regex_regex_replace || !bearer_regex_call("replace", pattern, pattern_len, subject, subject_len,
			replacement, replacement_len, flags, flags_len, response))
			return(std::numeric_limits<size_t>::max());
		wasm_regex_result = response["text"].to_string();
		if(wasm_regex_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_regex_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_regex_result.size());
	}
	return(bearer_copy_staged(wasm_regex_result, out, cap));
}

size_t bearer_codec_text(s32 operation, const char* input, size_t input_len, char* out, size_t cap)
{
	if(!out)
	{
		enum class CodecOperation { codec_base64_encode = 0, codec_base64_decode = 1, codec_uri_encode = 2, codec_uri_decode = 3, codec_json_decode = 4 };
		String value(input ? input : "", input ? input_len : 0);
		switch(static_cast<CodecOperation>(operation))
		{
			case CodecOperation::codec_base64_encode: wasm_codec_result = base64_encode(value); break;
			case CodecOperation::codec_base64_decode: {
				bool ok = false; wasm_codec_result = base64_decode(value, ok); if(!ok) wasm_codec_result.clear(); break;
			}
			case CodecOperation::codec_uri_encode: wasm_codec_result = uri_encode(value); break;
			case CodecOperation::codec_uri_decode: wasm_codec_result = uri_decode(value); break;
			default: wasm_codec_result.clear(); break;
		}
		if(wasm_codec_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_codec_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_codec_result.size());
	}
	return(bearer_copy_staged(wasm_codec_result, out, cap));
}

size_t bearer_codec_dval(s32 operation, const char* input, size_t input_len, char* out, size_t cap)
{
	enum class CodecOperation { codec_base64_encode = 0, codec_base64_decode = 1, codec_uri_encode = 2, codec_uri_decode = 3, codec_json_decode = 4 };
	if(!out)
	{
		if(static_cast<CodecOperation>(operation) != CodecOperation::codec_json_decode)
			return(std::numeric_limits<size_t>::max());
		String value(input ? input : "", input ? input_len : 0);
		wasm_codec_result = brb_encode(json_decode(value));
		if(wasm_codec_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_codec_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_codec_result.size());
	}
	return(bearer_copy_staged(wasm_codec_result, out, cap));
}

size_t bearer_crypto_string(s32 operation, const char* a, size_t a_len, const char* b, size_t b_len, char* out, size_t cap)
{
	enum class CryptoStringOperation { sha256 = 0, hmac_sha256 = 1, sha1 = 2, password_hash = 3 };
	if(!out)
	{
		String left(a ? a : "", a ? a_len : 0), right(b ? b : "", b ? b_len : 0);
		switch(static_cast<CryptoStringOperation>(operation))
		{
			case CryptoStringOperation::sha256: wasm_crypto_result = sha256(left); break;
			case CryptoStringOperation::hmac_sha256: wasm_crypto_result = hmac_sha256(left, right); break;
			case CryptoStringOperation::sha1: wasm_crypto_result = gen_sha1(left); break;
			case CryptoStringOperation::password_hash: wasm_crypto_result = password_hash(left); break;
			default: return(0);
		}
		return(wasm_crypto_result.size());
	}
	return(bearer_copy_staged(wasm_crypto_result, out, cap));
}

s32 bearer_crypto_bool(s32 operation, const char* a, size_t a_len, const char* b, size_t b_len)
{
	enum class CryptoBoolOperation { equal = 0, password_verify = 1, password_needs_rehash = 2 };
	String left(a ? a : "", a ? a_len : 0), right(b ? b : "", b ? b_len : 0);
	if(static_cast<CryptoBoolOperation>(operation) == CryptoBoolOperation::equal) return(crypto_equal(left, right));
	if(static_cast<CryptoBoolOperation>(operation) == CryptoBoolOperation::password_verify) return(password_verify(left, right));
	if(static_cast<CryptoBoolOperation>(operation) == CryptoBoolOperation::password_needs_rehash) return(password_needs_rehash(left));
	return(0);
}

size_t bearer_random_bytes(u64 count, char* out, size_t cap)
{
	if(!out)
	{
		wasm_crypto_result = random_bytes(count);
		return(wasm_crypto_result.size());
	}
	return(bearer_copy_staged(wasm_crypto_result, out, cap));
}

size_t bearer_capy_backtrace(s32 max_frames, s32 skip_frames, const char* stack, size_t depth, char* out, size_t cap)
{
	return(bearer_host_capy_backtrace(max_frames, skip_frames, stack, depth, out, cap));
}

u64 bearer_noise_u64(s32 operation, u64 a, u64 b, u64 index, u64 seed)
{
	enum class NoiseU64Operation { noise_gen_noise64 = 0, noise_gen_int = 1, noise_draw_int = 2 };
	if(static_cast<NoiseU64Operation>(operation) == NoiseU64Operation::noise_gen_noise64) return(gen_noise64(a, b));
	if(static_cast<NoiseU64Operation>(operation) == NoiseU64Operation::noise_gen_int) return(gen_int(a, b, index, seed));
	if(static_cast<NoiseU64Operation>(operation) == NoiseU64Operation::noise_draw_int) return(draw_int(a, b));
	return(0);
}

f64 bearer_noise_f64(s32 operation, f64 from, f64 to, u64 index, u64 seed, f64 precision)
{
	enum class NoiseF64Operation { noise_gen_noise01 = 0, noise_gen_float = 1, noise_draw_float = 2 };
	if(static_cast<NoiseF64Operation>(operation) == NoiseF64Operation::noise_gen_noise01) return(gen_noise01(index, seed));
	if(static_cast<NoiseF64Operation>(operation) == NoiseF64Operation::noise_gen_float) return(gen_float(from, to, index, seed, precision));
	if(static_cast<NoiseF64Operation>(operation) == NoiseF64Operation::noise_draw_float) return(draw_float(from, to, precision));
	return(0);
}

u64 bearer_noise32(u64 index, u64 seed)
{
	return(gen_noise32((u32)index, (u32)seed));
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
		DValue units = units_list();
		units.each([&](const DValue& unit, String) { result.push(unit); });
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

size_t bearer_file_pread(u64 handle, u64 offset, u64 length, char* out, size_t cap)
{
	if(!out)
	{
		wasm_file_read_result = file_pread(handle, offset, length);
		if(wasm_file_read_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_file_read_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_file_read_result.size());
	}
	return(bearer_copy_staged(wasm_file_read_result, out, cap));
}

u64 bearer_file_pwrite(u64 handle, u64 offset, const char* data, size_t data_len)
{
	return(file_pwrite(handle, offset, String(data ? data : "", data ? data_len : 0)));
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
	return(bearer_copy_staged(wasm_file_read_result, out, cap));
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
	return(bearer_copy_staged(wasm_file_temp_result, out, cap));
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

size_t bearer_component_resolve(const char* target, size_t target_len, char* out, size_t cap)
{
	return(bearer_copy_bytes(component_resolve(String(target ? target : "", target ? target_len : 0)), out, cap));
}

s32 bearer_component_render_bytes(const char* target, size_t target_len)
{
	return(component_render(String(target ? target : "", target ? target_len : 0)));
}

s32 bearer_component_render_props_brrb(const char* target, size_t target_len, const char* props, size_t props_len)
{
	DValue value;
	String error;
	if(!props || !brb_decode(String(props, props_len), value, &error))
		return(0);
	return(component_render(String(target ? target : "", target ? target_len : 0), value));
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
	return(bearer_copy_staged(wasm_component_capture_result, out, cap));
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
	String encoded = brb_encode_local(encoded_value);
	if(out && cap >= encoded.size())
		memcpy(out, encoded.data(), encoded.size());
	return(encoded.size());
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

static bool bearer_decode_local_brrb_span(const char* value, size_t value_len, DValue& decoded)
{
	String error;
	return(brb_decode_local(String(value ? value : "", value ? value_len : 0), decoded, &error));
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

size_t bearer_string_substr(const char* value, size_t value_len, s64 start, s64 length, char* out, size_t cap)
{
	return(bearer_copy_bytes(substr(String(value ? value : "", value ? value_len : 0), start, length), out, cap));
}

s64 bearer_string_strpos(const char* haystack, size_t haystack_len, const char* needle, size_t needle_len, s64 offset)
{
	return(strpos(String(haystack ? haystack : "", haystack ? haystack_len : 0), String(needle ? needle : "", needle ? needle_len : 0), offset));
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

s32 bearer_ws_send_scope(const char* message, size_t message_len, s32 binary, const char* scope, size_t scope_len)
{
	return(ws_send(String(message ? message : "", message ? message_len : 0), binary != 0,
		String(scope ? scope : "", scope ? scope_len : 0)));
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

static DValue bearer_session_snapshot(const Request* request)
{
	DValue session;
	session.set_type('M');
	session["id"] = request ? request->session_id : "";
	session["name"] = request ? request->session_name : "";
	session["values"].set_type('M');
	if(request)
		for(const auto& entry : request->session)
			session["values"][entry.first] = entry.second;
	return(session);
}

static size_t bearer_handler_input_encode(Request* request, char* out, size_t cap)
{
	if(!request)
		return(0);
	DValue snapshot;
	auto copy_map = [&](String key, const StringMap& values) {
		snapshot[key].set_type('M');
		for(const auto& entry : values)
			snapshot[key][entry.first] = entry.second;
	};
	auto param = [&](const String& key) {
		auto found = request->params.find(key);
		return(found == request->params.end() ? String("") : found->second);
	};
	copy_map("query", request->get);
	copy_map("form", request->post);
	snapshot["params"].set_type('M');
	for(const auto& entry : request->get)
		snapshot["params"][entry.first] = entry.second;
	for(const auto& entry : request->post)
		snapshot["params"][entry.first] = entry.second;
	if(wasm_handler_frames.size() > 1)
	{
		const WasmHandlerFrame& parent = wasm_handler_frames[wasm_handler_frames.size() - 2];
		if(parent.input_dvalue)
		{
			u32 length = 0;
			u32 capacity = 0;
			u32 payload = 0;
			const char* input = (const char*)parent.input_dvalue;
			memcpy(&length, input + 16, sizeof(length));
			memcpy(&capacity, input + 20, sizeof(capacity));
			memcpy(&payload, input + 24, sizeof(payload));
			DValue parent_snapshot;
			String error;
			if(length <= capacity && brb_decode(String((const char*)(uintptr_t)payload, length), parent_snapshot, &error) && parent_snapshot["params"].is_array())
				snapshot["params"] = parent_snapshot["params"];
		}
	}
	copy_map("cookies", request->cookies);
	snapshot["out"].set_type('M');
	snapshot["out"]["status"] = (f64)(request->flags.status ? request->flags.status : 200);
	snapshot["out"]["headers"].set_type('M');
	for(const auto& entry : request->header)
		snapshot["out"]["headers"][entry.first] = entry.second;
	snapshot["out"]["cookies"].set_type('M');
	for(const String& cookie : request->set_cookies)
	{
		if(cookie.rfind("Set-Cookie: ", 0) != 0)
			continue;
		String pair = cookie.substr(12);
		String name = nibble("=", pair);
		String value = nibble(";", pair);
		if(name != "")
			snapshot["out"]["cookies"][uri_decode(name)] = uri_decode(value);
	}
	snapshot["out"]["buffer"] = request->ob_stack.empty() ? request->out : request->ob_stack[0]->str();
	snapshot["server"].set_type('M');
	snapshot["headers"].set_type('M');
	for(const auto& entry : request->params)
	{
		const bool header = entry.first.rfind("HTTP_", 0) == 0 || entry.first == "CONTENT_TYPE" || entry.first == "CONTENT_LENGTH";
		const bool separate = entry.first == "REQUEST_METHOD" || entry.first.rfind("ROUTE_", 0) == 0 || entry.first == "SCRIPT_URL" || entry.first == "BASE_URL";
		if(header)
		{
			String name = entry.first.rfind("HTTP_", 0) == 0 ? entry.first.substr(5) : entry.first;
			for(char& character : name)
				character = character == '_' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			snapshot["headers"][name] = entry.second;
		}
		else if(!separate)
			snapshot["server"][entry.first] = entry.second;
	}
	snapshot["method"] = param("REQUEST_METHOD");
	snapshot["body"] = request->in;
	snapshot["files"].set_array();
	for(const UploadedFile& file : request->uploaded_files)
	{
		DValue item;
		item.set_type('M');
		item["name"] = file.file_name;
		item["temporary_path"] = file.tmp_name;
		item["size"] = (f64)file.size;
		snapshot["files"].push(item);
	}
	snapshot["route"].set_type('M');
	snapshot["route"]["path"] = param("ROUTE_PATH");
	snapshot["route"]["page"] = param("ROUTE_PAGE");
	snapshot["route"]["raw_path"] = param("ROUTE_PATH_RAW");
	snapshot["route"]["valid"].set_bool(param("ROUTE_VALID") == "1");
	snapshot["url"].set_type('M');
	snapshot["url"]["script"] = param("SCRIPT_URL");
	snapshot["url"]["base"] = param("BASE_URL");
	snapshot["session"] = bearer_session_snapshot(request);
	snapshot["call"] = request->call;
	snapshot["config"] = request->cfg;
	snapshot["props"] = request->props;
	snapshot["connection"] = request->connection;
	snapshot["unit"] = request->resources.current_unit_file;
	snapshot["websocket"].set_type('M');
	snapshot["websocket"]["connection_id"] = request->resources.websocket_connection_id;
	snapshot["websocket"]["scope"] = request->resources.websocket_scope;
	snapshot["websocket"]["opcode"] = (f64)request->resources.websocket_opcode;
	snapshot["websocket"]["binary"].set_bool(request->resources.websocket_is_binary);
	snapshot["websocket"]["connections"].set_array();
	for(const String& connection_id : request->resources.websocket_scope_connection_ids)
	{
		DValue value;
		value = connection_id;
		snapshot["websocket"]["connections"].push(value);
	}
	String encoded = brb_encode(snapshot);
	if(out && cap >= encoded.size() && !wasm_handler_frames.empty())
	{
		wasm_handler_frames.back().initial_out = snapshot["out"];
		wasm_handler_frames.back().input_dvalue = wasm_handler_frames.back().last_allocated_dvalue;
	}
	return(bearer_copy_bytes(encoded, out, cap));
}

size_t bearer_handler_input_brrb(Request* request, char* out, size_t cap)
{
	return(bearer_handler_input_encode(request, out, cap));
}

static void wasm_invoke_handler(WasmRequestHandler handler, Request& request)
{
	wasm_handler_frames.push_back({});
	handler(request);
	WasmHandlerFrame frame = std::move(wasm_handler_frames.back());
	wasm_handler_frames.pop_back();
	DValue* out = frame.written_out.key("out");
	if(!out || out->type != 'M' || out->is_list())
		return;
	auto changed = [&](const char* key) {
		return(brb_encode(frame.initial_out[key]) != brb_encode((*out)[key]));
	};
	if(changed("status"))
	{
		const DValue& status = (*out)["status"].deref();
		if(status.type != 'F')
			__builtin_trap();
		s64 code = status.to_s64();
		if(code < 100 || code > 999)
			__builtin_trap();
		request.set_status((s32)code);
	}
	if(changed("headers"))
	{
		const DValue& before = frame.initial_out["headers"].deref();
		const DValue& after = (*out)["headers"].deref();
		if(after.type != 'M' || after.is_list())
			__builtin_trap();
		if(before.type == 'M' && !before.is_list())
			before.each([&](const DValue&, String name) { if(!after.key(name)) request.header.erase(name); });
		after.each([&](const DValue& value, String name) {
			if(!http_header_name_valid(name))
				__builtin_trap();
			request.header[name] = http_header_value_clean(value.to_string());
		});
	}
	if(changed("cookies"))
	{
		const DValue& after = (*out)["cookies"].deref();
		if(after.type == 'M' && !after.is_list())
			after.each([&](const DValue& value, String name) {
				const DValue* prior = frame.initial_out["cookies"].key(name);
				if(!prior || brb_encode(*prior) != brb_encode(value))
					set_cookie(name, value.to_string());
			});
	}
	if(changed("buffer"))
	{
		request.out = (*out)["buffer"].to_string();
		if(!request.ob_stack.empty())
		{
			request.ob_stack[0]->str(request.out);
			request.ob_stack[0]->clear();
		}
	}
}

static void wasm_handler_input_release(void* value)
{
	if(wasm_handler_frames.empty() || !wasm_handler_frames.back().input_dvalue)
		return;
	const char* input = (const char*)wasm_handler_frames.back().input_dvalue;
	u32 length = 0;
	u32 capacity = 0;
	u32 payload = 0;
	memcpy(&length, input + 16, sizeof(length));
	memcpy(&capacity, input + 20, sizeof(capacity));
	memcpy(&payload, input + 24, sizeof(payload));
	if(length > capacity || value != (void*)(uintptr_t)payload)
		return;
	DValue snapshot;
	String error;
	if(brb_decode(String((const char*)(uintptr_t)payload, length), snapshot, &error))
		wasm_handler_frames.back().written_out = std::move(snapshot);
}

size_t bearer_dv_s32_to_brrb(s32 value, char* out, size_t cap)
{
	DValue encoded;
	encoded = (f64)value;
	return(bearer_copy_bytes(brb_encode(encoded), out, cap));
}

size_t bearer_dv_s64_to_brrb(s64 value, char* out, size_t cap)
{
	DValue encoded;
	encoded.set(std::to_string(value));
	return(bearer_copy_bytes(brb_encode(encoded), out, cap));
}

size_t bearer_dv_u64_to_brrb(u64 value, char* out, size_t cap)
{
	DValue encoded;
	encoded.set(std::to_string(value));
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
		if(!bearer_decode_local_brrb_span(entries[i].value, entries[i].value_len, child))
			return(0);
		if(list_mode)
			result.push(child);
		else
			result[String(entries[i].key ? entries[i].key : "", entries[i].key ? entries[i].key_len : 0)] = child;
	}
	return(bearer_copy_bytes(brb_encode_local(result), out, cap));
}

size_t bearer_dv_merge_brrb(const char* left, size_t left_len, const char* right, size_t right_len, char* out, size_t cap)
{
	if(!out)
	{
		wasm_dval_merge_result.clear();
		DValue left_value;
		DValue right_value;
		if(!bearer_decode_local_brrb_span(left, left_len, left_value) || !bearer_decode_local_brrb_span(right, right_len, right_value))
			return(std::numeric_limits<size_t>::max());
		wasm_dval_merge_result = brb_encode_local(array_merge(left_value, right_value));
		if(wasm_dval_merge_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_dval_merge_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_dval_merge_result.size());
	}
	return(bearer_copy_staged(wasm_dval_merge_result, out, cap));
}

static bool bearer_brrb_call_decode(const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, DValue& result, DValue& supplied, String& text, bool local = false)
{
	wasm_dval_merge_result.clear();
	auto decode = local ? bearer_decode_local_brrb_span : bearer_decode_brrb_span;
	if(!decode(value, value_len, result) || !decode(argument, argument_len, supplied))
		return(false);
	text = String(key ? key : "", key ? key_len : 0);
	return(true);
}

static size_t bearer_brrb_call_finish(const DValue& result, bool local = false)
{
	wasm_dval_merge_result = local ? brb_encode_local(result) : brb_encode(result);
	if(wasm_dval_merge_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
	{
		wasm_dval_merge_result.clear();
		return(std::numeric_limits<size_t>::max());
	}
	return(wasm_dval_merge_result.size());
}

static size_t bearer_brrb_call_copy(char* out, size_t cap)
{
	return(bearer_copy_staged(wasm_dval_merge_result, out, cap));
}

size_t bearer_dv_apply_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text, true))
		return(std::numeric_limits<size_t>::max());
	enum class DValueOperation { dval_dval_set = 0, dval_dval_push = 1, dval_dval_pop = 2, dval_dval_remove = 3, dval_dval_clear = 4, dval_dval_get_by_path = 5, dval_dval_get_or_create = 6, dval_dval_set_array = 7, dval_dval_set_bool = 8, dval_dval_set_type = 9, dval_dval_get_type_name = 10, dval_dval_is_array = 11, dval_dval_is_list = 12, dval_dval_key = 13, dval_dval_keys = 14, dval_dval_values = 15, dval_dval_put = 18 };
	switch(static_cast<DValueOperation>(operation))
	{
		case DValueOperation::dval_dval_set: result.set(supplied); break;
		case DValueOperation::dval_dval_push: result.push(supplied); break;
		case DValueOperation::dval_dval_pop: result.pop(); break;
		case DValueOperation::dval_dval_remove: result.remove(text); break;
		case DValueOperation::dval_dval_clear: result.clear(); break;
		case DValueOperation::dval_dval_get_by_path: result = result.get_by_path(text); break;
		case DValueOperation::dval_dval_get_or_create: result.get_or_create(text); break;
		case DValueOperation::dval_dval_set_array: result.set_array(); break;
		case DValueOperation::dval_dval_set_bool: result.set_bool(supplied.to_bool()); break;
		case DValueOperation::dval_dval_set_type: if(text.size() != 1) return(std::numeric_limits<size_t>::max()); result.set_type(text[0]); break;
		case DValueOperation::dval_dval_get_type_name: result.set(result.get_type_name()); break;
		case DValueOperation::dval_dval_is_array: result.set_bool(result.is_array()); break;
		case DValueOperation::dval_dval_is_list: result.set_bool(result.is_list()); break;
		case DValueOperation::dval_dval_key: { const DValue* child = result.key(text); if(child) result = *child; else result.set_none(); break; }
		case DValueOperation::dval_dval_keys: { result = result.keys(); break; }
		case DValueOperation::dval_dval_values: result = result.values(); break;
		case DValueOperation::dval_dval_put: result[text] = supplied; break;
			// Core utility adapters use copied BRRB values so the native helpers remain
			// the only implementation of parsing, routing, and diagnostic policy.
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result, true));
}

size_t bearer_text_parsing_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text))
		return(std::numeric_limits<size_t>::max());
	enum class TextOperation { text_safe_name = 0, text_trim = 1, text_float_val = 2, text_str_starts_with = 4, text_str_ends_with = 5, text_encode_query = 6, text_parse_query = 7, text_split = 8, text_split_space = 9, text_split_utf8 = 10, text_split_http_headers = 11, text_split_kv = 12, text_sort = 13 };
	switch(static_cast<TextOperation>(operation))
	{
		case TextOperation::text_safe_name: result.set(safe_name(result.to_string())); break;
		case TextOperation::text_trim: result.set(trim(result.to_string())); break;
		case TextOperation::text_float_val: result.set(float_val(result.to_string())); break;
		case TextOperation::text_str_starts_with: result.set_bool(str_starts_with(result.to_string(), text)); break;
		case TextOperation::text_str_ends_with: result.set_bool(str_ends_with(result.to_string(), text)); break;
		case TextOperation::text_encode_query: result.set(encode_query(result.to_stringmap())); break;
		case TextOperation::text_parse_query: {
			StringMap parsed = parse_query(result.to_string()); result.set_type('M');
			for(const auto& entry : parsed) result[entry.first] = entry.second;
			break;
		}
		case TextOperation::text_split: {
			std::vector<String> parts = split_strings(result.to_string(), text);
			result.set_array();
			for(const String& part : parts) { DValue child; child.set(part); result.push(child); }
			break;
		}
		case TextOperation::text_split_space: {
			std::vector<String> parts = split_space_strings(result.to_string()); result.set_array();
			for(const String& part : parts) { DValue child; child = part; result.push(child); }
			break;
		}
		case TextOperation::text_split_utf8: {
			std::vector<String> parts = split_utf8_strings(result.to_string(), supplied.to_bool()); result.set_array();
			for(const String& part : parts) { DValue child; child = part; result.push(child); }
			break;
		}
		case TextOperation::text_split_http_headers: {
			StringMap headers = split_http_headers(result.to_string()); result.set_type('M');
			for(const auto& entry : headers) result[entry.first] = entry.second;
			break;
		}
		case TextOperation::text_split_kv: {
			if(text.size() != 1) return(std::numeric_limits<size_t>::max());
			StringMap values = split_kv(result.to_string(), text[0], supplied["trim"].to_bool(true), supplied["uppercase"].to_bool());
			result.set_type('M');
			for(const auto& value : values) result[value.first] = value.second;
			break;
		}
		case TextOperation::text_sort: {
			if(!result.is_list()) return(std::numeric_limits<size_t>::max());
			std::vector<String> parts; bool valid = true;
			result.each([&](const DValue& item, String) { if(item.deref().type != 'S') valid = false; else parts.push_back(item.to_string()); });
			if(!valid) return(std::numeric_limits<size_t>::max());
			std::sort(parts.begin(), parts.end()); result.set_array(); for(const String& part : parts) { DValue child; child = part; result.push(child); }
			break;
		}
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result));
}

// Copied BRRB transport for URI and route/path normalization.
size_t bearer_route_path_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text))
		return(std::numeric_limits<size_t>::max());
	enum class RouteOperation { route_parse_uri = 0, route_route_path_normalize = 1, route_route_path_is_safe = 2, route_route_path_sanitize = 3, route_runtime_safe_key = 4 };
	switch(static_cast<RouteOperation>(operation))
	{
		case RouteOperation::route_parse_uri: {
			URI uri = parse_uri(result.to_string());
			result.set_type('M'); result["parts"].set_type('M'); result["query"].set_type('M');
			for(const auto& entry : uri.parts) result["parts"][entry.first] = entry.second;
			for(const auto& entry : uri.query) result["query"][entry.first] = entry.second;
			break;
		}
		case RouteOperation::route_route_path_normalize: result.set(route_path_normalize(result.to_string())); break;
		case RouteOperation::route_route_path_is_safe: result.set_bool(route_path_is_safe(result.to_string())); break;
		case RouteOperation::route_route_path_sanitize: result.set(route_path_sanitize(result.to_string(), text)); break;
		case RouteOperation::route_runtime_safe_key: result.set(runtime_safe_key(result.to_string(), text)); break;
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result));
}

// Copied BRRB transport for runtime diagnostics.
size_t bearer_runtime_diagnostics_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text))
		return(std::numeric_limits<size_t>::max());
	enum class DiagnosticOperation { diagnostic_signal_name = 0, diagnostic_backtrace_capture = 1, diagnostic_usleep = 2, diagnostic_var_dump = 3 };
	switch(static_cast<DiagnosticOperation>(operation))
	{
		case DiagnosticOperation::diagnostic_signal_name: result.set(signal_name((s32)result.to_s64())); break;
		case DiagnosticOperation::diagnostic_backtrace_capture: result.set(backtrace_capture((u32)result.to_s64(32), (u32)supplied.to_s64())); break;
		case DiagnosticOperation::diagnostic_usleep: { s64 usec = result.to_s64(); if(usec < 0) return(std::numeric_limits<size_t>::max()); result.set((f64)usleep((u32)usec)); break; }
		case DiagnosticOperation::diagnostic_var_dump: result.set(var_dump(result, text, supplied.to_string("\n"))); break;
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result));
}

size_t bearer_codec_archive_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text, true))
		return(std::numeric_limits<size_t>::max());
	enum class ArchiveOperation { archive_brb_decode = 0, archive_brb_encode = 1, archive_xml_decode = 2, archive_xml_encode = 3, archive_yaml_decode = 4, archive_yaml_encode = 5, archive_markdown_to_html = 6, archive_markdown_to_ast = 7, archive_json_consume_space = 8, archive_json_encode = 9, archive_json_encode_10 = 10, archive_html_escape = 11, archive_gz_compress = 12, archive_gz_uncompress = 13, archive_zip_list = 14, archive_zip_read = 15, archive_zip_create = 16, archive_zip_extract = 17 };
	switch(static_cast<ArchiveOperation>(operation))
	{
		case ArchiveOperation::archive_brb_decode: result = brb_decode(result.to_string()); break;
		case ArchiveOperation::archive_brb_encode: result.set(brb_encode(result)); break;
		case ArchiveOperation::archive_xml_decode: result = xml_decode(result.to_string()); break;
		case ArchiveOperation::archive_xml_encode: result.set(xml_encode(result, text.empty() ? "root" : text)); break;
		case ArchiveOperation::archive_yaml_decode: result = yaml_decode(result.to_string()); break;
		case ArchiveOperation::archive_yaml_encode: result.set(yaml_encode(result)); break;
		case ArchiveOperation::archive_markdown_to_html: result.set(markdown_to_html(result.to_string(), supplied)); break;
		case ArchiveOperation::archive_markdown_to_ast: result = markdown_to_ast(result.to_string(), supplied); break;
		case ArchiveOperation::archive_json_consume_space: { u32 index = (u32)supplied.to_u64(); json_consume_space(result.to_string(), index); result.set((f64)index); break; }
		case ArchiveOperation::archive_json_encode: result.set(json_encode(result)); break;
		case ArchiveOperation::archive_json_encode_10: result.set(json_encode(result.to_string())); break;
		case ArchiveOperation::archive_html_escape: result.set(html_escape(result.to_string())); break;
		case ArchiveOperation::archive_gz_compress: {
				DValue request, response; request["op"] = "gz_compress"; request["src"] = result.to_string();
				if(!wasm_zip_apply(request, response)) return(std::numeric_limits<size_t>::max()); result.set(response["result"].to_string()); break;
			}
		case ArchiveOperation::archive_gz_uncompress: {
				DValue request, response; request["op"] = "gz_uncompress"; request["src"] = result.to_string();
				if(!wasm_zip_apply(request, response)) return(std::numeric_limits<size_t>::max()); result.set(response["result"].to_string()); break;
			}
		case ArchiveOperation::archive_zip_list: {
				DValue request, response; request["op"] = "list"; request["path"] = result.to_string();
				if(!wasm_zip_apply(request, response)) return(std::numeric_limits<size_t>::max()); DValue* listed = response.key("result"); result = listed ? *listed : DValue(); break;
			}
		case ArchiveOperation::archive_zip_read: {
				DValue request, response; request["op"] = "read"; request["path"] = result.to_string(); request["entry"] = text;
				if(!wasm_zip_apply(request, response)) return(std::numeric_limits<size_t>::max()); result.set(response["result"].to_string()); break;
			}
		case ArchiveOperation::archive_zip_create: {
				DValue request, response; request["op"] = "create"; request["path"] = result.to_string(); request["entries"] = supplied;
				if(!wasm_zip_apply(request, response)) return(std::numeric_limits<size_t>::max()); result.set_bool(response["ok"].to_bool()); break;
			}
		case ArchiveOperation::archive_zip_extract: {
				DValue request, response; request["op"] = "extract"; request["path"] = result.to_string(); request["destination"] = text;
				if(!wasm_zip_apply(request, response)) return(std::numeric_limits<size_t>::max()); result.set_bool(response["ok"].to_bool()); break;
			}
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result));
}

size_t bearer_crypto_password_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text))
		return(std::numeric_limits<size_t>::max());
	enum class CryptoOperation { crypto_gen_sha1 = 0, crypto_hex = 1, crypto_sha256 = 2, crypto_hmac_sha256 = 3, crypto_crypto_equal = 4, crypto_password_hash = 5, crypto_password_verify = 6 };
	switch(static_cast<CryptoOperation>(operation))
	{
		case CryptoOperation::crypto_gen_sha1: result.set(gen_sha1(result.to_string())); break;
		case CryptoOperation::crypto_hex: result.set(hex(result.to_string())); break;
		case CryptoOperation::crypto_sha256: result.set(sha256(result.to_string())); break;
		case CryptoOperation::crypto_hmac_sha256: result.set(hmac_sha256(result.to_string(), supplied.to_string())); break;
		case CryptoOperation::crypto_crypto_equal: result.set_bool(crypto_equal(result.to_string(), supplied.to_string())); break;
		case CryptoOperation::crypto_password_hash: result.set(password_hash(result.to_string())); break;
		case CryptoOperation::crypto_password_verify: result.set_bool(password_verify(result.to_string(), supplied.to_string())); break;
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result));
}

size_t bearer_filesystem_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text))
		return(std::numeric_limits<size_t>::max());
	enum class FilesystemOperation { filesystem_basename = 0, filesystem_dirname = 1, filesystem_path_join = 2, filesystem_path_real = 3, filesystem_path_is_within = 4, filesystem_expand_path = 5, filesystem_file_get_contents = 6, filesystem_file_put_contents = 7, filesystem_file_append = 8, filesystem_file_copy = 9, filesystem_file_rename = 10, filesystem_file_stat = 11, filesystem_file_mtime = 12, filesystem_file_exists = 13, filesystem_file_truncate = 14, filesystem_file_chmod = 15, filesystem_file_symlink = 16, filesystem_ls = 17, filesystem_mkdir = 18, filesystem_dir_list = 19, filesystem_dir_remove = 20, filesystem_cwd_get = 21, filesystem_cwd_set = 22 };
	switch(static_cast<FilesystemOperation>(operation))
	{
		case FilesystemOperation::filesystem_basename: result.set(basename(result.to_string())); break;
		case FilesystemOperation::filesystem_dirname: result.set(dirname(result.to_string())); break;
		case FilesystemOperation::filesystem_path_join: result.set(path_join(result.to_string(), text)); break;
		case FilesystemOperation::filesystem_path_real: result.set(path_real(result.to_string())); break;
		case FilesystemOperation::filesystem_path_is_within: result.set_bool(path_is_within(result.to_string(), text)); break;
		case FilesystemOperation::filesystem_expand_path: result.set(expand_path(result.to_string(), text)); break;
		case FilesystemOperation::filesystem_file_get_contents: result.set(file_get_contents(result.to_string())); break;
		case FilesystemOperation::filesystem_file_put_contents: result.set_bool(file_put_contents(result.to_string(), supplied.to_string())); break;
		case FilesystemOperation::filesystem_file_append: result.set_bool(file_append(result.to_string(), supplied.to_string())); break;
		case FilesystemOperation::filesystem_file_copy: result.set_bool(file_copy(result.to_string(), text)); break;
		case FilesystemOperation::filesystem_file_rename: result.set_bool(file_rename(result.to_string(), text)); break;
		case FilesystemOperation::filesystem_file_stat: result = file_stat(result.to_string()); break;
		case FilesystemOperation::filesystem_file_mtime: result.set(std::to_string(file_mtime(result.to_string()))); break;
		case FilesystemOperation::filesystem_file_exists: result.set_bool(file_exists(result.to_string())); break;
		case FilesystemOperation::filesystem_file_truncate: result.set_bool(file_truncate(result.to_string(), supplied.to_u64())); break;
		case FilesystemOperation::filesystem_file_chmod: result.set_bool(file_chmod(result.to_string(), (u32)supplied.to_u64())); break;
		case FilesystemOperation::filesystem_file_symlink: result.set_bool(file_symlink(result.to_string(), text)); break;
		case FilesystemOperation::filesystem_ls: result = ls(result.to_string()); break;
		case FilesystemOperation::filesystem_mkdir: result.set_bool(mkdir(result.to_string())); break;
		case FilesystemOperation::filesystem_dir_list: result = dir_list(result.to_string()); break;
		case FilesystemOperation::filesystem_dir_remove: result.set_bool(dir_remove(result.to_string(), supplied.to_bool())); break;
		case FilesystemOperation::filesystem_cwd_get: result.set(cwd_get()); break;
		case FilesystemOperation::filesystem_cwd_set: cwd_set(result.to_string()); result.clear(); break;
			// Request operations deliberately execute against the workspace Request;
			// Capy only receives copied results and never duplicates request policy.
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result));
}

size_t bearer_request_workspace_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text))
		return(std::numeric_limits<size_t>::max());
	enum class RequestOperation {
		request_ob_start = 0, request_ob_close = 1, request_ob_get = 2, request_ob_get_close = 3,
		request_response_status = 4, request_redirect = 5, request_session_start = 6, request_session_set = 7,
		request_session_destroy = 8, request_session_id_create = 9, request_csrf_token = 10, request_csrf_valid = 11,
		request_csrf_rotate = 12, request_csrf_field = 13, request_request_route_from_raw_path = 20,
		request_request_perf = 21, request_response_cookie = 22
	};
	switch(static_cast<RequestOperation>(operation))
	{
		case RequestOperation::request_ob_start: ob_start(); result.clear(); break;
		case RequestOperation::request_ob_close: ob_close(); result.clear(); break;
		case RequestOperation::request_ob_get: result.set(ob_get()); break;
		case RequestOperation::request_ob_get_close: result.set(ob_get_close()); break;
		case RequestOperation::request_response_status: if(result.to_s64() < 100 || result.to_s64() > 999) return(std::numeric_limits<size_t>::max()); context->set_status((s32)result.to_s64(), text); result.clear(); break;
		case RequestOperation::request_redirect: redirect(result.to_string(), (s32)supplied.to_s64(302)); result.clear(); break;
		case RequestOperation::request_session_start: session_start(result.to_string()); result = bearer_session_snapshot(context); break;
		case RequestOperation::request_session_set:
				if(text == "set") context->session[result.to_string()] = supplied.to_string();
				else if(text == "remove") context->session.erase(result.to_string());
				else return(std::numeric_limits<size_t>::max());
				result = bearer_session_snapshot(context); break;
		case RequestOperation::request_session_destroy: session_destroy(result.to_string()); result = bearer_session_snapshot(context); break;
		case RequestOperation::request_session_id_create: result.set(session_id_create()); break;
		case RequestOperation::request_csrf_token: result.set(csrf_token(result.to_string(), text)); break;
		case RequestOperation::request_csrf_valid: result.set_bool(csrf_valid(result.to_string(), text, supplied.to_string())); break;
		case RequestOperation::request_csrf_rotate: csrf_rotate(result.to_string(), text); result.clear(); break;
		case RequestOperation::request_csrf_field: result.set(csrf_field(result.to_string(), text, supplied.to_string())); break;
		case RequestOperation::request_request_route_from_raw_path: result = request_route_from_raw_path(result.to_string(), text); break;
		case RequestOperation::request_request_perf: result = request_perf(); break;
		case RequestOperation::request_response_cookie: set_cookie(result.to_string(), supplied.to_string()); result.clear(); break;
			// Cache and process APIs retain sys.cpp as the semantic authority; only
			// copied values cross from Capy at this boundary.
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result));
}

static String bearer_cache_normalize_key(String key)
{
	for(char& c : key)
	{
		if(isspace((unsigned char)c))
			c = '_';
	}
	return(key);
}

static String bearer_cache_normalize_command(String command)
{
	size_t end = command.find("\r\n");
	String header = command.substr(0, end), payload = end == String::npos ? "" : command.substr(end);
	std::vector<String> parts = split_strings(header, " ");
	if(parts.size() < 2)
		return(command);
	String verb = to_lower(parts[0]);
	if(verb == "get" || verb == "gets")
	{
		for(size_t i = 1; i < parts.size(); i++)
			parts[i] = bearer_cache_normalize_key(parts[i]);
	}
	else if(verb == "gat" || verb == "gats")
	{
		if(parts.size() > 2)
			parts[2] = bearer_cache_normalize_key(parts[2]);
	}
	else if(verb == "set" || verb == "add" || verb == "replace" || verb == "append" || verb == "prepend" || verb == "cas" || verb == "delete" || verb == "incr" || verb == "decr" || verb == "touch")
		parts[1] = bearer_cache_normalize_key(parts[1]);
	return(join_strings(parts, " ") + payload);
}

size_t bearer_cache_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text))
		return(std::numeric_limits<size_t>::max());
	enum class CacheOperation { cache_memcache_connect = 0, cache_memcache_command = 1, cache_memcache_set = 2, cache_memcache_delete = 3, cache_memcache_get = 4, cache_memcache_get_multiple = 5 };
	switch(static_cast<CacheOperation>(operation))
	{
		case CacheOperation::cache_memcache_connect: result.set(std::to_string(memcache_connect(result.to_string(), (u16)supplied.to_u64(11211)))); break;
		case CacheOperation::cache_memcache_command: result.set(memcache_command(result.to_u64(), bearer_cache_normalize_command(text))); break;
		case CacheOperation::cache_memcache_set: result.set_bool(memcache_set(result.to_u64(), bearer_cache_normalize_key(text), supplied["value"].to_string(), supplied["expires"].to_u64(60 * 60))); break;
		case CacheOperation::cache_memcache_delete: result.set_bool(memcache_delete(result.to_u64(), bearer_cache_normalize_key(text))); break;
		case CacheOperation::cache_memcache_get: result.set(memcache_get(result.to_u64(), bearer_cache_normalize_key(text), supplied.to_string())); break;
		case CacheOperation::cache_memcache_get_multiple: result = memcache_get_multiple(result.to_u64(), bearer_memcache_normalize_keys(supplied)); break;
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result));
}

static String bearer_shell_exec_flags_error(const DValue& flags)
{
	const DValue& values = flags.deref();
	if(values.type != 'M' || values.is_list())
		return("shell_exec: flags must be a map. Valid keys: stdin, env, timeout_ms, background");
	String invalid;
	values.each([&](const DValue&, String key) {
		if(invalid == "" && key != "stdin" && key != "env" && key != "timeout_ms" && key != "background")
			invalid = key;
	});
	if(invalid != "")
		return("shell_exec: unknown key '" + invalid + "'. Valid keys: stdin, env, timeout_ms, background");
	if(const DValue* value = values.key("background"); value && value->deref().type != 'B')
		return("shell_exec: background must be a bool");
	if(const DValue* value = values.key("timeout_ms"); value && (value->deref().type != 'F' || !std::isfinite(value->deref()._float) || value->deref()._float <= 0))
		return("shell_exec: timeout_ms must be a positive number");
	if(const DValue* value = values.key("stdin"); value && value->deref().type != 'S')
		return("shell_exec: stdin must be a string");
	if(const DValue* env = values.key("env"); env)
	{
		const DValue& entries = env->deref();
		if(entries.type != 'M' || entries.is_list())
			return("shell_exec: env must be a map with string values");
		bool valid = true;
		entries.each([&](const DValue& value, String) { if(value.deref().type != 'S') valid = false; });
		if(!valid)
			return("shell_exec: env must be a map with string values");
	}
	return("");
}

size_t bearer_process_jobs_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text))
		return(std::numeric_limits<size_t>::max());
	enum class ProcessOperation { process_shell_escape = 0, process_shell_exec = 1, process_shell_exec_flags = 2, process_job_status = 3, process_job_result = 4, process_job_await = 5, process_job_cancel = 6, process_process_start_directory = 7, process_server_start_http = 8, process_server_stop = 9, process_task_submit = 10, process_task_status = 11, process_task_await = 12, process_task_cancel = 13 };
	switch(static_cast<ProcessOperation>(operation))
	{
		case ProcessOperation::process_shell_escape: result.set(shell_escape(result.to_string())); break;
		case ProcessOperation::process_shell_exec: result.set(shell_exec(result.to_string())); break;
		case ProcessOperation::process_shell_exec_flags: {
			String error = bearer_shell_exec_flags_error(supplied);
			if(error != "")
			{
				bearer_hard_error(error.data(), error.size());
				return(std::numeric_limits<size_t>::max());
			}
			result = shell_exec(result.to_string(), supplied);
			break;
		}
		case ProcessOperation::process_job_status: result = job_status(to_u64(result.to_string(), 0)); break;
		case ProcessOperation::process_job_result: result = job_result(to_u64(result.to_string(), 0)); break;
		case ProcessOperation::process_job_await: result = job_await(to_u64(result.to_string(), 0), supplied.to_u64()); break;
		case ProcessOperation::process_job_cancel: result.set_bool(job_cancel(to_u64(result.to_string(), 0))); break;
		case ProcessOperation::process_process_start_directory: result.set(process_start_directory()); break;
		case ProcessOperation::process_server_start_http: result.set((f64)server_start_http(result.to_string(), text, supplied["file"].to_string(), supplied["function"].to_string())); break;
		case ProcessOperation::process_server_stop: result.set_bool(server_stop(result.to_string())); break;
		case ProcessOperation::process_task_submit: result.set(task(result.to_string(), supplied)); break;
		case ProcessOperation::process_task_status: result = task_status(result.to_string()); break;
		case ProcessOperation::process_task_await: result = task_await(result.to_string(), supplied.to_u64()); break;
		case ProcessOperation::process_task_cancel: result = task_cancel(result.to_string()); break;
			// Network handles are stringified before BRRB so exact u64 values never
			// transit f64. The worker owns and closes the underlying descriptors.
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result));
}

size_t bearer_network_http_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text))
		return(std::numeric_limits<size_t>::max());
	enum class NetworkOperation { network_socket_connect = 0, network_socket_close = 1, network_socket_write = 2, network_socket_read = 3, network_http_request = 4, network_http_request_async = 5 };
	switch(static_cast<NetworkOperation>(operation))
	{
		case NetworkOperation::network_socket_connect: { s64 port = supplied.to_s64(); if(port < 0 || port > 65535) return(std::numeric_limits<size_t>::max()); result.set(std::to_string(socket_connect(result.to_string(), (u16)port))); break; }
		case NetworkOperation::network_socket_close: socket_close(to_u64(result.to_string(), 0)); result.clear(); break;
		case NetworkOperation::network_socket_write: result.set_bool(socket_write(to_u64(result.to_string(), 0), text)); break;
		case NetworkOperation::network_socket_read: { s64 max_length = supplied["max_length"].to_s64(); s64 timeout = supplied["timeout"].to_s64(); if(max_length < 0 || timeout < 0) return(std::numeric_limits<size_t>::max()); result.set(socket_read(to_u64(result.to_string(), 0), (u32)max_length, (u32)timeout)); break; }
		case NetworkOperation::network_http_request: result = http_request(result); break;
		case NetworkOperation::network_http_request_async: result.set(std::to_string(http_request_async(result))); break;
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result));
}

size_t bearer_time_format_brrb(s32 operation, const char* value, size_t value_len, const char* key, size_t key_len,
	const char* argument, size_t argument_len, char* out, size_t cap)
{
	if(out)
		return(bearer_brrb_call_copy(out, cap));
	DValue result, supplied;
	String text;
	if(!bearer_brrb_call_decode(value, value_len, key, key_len, argument, argument_len, result, supplied, text))
		return(std::numeric_limits<size_t>::max());
	enum class TimeOperation { time_time_parse = 0, time_time_format_local = 1, time_time_format_utc = 2, time_time_format_relative = 3 };
	switch(static_cast<TimeOperation>(operation))
	{
		case TimeOperation::time_time_parse: result.set(std::to_string(time_parse(result.to_string()))); break;
		case TimeOperation::time_time_format_local: result.set(time_format_local(result.to_string(), to_u64(supplied.to_string(), 0))); break;
		case TimeOperation::time_time_format_utc: result.set(time_format_utc(result.to_string(), to_u64(supplied.to_string(), 0))); break;
		case TimeOperation::time_time_format_relative: result.set(time_format_relative(to_u64(result.to_string(), 0), text, to_u64(supplied["medium_seconds"].to_string(), 0), supplied["medium_recent"].to_string(), to_u64(supplied["not_recent_seconds"].to_string(), 0), supplied["not_recent"].to_string())); break;
		default: return(std::numeric_limits<size_t>::max());
	}
	return(bearer_brrb_call_finish(result));
}


size_t bearer_dv_none_brrb(char* out, size_t cap)
{
	DValue value;
	value.set_none();
	return(bearer_copy_bytes(brb_encode_local(value), out, cap));
}

size_t bearer_dv_callable_brrb(s32 closure, s32 type, char* out, size_t cap)
{
	DValue value;
	value.set_type('C');
	value._ptr = (void*)(uintptr_t)(u32)closure;
	value._array_index = type;
	return(bearer_copy_bytes(brb_encode_local(value), out, cap));
}

static bool bearer_dv_callable_valid(const DValue& value)
{
	const DValue& node = value.deref();
	if(node.type != 'C')
		return(false);
	u32 pointer = (u32)(uintptr_t)node._ptr;
	size_t memory_size = (size_t)__builtin_wasm_memory_size(0) << 16;
	if(pointer == 0 || (pointer & 3) != 0 || pointer > memory_size || memory_size - pointer < 24)
		return(false);
	const u32* header = (const u32*)(uintptr_t)pointer;
	u32 references = header[0], kind = header[1], allocation_type = header[2], allocation_size = header[3];
	if(allocation_size < 24 || allocation_size > memory_size - pointer || header[5] != (u32)node._array_index)
		return(false);
	if(references == std::numeric_limits<u32>::max())
		return(kind == std::numeric_limits<u32>::max() && allocation_type == 0x3fffffffu && allocation_size == 24);
	return(references != 0 && kind == 1 && allocation_type >= 0x40000000u);
}

s32 bearer_dv_callable_extract_brrb(const char* value, size_t value_len, s32 type)
{
	DValue decoded;
	if(!bearer_decode_local_brrb_span(value, value_len, decoded) || decoded.deref().type != 'C' || decoded._array_index != type || !bearer_dv_callable_valid(decoded))
		return(0);
	return((s32)(uintptr_t)decoded._ptr);
}

static void bearer_dv_callable_each(const DValue& value, const std::function<void(s32)>& visit)
{
	const DValue& node = value.deref();
	if(node.type == 'C' && bearer_dv_callable_valid(node)) visit((s32)(uintptr_t)node._ptr);
	else if(node.type == 'M') for(const auto& entry : node._map) bearer_dv_callable_each(entry.second, visit);
}

s32 bearer_dv_callable_at_brrb(const char* value, size_t value_len, s32 ordinal)
{
	DValue decoded;
	if(ordinal < 0 || !bearer_decode_local_brrb_span(value, value_len, decoded)) return(0);
	s32 position = 0, closure = 0;
	bearer_dv_callable_each(decoded, [&](s32 value) { if(position++ == ordinal) closure = value; });
	return(closure);
}

static const DValue* bearer_dv_lookup(const DValue& value, s32 index_mode, const char* key, size_t key_len, s32 index)
{
	if(!value.is_array() || (index_mode && index < 0))
		return(0);
	String lookup = index_mode ? std::to_string(index) : String(key ? key : "", key ? key_len : 0);
	return(value.key(lookup));
}

s32 bearer_dv_read_brrb(const char* value, size_t value_len, s32 index_mode,
	const char* key, size_t key_len, s32 index, char* out, size_t cap)
{
	if(out)
		return((s32)bearer_copy_staged(wasm_dval_read_result, out, cap));
	wasm_dval_read_result.clear();
	DValue decoded;
	if(!bearer_decode_local_brrb_span(value, value_len, decoded))
		return(-2);
	const DValue* child = bearer_dv_lookup(decoded, index_mode, key, key_len, index);
	if(child)
		wasm_dval_read_result = brb_encode_local(*child);
	else
	{
		DValue none;
		none.set_none();
		wasm_dval_read_result = brb_encode_local(none);
	}
	return((s32)wasm_dval_read_result.size());
}

s32 bearer_dv_is_none_brrb(const char* value, size_t value_len)
{
	DValue decoded;
	if(!bearer_decode_local_brrb_span(value, value_len, decoded))
		return(-1);
	return(decoded.is_none() ? 1 : 0);
}

static bool bearer_dv_path_key(const DValue& segment, String& key)
{
	const DValue& value = segment.deref();
	if(value.type == 'S')
	{
		key = value._String;
		return(true);
	}
	if(value.type != 'F' || !std::isfinite(value._float) || value._float < (f64)std::numeric_limits<s32>::min() ||
		value._float > (f64)std::numeric_limits<s32>::max() || std::trunc(value._float) != value._float)
		return(false);
	key = std::to_string((s32)value._float);
	return(true);
}

size_t bearer_dv_require_brrb(const char* value, size_t value_len, const char* selector, size_t selector_len, char* out, size_t cap)
{
	if(out)
		return(bearer_copy_staged(wasm_dval_require_result, out, cap));
	wasm_dval_require_result.clear();
	DValue decoded, segment;
	String key;
	if(!bearer_decode_local_brrb_span(value, value_len, decoded) || !bearer_decode_local_brrb_span(selector, selector_len, segment) ||
		!decoded.is_array() || !bearer_dv_path_key(segment, key) ||
		(decoded.is_list() && segment.deref().type == 'F' && segment.deref()._float < 0))
		return(std::numeric_limits<size_t>::max());
	const DValue* child = decoded.key(key);
	if(!child)
		return(std::numeric_limits<size_t>::max());
	wasm_dval_require_result = brb_encode_local(*child);
	return(wasm_dval_require_result.size());
}

size_t bearer_dv_set_path_brrb(const char* root, size_t root_len, const char* path, size_t path_len,
	const char* replacement, size_t replacement_len, char* out, size_t cap)
{
	if(out)
		return(bearer_copy_staged(wasm_dval_path_result, out, cap));
	wasm_dval_path_result.clear();
	DValue result, selectors, supplied;
	if(!bearer_decode_local_brrb_span(root, root_len, result) || !bearer_decode_local_brrb_span(path, path_len, selectors) ||
		!bearer_decode_local_brrb_span(replacement, replacement_len, supplied) || !selectors.is_list() || selectors._map.empty())
		return(std::numeric_limits<size_t>::max());
	DValue* current = &result;
	for(size_t position = 0; position < selectors._map.size(); position++)
	{
		const DValue* segment = selectors.key(std::to_string(position));
		String key;
		if(!segment || !bearer_dv_path_key(*segment, key))
			return(std::numeric_limits<size_t>::max());
		bool last = position + 1 == selectors._map.size();
		if(current->type != 'M')
			current->set_type('M');
		if(current->is_list())
		{
			const DValue& selector = segment->deref();
			if(selector.type != 'F' || selector._float < 0)
				return(std::numeric_limits<size_t>::max());
			auto child = current->_map.find(key);
			if(child == current->_map.end())
				return(std::numeric_limits<size_t>::max());
			if(last)
			{
				child->second = supplied;
				break;
			}
			current = &child->second;
			continue;
		}
		auto child = current->_map.find(key);
		if(last)
		{
			if(child == current->_map.end())
				current->_map[key] = supplied;
			else
				child->second = supplied;
			break;
		}
		if(child == current->_map.end())
		{
			DValue map;
			map.set_type('M');
			child = current->_map.emplace(key, std::move(map)).first;
		}
		current = &child->second;
	}
	wasm_dval_path_result = brb_encode_local(result);
	return(wasm_dval_path_result.size());
}

s32 bearer_dv_get_brrb(const char* value, size_t value_len, s32 index_mode,
	const char* key, size_t key_len, s32 index, char* out, size_t cap)
{
	DValue decoded;
	if(!bearer_decode_local_brrb_span(value, value_len, decoded) || !decoded.is_array())
		return(-2);
	String lookup = index_mode ? std::to_string(index) : String(key ? key : "", key ? key_len : 0);
	const DValue* child = decoded.key(lookup);
	if(!child)
		return(-1);
	String encoded = brb_encode_local(*child);
	bearer_copy_bytes(encoded, out, cap);
	return((s32)encoded.size());
}

s32 bearer_dv_count_brrb(const char* value, size_t value_len)
{
	DValue decoded;
	if(!bearer_decode_local_brrb_span(value, value_len, decoded) || !decoded.is_array())
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
	if(!bearer_decode_local_brrb_span(value, value_len, decoded) || !decoded.is_array() || !bearer_dv_entry_at(decoded, ordinal, key, child))
		return(-1);
	bearer_copy_bytes(key, out, cap);
	return((s32)key.size());
}

s32 bearer_dv_entry_value_brrb(const char* value, size_t value_len, size_t ordinal, char* out, size_t cap)
{
	DValue decoded;
	String key;
	const DValue* child = 0;
	if(!bearer_decode_local_brrb_span(value, value_len, decoded) || !decoded.is_array() || !bearer_dv_entry_at(decoded, ordinal, key, child))
		return(-1);
	String encoded = brb_encode_local(*child);
	bearer_copy_bytes(encoded, out, cap);
	return((s32)encoded.size());
}

static bool bearer_dv_scalar(const DValue& value, const DValue*& scalar)
{
	scalar = &value.deref();
	return(scalar->type == 'S' || scalar->type == 'F' || scalar->type == 'B');
}

static bool bearer_dv_decimal_s32(const String& text, s32& result)
{
	if(text.empty()) return(false);
	const char* begin = text.data();
	const char* end = begin + text.size();
	if(*begin == '+') ++begin;
	if(begin == end) return(false);
	auto parsed = std::from_chars(begin, end, result, 10);
	return(parsed.ec == std::errc() && parsed.ptr == end);
}

static bool bearer_dv_decimal_s64(const String& text, s64& result)
{
	if(text.empty()) return(false);
	const char* begin = text.data();
	const char* end = begin + text.size();
	if(*begin == '+') ++begin;
	if(begin == end) return(false);
	auto parsed = std::from_chars(begin, end, result, 10);
	return(parsed.ec == std::errc() && parsed.ptr == end);
}

static bool bearer_dv_decimal_u64(const String& text, u64& result)
{
	if(text.empty()) return(false);
	const char* begin = text.data();
	const char* end = begin + text.size();
	if(*begin == '-') return(false);
	if(*begin == '+') ++begin;
	if(begin == end) return(false);
	auto parsed = std::from_chars(begin, end, result, 10);
	return(parsed.ec == std::errc() && parsed.ptr == end);
}

static String bearer_dv_extract_string_value(const char* value, size_t value_len, const char* fallback, size_t fallback_len)
{
	DValue decoded;
	const DValue* scalar = 0;
	const String otherwise(fallback ? fallback : "", fallback ? fallback_len : 0);
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !bearer_dv_scalar(decoded, scalar)) return(otherwise);
	if(scalar->type == 'S') return(scalar->_String.empty() ? otherwise : scalar->_String);
	if(scalar->type == 'F') return(std::isfinite(scalar->_float) ? bearer_format_f64_value(scalar->_float) : otherwise);
	return(scalar->_bool ? "true" : "false");
}

size_t bearer_dv_extract_string(const char* value, size_t value_len, const char* fallback, size_t fallback_len, char* out, size_t cap)
{
	if(!out)
	{
		wasm_dval_extract_string_result = bearer_dv_extract_string_value(value, value_len, fallback, fallback_len);
		if(wasm_dval_extract_string_result.size() > (size_t)std::numeric_limits<s32>::max() - 20)
		{
			wasm_dval_extract_string_result.clear();
			return(std::numeric_limits<size_t>::max());
		}
		return(wasm_dval_extract_string_result.size());
	}
	return(bearer_copy_staged(wasm_dval_extract_string_result, out, cap));
}

s32 bearer_dv_extract_bool(const char* value, size_t value_len, s32 fallback)
{
	DValue decoded;
	const DValue* scalar = 0;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !bearer_dv_scalar(decoded, scalar)) return(fallback);
	if(scalar->type == 'B') return(scalar->_bool ? 1 : 0);
	if(scalar->type == 'F') return(std::isfinite(scalar->_float) ? (scalar->_float == 0 ? 0 : 1) : fallback);
	// Exact wide integers ride in the string slot (see bearer_dv_extract_u64), so a
	// u64-backed dval must test numerically before falling back to boolean words.
	// Without this, bool(dval(u64(7))) answered false for every nonzero handle.
	if(scalar->type == 'S')
	{
		s64 signed_value = 0;
		if(bearer_dv_decimal_s64(scalar->_String, signed_value)) return(signed_value != 0 ? 1 : 0);
		u64 unsigned_value = 0;
		if(bearer_dv_decimal_u64(scalar->_String, unsigned_value)) return(unsigned_value != 0 ? 1 : 0);
	}
	return(to_bool(scalar->_String, fallback != 0) ? 1 : 0);
}

s32 bearer_dv_extract_s32(const char* value, size_t value_len, s32 fallback)
{
	DValue decoded;
	const DValue* scalar = 0;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !bearer_dv_scalar(decoded, scalar)) return(fallback);
	if(scalar->type == 'B') return(scalar->_bool ? 1 : 0);
	if(scalar->type == 'S') { s32 result = 0; return(bearer_dv_decimal_s32(scalar->_String, result) ? result : fallback); }
	if(!std::isfinite(scalar->_float) || scalar->_float < (f64)std::numeric_limits<s32>::min() || scalar->_float > (f64)std::numeric_limits<s32>::max()) return(fallback);
	return((s32)std::trunc(scalar->_float));
}

s64 bearer_dv_extract_s64(const char* value, size_t value_len, s64 fallback)
{
	DValue decoded;
	const DValue* scalar = 0;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !bearer_dv_scalar(decoded, scalar)) return(fallback);
	if(scalar->type == 'B') return(scalar->_bool ? 1 : 0);
	if(scalar->type == 'S') { s64 result = 0; return(bearer_dv_decimal_s64(scalar->_String, result) ? result : fallback); }
	if(!std::isfinite(scalar->_float) || scalar->_float < -9223372036854775808.0 || scalar->_float >= 9223372036854775808.0) return(fallback);
	return((s64)std::trunc(scalar->_float));
}

u64 bearer_dv_extract_u64(const char* value, size_t value_len, u64 fallback)
{
	DValue decoded;
	const DValue* scalar = 0;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !bearer_dv_scalar(decoded, scalar)) return(fallback);
	if(scalar->type == 'B') return(scalar->_bool ? 1 : 0);
	if(scalar->type == 'S') { u64 result = 0; return(bearer_dv_decimal_u64(scalar->_String, result) ? result : fallback); }
	if(!std::isfinite(scalar->_float) || scalar->_float < 0 || scalar->_float >= 18446744073709551616.0) return(fallback);
	return((u64)std::trunc(scalar->_float));
}

s32 bearer_dv_validate_radix(const char* value, size_t value_len, const char* name, size_t name_len)
{
	DValue decoded;
	const DValue* scalar = 0;
	const String constructor(name ? name : "", name ? name_len : 0);
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !bearer_dv_scalar(decoded, scalar) || scalar->type != 'F' ||
		!std::isfinite(scalar->_float) || std::trunc(scalar->_float) != scalar->_float || scalar->_float < 2 || scalar->_float > 36)
	{
		const String message = constructor + ": base must be a whole number from 2 to 36";
		bearer_hard_error(message.data(), message.size());
		return(10);
	}
	return((s32)scalar->_float);
}

static String bearer_dv_parse_text(const char* value, size_t value_len, s32 base)
{
	if(base < 2 || base > 36) return("");
	String result = trim(String(value ? value : "", value ? value_len : 0));
	if(!result.empty() && result[0] == '+') result.erase(0, 1);
	return(result);
}

s32 bearer_dv_parse_s32(const char* value, size_t value_len, s32 base, s32 fallback)
{
	const String text = bearer_dv_parse_text(value, value_len, base);
	s32 result = 0;
	auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
	return(!text.empty() && parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() ? result : fallback);
}

s64 bearer_dv_parse_s64(const char* value, size_t value_len, s32 base, s64 fallback)
{
	const String text = bearer_dv_parse_text(value, value_len, base);
	s64 result = 0;
	auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
	return(!text.empty() && parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() ? result : fallback);
}

u64 bearer_dv_parse_u64(const char* value, size_t value_len, s32 base, u64 fallback)
{
	const String text = bearer_dv_parse_text(value, value_len, base);
	if(text.empty() || text[0] == '-') return(fallback);
	u64 result = 0;
	auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
	return(parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() ? result : fallback);
}

f64 bearer_dv_extract_f64(const char* value, size_t value_len, f64 fallback)
{
	DValue decoded;
	const DValue* scalar = 0;
	if(!bearer_decode_brrb_span(value, value_len, decoded) || !bearer_dv_scalar(decoded, scalar)) return(fallback);
	if(scalar->type == 'B') return(scalar->_bool ? 1.0 : 0.0);
	if(scalar->type == 'F') return(std::isfinite(scalar->_float) ? scalar->_float : fallback);
	if(scalar->_String.empty()) return(fallback);
	f64 result = 0;
	const char* begin = scalar->_String.data();
	const char* end = begin + scalar->_String.size();
	auto parsed = std::from_chars(begin, end, result, std::chars_format::general);
	return(parsed.ec == std::errc() && parsed.ptr == end && std::isfinite(result) ? result : fallback);
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

s32 bearer_unit_load(const char* target, size_t target_len)
{
	String current = context ? context->resources.current_unit_file : "";
	return(bearer_host_unit_load(target, target_len, current.data(), current.size()));
}

size_t bearer_module_call_brrb(s32 capability, const char* name, size_t name_len,
	const char* input, size_t input_len, char* out, size_t cap)
{
	if(!out)
	{
		s32 staged = bearer_host_module_staged_size(capability, name, name_len, input, input_len);
		if(staged >= 0)
			return((size_t)staged);
		DValue call_value;
		String error;
		if(!brb_decode(String(input ? input : "", input ? input_len : 0), call_value, &error))
			return(std::numeric_limits<size_t>::max());
		s32 slot = bearer_host_module_resolve(capability, name, name_len);
		if(slot <= 0)
			return(std::numeric_limits<size_t>::max());
		WasmDValueCallHandler call = (WasmDValueCallHandler)(uintptr_t)slot;
		bearer_host_module_enter(capability);
		DValue* result = call(&call_value);
		bearer_host_module_leave();
		String encoded = brb_encode(result ? *result : DValue());
		if(bearer_host_module_stage(capability, name, name_len, input, input_len,
			encoded.data(), encoded.size()) < 0)
			return(std::numeric_limits<size_t>::max());
		return(encoded.size());
	}
	s32 copied = bearer_host_module_copy(capability, name, name_len, input, input_len, out, cap);
	return(copied < 0 ? std::numeric_limits<size_t>::max() : (size_t)copied);
}

size_t bearer_unit_call_brrb(const char* target, size_t target_len,
	const char* function_name, size_t function_len,
	const char* input, size_t input_len, char* out, size_t cap)
{
	String name(function_name ? function_name : "", function_name ? function_len : 0);
	if(wasm_unit_call_handler(name) == "")
	{
		String default_input;
		if(!input_len)
			default_input = brb_encode(DValue());
		s32 capability = bearer_unit_load(target, target_len);
		return(bearer_module_call_brrb(capability, name.data(), name.size(),
			input_len ? input : default_input.data(), input_len ? input_len : default_input.size(), out, cap));
	}
	if(out == 0)
	{
		wasm_unit_call_encoded_result.clear();
		DValue call_value;
		String error;
		if(input_len && !brb_decode(String(input ? input : "", input ? input_len : 0), call_value, &error))
			return(0);
		DValue* result = unit_call(
			String(target ? target : "", target ? target_len : 0), name,
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
	wasm_output = wasm_request.ob_stack.empty() ? String("") : std::move(*wasm_request.ob_stack[0]).str();
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
