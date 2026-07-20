#include "uri.h"

#include <cctype>
#include <fcntl.h>
#include <unistd.h>

#ifdef __UCE_WASM_CORE__
extern "C" size_t uce_host_random(char* buf, size_t len);
#endif

String base64_encode(String raw)
{
	static const char* chars =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"0123456789+/";

	String result;
	int val = 0;
	int bits = -6;
	for(unsigned char c : raw)
	{
		val = (val << 8) + c;
		bits += 8;
		while(bits >= 0)
		{
			result.append(1, chars[(val >> bits) & 0x3F]);
			bits -= 6;
		}
	}
	if(bits > -6)
		result.append(1, chars[((val << 8) >> (bits + 8)) & 0x3F]);
	while(result.length() % 4 != 0)
		result.append(1, '=');
	return(result);
}

namespace {

int base64_decode_value(char c)
{
	if(c >= 'A' && c <= 'Z')
		return(c - 'A');
	if(c >= 'a' && c <= 'z')
		return(c - 'a' + 26);
	if(c >= '0' && c <= '9')
		return(c - '0' + 52);
	if(c == '+')
		return(62);
	if(c == '/')
		return(63);
	return(-1);
}

}

String base64_decode(String raw, bool& ok)
{
	ok = false;
	String cleaned;
	for(char c : raw)
	{
		if(!isspace((unsigned char)c))
			cleaned.append(1, c);
	}

	if(cleaned.length() == 0)
	{
		ok = true;
		return("");
	}
	if((cleaned.length() % 4) != 0)
		return("");

	String result;
	for(u32 i = 0; i < cleaned.length(); i += 4)
	{
		int values[4];
		int padding = 0;
		for(u32 j = 0; j < 4; j++)
		{
			char c = cleaned[i + j];
			if(c == '=')
			{
				values[j] = 0;
				padding += 1;
			}
			else
			{
				values[j] = base64_decode_value(c);
				if(values[j] < 0)
					return("");
			}
		}

		if(padding > 2)
			return("");
		if(padding > 0 && i + 4 != cleaned.length())
			return("");
		if(cleaned[i + 2] == '=' && cleaned[i + 3] != '=')
			return("");

		result.append(1, (char)((values[0] << 2) | (values[1] >> 4)));
		if(cleaned[i + 2] != '=')
			result.append(1, (char)(((values[1] & 0x0F) << 4) | (values[2] >> 2)));
		if(cleaned[i + 3] != '=')
			result.append(1, (char)(((values[2] & 0x03) << 6) | values[3]));
	}

	ok = true;
	return(result);
}

bool ws_is_valid_utf8(String input)
{
	u32 i = 0;
	while(i < input.length())
	{
		u8 c = (u8)input[i];
		u32 trailing = 0;
		u32 codepoint = 0;

		if(c <= 0x7F)
		{
			i += 1;
			continue;
		}
		else if((c & 0xE0) == 0xC0)
		{
			trailing = 1;
			codepoint = c & 0x1F;
			if(codepoint == 0)
				return(false);
		}
		else if((c & 0xF0) == 0xE0)
		{
			trailing = 2;
			codepoint = c & 0x0F;
		}
		else if((c & 0xF8) == 0xF0)
		{
			trailing = 3;
			codepoint = c & 0x07;
		}
		else
		{
			return(false);
		}

		if(i + trailing >= input.length())
			return(false);

		for(u32 j = 1; j <= trailing; j++)
		{
			u8 follow = (u8)input[i + j];
			if((follow & 0xC0) != 0x80)
				return(false);
			codepoint = (codepoint << 6) | (follow & 0x3F);
		}

		if((trailing == 1 && codepoint < 0x80) ||
			(trailing == 2 && codepoint < 0x800) ||
			(trailing == 3 && codepoint < 0x10000))
			return(false);
		if(codepoint > 0x10FFFF)
			return(false);
		if(codepoint >= 0xD800 && codepoint <= 0xDFFF)
			return(false);

		i += trailing + 1;
	}
	return(true);
}

String var_dump(URI uri, String prefix, String postfix)
{
	return(
		prefix + "URI Parts: " + postfix +
		var_dump(uri.parts, prefix+"  ", postfix)+
		prefix + "  Query: " + postfix +
		var_dump(uri.query, prefix+"    ", postfix)
	);
}

