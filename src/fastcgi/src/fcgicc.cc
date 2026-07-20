/*
 * Copyright 2008, 2009 Andrey Zholos. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *	this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *	this list of conditions and the following disclaimer in the documentation
 *	and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of contributors
 *	may be used to endorse or promote products derived from this software
 *	without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This file is part of the FastCGI C++ Class library (fcgicc) version 0.1,
 * available at http://althenia.net/fcgicc
 */


#include "fcgicc.h"

#include <cstring>
#include <stdexcept>

#include <errno.h> // E*
#include <fcntl.h>
#include <unistd.h> // read, write, close, unlink
#include <arpa/inet.h> // hton*
#include <netinet/in.h> // sockaddr_in, INADDR_*
#include <sys/select.h> // select, fd_set, FD_*, timeval
#include <sys/socket.h> // socket, bind, accept, listen, sockaddr, AF_*, SOCK_*
#include <sys/stat.h> // mkdir
#include <sys/un.h> // sockaddr_un

#include "../fastcgi_devkit/fastcgi.h"

namespace {

struct TransportLimits {
	u64 max_client_connections = 256;
	u64 max_http_header_bytes = 16 * 1024;
	u64 max_http_body_bytes = 1024 * 1024;
	u64 max_websocket_frame_bytes = 1024 * 1024;
	u64 max_websocket_message_bytes = 1024 * 1024;
	u64 max_websocket_output_bytes = 4 * 1024 * 1024;
	u64 max_response_bytes = 8 * 1024 * 1024;
	f64 http_request_timeout_seconds = 15.0;
	f64 connection_idle_timeout_seconds = 120.0;

	u64 max_http_buffer_bytes() const
	{
		return(max_http_header_bytes + max_http_body_bytes);
	}

	u64 max_websocket_buffer_bytes() const
	{
		return(max_websocket_message_bytes + 16 * 1024);
	}
};

TransportLimits transport_limits()
{
	TransportLimits limits;
	limits.max_client_connections = to_u64(server_state.config["TRANSPORT_MAX_CLIENT_CONNECTIONS"], limits.max_client_connections);
	limits.max_http_header_bytes = to_u64(server_state.config["TRANSPORT_MAX_HTTP_HEADER_BYTES"], limits.max_http_header_bytes);
	limits.max_http_body_bytes = to_u64(server_state.config["TRANSPORT_MAX_HTTP_BODY_BYTES"], limits.max_http_body_bytes);
	limits.max_websocket_frame_bytes = to_u64(server_state.config["TRANSPORT_MAX_WEBSOCKET_FRAME_BYTES"], limits.max_websocket_frame_bytes);
	limits.max_websocket_message_bytes = to_u64(server_state.config["TRANSPORT_MAX_WEBSOCKET_MESSAGE_BYTES"], limits.max_websocket_message_bytes);
	limits.max_websocket_output_bytes = to_u64(server_state.config["TRANSPORT_MAX_WEBSOCKET_OUTPUT_BYTES"], limits.max_websocket_output_bytes);
	limits.max_response_bytes = to_u64(server_state.config["TRANSPORT_MAX_RESPONSE_BYTES"], limits.max_response_bytes);
	limits.http_request_timeout_seconds = to_f64(server_state.config["TRANSPORT_HTTP_REQUEST_TIMEOUT_SECONDS"], limits.http_request_timeout_seconds);
	limits.connection_idle_timeout_seconds = to_f64(server_state.config["TRANSPORT_CONNECTION_IDLE_TIMEOUT_SECONDS"], limits.connection_idle_timeout_seconds);
	return(limits);
}

}

static String
make_http_text_response(String status_line, String body, String extra_headers = "")
{
	return(
		status_line + "\r\n"
		"Content-Type: text/plain; charset=utf-8\r\n" +
		extra_headers +
		"Content-Length: " + std::to_string(body.length()) + "\r\n"
		"Connection: close\r\n\r\n" +
		body
	);
}

static String
render_header_map(StringMap headers)
{
	String result;
	for(auto& item : headers)
	{
		if(!http_header_name_valid(item.first))
			continue;
		result += item.first + ": " + http_header_value_clean(item.second) + "\r\n";
	}
	return(result);
}

static String
render_set_cookie_headers(StringList headers)
{
	String result;
	for(String header : headers)
	{
		if(!http_set_cookie_header_valid(header))
			continue;
		result += header + "\r\n";
	}
	return(result);
}

static String
http_script_root()
{
	if(context && context->server)
		return(first(
			context->server->config["HTTP_DOCUMENT_ROOT"],
			context->server->config["COMPILER_SYS_PATH"],
			cwd_get()
		));
	return(cwd_get());
}

static String
strip_leading_slashes(String s)
{
	while(s.length() > 0 && s[0] == '/')
		s.erase(0, 1);
	return(s);
}

static bool
is_valid_close_code(u16 status_code)
{
	if(status_code < 1000)
		return(false);
	if(status_code == 1004 || status_code == 1005 || status_code == 1006 || status_code == 1015)
		return(false);
	if(status_code <= 1014)
		return(true);
	if(status_code >= 3000 && status_code <= 4999)
		return(true);
	return(false);
}

static void
ensure_parent_directories(const std::string& path)
{
	std::string::size_type slash = path.rfind('/');
	if(slash == std::string::npos || slash == 0)
		return;

	std::string current;
	std::string directory = path.substr(0, slash);
	for(std::string::size_type i = 1; i <= directory.length(); ++i)
	{
		if(i != directory.length() && directory[i] != '/')
			continue;
		current = directory.substr(0, i);
		if(current == "")
			continue;
		if(::mkdir(current.c_str(), 0775) == -1 && errno != EEXIST)
			throw std::runtime_error("mkdir() failed for Unix socket directory");
	}
}

static void
set_socket_nonblocking(int socket_handle)
{
	int flags = fcntl(socket_handle, F_GETFL, 0);
	if(flags == -1)
		throw std::runtime_error("fcntl(F_GETFL) failed");
	if(fcntl(socket_handle, F_SETFL, flags | O_NONBLOCK) == -1)
		throw std::runtime_error("fcntl(F_SETFL) failed");
}

