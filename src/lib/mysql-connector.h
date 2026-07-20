#pragma once

struct MySQLFieldInfo {
	String name;
	String table;
	String db;
	u64 length;
	String def;
	u64 max_length;
	u64 flags;
	u32 type;
};

struct MySQL {

	void* connection = 0;
	u32 _preload_next_error_code = 0;
	u32 affected_rows = 0;
	u32 field_count = 0;
	u32 row_count = 0;
	u64 insert_id = 0;
	String statement_info = ""; //
	bool request_cleanup_delete = false;
	bool request_pooled = false;
	bool worker_persistent = false;
	u32 request_leases = 0;
	String request_host;
	String request_username;
	String request_password;
	String request_database;

	std::vector<MySQLFieldInfo> field_info;

	bool connect(String host = "localhost", String username = "root", String password = "", String database = "");
	bool reset_connection();
	void disconnect();
	String error();
	String escape(String raw, char quote_char = '\'');
	String parse_query_parameters(String query, StringMap m);
	DValue query(String q);
	DValue query(String q, StringMap params);
	DValue get_pending_result();

	// Unregisters non-pooled instances from request tracking. mysql_connect()
	// handles remain request-owned until cleanup after their leases are released.
	~MySQL() { disconnect(); }

};

inline MySQL* mysql_connect(String host = "localhost", String username = "root", String password = "", String database = "")
{
	if(context)
	{
		for(void* raw : context->resources.mysql_connections)
		{
			MySQL* db = (MySQL*)raw;
			if(db && db->request_pooled && db->connection && db->request_host == host && db->request_username == username && db->request_password == password && db->request_database == database)
			{
				db->request_leases++;
				return(db);
			}
		}
	}
	MySQL* db = new MySQL();
	db->request_cleanup_delete = true;
	db->request_pooled = context != 0;
	db->request_leases = 1;
	db->request_host = host;
	db->request_username = username;
	db->request_password = password;
	db->request_database = database;
	db->connect(host, username, password, database);
	return(db);
}

// A failed connection retains a request-owned wrapper so mysql_error() remains
// available. Callers that need to decide whether to issue work must test the
// connection state rather than only the wrapper pointer.
inline bool mysql_connected(MySQL* m)
{
	return(m != 0 && m->connection != 0);
}

inline void mysql_disconnect(MySQL* m)
{
	if(!m)
		return;
	if(m->request_pooled)
	{
		if(m->request_leases > 0)
			m->request_leases--;
		return;
	}
	m->disconnect();
	delete m;
}

inline String mysql_error(MySQL* m)
{
	return(m->error());
}

String mysql_escape(String raw, char quote_char);

inline DValue mysql_query(MySQL* m, String q)
{
	return(m->query(q));
}

inline DValue mysql_query(MySQL* m, String q, StringMap params)
{
	return(m->query(q, params));
}

inline u64 mysql_insert_id(MySQL* m)
{
	return(m->insert_id);
}

inline u32 mysql_affected_rows(MySQL* m)
{
	return(m ? m->affected_rows : 0);
}