String uri_decode(String q)
{
	String result;
	for(u32 i = 0; i < q.length(); i++)
	{
		char c = q[i];
		if(c == '%' && i + 2 < q.length() && isxdigit((unsigned char)q[i + 1]) && isxdigit((unsigned char)q[i + 2]))
		{
			result.append(1, hex_to_u8(q.substr(i+1, 2)));
			i += 2;
		}
		else if(c == '+')
		{
			result.append(1, ' ');
		}
		else
		{
			result.append(1, c);
		}
	}
	return(result);
}

String uri_encode(String q)
{
	String result;
	for(u32 i = 0; i < q.length(); i++)
	{
		char c = q[i];
		if(isalnum((unsigned char)c) || c == '~' || c == '.' || c == '_' || c == '-')
			result.append(1, c);
		else
		{
			result.append(1, '%');
			result.append(to_hex((u8)c, 2));
		}
	}
	return(result);
}

StringMap parse_query(String q)
{
	return(parse_query(q, 0));
}

StringMap parse_query(String q, String* first_keyless_path)
{
	StringMap result;
	if(first_keyless_path)
		*first_keyless_path = "";
	for(String part : split(q, "&"))
	{
		if(part == "")
			continue;
		size_t equals = part.find('=');
		if(equals == String::npos)
		{
			String key = uri_decode(part);
			if(first_keyless_path && *first_keyless_path == "")
				*first_keyless_path = key;
			result[key] = "";
		}
		else
		{
			result[uri_decode(part.substr(0, equals))] = uri_decode(part.substr(equals + 1));
		}
	}
	return(result);
}

String encode_query(StringMap map)
{
	String result;

	for (auto it = map.begin(); it != map.end(); ++it)
	{
		if(result.length() > 0)
			result.append(1, '&');
		result.append(uri_encode(it->first) + "=" + uri_encode(it->second));
	}

	return(result);
}

String route_path_normalize(String path)
{
	path = trim(path);
	size_t start = path.find_first_not_of('/');
	if(start == String::npos)
		return("");
	size_t end = path.find_last_not_of('/');
	return(path.substr(start, end - start + 1));
}

bool route_path_normalized_is_safe(String path)
{
	if(path == "")
		return(true);
	for(String part : split(path, "/"))
	{
		if(part == "" || part == "." || part == "..")
			return(false);
		for(unsigned char c : part)
		{
			if(!(std::isalnum(c) || c == '-' || c == '_'))
				return(false);
		}
	}
	return(true);
}

bool route_path_is_safe(String path)
{
	return(route_path_normalized_is_safe(route_path_normalize(path)));
}

String route_path_sanitize(String path, String default_path)
{
	path = route_path_normalize(path);
	if(path == "")
		path = route_path_normalize(default_path);
	if(!route_path_normalized_is_safe(path))
		return("");
	return(path);
}

String route_path_sanitize_normalized(String path, String default_path)
{
	if(path == "")
		path = route_path_normalize(default_path);
	if(!route_path_normalized_is_safe(path))
		return("");
	return(path);
}

String request_script_url(Request& context)
{
	String url = first(context.params["DOCUMENT_URI"], context.params["SCRIPT_NAME"]);
	if(str_ends_with(url, "/index.uce"))
		url = url.substr(0, url.length() - String("index.uce").length());
	return(url);
}

String request_base_url_from_script_url(String script_url)
{
	String base = dirname(script_url);
	if(base == "")
		base = "/";
	if(base[base.length() - 1] != '/')
		base.append(1, '/');
	return(base);
}

String request_base_url(Request& context)
{
	return(request_base_url_from_script_url(request_script_url(context)));
}

String request_query_path(Request& context, String default_path)
{
	return(request_query_route(context, default_path)["l_path"].to_string());
}

DValue request_query_route(Request& context, String default_path)
{
	String raw_path = "";
	parse_query(context.params["QUERY_STRING"], &raw_path);
	return(request_route_from_raw_path(raw_path, default_path));
}

DValue request_route_from_raw_path(String raw_path, String default_path)
{
	DValue route;
	String normalized_path = route_path_normalize(raw_path);
	String route_path = route_path_sanitize_normalized(normalized_path, default_path);
	bool valid = route_path != "";
	route["raw_path"] = normalized_path;
	route["l_path"] = route_path;
	String page = route_path;
	route["page"] = valid ? nibble(page, "/") : "";
	if(valid && route["page"].to_string() == "")
		route["page"] = default_path;
	route["valid"].set_bool(valid);
	return(route);
}