void
FastCGIServer::shutdown()
{

	if(getpid() != parent_pid) // if we're a child process, we must not close the handles
		return;

	for (std::vector<int>::iterator it = server_sockets.begin();
		it != server_sockets.end(); ++it)
	{
		printf("Closing server socket %i\n", *it);
		close(*it);
	}

	for (std::vector<std::string>::iterator it = listen_unlink.begin();
		it != listen_unlink.end(); ++it)
		file_unlink(*it);

	for (std::map<int, Connection*>::iterator it = client_sockets.begin();
		it != client_sockets.end(); ++it)
	{
		close(it->first);
		for (RequestList::iterator req_it = it->second->requests.begin();
			req_it != it->second->requests.end(); ++req_it)
			delete req_it->second;
		delete it->second;
	}

	server_sockets.clear();

}

FastCGIServer::~FastCGIServer()
{
	shutdown();
}

int
FastCGIServer::listen_http(unsigned tcp_port)
{
	int server_socket = listen(tcp_port);
	server_socket_types[server_socket] = 'H';
	return server_socket;
}

int
FastCGIServer::listen_http(unsigned tcp_port, const std::string& bind_address)
{
	int server_socket = listen(tcp_port, bind_address);
	server_socket_types[server_socket] = 'H';
	return server_socket;
}

int
FastCGIServer::listen_cli(const std::string& local_path)
{
	int server_socket = listen(local_path);
	server_socket_types[server_socket] = 'C';
	printf("(P) CLI command socket ready at %s\n", local_path.c_str());
	return server_socket;
}

int
FastCGIServer::listen(unsigned tcp_port)
{
	return(listen(tcp_port, "0.0.0.0"));
}

int
FastCGIServer::adopt_listener(int socket_handle, char type)
{
	int accepting = 0;
	socklen_t accepting_size = sizeof(accepting);
	if(socket_handle < 0 || getsockopt(socket_handle, SOL_SOCKET, SO_ACCEPTCONN,
		&accepting, &accepting_size) != 0 || !accepting)
		throw std::runtime_error("inherited descriptor is not a listening socket");
	set_socket_nonblocking(socket_handle);
	server_sockets.push_back(socket_handle);
	server_socket_types[socket_handle] = type;
	printf("(P) adopted #%i inherited %s listener\n", socket_handle,
		type == 'F' ? "FastCGI" : "server");
	return(socket_handle);
}

int
FastCGIServer::listen(unsigned tcp_port, const std::string& bind_address)
{
	int server_socket = socket(PF_INET, SOCK_STREAM, 0);
	if (server_socket == -1)
		throw std::runtime_error("socket() failed");

	try {

		int iSetOption = 1;
		setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&iSetOption,
			sizeof(iSetOption));

		struct sockaddr_in sa;
		bzero(&sa, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_port = htons(tcp_port);
		if(bind_address == "" || bind_address == "0.0.0.0" || bind_address == "*")
			sa.sin_addr.s_addr = htonl(INADDR_ANY);
		else if(inet_pton(AF_INET, bind_address.c_str(), &sa.sin_addr) != 1)
			throw std::runtime_error("invalid bind address");
		if (bind(server_socket, (struct sockaddr*)&sa, sizeof(sa)) == -1)
			throw std::runtime_error("bind() failed");

		if (::listen(server_socket, 100))
			throw std::runtime_error("listen() failed");

		set_socket_nonblocking(server_socket);
		server_sockets.push_back(server_socket);
	} catch (...) {
		close(server_socket);
		throw;
	}

	server_socket_types[server_socket] = 'F';
	printf("(P) listening to #%i %s:%i\n", server_socket, bind_address.c_str(), tcp_port);
	return server_socket;
}

int
FastCGIServer::listen(const std::string& local_path)
{
	int server_socket = socket(PF_UNIX, SOCK_STREAM, 0);
	if (server_socket == -1)
	throw std::runtime_error("socket() failed");

	try {
		struct sockaddr_un sa;
		bzero(&sa, sizeof(sa));
		sa.sun_family = AF_LOCAL;

		std::string::size_type size = local_path.size();
		if (size >= sizeof(sa.sun_path))
			throw std::runtime_error("path too long");
		if (local_path.find_first_of('\0') != std::string::npos)
			throw std::runtime_error("null character in path");

		std::memcpy(sa.sun_path, local_path.data(), size);

		ensure_parent_directories(local_path);
		file_unlink(local_path);
		try {
			if (bind(server_socket, (struct sockaddr*)&sa,
				sizeof(sa) - (sizeof(sa.sun_path) - size - 1)) == -1)
				throw std::runtime_error("bind() failed");

			if (::listen(server_socket, 100))
				throw std::runtime_error("listen() failed");

			set_socket_nonblocking(server_socket);
			server_sockets.push_back(server_socket);
			listen_unlink.push_back(local_path);

			printf("(P) listening to #%i socket %s\n", server_socket, local_path.c_str());

		} catch (...) {
			file_unlink(local_path);
			throw;
		}

	} catch (...) {
		close(server_socket);
		throw;
	}
	return server_socket;
}

int
FastCGIServer::send_output_buffer(Connection& con)
{
	int write_result = write(con.client_socket,
		con.output_buffer.data(),
		con.output_buffer.size());
	if(write_result == -1)
	{
		if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return(0);
		if(errno == ECONNRESET || errno == EPIPE)
			return(-1);
		throw std::runtime_error("write() failed");
	}
	con.output_buffer.erase(0, write_result);
	return write_result;
}

void
FastCGIServer::close_http_listeners()
{
	for(std::vector<int>::iterator it = server_sockets.begin(); it != server_sockets.end();)
	{
		int socket_handle = *it;
		if(server_socket_types[socket_handle] == 'H')
		{
			close(socket_handle);
			server_socket_types.erase(socket_handle);
			it = server_sockets.erase(it);
			continue;
		}
		++it;
	}
}

bool
FastCGIServer::is_http_like_type(char type)
{
	return(type == 'H' || type == 'C');
}

