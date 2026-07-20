#include "../3rdparty/sqlite/sqlite3.h"
#include "sqlite-connector.h"

namespace {

std::map<String, SQLite*> sqlite_worker_connection_cache;

void sqlite_register_request_connection(SQLite* db)
{
	if(!context || !db)
		return;
	auto& connections = context->resources.sqlite_connections;
	if(std::find(connections.begin(), connections.end(), db) == connections.end())
		connections.push_back(db);
}

void sqlite_unregister_request_connection(SQLite* db)
{
	if(!context || !db)
		return;
	auto& connections = context->resources.sqlite_connections;
	connections.erase(std::remove(connections.begin(), connections.end(), db), connections.end());
}

void sqlite_forget_worker_cached_connection(SQLite* db)
{
	if(!db || db->path == "")
		return;
	auto found = sqlite_worker_connection_cache.find(db->path);
	if(found != sqlite_worker_connection_cache.end() && found->second == db)
		sqlite_worker_connection_cache.erase(found);
}

}

void SQLite::set_error(s32 code, String info)
{
	error_code = code;
	if(connection)
		statement_info = sqlite3_errmsg((sqlite3*)connection);
	else
		statement_info = info;
	if(info != "")
		statement_info = info + (statement_info != "" ? ": " + statement_info : "");
}

bool SQLite::apply_default_pragmas()
{
	char* err = 0;
	const char* pragmas =
		"PRAGMA foreign_keys = ON;"
		"PRAGMA journal_mode = WAL;"
		"PRAGMA synchronous = NORMAL;";
	s32 rc = sqlite3_exec((sqlite3*)connection, pragmas, 0, 0, &err);
	if(rc != SQLITE_OK)
	{
		String message;
		if(err)
		{
			message = err;
			sqlite3_free(err);
		}
		set_error(rc, "sqlite default pragmas failed" + (message != "" ? ": " + message : ""));
		return(false);
	}
	return(true);
}

bool SQLite::connect(String path)
{
	disconnect();
	this->path = path;
	// Register regardless of outcome: tracking is about the wrapper's
	// lifetime, not the connection's. disconnect()/~SQLite() unregister.
	sqlite_register_request_connection(this);
	s32 rc = sqlite3_open_v2(path.c_str(), (sqlite3**)&connection, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, 0);
	if(rc != SQLITE_OK)
	{
		set_error(rc, "sqlite open failed for " + path);
		if(connection)
		{
			sqlite3_close((sqlite3*)connection);
			connection = 0;
		}
		return(false);
	}
	sqlite3_busy_timeout((sqlite3*)connection, 5000);
	if(!apply_default_pragmas())
		return(false);
	statement_info = "connected";
	return(true);
}

void SQLite::disconnect()
{
	sqlite_forget_worker_cached_connection(this);
	if(!connection)
	{
		sqlite_unregister_request_connection(this);
		return;
	}
	sqlite3_close((sqlite3*)connection);
	connection = 0;
	sqlite_unregister_request_connection(this);
}

String SQLite::error()
{
	if(statement_info != "")
		return(statement_info);
	if(connection)
		return(sqlite3_errmsg((sqlite3*)connection));
	return("");
}

bool SQLite::bind_params(void* statement, const StringMap& params)
{
	sqlite3_stmt* stmt = (sqlite3_stmt*)statement;
	s32 count = sqlite3_bind_parameter_count(stmt);
	for(s32 i = 1; i <= count; i++)
	{
		const char* raw_name = sqlite3_bind_parameter_name(stmt, i);
		if(!raw_name || raw_name[0] == '\0')
		{
			set_error(SQLITE_MISUSE, "sqlite positional ? placeholders are not supported; use named :name placeholders");
			return(false);
		}
		String name = raw_name;
		if(name[0] != ':')
		{
			set_error(SQLITE_MISUSE, "sqlite only supports :name placeholders; found " + name);
			return(false);
		}
		String key = name.substr(1);
		auto found = params.find(key);
		s32 rc;
		if(found == params.end())
			rc = sqlite3_bind_null(stmt, i);
		else
			rc = sqlite3_bind_text(stmt, i, found->second.c_str(), found->second.length(), SQLITE_STATIC);
		if(rc != SQLITE_OK)
		{
			set_error(rc, "sqlite bind failed for " + name);
			return(false);
		}
	}
	return(true);
}

DValue SQLite::collect_rows(void* statement)
{
	sqlite3_stmt* stmt = (sqlite3_stmt*)statement;
	DValue result;
	s32 column_count = sqlite3_column_count(stmt);
	std::vector<String> column_names;
	column_names.reserve(column_count);
	for(s32 i = 0; i < column_count; i++)
		column_names.push_back(sqlite3_column_name(stmt, i));
	while(true)
	{
		s32 rc = sqlite3_step(stmt);
		if(rc == SQLITE_ROW)
		{
			DValue row;
			for(s32 i = 0; i < column_count; i++)
			{
				const String& name = column_names[i];
				switch(sqlite3_column_type(stmt, i))
				{
					case SQLITE_INTEGER:
						row[name].set((s64)sqlite3_column_int64(stmt, i));
						break;
					case SQLITE_FLOAT:
						row[name].set((f64)sqlite3_column_double(stmt, i));
						break;
					case SQLITE_TEXT:
					{
						const unsigned char* text = sqlite3_column_text(stmt, i);
						row[name].set(text ? String((const char*)text) : "");
						break;
					}
					case SQLITE_BLOB:
					{
						const char* data = (const char*)sqlite3_column_blob(stmt, i);
						s32 bytes = sqlite3_column_bytes(stmt, i);
						row[name].set(data && bytes > 0 ? String(data, bytes) : "");
						break;
					}
					case SQLITE_NULL:
					default:
						break;
				}
			}
			result.push(row);
			continue;
		}
		if(rc == SQLITE_DONE)
		{
			affected_rows = sqlite3_changes((sqlite3*)connection);
			insert_id = sqlite3_last_insert_rowid((sqlite3*)connection);
			return(result);
		}
		set_error(rc, "sqlite step failed");
		return(DValue());
	}
}