void request_populate_context_params(Request& context, String default_path)
{
	String raw_path;
	parse_query(context.params["QUERY_STRING"], &raw_path);
	request_populate_context_params_from_route(context, raw_path, default_path);
}

void request_populate_context_params_from_route(Request& context, String raw_path, String default_path)
{
	DValue route = request_route_from_raw_path(raw_path, default_path);
	String script_url = request_script_url(context);
	context.params["SCRIPT_URL"] = script_url;
	context.params["BASE_URL"] = request_base_url_from_script_url(script_url);
	context.params["ROUTE_PATH"] = route["l_path"].to_string();
	context.params["ROUTE_PAGE"] = route["page"].to_string();
	context.params["ROUTE_PATH_RAW"] = route["raw_path"].to_string();
	context.params["ROUTE_VALID"] = route["valid"].to_bool() ? "1" : "0";
}

bool http_header_name_valid(String name)
{
	if(name == "")
		return(false);
	for(char c : name)
	{
		if(!(std::isalnum((unsigned char)c) || c == '-' || c == '_'))
			return(false);
	}
	return(true);
}

String http_header_value_clean(String value)
{
	for(char& c : value)
	{
		if(c == '\r' || c == '\n')
			c = ' ';
	}
	return(value);
}

bool http_set_cookie_header_valid(String header)
{
	if(header.find('\r') != String::npos || header.find('\n') != String::npos)
		return(false);
	return(str_starts_with(to_lower(header), "set-cookie: "));
}

String http_status_line_clean(String status_line)
{
	if(status_line.find('\r') == String::npos && status_line.find('\n') == String::npos)
		return(status_line);
	if(str_starts_with(status_line, "HTTP/"))
		return("HTTP/1.1 500 Internal Server Error");
	return("Status: 500 Internal Server Error");
}

namespace {

String cookie_attribute_value_clean(String value)
{
	for(char& c : value)
	{
		if(c == '\r' || c == '\n' || c == ';')
			c = ' ';
	}
	return(trim(value));
}

}

void redirect(String url, s32 code)
{
	context->header["Location"] = http_header_value_clean(url);
	context->set_status(code);
}

String trim_wrapping_quotes(String raw)
{
	if(raw.length() >= 2 && raw[0] == '"' && raw[raw.length()-1] == '"')
		return(raw.substr(1, raw.length()-2));
	return(raw);
}

void parse_multipart_content_disposition(
	String raw,
	String& disposition_type,
	String& field_name,
	String& file_name)
{
	auto parts = split(raw, ";");
	if(parts.size() == 0)
		return;

	disposition_type = to_lower(trim(parts[0]));
	for(u32 i = 1; i < parts.size(); i++)
	{
		String part = trim(parts[i]);
		String key = to_lower(trim(nibble(part, "=")));
		String value = trim_wrapping_quotes(trim(part));
		if(key == "name")
			field_name = value;
		else if(key == "filename")
			file_name = value;
	}
}

String make_upload_tmp_name()
{
	String upload_path = context->server->config["TMP_UPLOAD_PATH"];
	if(upload_path.length() > 0 && upload_path[upload_path.length()-1] != '/')
		upload_path.append(1, '/');
	return(upload_path + session_id_create());
}

StringMap parse_multipart(String q, String boundary, std::vector<UploadedFile>& uploaded_files)
{
	StringMap result;

	if(boundary == "")
		return(result);

	u64 i = 0;
	while(i < q.length())
	{
		auto start_pos = q.find(boundary, i);
		if(start_pos == String::npos)
			break;

		start_pos += boundary.length();
		if(q.substr(start_pos, 2) == "--")
			break;
		if(q.substr(start_pos, 2) == "\r\n")
			start_pos += 2;

		auto header_end = q.find("\r\n\r\n", start_pos);
		if(header_end == String::npos)
			break;

		String header_block = q.substr(start_pos, header_end - start_pos);
		auto end_pos = q.find(boundary, header_end + 4);
		if(end_pos == String::npos)
			break;

		String body = q.substr(header_end + 4, end_pos - (header_end + 4));
		if(body.length() >= 2 && body.substr(body.length()-2) == "\r\n")
			body.resize(body.length()-2);

		String disposition_type;
		String field_name;
		String file_name;
		for(auto header_line : split(header_block, "\r\n"))
		{
			String header_name = to_lower(trim(nibble(header_line, ":")));
			String header_value = trim(header_line);
			if(header_name == "content-disposition")
			{
				parse_multipart_content_disposition(
					header_value,
					disposition_type,
					field_name,
					file_name
				);
			}
		}

		if(field_name != "")
		{
			bool is_uploaded_file =
				(disposition_type == "form-data" || disposition_type == "attachment") &&
				file_name != "";

			if(is_uploaded_file)
			{
				UploadedFile f;
				f.tmp_name = make_upload_tmp_name();
				f.file_name = file_name;
				f.size = body.length();
				file_put_contents(f.tmp_name, body);
				uploaded_files.push_back(f);
				result[field_name] = file_name;
			}
			else
			{
				result[field_name] = body;
			}
		}

		i = end_pos + boundary.length();
	}

	return(result);
}