FastCGIServer::Connection*
FastCGIServer::open_client_connection(int server_socket, int client_socket)
{
	set_socket_nonblocking(client_socket);
	printf("Opening socket %i\n", client_socket);
	Connection* connection = new Connection();
	connection->client_socket = client_socket;
	connection->server_socket = server_socket;
	connection->type = server_socket_types[server_socket];
	connection->opened_at = time_precise();
	connection->last_activity_at = connection->opened_at;
	client_sockets[client_socket] = connection;

	if(is_http_like_type(connection->type))
	{
		FastCGIRequest* new_request = new FastCGIRequest();
		new_request->resources.client_socket = client_socket;
		new_request->resources.server_socket = server_socket;
		new_request->stats.time_init = connection->opened_at;
		connection->requests[client_socket] = new_request;
	}

	return(connection);
}

bool
FastCGIServer::reject_http_connection(Connection& connection, String status_line, String body, String extra_headers)
{
	connection.output_buffer += make_http_text_response(status_line, body, extra_headers);
	connection.close_socket = true;
	return(false);
}

void
FastCGIServer::enforce_connection_timeouts(Connection& connection)
{
	if(connection.close_socket)
		return;

	TransportLimits limits = transport_limits();
	f64 now = time_precise();
	if(is_http_like_type(connection.type) && !connection.is_websocket && !connection.requests.empty())
	{
		FastCGIRequest* pending_request = connection.requests.begin()->second;
		if(!pending_request->flags.input_closed && now - connection.opened_at > limits.http_request_timeout_seconds)
		{
			reject_http_connection(connection, "HTTP/1.1 408 Request Timeout", "request timed out\n");
			return;
		}
	}
	if(now - connection.last_activity_at > limits.connection_idle_timeout_seconds)
		connection.close_socket = true;
}

bool
FastCGIServer::queue_websocket_frame(Connection& connection, String frame)
{
	if(connection.output_buffer.length() + frame.length() > transport_limits().max_websocket_output_bytes)
	{
		fail_websocket_connection(connection, 1013, "websocket output queue is full");
		return(false);
	}
	connection.output_buffer += frame;
	return(true);
}

bool
FastCGIServer::queue_websocket_payload(Connection& connection, String message, bool binary)
{
	if(message.length() > transport_limits().max_websocket_message_bytes)
		return(false);
	return(queue_websocket_frame(connection, ws_encode_frame(message, binary ? 0x2 : 0x1)));
}

void
FastCGIServer::close_websocket_connection(Connection& connection, u16 status_code, String reason)
{
	if(!is_valid_close_code(status_code))
		status_code = 1002;
	if(reason.length() > 123)
		reason = reason.substr(0, 123);
	if(!ws_is_valid_utf8(reason))
		reason = "";
	if(!connection.close_socket)
		connection.output_buffer += ws_close_frame(status_code, reason);
	connection.close_socket = true;
}

void
FastCGIServer::fail_websocket_connection(Connection& connection, u16 status_code, String reason)
{
	// Drop stale application frames on failure so slow peers cannot keep a large
	// queued buffer alive. Preserve an unsent 101 response, though: if the client
	// pipelined a bad WebSocket frame immediately after the HTTP upgrade, the
	// close frame must still follow the accepted upgrade response.
	if(!str_starts_with(connection.output_buffer, "HTTP/1.1 101 Switching Protocols\r\n"))
		connection.output_buffer = "";
	close_websocket_connection(connection, status_code, reason);
}

void
FastCGIServer::dispatch_websocket_message(Connection& connection, RequestID request_id, String payload, u8 opcode)
{
	RequestList::iterator it = connection.requests.find(request_id);
	if(it == connection.requests.end() || !on_websocket_message)
		return;

	it->second->resources.is_websocket = true;
	it->second->resources.websocket_connection_id = connection.websocket_connection_id;
	it->second->resources.websocket_scope = connection.websocket_scope;
	it->second->resources.websocket_connection_state = &connection.websocket_state;
	it->second->connection.set_reference(&connection.websocket_state);
	it->second->resources.websocket_opcode = opcode;
	it->second->resources.websocket_is_binary = (opcode == 0x2);
	it->second->resources.websocket_is_text = (opcode == 0x1);
	on_websocket_message(*it->second, payload, opcode);
}

