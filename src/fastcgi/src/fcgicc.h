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
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
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


#ifndef FCGICC_H
#define FCGICC_H

#include <map>
#include <string>
#include <vector>

struct FastCGIServer {
	~FastCGIServer();
	void shutdown();

	// called when the parameters and standard input have been receieved
	std::function<int(FastCGIRequest&)> on_request = 0;
	std::function<int(FastCGIRequest&)> on_data = 0;
	std::function<int(FastCGIRequest&)> on_complete = 0;
	std::function<int(FastCGIRequest&)> on_cli_complete = 0;
	std::function<int(FastCGIRequest&, const String&, u8)> on_websocket_message = 0;

	int listen(unsigned tcp_port);
	int listen(unsigned tcp_port, const std::string& bind_address);
	int listen_http(unsigned tcp_port);
	int listen_http(unsigned tcp_port, const std::string& bind_address);
	int listen_cli(const std::string& local_path);
	int listen(const std::string& local_path);
	int adopt_listener(int socket_handle, char type = 'F');

	void process(int timeout_ms = -1); // timeout_ms<0 blocks forever
	void process_forever();
	int calls_until_termination = 8; // set this to -1 to never terminate
	bool resolve_http_script_filename = true;
	String http_document_root = "";

	typedef unsigned RequestID;
	typedef std::map<RequestID, FastCGIRequest*> RequestList;

	struct Connection {
		RequestList requests;
		u64 client_socket = 0;
		u64 server_socket = 0;
		std::string input_buffer;
		std::string output_buffer;
		bool close_responsibility = false;
		bool close_socket = false;
		bool is_websocket = false;
		String websocket_connection_id;
		String websocket_scope;
		DValue websocket_state;
		String websocket_fragment_buffer;
		u8 websocket_fragment_opcode = 0;
		f64 opened_at = 0;
		f64 last_activity_at = 0;
		char type = 'F'; // F = FastCGI, H = HttpServer/WebSocket, C = CLI-over-HTTP Unix socket
	};

	typedef StringMap Pairs;

	std::vector<int> server_sockets;
	std::vector<std::string> listen_unlink;

	std::map<int, char> server_socket_types;
	std::map<int, Connection*> client_sockets;

	void close_http_listeners();
	void read_fgci(Connection&);
	static bool is_http_like_type(char type);
	Connection* open_client_connection(int server_socket, int client_socket);
	void enforce_connection_timeouts(Connection& connection);
	bool reject_http_connection(Connection& connection, String status_line, String body, String extra_headers = "");
	bool queue_websocket_frame(Connection& connection, String frame);
	bool queue_websocket_payload(Connection& connection, String message, bool binary = false);
	void process_websocket_input(Connection&);
	bool validate_websocket_upgrade(FastCGIRequest& request, Connection& connection);
	void begin_websocket_upgrade(FastCGIRequest& request, Connection& connection, String& data);
	void fail_websocket_connection(Connection& connection, u16 status_code, String reason = "");
	void close_websocket_connection(Connection& connection, u16 status_code = 1000, String reason = "");
	void dispatch_websocket_message(Connection& connection, RequestID request_id, String payload, u8 opcode);
	bool websocket_send_to(String connection_id, String message, bool binary = false);
	u64 websocket_broadcast(String scope, String message, bool binary = false);
	StringList websocket_connection_ids(String scope = "");
	bool websocket_close(String connection_id, u16 status_code = 1000, String reason = "");
	static void request_write_fgci(Connection&, RequestID, FastCGIRequest&);
	static void write_fgci(Connection&);
	static Pairs parse_pairs_fcgi(const char*, std::string::size_type);
	static void write_pair_fcgi(std::string& buffer,
		const std::string& key, const std::string&);
	static void write_data_fcgi(std::string& buffer, RequestID id,
		const std::string& input, unsigned char type);
	static void assemble_output_buffer(FastCGIRequest& request, Connection* connection = 0);
	int send_output_buffer(Connection& con);
	bool parse_http_message(FastCGIRequest& request, String& data);
	void process_http_request(FastCGIRequest& request, String& data);
	void process_cli_request(FastCGIRequest& request, String& data);
	void process_http_like_connection_input(Connection& connection);

};

#endif // !FCGICC_H