URI parse_uri(String uri_String)
{
	URI result;

	if(uri_String == "")
	{
		result.parts["raw"] = "";
		return(result);
	}

	u8 state = 0;
	String current = "";

	result.parts["raw"] = uri_String;

	String part_names[] = {
		"scheme",
		"host",
		"port",
		"path",
		"query",
		"fragment",
	};

	if(uri_String[0] == '/')
		state = 3;

	for (char &c: uri_String)
	{
		bool append_it = true;

		switch(state)
		{
			case(0): // scheme
				if(c == ':')
				{
					result.parts[part_names[state]] = current;
					append_it = false;
					current = "";
					state = 1;
				}
				break;
			case(1): // host name
				if(c == '/')
				{
					if(current == "")
					{
						append_it = false;
						break;
					}
					result.parts[part_names[state]] = current;
					append_it = false;
					current = "";
					state = 3;
				}
				else if(c == ':')
				{
					result.parts[part_names[state]] = current;
					append_it = false;
					current = "";
					state = 2;
				}
				break;
			case(2): // port
				if(c == '/')
				{
					result.parts[part_names[state]] = current;
					append_it = false;
					current = "";
					state = 3;
				}
				break;
			case(3): // path
				if(c == '/' && current == "")
				{
					append_it = false;
					break;
				}
				if(c == '?')
				{
					result.parts[part_names[state]] = current;
					append_it = false;
					current = "";
					state = 4;
				}
				break;
			case(4): // query
				if(c == '#')
				{
					result.parts[part_names[state]] = current;
					append_it = false;
					current = "";
					state = 5;
				}
				break;
		}

		if(append_it)
			current.append(1, c);
	}

	result.parts[part_names[state]] = current;

	result.query = parse_query(result.parts["query"]);

	return(result);
}

void set_cookie(
	String name, String value,
	u64 expires, String path, String domain,
	bool secure, bool http_only)
{
	String cookie = "Set-Cookie: ";
	cookie.append(uri_encode(cookie_attribute_value_clean(name)) + "=" + uri_encode(value));
	if(expires > 0)
		cookie.append(String("; Expires=") + time_format_utc("RFC1123", expires));
	if(path != "")
		cookie.append("; Path=" + cookie_attribute_value_clean(path));
	if(domain != "")
		cookie.append("; Domain=" + cookie_attribute_value_clean(domain));
	if(secure)
		cookie.append("; Secure");
	if(http_only)
		cookie.append("; HttpOnly");
	cookie.append("; SameSite=Lax");
	context->set_cookies.push_back(cookie);
	context->cookies[name] = value;
}

StringMap parse_cookies(String cookie_String)
{
	StringMap result;
	while(cookie_String.length() > 0)
	{
		String key = trim(nibble("=", cookie_String));
		String value = nibble(";", cookie_String);
		result[key] = value;
	}
	return(result);
}

String session_id_create()
{
#ifdef __UCE_WASM_CORE__
	unsigned char bytes[32];
	if(uce_host_random((char*)bytes, sizeof(bytes)) != sizeof(bytes))
		__builtin_trap();
	String result;
	static const char* hex = "0123456789abcdef";
	for(unsigned char b : bytes)
	{
		result.push_back(hex[b >> 4]);
		result.push_back(hex[b & 0x0f]);
	}
	return(result);
#else
	unsigned char bytes[32];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if(fd == -1)
		throw std::runtime_error("session_id_create(): could not open /dev/urandom");
	size_t offset = 0;
	while(offset < sizeof(bytes))
	{
		ssize_t count = read(fd, bytes + offset, sizeof(bytes) - offset);
		if(count <= 0)
		{
			close(fd);
			throw std::runtime_error("session_id_create(): could not read random bytes");
		}
		offset += (size_t)count;
	}
	close(fd);
	String result;
	static const char* hex = "0123456789abcdef";
	for(unsigned char b : bytes)
	{
		result.push_back(hex[b >> 4]);
		result.push_back(hex[b & 0x0f]);
	}
	return(result);
#endif
}