void
FastCGIServer::process(int timeout_ms)
{
	char buffer[64*1024];
	fd_set fs_read;
	fd_set fs_write;
	int nfd = 0;
	struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };

	FD_ZERO(&fs_read);
	FD_ZERO(&fs_write);

	for(auto socket_handle : server_sockets)
	{
		FD_SET(socket_handle, &fs_read);
		nfd = std::max(nfd, socket_handle);
	}

	for(auto con : client_sockets)
	{
		FD_SET(con.first, &fs_read);
		if(!con.second->output_buffer.empty() || con.second->close_socket)
			FD_SET(con.first, &fs_write);
		nfd = std::max(nfd, con.first);
	}

	int select_result = select(
		nfd + 1,
		&fs_read,
		&fs_write,
		NULL,
		timeout_ms < 0 ? NULL : &tv
	);
	if(select_result == -1)
	{
		if(errno == EINTR)
			return;
		throw std::runtime_error("select() failed");
	}

	for(auto socket_handle : server_sockets)
	{
		if(!FD_ISSET(socket_handle, &fs_read))
			continue;

		for(;;)
		{
			int client_socket = accept(socket_handle, NULL, NULL);
			if(client_socket == -1)
			{
				if(errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				if(errno == EINTR)
					continue;
				throw std::runtime_error("accept() failed");
			}

			if(client_sockets.size() >= transport_limits().max_client_connections)
			{
				printf("(!) rejecting socket %i: too many clients\n", client_socket);
				close(client_socket);
				continue;
			}

			open_client_connection(socket_handle, client_socket);
		}
	}

	for(std::map<int, Connection*>::iterator it = client_sockets.begin();
		it != client_sockets.end();)
	{
		int read_socket = it->first;
		Connection* connection = it->second;
		enforce_connection_timeouts(*connection);

		if(FD_ISSET(read_socket, &fs_read))
		{
			int read_result = read(read_socket, buffer, sizeof(buffer));
			if(read_result == -1)
			{
				if(errno == ECONNRESET)
					goto close_socket;
				if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
					throw std::runtime_error("read() on socket failed");
			}
			else if(read_result == 0)
			{
				if(connection->type == 'H' && connection->is_websocket)
				{
					connection->close_socket = true;
				}
				else if(is_http_like_type(connection->type) && connection->input_buffer != "")
				{
					process_http_like_connection_input(*connection);
					if(!connection->is_websocket && (connection->close_socket || !connection->output_buffer.empty()))
						connection->input_buffer = "";
					else if(!connection->is_websocket)
						connection->close_socket = true;
				}
				else
				{
					connection->close_socket = true;
				}
			}
			else
			{
				connection->last_activity_at = time_precise();
				connection->input_buffer.append(buffer, read_result);
				if(is_http_like_type(connection->type))
				{
					if(!connection->is_websocket && connection->input_buffer.length() > transport_limits().max_http_buffer_bytes())
						reject_http_connection(*connection, "HTTP/1.1 413 Payload Too Large", "request is too large\n");
					else
						process_http_like_connection_input(*connection);
					if(!connection->is_websocket && (connection->close_socket || !connection->output_buffer.empty()))
						connection->input_buffer = "";
				}
				else
				{
					read_fgci(*connection);
				}
			}
		}

		if(connection->is_websocket && connection->output_buffer.length() > transport_limits().max_websocket_output_bytes)
			fail_websocket_connection(*connection, 1013, "websocket output queue is full");

		if(!connection->output_buffer.empty() && FD_ISSET(read_socket, &fs_write))
		{
			if(connection->type == 'F')
				write_fgci(*connection);
			if(send_output_buffer(*connection) == -1)
				goto close_socket;
		}

		if(connection->is_websocket && connection->output_buffer.empty() && !connection->input_buffer.empty() && !connection->close_socket)
			process_websocket_input(*connection);

		if(connection->close_socket && connection->output_buffer.empty())
		{
		close_socket:
			printf("Closing socket %i\n", it->first);
			int close_result = close(it->first);
			if(close_result == -1 && errno != ECONNRESET)
				throw std::runtime_error("close() failed");
			Connection* doomed_connection = it->second;
			client_sockets.erase(it++);
			delete doomed_connection;
			if(calls_until_termination != -1 && client_sockets.size() == 0)
			{
				calls_until_termination -= 1;
				if(calls_until_termination <= 0)
					exit(0);
			}
		}
		else
		{
			++it;
		}
	}
}

bool
FastCGIServer::parse_http_message(FastCGIRequest& request, String& data)
{
	Connection* connection = client_sockets[request.resources.client_socket];
	TransportLimits limits = transport_limits();
	auto header_end = data.find("\r\n\r\n");
	if(header_end == String::npos)
	{
		if(data.length() > limits.max_http_header_bytes)
			reject_http_connection(*connection, "HTTP/1.1 431 Request Header Fields Too Large", "request headers are too large\n");
		return(false);
	}

	if(header_end > limits.max_http_header_bytes)
		return(reject_http_connection(*connection, "HTTP/1.1 431 Request Header Fields Too Large", "request headers are too large\n"));

	if(request.params.size() == 0)
	{
		request.params = split_http_headers(data.substr(0, header_end));
		request.flags.params_closed = true;
	}

	u64 content_length = int_val(first(request.params["CONTENT_LENGTH"], "0"));
	if(content_length > limits.max_http_body_bytes)
		return(reject_http_connection(*connection, "HTTP/1.1 413 Payload Too Large", "request body is too large\n"));

	u64 request_size = header_end + 4 + content_length;
	if(request_size > limits.max_http_buffer_bytes())
		return(reject_http_connection(*connection, "HTTP/1.1 413 Payload Too Large", "request is too large\n"));
	if(data.length() < request_size)
		return(false);

	request.in = data.substr(header_end + 4, content_length);
	request.flags.input_closed = true;
	data.erase(0, request_size);
	return(true);
}

void
FastCGIServer::process_http_like_connection_input(Connection& connection)
{
	if(connection.type == 'H' && connection.is_websocket)
	{
		process_websocket_input(connection);
		return;
	}

	FastCGIRequest& request = *connection.requests[connection.client_socket];
	if(connection.type == 'C')
		process_cli_request(request, connection.input_buffer);
	else
		process_http_request(request, connection.input_buffer);
}

void
FastCGIServer::process_cli_request(FastCGIRequest& request, String& data)
{
	if(!parse_http_message(request, data))
		return;

	Connection* connection = client_sockets[request.resources.client_socket];
	if(!on_cli_complete)
	{
		connection->output_buffer += make_http_text_response(
			"HTTP/1.1 500 Internal Server Error",
			"UCE CLI dispatcher is not configured\n"
		);
	}
	else
	{
		on_cli_complete(request);
		assemble_output_buffer(request, connection);
	}
	connection->close_socket = true;
}

bool
FastCGIServer::validate_websocket_upgrade(FastCGIRequest& request, Connection& connection)
{
	String request_method = trim(request.params["REQUEST_METHOD"]);
	String request_protocol = trim(request.params["SERVER_PROTOCOL"]);
	String websocket_connection = to_lower(request.params["HTTP_CONNECTION"]);
	String websocket_host = trim(request.params["HTTP_HOST"]);
	String websocket_key = trim(request.params["HTTP_SEC_WEBSOCKET_KEY"]);
	String websocket_version = trim(request.params["HTTP_SEC_WEBSOCKET_VERSION"]);

	if(request_method != "GET")
		return(reject_http_connection(connection, "HTTP/1.1 405 Method Not Allowed", "websocket upgrades require GET"));
	if(request_protocol != "HTTP/1.1")
		return(reject_http_connection(connection, "HTTP/1.1 400 Bad Request", "websocket upgrades require HTTP/1.1"));
	if(websocket_host == "")
		return(reject_http_connection(connection, "HTTP/1.1 400 Bad Request", "missing Host header"));
	if(websocket_connection.find("upgrade") == String::npos)
		return(reject_http_connection(connection, "HTTP/1.1 400 Bad Request", "missing Connection: Upgrade header"));
	if(websocket_key == "")
		return(reject_http_connection(connection, "HTTP/1.1 400 Bad Request", "missing Sec-WebSocket-Key header"));
	if(!ws_is_valid_client_key(websocket_key))
		return(reject_http_connection(connection, "HTTP/1.1 400 Bad Request", "invalid Sec-WebSocket-Key header"));
	if(websocket_version == "")
		return(reject_http_connection(connection, "HTTP/1.1 400 Bad Request", "missing Sec-WebSocket-Version header"));
	if(websocket_version != "13")
		return(reject_http_connection(connection, "HTTP/1.1 426 Upgrade Required", "unsupported websocket version", "Sec-WebSocket-Version: 13\r\n"));

	return(true);
}

