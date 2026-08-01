#include "../3rdparty/mysql/mysql.h"
#include <stdio.h>
#include <stdlib.h>
#include <climits>
#include "mysql-connector.h"

// Same-target mysql_connect() calls lease one request-local connection.
// mysql_disconnect() releases a lease; cleanup_mysql_connections() owns the
// actual close at request end, including exception/fatal recovery paths.
static void mysql_register_request_connection(MySQL* db)
{
	if(!context || !db || db->worker_persistent)
		return;
	auto& connections = context->resources.mysql_connections;
	if(std::find(connections.begin(), connections.end(), (void*)db) == connections.end())
		connections.push_back(db);
}

bool MySQL::connect(String host, String username, String password, String database)
{
	request_host = host;
	request_username = username;
	request_password = password;
	request_database = database;
	// Register regardless of outcome: tracking is about the wrapper's
	// lifetime, not the connection's. disconnect()/~MySQL() unregister.
	mysql_register_request_connection(this);
	//switch_to_system_alloc();
	connection = mysql_init(NULL);
	if (connection == NULL)
	{
		fprintf(stderr, "mysql_init failed\n");
		//switch_to_arena(context->mem);
		_preload_next_error_code = CR_OUT_OF_MEMORY;
		statement_info = "mysql_init failed";
		return(false);
	}

	if (mysql_real_connect((MYSQL*)connection, host.c_str(), username.c_str(), password.c_str(),
	  database == "" ? NULL : database.c_str(), 0, NULL, 0) == NULL)
	{
		auto e = mysql_error((MYSQL*)connection);
		fprintf(stderr, "%s\n", e);
		_preload_next_error_code = CR_UNKNOWN_ERROR;
		statement_info.assign(e);
		mysql_close((MYSQL*)connection);
		connection = NULL;
		//switch_to_arena(context->mem);
		return(false);
	}

	/*
	if (mysql_query(con, "CREATE DATABASE testdb"))
	{
		fprintf(stderr, "%s\n", mysql_error(con));
		mysql_close(con);
		exit(1);
	}
	*/
	//switch_to_arena(context->mem);
	statement_info = String("connected");
	return(true);
}

bool MySQL::reset_connection()
{
	if(!connection)
		return(false);
	MYSQL* mysql = (MYSQL*)connection;
	String selected_database = mysql->db == NULL ? "" : mysql->db;
	// RESET CONNECTION deliberately preserves the selected database. An
	// unqualified lease cannot safely inherit one selected by its prior request.
	if(request_database == "" && selected_database != "")
		return(false);
	if(mysql_reset_connection(mysql) != 0)
		return(false);
	if(request_database != "" && selected_database != request_database && mysql_select_db(mysql, request_database.c_str()) != 0)
		return(false);
	_preload_next_error_code = 0;
	affected_rows = 0;
	field_count = 0;
	row_count = 0;
	insert_id = 0;
	statement_info = "reset";
	field_info.clear();
	request_leases = 0;
	return(true);
}

String MySQL::escape(String raw, char quote_char)
{
	if(!connection)
		return(mysql_escape(raw, quote_char));
	if(raw.size() > ULONG_MAX / 2)
	{
		_preload_next_error_code = CR_UNKNOWN_ERROR;
		parameter_error = true;
		statement_info = "mysql parameter is too large";
		return("");
	}
	char quote = quote_char > 0 ? quote_char : '\'';
	auto escape_without_backslashes = [&]() {
		if(raw.find('\0') != String::npos)
		{
			_preload_next_error_code = CR_UNKNOWN_ERROR;
			parameter_error = true;
			statement_info = "mysql text parameters cannot contain NUL when NO_BACKSLASH_ESCAPES is active";
			return(String(""));
		}
		String escaped;
		escaped.reserve(raw.size() * 2);
		for(char value : raw)
		{
			escaped.push_back(value);
			if(value == quote) escaped.push_back(value);
		}
		return(quote_char > 0 ? String(1, quote_char) + escaped + String(1, quote_char) : escaped);
	};
	if(((MYSQL*)connection)->server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES)
		return(escape_without_backslashes());
	String escaped(raw.size() * 2 + 1, '\0');
	unsigned long length = mysql_real_escape_string_quote((MYSQL*)connection, escaped.data(), raw.data(), (unsigned long)raw.size(), quote);
	if(length == (unsigned long)-1)
	{
		unsigned int escape_error = mysql_errno((MYSQL*)connection);
		if(escape_error == 0)
			return(escape_without_backslashes());
		_preload_next_error_code = escape_error;
		parameter_error = true;
		statement_info = mysql_error((MYSQL*)connection);
		return("");
	}
	escaped.resize(length);
	return(quote_char > 0 ? String(1, quote_char) + escaped + String(1, quote_char) : escaped);
}

String mysql_escape(String raw, char quote_char)
{
	String result;
	if(quote_char > 0)
		result.append(1, quote_char);

	for(u32 i = 0; i < raw.length(); i++)
	{
		char c = raw[i];
		switch(c)
		{
			case('\n'):
				result.append("\\n");
				break;
			case('\r'):
				result.append("\\r");
				break;
			case('\t'):
				result.append("\\t");
				break;
			case('\\'):
			case('\''):
			case('"'):
				result.append(1, '\\');
				result.append(1, c);
				break;
			default:
				result.append(1, c);
				break;
		}
	}

	if(quote_char > 0)
		result.append(1, quote_char);
	return(result);
}