bool is_valid_session_id(String session_id)
{
	if(session_id.length() < 16 || session_id.length() > 128)
		return(false);
	for(auto c : session_id)
	{
		if(!isxdigit(c))
			return(false);
	}
	return(true);
}

String session_file_path(String session_id)
{
	if(!is_valid_session_id(session_id))
		return("");
	return(context->server->config["SESSION_PATH"] + "/" + session_id);
}

namespace {

String session_serialize(const StringMap& data)
{
	return(encode_query(data));
}

String session_hash_serialized(String serialized)
{
	return(gen_sha1(serialized));
}

String session_load_serialized(String session_path)
{
	if(session_path == "" || !file_exists(session_path))
		return("");
	return(file_get_contents(session_path));
}

}

StringMap load_session_data(String session_id)
{
	String session_path = session_file_path(session_id);
	if(session_path == "")
		return(StringMap());
	return(parse_query(session_load_serialized(session_path)));
}

void save_session_data(String session_id, StringMap data)
{
	String session_path = session_file_path(session_id);
	String encoded = session_serialize(data);
	String encoded_hash = session_hash_serialized(encoded);
	if(session_path == "")
	{
		printf("(!) Refusing to save invalid session id\n");
		return;
	}
	if(context && encoded_hash == context->session_loaded_hash)
		return;
	if(!file_put_contents(session_path, encoded))
	{
		printf("(!) Refusing to save session file %s\n", session_path.c_str());
		return;
	}
	if(context)
		context->session_loaded_hash = encoded_hash;
}

String session_start(String session_name)
{
	if(context->session_name == session_name && context->session_id != "")
		return(context->session_id);
	context->session.clear();
	context->session_loaded_hash = "";
	context->session_id = "";
	context->session_name = "";

	String session_id = context->cookies[session_name];
	if(!is_valid_session_id(session_id) || !file_exists(session_file_path(session_id)))
		session_id = "";

	if(session_id.length() == 0)
	{
		session_id = session_id_create();
		bool secure_cookie = context->server->config["SESSION_COOKIE_SECURE"] == "1";
		set_cookie(session_name, session_id, time() + int_val(context->server->config["SESSION_TIME"]), "/", "", secure_cookie, true);
	}
	context->session_id = session_id;
	context->session_name = session_name;
	auto session_path = session_file_path(context->session_id);
	auto serialized = session_load_serialized(session_path);
	context->session_loaded_hash = session_hash_serialized(serialized);
	context->session = parse_query(serialized);
	return(context->session_id);
}

void session_destroy(String session_name)
{
	String cookie_session_id = context->cookies[session_name];
	String active_session_id = context->session_name == session_name ? context->session_id : "";
	String destroy_session_id = active_session_id != "" ? active_session_id : cookie_session_id;
	if(cookie_session_id.length() > 0 || active_session_id.length() > 0)
	{
		bool secure_cookie = context->server->config["SESSION_COOKIE_SECURE"] == "1";
		set_cookie(session_name, "", time() - int_val(context->server->config["SESSION_TIME"]), "/", "", secure_cookie, true);
		String session_path = session_file_path(destroy_session_id);
		if(session_path != "")
			file_unlink(session_path);
		if(active_session_id != "")
		{
			context->session.clear();
			context->session_loaded_hash = session_hash_serialized(session_serialize(context->session));
			context->session_id = "";
			context->session_name = "";
		}
	}
}

static String csrf_session_key(String token_name)
{
	token_name = first(trim(token_name), "csrf_token");
	return("_csrf_" + token_name);
}

String csrf_token(String session_name, String token_name)
{
	session_start(session_name);
	String key = csrf_session_key(token_name);
	if(context->session[key] == "")
	{
		String random = random_bytes(32);
		if(random.size() != 32)
			random = session_id_create();
		context->session[key] = sha256_hex(random);
	}
	return(context->session[key]);
}