void
FastCGIServer::begin_websocket_upgrade(FastCGIRequest& request, Connection& connection, String& data)
{
	connection.is_websocket = true;
	connection.websocket_connection_id = std::to_string(getpid()) + ":" + std::to_string(connection.client_socket);
	connection.websocket_scope = first(
		request.params["SCRIPT_FILENAME"],
		request.params["DOCUMENT_URI"],
		request.params["REQUEST_URI"]
	);
	request.resources.is_websocket = true;
	request.resources.websocket_connection_id = connection.websocket_connection_id;
	request.resources.websocket_scope = connection.websocket_scope;
	request.resources.websocket_connection_state = &connection.websocket_state;
	request.connection.set_reference(&connection.websocket_state);

	connection.output_buffer +=
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Accept: " + ws_make_accept_key(trim(request.params["HTTP_SEC_WEBSOCKET_KEY"])) + "\r\n\r\n";

	if(!data.empty())
		process_websocket_input(connection);
}

void
FastCGIServer::process_http_request(FastCGIRequest& request, String& data)
{
	if(!parse_http_message(request, data))
		return;

	if(resolve_http_script_filename && request.params["SCRIPT_FILENAME"] == "" && request.params["DOCUMENT_URI"] != "")
	{
		String document_root = first(http_document_root, http_script_root());
		String document_uri = strip_leading_slashes(request.params["DOCUMENT_URI"]);
		for(String part : split(document_uri, "/"))
		{
			if(part == "..")
			{
				reject_http_connection(*client_sockets[request.resources.client_socket], "HTTP/1.1 404 Not Found", "script not found\n");
				return;
			}
		}
		String candidate = path_join(document_root, document_uri);
		String real_root = path_real(document_root);
		String real_candidate = path_real(candidate);
		if(real_root == "" || real_candidate == "" || !path_is_within(real_candidate, real_root))
		{
			reject_http_connection(*client_sockets[request.resources.client_socket], "HTTP/1.1 404 Not Found", "script not found\n");
			return;
		}
		request.params["DOCUMENT_ROOT"] = real_root;
		request.params["SCRIPT_FILENAME"] = real_candidate;
	}

	// Any .uce unit may expose WS(Request&). The runtime accepts WebSocket
	// upgrades based on HTTP headers and dispatches messages to that handler.
	if(to_lower(request.params["HTTP_UPGRADE"]) == "websocket")
	{
		Connection* connection = client_sockets[request.resources.client_socket];
		if(validate_websocket_upgrade(request, *connection))
			begin_websocket_upgrade(request, *connection, data);
	}
	else
	{
		if(request.stats.time_start == 0)
			request.stats.time_start = time_precise();
		on_complete(request);

		assemble_output_buffer(
			request,
			client_sockets[request.resources.client_socket]);

		printf("data written: %i bytes\n",
			client_sockets[request.resources.client_socket]->output_buffer.size());

		client_sockets[request.resources.client_socket]->close_socket = true;
	}
}

void
FastCGIServer::process_websocket_input(Connection& connection)
{
	TransportLimits limits = transport_limits();
	if(connection.input_buffer.length() > limits.max_websocket_buffer_bytes())
	{
		fail_websocket_connection(connection, 1009, "websocket input buffer is too large");
		return;
	}

	while(!connection.input_buffer.empty())
	{
		WSFrame frame;
		String error;
		if(!frame.parse(connection.input_buffer, error))
		{
			if(error != "")
				fail_websocket_connection(connection, 1002, error);
			return;
		}

		if(frame.payload_length > limits.max_websocket_frame_bytes)
		{
			fail_websocket_connection(connection, 1009, "websocket frame is too large");
			return;
		}

		connection.input_buffer.erase(0, frame.frame_length);

		if(!frame.mask_bit)
		{
			fail_websocket_connection(connection, 1002, "client frames must be masked");
			return;
		}

		bool is_control_frame = (frame.opcode & 0x08) != 0;
		if(is_control_frame && !frame.is_final_fragment)
		{
			fail_websocket_connection(connection, 1002, "control frames must not be fragmented");
			return;
		}

		switch(frame.opcode)
		{
			case 0x0:
			{
				if(connection.websocket_fragment_opcode == 0)
				{
					fail_websocket_connection(connection, 1002, "unexpected continuation frame");
					return;
				}

				if(connection.websocket_fragment_buffer.length() + frame.payload.length() > limits.max_websocket_message_bytes)
				{
					fail_websocket_connection(connection, 1009, "websocket message is too large");
					return;
				}
				connection.websocket_fragment_buffer += frame.payload;
				if(!frame.is_final_fragment)
					break;

				String payload = connection.websocket_fragment_buffer;
				u8 opcode = connection.websocket_fragment_opcode;
				connection.websocket_fragment_buffer = "";
				connection.websocket_fragment_opcode = 0;

				if(opcode == 0x1 && !ws_is_valid_utf8(payload))
				{
					fail_websocket_connection(connection, 1007, "invalid UTF-8 text message");
					return;
				}

				dispatch_websocket_message(connection, connection.client_socket, payload, opcode);
				break;
			}
			case 0x1:
			case 0x2:
			{
				if(connection.websocket_fragment_opcode != 0)
				{
					fail_websocket_connection(connection, 1002, "new data frame while fragmented message is active");
					return;
				}

				if(frame.payload.length() > limits.max_websocket_message_bytes)
				{
					fail_websocket_connection(connection, 1009, "websocket message is too large");
					return;
				}

				if(frame.is_final_fragment)
				{
					if(frame.opcode == 0x1 && !ws_is_valid_utf8(frame.payload))
					{
						fail_websocket_connection(connection, 1007, "invalid UTF-8 text message");
						return;
					}

					dispatch_websocket_message(connection, connection.client_socket, frame.payload, frame.opcode);
					break;
				}

				connection.websocket_fragment_buffer = frame.payload;
				connection.websocket_fragment_opcode = frame.opcode;
				break;
			}
			case 0x8:
			{
				if(frame.payload.length() == 1)
				{
					fail_websocket_connection(connection, 1002, "invalid close frame payload");
					return;
				}

				u16 status_code = 1000;
				String reason = "";
				if(frame.payload.length() >= 2)
				{
					status_code =
						((u16)(u8)frame.payload[0] << 8) |
						(u16)(u8)frame.payload[1];
					reason = frame.payload.substr(2);
					if(!is_valid_close_code(status_code))
					{
						fail_websocket_connection(connection, 1002, "invalid close status code");
						return;
					}
					if(!ws_is_valid_utf8(reason))
					{
						fail_websocket_connection(connection, 1007, "invalid UTF-8 close reason");
						return;
					}
				}

				close_websocket_connection(connection, status_code, reason);
				return;
			}
			case 0x9:
				if(!queue_websocket_frame(connection, ws_encode_frame(frame.payload, 0xA)))
					return;
				break;
			case 0xA:
				break;
			default:
				fail_websocket_connection(connection, 1002, "unsupported websocket opcode");
				return;
		}
	}
}