DValue field_to_dv_node(char* data_ptr, MySQLFieldInfo field_info, u32 len)
{
	DValue result;
	if(data_ptr == 0)
		return(result);
	String raw(data_ptr, len);
	switch(field_info.type)
	{
		case(MYSQL_TYPE_TINY):
			result.set(atoll(raw.c_str()));
			break;
		case(MYSQL_TYPE_SHORT):
			result.set(atoll(raw.c_str()));
			break;
		case(MYSQL_TYPE_LONG):
			result.set(atoll(raw.c_str()));
			break;
		case(MYSQL_TYPE_INT24):
			result.set(atoll(raw.c_str()));
			break;
		case(MYSQL_TYPE_LONGLONG):
			result.set(atoll(raw.c_str()));
			break;
		case(MYSQL_TYPE_FLOAT):
			result.set(atof(raw.c_str()));
			break;
		case(MYSQL_TYPE_DOUBLE):
			result.set(atof(raw.c_str()));
			break;
		case(MYSQL_TYPE_NULL):
			break;
		default:
			result.set(raw);
			break;
	}
	return(result);
}

DValue MySQL::get_pending_result()
{
	DValue result_data;
	// based on: https://dev.mysql.com/doc/c-api/5.7/en/mysql-field-count.html
	MYSQL_RES *result;
	result = mysql_store_result((MYSQL*)connection);
	insert_id = mysql_insert_id((MYSQL*)connection);
	//statement_info.assign(mysql_info((MYSQL*)connection));
    if (result)  // there are rows
    {
        field_count = mysql_num_fields(result);
        row_count = mysql_num_rows(result);

		field_info.clear();
		unsigned int i;
		MYSQL_FIELD *fields;
		fields = mysql_fetch_fields(result);
		for(i = 0; i < field_count; i++)
		{
			MySQLFieldInfo fi;
			if(fields[i].name) fi.name.assign(fields[i].name);
			if(fields[i].table) fi.table.assign(fields[i].table);
			if(fields[i].db) fi.db.assign(fields[i].db);
			fi.length = (fields[i].length);
			if(fields[i].def) fi.def.assign(fields[i].def);
			fi.max_length = (fields[i].max_length);
			fi.flags = (fields[i].flags);
			fi.type = (fields[i].type);
			field_info.push_back(fi);
		}

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result)))
		{
			DValue row_data;
			auto lengths = mysql_fetch_lengths(result);
			for(i = 0; i < field_count; i++)
			{
				row_data[field_info[i].name] = field_to_dv_node(row[i], field_info[i], lengths[i]);
			}
			result_data.push(row_data);
		}

        mysql_free_result(result);
    }
    else  // mysql_store_result() returned nothing; should it have?
    {
        if(mysql_field_count((MYSQL*)connection) == 0)
        {
            // query does not return data
            // (it was not a SELECT)
            affected_rows = mysql_affected_rows((MYSQL*)connection);
        }
        else // mysql_store_result() should have returned data
        {
			// error
        }
    }
    return(result_data);
}

static bool mysql_has_unquoted_positional_placeholder(String query, bool no_backslash_escapes);

DValue MySQL::query(String q)
{
	affected_rows = 0;
	bool no_backslash_escapes = connection && (((MYSQL*)connection)->server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES);
	if(mysql_has_unquoted_positional_placeholder(q, no_backslash_escapes))
	{
		_preload_next_error_code = CR_UNKNOWN_ERROR;
		statement_info = "mysql positional ? placeholders are not supported; use named :name placeholders";
		return(DValue());
	}
	if(!connection)
	{
		if(_preload_next_error_code == 0)
			_preload_next_error_code = CR_UNKNOWN_ERROR;
		if(statement_info == "")
			statement_info = "mysql connection is not open";
		return(DValue());
	}
	statement_info = "";
	_preload_next_error_code = mysql_query((MYSQL*)connection, q.c_str());
	DValue result;
	if(_preload_next_error_code == 0)
		result = get_pending_result();
	else
		statement_info = mysql_error((MYSQL*)connection);
	return(result);
}

static bool mysql_has_unquoted_positional_placeholder(String query, bool no_backslash_escapes)
{
	char quote = 0;
	bool line_comment = false, block_comment = false;
	for(size_t i = 0; i < query.size(); i++)
	{
		char c = query[i], next = i + 1 < query.size() ? query[i + 1] : 0;
		if(line_comment) { if(c == '\n' || c == '\r') line_comment = false; continue; }
		if(block_comment) { if(c == '*' && next == '/') { block_comment = false; i++; } continue; }
		if(quote)
		{
			if(!no_backslash_escapes && c == '\\') { if(next) i++; continue; }
			if(c == quote) { if(next == quote) i++; else quote = 0; }
			continue;
		}
		if(c == '\'' || c == '"' || c == '`') { quote = c; continue; }
		if(c == '#') { line_comment = true; continue; }
		if(c == '-' && next == '-' && (i + 2 >= query.size() || isspace((unsigned char)query[i + 2]))) { line_comment = true; i++; continue; }
		if(c == '/' && next == '*') { block_comment = true; i++; continue; }
		if(c == '?') return(true);
	}
	return(false);
}