String csrf_field(String session_name, String token_name, String field_name)
{
	token_name = first(trim(token_name), "csrf_token");
	field_name = first(trim(field_name), token_name);
	return("<input type=\"hidden\" name=\"" + html_escape(field_name) + "\" value=\"" + html_escape(csrf_token(session_name, token_name)) + "\">");
}

bool csrf_valid(String submitted_token, String session_name, String token_name)
{
	session_start(session_name);
	String expected = context->session[csrf_session_key(token_name)];
	return(expected != "" && submitted_token != "" && crypto_equal(submitted_token, expected));
}

void csrf_rotate(String session_name, String token_name)
{
	session_start(session_name);
	context->session.erase(csrf_session_key(token_name));
}

String ws_make_accept_key(String client_key)
{
	return(base64_encode(gen_sha1(
		client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
		true
	)));
}

bool ws_is_valid_client_key(String client_key)
{
	bool ok = false;
	String decoded = base64_decode(trim(client_key), ok);
	return(ok && decoded.length() == 16);
}

String ws_encode_frame(String payload, u8 opcode, bool is_final_fragment)
{
	String frame;
	u8 first_byte = opcode & 0x0F;
	if(is_final_fragment)
		first_byte |= 0x80;
	frame.append(1, first_byte);

	u64 payload_length = payload.length();
	if(payload_length <= 125)
	{
		frame.append(1, (char)payload_length);
	}
	else if(payload_length <= 0xFFFF)
	{
		frame.append(1, 126);
		frame.append(1, (char)((payload_length >> 8) & 0xFF));
		frame.append(1, (char)(payload_length & 0xFF));
	}
	else
	{
		frame.append(1, 127);
		for(int shift = 56; shift >= 0; shift -= 8)
			frame.append(1, (char)((payload_length >> shift) & 0xFF));
	}

	frame += payload;
	return(frame);
}

String ws_close_frame(u16 status_code, String reason)
{
	String payload;
	payload.append(1, (char)((status_code >> 8) & 0xFF));
	payload.append(1, (char)(status_code & 0xFF));
	payload += reason;
	return(ws_encode_frame(payload, 0x8));
}

bool WSFrame::parse(const String& buffer, String& error)
{
	error = "";
	payload = "";

	if(buffer.length() < 2)
		return(false);

	const unsigned char* raw = (const unsigned char*)buffer.data();
	opcode = raw[0] & 0x0F;
	is_final_fragment = (raw[0] & 0x80) != 0;
	rsv1 = (raw[0] & 0x40) != 0;
	rsv2 = (raw[0] & 0x20) != 0;
	rsv3 = (raw[0] & 0x10) != 0;
	mask_bit = (raw[1] & 0x80) != 0;
	payload_length = raw[1] & 0x7F;
	header_length = 2;

	if(payload_length == 126)
	{
		if(buffer.length() < 4)
			return(false);
		payload_length = ((u64)raw[2] << 8) | (u64)raw[3];
		if(payload_length < 126)
		{
			error = "non-minimal websocket frame length";
			return(false);
		}
		header_length = 4;
	}
	else if(payload_length == 127)
	{
		if(buffer.length() < 10)
			return(false);
		if((raw[2] & 0x80) != 0)
		{
			error = "invalid websocket frame length";
			return(false);
		}
		payload_length = 0;
		for(u32 i = 0; i < 8; i++)
			payload_length = (payload_length << 8) | (u64)raw[2 + i];
		if(payload_length <= 0xFFFF)
		{
			error = "non-minimal websocket frame length";
			return(false);
		}
		header_length = 10;
	}

	u64 mask_offset = header_length;
	if(mask_bit)
		header_length += 4;

	frame_length = header_length + payload_length;
	if(frame_length < header_length)
	{
		error = "invalid websocket frame length";
		return(false);
	}
	if(rsv1 || rsv2 || rsv3)
	{
		error = "reserved websocket bits are not supported";
		return(false);
	}
	bool is_control_frame = (opcode & 0x08) != 0;
	if(is_control_frame && payload_length > 125)
	{
		error = "control frames must be 125 bytes or less";
		return(false);
	}
	if(buffer.length() < frame_length)
		return(false);

	payload.assign(buffer.data() + header_length, payload_length);
	if(mask_bit)
	{
		const unsigned char* mask = raw + mask_offset;
		for(u64 i = 0; i < payload.length(); i++)
			payload[i] = payload[i] ^ mask[i % 4];
	}

	return(true);
}