bool
FastCGIServer::websocket_send_to(String connection_id, String message, bool binary)
{
	for(auto& item : client_sockets)
	{
		Connection* connection = item.second;
		if(connection->is_websocket && connection->websocket_connection_id == connection_id)
			return(queue_websocket_payload(*connection, message, binary));
	}
	return(false);
}

u64
FastCGIServer::websocket_broadcast(String scope, String message, bool binary)
{
	u64 sent = 0;
	if(message.length() > transport_limits().max_websocket_message_bytes)
		return(sent);

	String frame = ws_encode_frame(message, binary ? 0x2 : 0x1);
	for(auto& item : client_sockets)
	{
		Connection* connection = item.second;
		if(!connection->is_websocket)
			continue;
		if(scope != "" && connection->websocket_scope != scope)
			continue;
		if(queue_websocket_frame(*connection, frame))
			sent += 1;
	}
	return(sent);
}

StringList
FastCGIServer::websocket_connection_ids(String scope)
{
	StringList result;
	for(auto& item : client_sockets)
	{
		Connection* connection = item.second;
		if(!connection->is_websocket)
			continue;
		if(scope != "" && connection->websocket_scope != scope)
			continue;
		result.push_back(connection->websocket_connection_id);
	}
	return(result);
}

bool
FastCGIServer::websocket_close(String connection_id, u16 status_code, String reason)
{
	for(auto& item : client_sockets)
	{
		Connection* connection = item.second;
		if(connection->is_websocket && connection->websocket_connection_id == connection_id)
		{
			close_websocket_connection(*connection, status_code, reason);
			return(true);
		}
	}
	return(false);
}

void
FastCGIServer::process_forever()
{
	for (;;)
	process();
}