DValue MySQL::query(String q, StringMap params)
{
	// Positional ? placeholders survive named substitution (ordinary values
	// are quoted by escape()), so the check in query(String) covers this path.
	parameter_error = false;
	String parsed = parse_query_parameters(q, params);
	if(parameter_error)
	{
		_preload_next_error_code = CR_UNKNOWN_ERROR;
		return(DValue());
	}
	return(query(parsed));
}

String MySQL::parse_query_parameters(String query, StringMap map)
{
	String result;
	bool no_backslash_escapes = connection && (((MYSQL*)connection)->server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES);
	char quote = 0;
	bool line_comment = false, block_comment = false;
	for(size_t i = 0; i < query.size(); i++)
	{
		char c = query[i], next = i + 1 < query.size() ? query[i + 1] : 0;
		if(line_comment)
		{
			result.push_back(c);
			if(c == '\n' || c == '\r') line_comment = false;
			continue;
		}
		if(block_comment)
		{
			result.push_back(c);
			if(c == '*' && next == '/') { result.push_back(next); i++; block_comment = false; }
			continue;
		}
		if(quote)
		{
			result.push_back(c);
			if(!no_backslash_escapes && c == '\\') { if(next) { result.push_back(next); i++; } continue; }
			if(c == quote) { if(next == quote) { result.push_back(next); i++; } else quote = 0; }
			continue;
		}
		if(c == '\'' || c == '"' || c == '`') { quote = c; result.push_back(c); continue; }
		if(c == '#') { line_comment = true; result.push_back(c); continue; }
		if(c == '-' && next == '-' && (i + 2 >= query.size() || isspace((unsigned char)query[i + 2]))) { line_comment = true; result.append("--"); i++; continue; }
		if(c == '/' && next == '*') { block_comment = true; result.append("/*"); i++; continue; }
		if(c != ':') { result.push_back(c); continue; }

		size_t end = i + 1;
		while(end < query.size() && (isalnum((unsigned char)query[end]) || query[end] == '_')) end++;
		String identifier = query.substr(i + 1, end - i - 1);
		auto parameter = map.find(identifier);
		bool unsigned_value = end < query.size() && query[end] == '!' && (end + 1 >= query.size() || query[end + 1] != '=');
		if(unsigned_value)
		{
			String value = parameter == map.end() ? String("") : parameter->second;
			bool valid = identifier != "" && value != "";
			for(char digit : value) if(!isdigit((unsigned char)digit)) valid = false;
			if(!valid)
			{
				parameter_error = true;
				statement_info = "mysql unsigned parameter :" + identifier + "! must contain only decimal digits";
				return("");
			}
			result.append(value);
			i = end;
			continue;
		}
		if(identifier == "" || parameter == map.end())
		{
			parameter_error = true;
			statement_info = "mysql named parameter :" + identifier + " is missing";
			return("");
		}
		result.append(escape(parameter->second));
		if(parameter_error) return("");
		i = end - 1;
	}
	return(result);
}

void MySQL::disconnect()
{
	if(connection)
		mysql_close((MYSQL*)connection);
	connection = NULL;
	request_leases = 0;
	if(context)
	{
		auto& connections = context->resources.mysql_connections;
		connections.erase(std::remove(connections.begin(), connections.end(), this), connections.end());
	}
}

String MySQL::error()
{
	if(_preload_next_error_code)
	{
		if(statement_info != "")
		{
			String result = statement_info;
			statement_info = "";
			_preload_next_error_code = 0;
			return(result);
		}
		String p = "Unknown error";
		switch(_preload_next_error_code)
		{
			case(CR_COMMANDS_OUT_OF_SYNC):
				p = "Commands out of sync";
				break;
			case(CR_SERVER_GONE_ERROR):
				p = "Server connection error";
				break;
			case(CR_SERVER_LOST):
				p = "Server hung up";
				break;
			case(CR_OUT_OF_MEMORY):
				p = "Out of memory";
				break;
			default:
			case(CR_UNKNOWN_ERROR):
				p = "Unknown server error";
				break;
		}
		_preload_next_error_code = 0;
		return(p);
	}
	if(!connection)
		return("");
	const char* res = mysql_error((MYSQL*)connection);
	if(res)
	{
		return(String(res));
	}
	else
	{
		return("");
	}
}

void cleanup_mysql_connections()
{
	//switch_to_system_alloc();
	if(!context)
		return;
	while(!context->resources.mysql_connections.empty())
	{
		MySQL* db = (MySQL*)context->resources.mysql_connections.back();
		context->resources.mysql_connections.pop_back();
		bool should_delete = db->request_cleanup_delete;
		db->request_pooled = false;
		db->disconnect();
		if(should_delete)
			delete db;
	}
	//switch_to_arena(context->mem);
}