DValue SQLite::query(String q)
{
	StringMap params;
	return(query(q, params));
}

DValue SQLite::query(String q, const StringMap& params)
{
	DValue result;
	affected_rows = 0;
	insert_id = 0;
	error_code = SQLITE_OK;
	statement_info = "";
	if(!connection)
	{
		set_error(SQLITE_MISUSE, "sqlite query called without an open connection");
		return(result);
	}
	sqlite3_stmt* stmt = 0;
	const char* tail = 0;
	s32 rc = sqlite3_prepare_v2((sqlite3*)connection, q.c_str(), q.length(), &stmt, &tail);
	if(rc != SQLITE_OK)
	{
		set_error(rc, "sqlite prepare failed");
		return(result);
	}
	if(!stmt)
	{
		statement_info = "ok";
		return(result);
	}
	if(tail && trim(String(tail)) != "")
	{
		sqlite3_stmt* trailing_stmt = 0;
		const char* trailing_tail = 0;
		rc = sqlite3_prepare_v2((sqlite3*)connection, tail, -1, &trailing_stmt, &trailing_tail);
		if(rc != SQLITE_OK)
		{
			sqlite3_finalize(stmt);
			set_error(rc, "sqlite_query accepts exactly one SQL statement per call; trailing SQL after the first statement was rejected");
			return(result);
		}
		if(trailing_stmt)
		{
			sqlite3_finalize(trailing_stmt);
			sqlite3_finalize(stmt);
			set_error(SQLITE_MISUSE, "sqlite_query accepts exactly one SQL statement per call; trailing SQL after the first statement was rejected");
			return(result);
		}
	}
	if(!bind_params(stmt, params))
	{
		sqlite3_finalize(stmt);
		return(result);
	}
	result = collect_rows(stmt);
	rc = sqlite3_finalize(stmt);
	if(rc != SQLITE_OK && error_code == SQLITE_OK)
		set_error(rc, "sqlite finalize failed");
	if(statement_info == "")
		statement_info = "ok";
	return(result);
}

SQLite* sqlite_connect(String path)
{
	auto found = sqlite_worker_connection_cache.find(path);
	if(found != sqlite_worker_connection_cache.end() && found->second && found->second->connection)
	{
		SQLite* cached = found->second;
		cached->error_code = SQLITE_OK;
		cached->affected_rows = 0;
		cached->insert_id = 0;
		cached->statement_info = "connected";
		sqlite_register_request_connection(cached);
		return(cached);
	}

	SQLite* db = new SQLite();
	if(db->connect(path) && db->connection)
	{
		db->worker_cache = true;
		sqlite_worker_connection_cache[path] = db;
	}
	else
		db->request_cleanup_delete = true;
	return(db);
}

void sqlite_disconnect(SQLite* db)
{
	if(!db)
		return;
	db->disconnect();
	delete db;
}

String sqlite_error(SQLite* db)
{
	if(!db)
		return("sqlite connection is null");
	return(db->error());
}

DValue sqlite_query(SQLite* db, String q)
{
	if(!db)
		return(DValue());
	return(db->query(q));
}

DValue sqlite_query(SQLite* db, String q, const StringMap& params)
{
	if(!db)
		return(DValue());
	return(db->query(q, params));
}

u64 sqlite_insert_id(SQLite* db)
{
	if(!db)
		return(0);
	return(db->insert_id);
}

u32 sqlite_affected_rows(SQLite* db)
{
	if(!db)
		return(0);
	return(db->affected_rows);
}

void cleanup_sqlite_connections()
{
	if(!context)
		return;
	while(!context->resources.sqlite_connections.empty())
	{
		SQLite* db = (SQLite*)context->resources.sqlite_connections.back();
		context->resources.sqlite_connections.pop_back();
		if(db->worker_cache)
		{
			// A page that ran BEGIN and faulted must not leak its transaction
			// (and the WAL write lock) into the next request on this worker.
			if(db->connection && !sqlite3_get_autocommit((sqlite3*)db->connection))
				sqlite3_exec((sqlite3*)db->connection, "ROLLBACK", 0, 0, 0);
			db->affected_rows = 0;
			db->insert_id = 0;
			db->error_code = SQLITE_OK;
			db->statement_info = "";
			continue;
		}
		bool should_delete = db->request_cleanup_delete;
		db->disconnect();
		if(should_delete)
			delete db;
	}
}