void
FastCGIServer::read_fgci(Connection& connection)
{
	std::string::size_type n = 0;
	while (connection.input_buffer.size() - n >= FCGI_HEADER_LEN) {
	const FCGI_Header& header = *reinterpret_cast<const FCGI_Header*>(
				connection.input_buffer.data() + n);
	if (header.version != FCGI_VERSION_1) {
				connection.close_socket = true;
				break;
	}

	unsigned content_length =
				(header.contentLengthB1 << 8) + header.contentLengthB0;
	if (connection.input_buffer.size() - n <
					FCGI_HEADER_LEN + content_length + header.paddingLength)
				break;
	const char* content =
				connection.input_buffer.data() + n + FCGI_HEADER_LEN;

	RequestID request_id = (header.requestIdB1 << 8) + header.requestIdB0;

	switch (header.type)
	{
		case FCGI_GET_VALUES:
		{
			Pairs pairs = parse_pairs_fcgi(content, content_length);

			std::string::size_type base = connection.output_buffer.size();
			connection.output_buffer.push_back(FCGI_VERSION_1);
			connection.output_buffer.push_back(FCGI_GET_VALUES_RESULT);
			connection.output_buffer.append(FCGI_HEADER_LEN - 2, 0);

			for (Pairs::iterator it = pairs.begin(); it != pairs.end(); ++it)
				if (it->first == FCGI_MAX_CONNS)
				write_pair_fcgi(connection.output_buffer,
							it->first, std::string("100"));
				else if (it->first == FCGI_MAX_REQS)
				write_pair_fcgi(connection.output_buffer,
							it->first, std::string("1000"));
				else if (it->first == FCGI_MPXS_CONNS)
				write_pair_fcgi(connection.output_buffer,
							it->first, std::string("1"));

			std::string::size_type len = connection.output_buffer.size() - base;
			connection.output_buffer[base + 4] = (len >> 8) & 0xff;
			connection.output_buffer[base + 5] = len & 0xff;
			break;
		}
		case FCGI_BEGIN_REQUEST:
		{
			if (content_length < sizeof(FCGI_BeginRequestBody))
				break;
			const FCGI_BeginRequestBody& body =
				*reinterpret_cast<const FCGI_BeginRequestBody*>(content);

			if (!(body.flags & FCGI_KEEP_CONN))
				connection.close_responsibility = true;

			unsigned role = (body.roleB1 << 8) + body.roleB0;
			if (role != FCGI_RESPONDER)
			{
				FCGI_EndRequestRecord unknown;
				bzero(&unknown, sizeof(unknown));
				unknown.header.version = FCGI_VERSION_1;
				unknown.header.type = FCGI_END_REQUEST;
				unknown.header.contentLengthB0 = sizeof(unknown.body);
				unknown.body.protocolStatus = FCGI_UNKNOWN_ROLE;
				connection.output_buffer.append(
				reinterpret_cast<const char*>(&unknown), sizeof(unknown));
				if (connection.close_responsibility)
					connection.close_socket = true;
				break;
			}

			{
				RequestList::iterator it = connection.requests.find(request_id);
				if (it != connection.requests.end())
				{
					//printf("- delete request object\n");
					//switch_to_arena(it->second->mem);
					delete it->second;
					//switch_to_system_alloc();
					connection.requests.erase(it);
				}
			}

			if(connection.requests.size() > 1)
			{
				printf("(!) %i requests in flight at the same time!\n", connection.requests.size());
			}

			FastCGIRequest* new_request = new FastCGIRequest();
			new_request->resources.client_socket = connection.client_socket;
			new_request->resources.server_socket = connection.server_socket;
			new_request->stats.time_init = time_precise();
			connection.requests[request_id] = new_request;

			break;
		}
		case FCGI_ABORT_REQUEST:
		{
			RequestList::iterator it = connection.requests.find(request_id);
			if (it == connection.requests.end())
				break;

			FCGI_EndRequestRecord aborted;
			bzero(&aborted, sizeof(aborted));
			aborted.header.version = FCGI_VERSION_1;
			aborted.header.type = FCGI_END_REQUEST;
			aborted.header.contentLengthB0 = sizeof(aborted.body);
			aborted.body.appStatusB0 = 1;
			aborted.body.protocolStatus = FCGI_REQUEST_COMPLETE;
			connection.output_buffer.append(
				reinterpret_cast<const char*>(&aborted), sizeof(aborted));
			if (connection.close_responsibility)
				connection.close_socket = true;

			delete it->second;
			connection.requests.erase(it);
			break;
		}
		case FCGI_PARAMS:
		{
			RequestList::iterator it = connection.requests.find(request_id);
			if (it == connection.requests.end())
				break;

			FastCGIRequest& request = *it->second;
			//switch_to_arena(it->second->mem);
			if (!request.flags.params_closed)
				if (content_length != 0) {
					if(request.resources.params_buffer.size() + content_length > transport_limits().max_http_header_bytes)
					{
						request.flags.status = 1;
						connection.close_socket = true;
						break;
					}
					request.resources.params_buffer.append(content, content_length);
				}
				else {
					request.params = parse_pairs_fcgi(
						request.resources.params_buffer.data(),
						request.resources.params_buffer.size());
					request.resources.params_buffer.clear();
					request.flags.params_closed = true;
					request.stats.time_params = time_precise();
					//std::cout << "Params " << var_dump(request.params) << "\n";
					request.flags.status = on_request(request);
					if (request.flags.status == 0 && !request.in.empty())
					{
						request.flags.status = on_data(request);
						if (request.flags.status == 0 && request.flags.input_closed)
							request.flags.status = on_complete(request);
					}
					request_write_fgci(connection, request_id, request);
				}
			//switch_to_system_alloc();
			break;
		}
		case FCGI_STDIN:
		{
			RequestList::iterator it = connection.requests.find(request_id);
			if (it == connection.requests.end())
				break;

			FastCGIRequest& request = *it->second;
			//switch_to_arena(it->second->mem);
			if (!request.flags.input_closed)
				if (content_length != 0) {
					if(request.in.size() + content_length > transport_limits().max_http_body_bytes)
					{
						request.flags.status = 1;
						connection.close_socket = true;
						break;
					}
					request.in.append(content, content_length);
					if (request.flags.params_closed && request.flags.status == 0)
					{
						request.flags.status = on_data(request);
						request_write_fgci(connection, request_id, request);
					}
				} else {
					request.flags.input_closed = true;
					request.stats.time_input = time_precise();
					if (request.flags.params_closed && request.flags.status == 0) {
								request.flags.status = on_complete(request);
								request_write_fgci(connection, request_id, request);
					}
				}
			//switch_to_system_alloc();
			break;
		}
		case FCGI_DATA:
					break;
		default:
		{
			FCGI_UnknownTypeRecord unknown;
			bzero(&unknown, sizeof(unknown));
			unknown.header.version = FCGI_VERSION_1;
			unknown.header.type = FCGI_UNKNOWN_TYPE;
			unknown.header.contentLengthB0 = sizeof(unknown.body);
			unknown.body.type = header.type;
			connection.output_buffer.append(
				reinterpret_cast<const char*>(&unknown), sizeof(unknown));
		}
	}

	n += FCGI_HEADER_LEN + content_length + header.paddingLength;
	}

	connection.input_buffer.erase(0, n);
}

void
FastCGIServer::assemble_output_buffer(FastCGIRequest& request, Connection* connection)
{
	request.out =
		http_status_line_clean(request.response_code)+"\r\n"+
		render_header_map(request.header) +
		render_set_cookie_headers(request.set_cookies) +
		"\r\n";

	for(auto obs : request.ob_stack)
	{
		request.out += obs->str();
		delete obs;
	}
	if(request.out.length() > transport_limits().max_response_bytes)
	{
		request.set_status(500, "Response Too Large");
		request.header.clear();
		request.header["Content-Type"] = "text/plain; charset=utf-8";
		request.out = http_status_line_clean(request.response_code) + "\r\n" + render_header_map(request.header) + "\r\nresponse exceeded configured output limit\n";
	}
	request.ob_stack.clear();
	request.flags.output_closed = true;
	request.stats.time_end = time_precise();
	if(request.flags.log_request)
	{
		const char* transport = !connection ? "fastcgi" :
			connection->type == 'H' ? "http" :
			connection->type == 'C' ? "cli" : "unknown";
		auto elapsed_us = [&](f64 from, f64 to) -> f64 {
			return(from > 0 && to >= from ? (to - from) * 1000000.0 : 0.0);
		};
		printf("(r) pid:%i\t%s\t%0.6fs\tfps:%0.0f\tout:%0.1fkB\tmem:%0.0f/%0.0fkB\twasm-ready:%0.3fms\twasm:%0.3fms\tworkspace:%0.3fms\tinvoke:%0.3fms\tcollect:%0.3fms\tpost:%0.3fms\ttransport:%s\n",
			my_pid,
			request.params["REQUEST_URI"].c_str(),
			request.stats.time_end - request.stats.time_start,
			1.0 / (request.stats.time_end - request.stats.time_start),
			(f32)(request.out.length()/1024),
			(f32)(request.stats.mem_high/1024),
			(f32)(request.stats.mem_alloc/1024),
			elapsed_us(request.stats.time_start, request.stats.wasm_handler_ready) / 1000.0,
			elapsed_us(request.stats.wasm_backend_started, request.stats.wasm_backend_finished) / 1000.0,
			(f64)request.stats.wasm_workspace_complete_us / 1000.0,
			(f64)request.stats.wasm_entry_invoke_us / 1000.0,
			(f64)request.stats.wasm_output_collect_us / 1000.0,
			elapsed_us(request.stats.wasm_backend_finished, request.stats.time_end) / 1000.0,
			transport
		);
	}

	if(connection)
	{
		connection->output_buffer.clear();
		connection->output_buffer.append(request.out);
		request.out.clear();
	}

}

void
FastCGIServer::request_write_fgci(Connection& connection, RequestID id,
												FastCGIRequest& request)
{
	if (!request.out.empty())
	{
		write_data_fcgi(connection.output_buffer, id, request.out, FCGI_STDOUT);
		//switch_to_arena(request.mem);
		request.out.clear();
		//switch_to_system_alloc();
	}
	if (!request.err.empty())
	{
		write_data_fcgi(connection.output_buffer, id, request.err, FCGI_STDERR);
		//switch_to_arena(request.mem);
		request.err.clear();
		//switch_to_system_alloc();
	}
	if ((request.flags.input_closed || request.flags.status != 0) &&
		!request.flags.output_closed)
	{
		//switch_to_arena(request.mem);
		assemble_output_buffer(request);

		//switch_to_system_alloc();
		write_data_fcgi(connection.output_buffer, id, request.out, FCGI_STDOUT);
		write_data_fcgi(connection.output_buffer, id, request.err, FCGI_STDERR);
		request.out.clear();
		request.err.clear();

		FCGI_EndRequestRecord complete;
		bzero(&complete, sizeof(complete));
		complete.header.version = FCGI_VERSION_1;
		complete.header.type = FCGI_END_REQUEST;
		complete.header.requestIdB1 = (id >> 8) & 0xff;
		complete.header.requestIdB0 = id & 0xff;
		complete.header.contentLengthB0 = sizeof(complete.body);
		complete.body.appStatusB3 = (request.flags.status >> 24) & 0xff;
		complete.body.appStatusB2 = (request.flags.status >> 16) & 0xff;
		complete.body.appStatusB1 = (request.flags.status >> 8) & 0xff;
		complete.body.appStatusB0 = request.flags.status & 0xff;
		complete.body.protocolStatus = FCGI_REQUEST_COMPLETE;
		connection.output_buffer.append(
					reinterpret_cast<const char*>(&complete), sizeof(complete));
		if (connection.close_responsibility)
					connection.close_socket = true;


		//printf("- output done\n");
	}
	//switch_to_system_alloc();
}


void
FastCGIServer::write_fgci(Connection& connection)
{
	for (RequestList::iterator it = connection.requests.begin();
				it != connection.requests.end();)
	{
		request_write_fgci(connection, it->first, *it->second);
		if (it->second->flags.params_closed && it->second->flags.input_closed)
		{
			//switch_to_arena(it->second->mem);
			//printf("- write_fgci close\n");
			delete it->second;
			//switch_to_system_alloc();
			connection.requests.erase(it++);
		}
		else
			++it;
	}
}


FastCGIServer::Pairs
FastCGIServer::parse_pairs_fcgi(const char* data, std::string::size_type n)
{
	Pairs pairs;

	const unsigned char* u = reinterpret_cast<const unsigned char*>(data);

	for (std::string::size_type m = 0; m < n;) {
	std::string::size_type name_length, value_length;

	if (u[m] >> 7) {
				if (n - m < 4)
					break;
				name_length = ((u[m] & 0x7f) << 24) + (u[m + 1] << 16) +
					(u[m + 2] << 8) + u[m + 3];
				m += 4;
	} else
				name_length = u[m++];
	if (m >= n)
				break;

	if (u[m] >> 7) {
				if (n - m < 4)
					break;
				value_length = ((u[m] & 0x7f) << 24) + (u[m + 1] << 16) +
					(u[m + 2] << 8) + u[m + 3];
				m += 4;
	} else
				value_length = u[m++];

	if (n - m < name_length)
				break;
	std::string key(data + m, name_length);
	m += name_length;

	if (n - m < value_length)
				break;
	pairs.insert(Pairs::value_type(
				key, std::string(data + m, value_length)));
	m += value_length;
	}

	return pairs;
}


void
FastCGIServer::write_pair_fcgi(std::string& buffer,
							const std::string& key, const std::string& value)
{
	if (key.size() > 0x7f) {
		buffer.push_back(0x80 + ((key.size() >> 24) & 0x7f));
		buffer.push_back((key.size() >> 16) & 0xff);
		buffer.push_back((key.size() >> 8) & 0xff);
		buffer.push_back(key.size() & 0xff);
	} else
		buffer.push_back(key.size());

	if (value.size() > 0x7f) {
		buffer.push_back(0x80 + ((value.size() >> 24) & 0x7f));
		buffer.push_back((value.size() >> 16) & 0xff);
		buffer.push_back((value.size() >> 8) & 0xff);
		buffer.push_back(value.size() & 0xff);
	} else
		buffer.push_back(value.size());

	buffer.append(key);
	buffer.append(value);
}


void
FastCGIServer::write_data_fcgi(std::string& buffer, RequestID id,
							const std::string& input, unsigned char type)
{
	FCGI_Header header;
	bzero(&header, sizeof(header));
	header.version = FCGI_VERSION_1;
	header.type = type;
	header.requestIdB1 = (id >> 8) & 0xff;
	header.requestIdB0 = id & 0xff;

	for (std::string::size_type n = 0;;) {
		std::string::size_type written = std::min(input.size() - n,
					(std::string::size_type)0xffffu);

		header.contentLengthB1 = written >> 8;
		header.contentLengthB0 = written & 0xff;
		header.paddingLength = (8 - (written % 8)) % 8;
		buffer.append(
					reinterpret_cast<const char*>(&header), sizeof(header));
		buffer.append(input.data() + n, written);
		buffer.append(header.paddingLength, 0);

		n += written;
		if (n == input.size())
				break;
	}
}
