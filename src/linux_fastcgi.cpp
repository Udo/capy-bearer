#include "lib/bearer_lib.cpp"
// The wasm backend is its own object (src/wasm/wasm_module.cpp → wasm.o); the
// main object only needs its declarations, so editing the wasm runtime no
// longer recompiles the whole native TU.
#include "wasm/backend.h"
// Minimal FastCGI client: connection brokers forward to a clean-engine worker.
#include "lib/fcgi_forward.h"
#include <csetjmp>
#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>

ServerState server_state;

#include "fastcgi/src/fcgicc.cc"

FastCGIServer server;
std::vector<pid_t> proactive_compiler_pids;
pid_t priority_compiler_pid = 0;

// The central WS broker process: owns the WS port + every connection, forwards
// renders to the worker pool over bearer.sock, and applies ws_* commands flushed
// back from workers. ws_broker_outbound holds in-flight async render
// connections and their enqueue timestamps.
struct WsBrokerOutbound
{
	String pending;
	f64 started_at;
};

FastCGIServer ws_broker;
pid_t ws_broker_pid = 0;
std::map<int, WsBrokerOutbound> ws_broker_outbound;
static sigjmp_buf request_fault_jmp;
static volatile sig_atomic_t request_fault_active = 0;
static volatile sig_atomic_t request_fault_signal = 0;
static Request* request_fault_request = 0;
// Raw frame pointers only: the signal handler must not allocate, so frames are
// captured here and symbolized after the siglongjmp.
static void* request_fault_frames[64];
static volatile sig_atomic_t request_fault_frame_count = 0;

void close_inherited_server_sockets();
u64 request_seed_from_time(f64 time_value);

using WasmInvocationClock = std::chrono::steady_clock;
u64 wasm_invocation_remaining_ms(WasmInvocationClock::time_point deadline)
{
	auto now = WasmInvocationClock::now();
	if(now >= deadline)
		return(0);
	return((u64)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

void prepare_request_body_maps(Request& request);

Request* set_active_request(Request& request)
{
	Request* previous_context = context;
	context = &request;
	return(previous_context);
}

void restore_active_request(Request* previous_context)
{
	context = previous_context;
}

String request_status_line(Request& request, int status_code, String reason)
{
	String status = std::to_string(status_code) + " " + reason;
	if(request.params["GATEWAY_INTERFACE"] != "")
		return("Status: " + status);
	return("HTTP/1.1 " + status);
}

void clear_request_output(Request& request)
{
	for(auto* stream : request.ob_stack)
		delete stream;
	request.ob_stack.clear();
	request.ob_start();
}

bool render_wasm_error_page(Request& request, String config_key, s32 status_code, String status_reason, DValue error_info,
	WasmInvocationClock::time_point invocation_deadline, u64 invocation_budget_ms)
{
	if(!(request.params["REQUEST_METHOD"] != "" && request.ob && request.ob_stack.size() > 0))
		return(false);
	if(!request.server || request.resources.error_page_active)
		return(false);
	String unit_file = compiler_error_page_unit(&request, config_key);
	if(unit_file == "")
		return(false);

	String previous_response_code = request.response_code;
	s32 previous_status = request.flags.status;
	String previous_content_type = request.header["Content-Type"];

	request.resources.error_page_active = true;
	request.call["error"] = error_info;
	request.set_status(status_code, status_reason);
	request.header["Content-Type"] = first(request.server->config["CONTENT_TYPE"], "text/html; charset=utf-8");

	String unit = compiler_normalize_unit_path(&request, unit_file);
	bool compile_timed_out = false;
	if(!wasm_backend_should_handle(request, unit))
	{
		u64 remaining_ms = wasm_invocation_remaining_ms(invocation_deadline);
		if(remaining_ms == 0)
			compile_timed_out = true;
		else
			get_shared_unit_bounded(&request, unit, remaining_ms, &compile_timed_out);
	}

	ob_start();
	String wasm_error = "";
	if(compile_timed_out)
		wasm_error = "BEARER_INVOCATION_TIMEOUT: wasm invocation exceeded " + std::to_string(invocation_budget_ms) + " ms";
	else if(wasm_backend_should_handle(request, unit))
		wasm_error = wasm_backend_serve(request, unit, "render", wasm_invocation_remaining_ms(invocation_deadline));
	else
		wasm_error = "error page wasm unit unavailable after compile: " + unit_file;
	String html = ob_get_close();
	request.resources.error_page_active = false;

	if(wasm_error != "")
	{
		printf("(!) configured %s page %s failed to render: %s\n", config_key.c_str(), unit_file.c_str(), trim(wasm_error).c_str());
		request.response_code = previous_response_code;
		request.flags.status = previous_status;
		request.header["Content-Type"] = previous_content_type;
		request.call.remove("error");
		return(false);
	}
	// Rendering the error-page unit set the response to its own (200) status;
	// re-assert the error status + html content-type so the client sees e.g. 500.
	request.set_status(status_code, status_reason);
	request.header["Content-Type"] = first(request.server->config["CONTENT_TYPE"], "text/html; charset=utf-8");
	print(html);
	return(true);
}

void render_request_failure(Request& request, String title, String details, String trace,
	WasmInvocationClock::time_point invocation_deadline, u64 invocation_budget_ms, int status_code = 500)
{
	request.response_code = request_status_line(request, status_code, "Internal Server Error");
	request.header.clear();
	request.set_cookies.clear();
	request.header["Content-Type"] = "text/plain; charset=utf-8";
	request.err.clear();

	Request* previous_context = set_active_request(request);
	clear_request_output(request);

	if(!request.resources.is_cli)
	{
		DValue error_info;
		error_info["type"] = (title == "function disabled by server policy") ? "policy_blocked" : "runtime_error";
		error_info["title"] = title;
		error_info["details"] = details;
		error_info["source"] = request.params["SCRIPT_FILENAME"];
		error_info["request_uri"] = first(request.params["REQUEST_URI"], request.params["SCRIPT_FILENAME"]);
		if(request.server && request.params["SCRIPT_FILENAME"] != "")
			error_info["generated_cpp"] = compiler_generated_cpp_path(&request, request.params["SCRIPT_FILENAME"]);
		if(request_fault_signal != 0)
		{
			error_info["signal"] = (f64)request_fault_signal;
			error_info["signal_name"] = signal_name((int)request_fault_signal);
		}
		error_info["trace"] = trace;
		if(render_wasm_error_page(request, "page_runtime_error", status_code, "Internal Server Error", error_info,
			invocation_deadline, invocation_budget_ms))
		{
			request.err = "BEARER runtime error: " + title + (details != "" ? " (" + details + ")" : "");
			restore_active_request(previous_context);
			return;
		}
	}

	String body;
	body += "BEARER runtime error\n";
	body += "Request: " + first(request.params["REQUEST_URI"], request.params["SCRIPT_FILENAME"]) + "\n";
	body += "Script: " + request.params["SCRIPT_FILENAME"] + "\n";
	body += "Error: " + title + "\n";
	if(details != "")
		body += "Details: " + details + "\n";
	if(request.server && request.params["SCRIPT_FILENAME"] != "")
	{
		String generated = compiler_generated_cpp_path(&request, request.params["SCRIPT_FILENAME"]);
		body += "Generated C++: " + generated + "\n";
		body += "Hint: if this came from template code, inspect the generated C++ and the nearest .uce literal/code delimiter before changing runtime code.\n";
	}
	else
		body += "Hint: inspect the requested .uce file, its generated C++, and any component/unit called immediately before this failure.\n";
	if(request_fault_signal != 0)
	{
		String sig_label = signal_name((int)request_fault_signal);
		body += "Signal: " + std::to_string((int)request_fault_signal);
		if(sig_label != "")
			body += " (" + sig_label + ")";
		body += "\n";
	}
	if(trace != "")
		body += "\nTrace:\n" + trace;

	print(body);
	request.err = body;
	request.flags.status = status_code;
	restore_active_request(previous_context);
}

void on_request_fault_signal(int sig)
{
	request_fault_signal = sig;
	if(request_fault_active && request_fault_request)
	{
		request_fault_frame_count = backtrace(request_fault_frames, 64);
		siglongjmp(request_fault_jmp, 1);
	}
	on_segfault(sig);
}

void install_request_fault_handlers()
{
	signal(SIGSEGV, on_request_fault_signal);
	signal(SIGABRT, on_request_fault_signal);
	signal(SIGBUS, on_request_fault_signal);
	signal(SIGILL, on_request_fault_signal);
	signal(SIGFPE, on_request_fault_signal);
	signal(SIGALRM, on_request_fault_signal);
}

void restore_request_fault_handlers()
{
	signal(SIGSEGV, on_segfault);
	signal(SIGABRT, on_segfault);
	signal(SIGBUS, on_segfault);
	signal(SIGILL, on_segfault);
	signal(SIGFPE, on_segfault);
	signal(SIGALRM, on_segfault);
}


String current_ws_scope()
{
	if(!context)
		return("");
	return(first(
		context->resources.websocket_scope,
		context->params["SCRIPT_FILENAME"]
	));
}

String normalize_ws_scope(String scope)
{
	if(scope == "")
		return(current_ws_scope());
	if(scope[0] == '/')
		return(scope);
	return(expand_path(scope, cwd_get()));
}

String ws_connection_id()
{
	if(!context)
		return("");
	return(context->resources.websocket_connection_id);
}

String ws_message()
{
	if(!context)
		return("");
	return(context->in);
}

String ws_scope()
{
	return(current_ws_scope());
}

u8 ws_opcode()
{
	if(!context)
		return(0);
	return(context->resources.websocket_opcode);
}

bool ws_is_binary()
{
	if(!context)
		return(false);
	return(context->resources.websocket_is_binary);
}

StringList ws_connections(String scope)
{
	return(server.websocket_connection_ids(normalize_ws_scope(scope)));
}

u64 ws_connection_count(String scope)
{
	return(ws_connections(scope).size());
}

bool ws_send(String message, bool binary, String scope)
{
	return(server.websocket_broadcast(normalize_ws_scope(scope), message, binary) > 0);
}

bool ws_send_to(String connection_id, String message, bool binary)
{
	return(server.websocket_send_to(connection_id, message, binary));
}

bool ws_close(String connection_id)
{
	if(connection_id == "")
		connection_id = ws_connection_id();
	if(connection_id == "")
		return(false);
	return(server.websocket_close(connection_id));
}

bool cli_path_is_safe(String command)
{
	for(auto& segment : split(command, "/"))
	{
		if(segment == "..")
			return(false);
	}
	return(true);
}

String cli_resolve_unit_path(Request& request, String command, String& document_root)
{
	document_root = trim(first(request.params["DOCUMENT_ROOT"], request.server ? request.server->config["HTTP_DOCUMENT_ROOT"] : String("")));
	if(document_root == "" && request.server)
	{
		String site_directory = trim(request.server->config["SITE_DIRECTORY"]);
		String compiler_root = trim(request.server->config["COMPILER_SYS_PATH"]);
		if(site_directory != "")
			document_root = site_directory[0] == '/' ? site_directory : path_join(compiler_root, site_directory);
	}
	if(document_root == "")
		document_root = cwd_get();
	if(document_root.length() > 1 && document_root[document_root.length()-1] == '/')
		document_root.resize(document_root.length()-1);

	String script_filename = document_root + command;
	if(file_exists(script_filename))
		return(script_filename);

	String site_filename = document_root + "/site" + command;
	if(file_exists(site_filename))
	{
		document_root = document_root + "/site";
		return(site_filename);
	}

	return("");
}

u64 request_seed_from_time(f64 time_value)
{
	u64 bits = 0;
	static_assert(sizeof(bits) == sizeof(time_value));
	memcpy(&bits, &time_value, sizeof(bits));
	return(gen_noise64(bits));
}

void prepare_request_body_maps(Request& request)
{
	if(request.server)
		request.params["BEARER_BIN_DIRECTORY"] = request.server->config["BIN_DIRECTORY"];

	String route_path;
	request.get = parse_query(request.params["QUERY_STRING"], &route_path);
	request_populate_context_params_from_route(request, route_path);
	if(request.params["HTTP_COOKIE"].length() > 0)
		request.cookies = parse_cookies(request.params["HTTP_COOKIE"]);

	String ct_info = request.params["CONTENT_TYPE"];
	String ct_type = nibble(ct_info, ";");
	if(request.params["REQUEST_METHOD"] == "POST")
	{
		if(ct_type == "multipart/form-data")
		{
			nibble("boundary=", ct_info);
			request.post = parse_multipart(request.in, String("--")+ct_info, request.uploaded_files);
		}
		else
		{
			request.post = parse_query(request.in);
		}
	}
}

int handle_cli_complete(FastCGIRequest& request)
{
	Request* previous_context = set_active_request(request);
	server_state.request_count += 1;
	request.server = &server_state;
	request.resources.is_cli = true;
	request.params["BEARER_CLI"] = "1";
	request.stats.time_start = time_precise();
	request.header["Content-Type"] = "text/plain; charset=utf-8";
	request.random_index = 0;
	request.random_seed = request_seed_from_time(request.stats.time_start);
	request.ob_start();
	u64 invocation_budget_ms = wasm_backend_invocation_timeout_ms(request);
	auto invocation_deadline = WasmInvocationClock::now() + std::chrono::milliseconds(invocation_budget_ms);

	String method = trim(request.params["REQUEST_METHOD"]);
	String command = trim(first(request.params["DOCUMENT_URI"], request.params["REQUEST_URI"]));

	try
	{
		if(method != "GET" && method != "POST")
		{
			request.set_status(405, "Method Not Allowed");
			request.header["Allow"] = "GET, POST";
			print("BEARER CLI socket accepts GET and POST commands only\n");
		}
		else if(command == "/" || command == "/help")
		{
			print("BEARER CLI command socket\n\nAvailable test hooks:\n  GET /ping\n  GET /status\n");
		}
		else if(command == "/ping" || command == "/test")
		{
			print("bearer-cli: ok\n");
		}
		else if(command == "/status")
		{
			print("pid=", std::to_string(getpid()), "\nclients=", std::to_string(server.client_sockets.size()), "\n");
		}
		else if(command.length() >= 4 && command.substr(command.length() - 4) == ".uce")
		{
			if(!cli_path_is_safe(command))
			{
				request.set_status(400, "Bad Request");
				print("invalid BEARER CLI unit path\n");
			}
			else
			{
				String document_root;
				String script_filename = cli_resolve_unit_path(request, command, document_root);
				if(script_filename == "")
				{
					request.set_status(404, "Not Found");
					print("BEARER CLI unit not found: ", command, "\n");
				}
				else
				{
					request.params["DOCUMENT_ROOT"] = document_root;
					request.params["SCRIPT_FILENAME"] = script_filename;
					prepare_request_body_maps(request);
					request.props = DValue();
					// The CLI socket path installs no native fault handler, so the
					// wasm worker (whose traps are signal-based) can run directly.
					String cli_unit = compiler_normalize_unit_path(&request, script_filename);
					// W7e: compile a cold/stale unit on demand; native execution has
					// been removed, so a unit that still cannot be served by wasm is a
					// request failure instead of a fallback path.
					SharedUnit* cli_compile_state = 0;
					bool cli_compile_timed_out = false;
					bool cli_wasm_ready = wasm_backend_should_handle(request, cli_unit);
					if(!cli_wasm_ready)
					{
						u64 remaining_ms = wasm_invocation_remaining_ms(invocation_deadline);
						if(remaining_ms == 0)
							cli_compile_timed_out = true;
						else
							cli_compile_state = get_shared_unit_bounded(&request, cli_unit, remaining_ms, &cli_compile_timed_out);
						if(!cli_compile_timed_out)
							cli_wasm_ready = wasm_backend_should_handle(request, cli_unit);
					}
					if(cli_compile_timed_out)
					{
						request.set_status(500, "Internal Server Error");
						print("BEARER_INVOCATION_TIMEOUT: wasm invocation exceeded ", invocation_budget_ms, " ms\n");
					}
					else if(cli_wasm_ready)
					{
						String wasm_error = wasm_backend_serve(request, cli_unit, "cli", wasm_invocation_remaining_ms(invocation_deadline));
						if(wasm_error != "")
						{
							request.set_status(500, "Internal Server Error");
							print("BEARER CLI wasm error: ", wasm_error, "\n");
						}
					}
					else
					{
						request.set_status(500, "Internal Server Error");
						String compile_error = cli_compile_state ? first(cli_compile_state->compiler_messages, cli_compile_state->compile_error_status) : String("");
						print("BEARER CLI wasm unit unavailable after compile: ", script_filename);
						if(compile_error != "")
							print(": ", compile_error);
						print("\n");
					}
				}
			}
		}
		else
		{
			request.set_status(404, "Not Found");
			print("unknown BEARER CLI command: ", command, "\n");
		}
	}
	catch(const std::exception& e)
	{
		render_request_failure(request, "uncaught exception during CLI request", e.what(), backtrace_capture(32, 1),
			invocation_deadline, invocation_budget_ms, 500);
	}
	catch(...)
	{
		render_request_failure(request, "unknown uncaught exception during CLI request", "", backtrace_capture(32, 1),
			invocation_deadline, invocation_budget_ms, 500);
	}

	for(auto &f : request.uploaded_files)
		file_unlink(f.tmp_name);
	cleanup_mysql_connections();
	cleanup_sqlite_connections();
	restore_active_request(previous_context);
	return(request.flags.status);
}

int handle_request(FastCGIRequest& request) {
	// This is always the first event to occur.  It occurs when the
	// server receives all parameters.  There may be more data coming on the
	// standard input stream.
	if(request.stats.time_init == 0)
		request.stats.time_init = time_precise();
	if(request.params.count("REQUEST_URI"))
		return(0);  // OK, continue processing
	else
		return(1);  // stop processing and return error code
}

int handle_data(FastCGIRequest& request) {
    // Request bodies are accumulated by the FastCGI transport and parsed once
    // the input stream is closed in handle_complete().
    return 0;
}

int handle_complete(FastCGIRequest& request) {
	Request* previous_context = set_active_request(request);
	server_state.request_count += 1;
	request.server = &server_state;
	if(request.stats.time_init == 0)
		request.stats.time_init = time_precise();
	request.stats.time_start = time_precise();
	//request.stats.mem_alloc = 0;
	//request.stats.mem_high = 0;
    request.header["Content-Type"] = (request.resources.is_cli ? "text/plain; charset=utf-8" : context->server->config["CONTENT_TYPE"]);
	request.random_index = 0;
	request.random_seed = request_seed_from_time(request.stats.time_start);
	request.ob_start();
	u64 invocation_budget_ms = wasm_backend_invocation_timeout_ms(request);
	auto invocation_deadline = WasmInvocationClock::now() + std::chrono::milliseconds(invocation_budget_ms);
	request_fault_request = &request;
	request_fault_active = 1;
	request_fault_signal = 0;
	request_fault_frame_count = 0;
	install_request_fault_handlers();

	String failure_title = "";
	String failure_details = "";
	String failure_trace = "";

	if(sigsetjmp(request_fault_jmp, 1) != 0)
	{
		failure_title = "fatal signal during request";
		failure_details = "worker recovered before closing the upstream connection";
		failure_trace = backtrace_get_frames(request_fault_frames, request_fault_frame_count, 1);
		// The siglongjmp skipped UnitInvocationScope's destructor, so the
		// worker may still sit in the crashed unit's source directory.
		cwd_set(process_start_directory());
	}
	else
	{
		try
		{
			prepare_request_body_maps(request);
			request.props = DValue();

			// Suspend the native SIGSEGV/SIGILL request recovery handler around a
			// wasm invocation: Wasmtime uses host signals internally to implement
			// guest traps, and the native handler would otherwise turn a clean
			// guest trap into a native fatal signal. Sets failure_* on error.
			auto serve_via_wasm = [&](const String& entry_unit, const String& handler) {
				request.stats.wasm_handler_ready = time_precise();
				request_fault_active = 0;
				restore_request_fault_handlers();
				request.stats.wasm_backend_started = time_precise();
				String wasm_error = wasm_backend_serve(request, entry_unit, handler, wasm_invocation_remaining_ms(invocation_deadline));
				request.stats.wasm_backend_finished = time_precise();
				install_request_fault_handlers();
				request_fault_active = 1;
				if(wasm_error != "")
				{
					size_t blocked_at = wasm_error.find("BEARER_POLICY_BLOCKED:");
					if(blocked_at != String::npos)
					{
						String fn = wasm_error.substr(blocked_at + 19);
						size_t fn_end = fn.find_first_of(" \t\r\n\"");
						if(fn_end != String::npos)
							fn = fn.substr(0, fn_end);
						failure_title = "function disabled by server policy";
						failure_details = "this unit called " + fn + ", which is disabled on this server by configuration (BEARER_HOSTCALL_BLOCKLIST)";
						failure_trace = "";
					}
					else
					{
						failure_title = "wasm runtime error during request";
						failure_details = "";
						failure_trace = wasm_error;
					}
				}
			};

			f64 ready_phase_started = time_precise();
			String entry_unit = compiler_normalize_unit_path(&request, request.params["SCRIPT_FILENAME"]);
			request.stats.wasm_ready_normalize_us += (u64)((time_precise() - ready_phase_started) * 1000000.0);
			String request_method = to_upper(trim(request.params["REQUEST_METHOD"]));
			bool read_request = request_method == "GET" || request_method == "HEAD" || request_method == "OPTIONS";
			ready_phase_started = time_precise();
			bool stale_mutation = !request.resources.is_cli && !read_request && compiler_unit_needs_recompile(&request, entry_unit);
			request.stats.wasm_ready_mutation_check_us += (u64)((time_precise() - ready_phase_started) * 1000000.0);
			if(stale_mutation)
			{
				compiler_prioritize_unit(&request, entry_unit);
				request.set_status(503, "Service Unavailable");
				request.header["Content-Type"] = "text/plain; charset=utf-8";
				request.header["Retry-After"] = "1";
				print("The requested code is being updated. Retry this request shortly.\n");
			}
			// W7e: every unit runs on wasm. When the artifact is missing or stale
			// (cold worker, or source edited since the last compile), compile the
			// unit on demand — get_shared_unit() builds the .wasm side-module — and
			// recheck. Native execution has been removed, so a unit that still cannot
			// be served by wasm becomes a clean 500 request failure.
			SharedUnit* entry_compile_state = 0;
			bool entry_compile_timed_out = false;
			auto wasm_ready = [&](const String& unit) -> bool {
				if(wasm_backend_should_handle(request, unit))
					return(true);
				u64 remaining_ms = wasm_invocation_remaining_ms(invocation_deadline);
				if(remaining_ms == 0)
					entry_compile_timed_out = true;
				else
					entry_compile_state = get_shared_unit_bounded(&request, unit, remaining_ms, &entry_compile_timed_out);
				return(!entry_compile_timed_out && wasm_backend_should_handle(request, unit));
			};
			auto fail_wasm_unavailable = [&](const String& handler) {
				if(entry_compile_timed_out)
				{
					failure_title = "wasm invocation timeout";
					failure_details = "";
					failure_trace = "BEARER_INVOCATION_TIMEOUT: wasm invocation exceeded " + std::to_string(invocation_budget_ms) + " ms";
					return;
				}
				failure_title = "wasm unit unavailable after compile";
				String compile_error = entry_compile_state ? first(entry_compile_state->compiler_messages, entry_compile_state->compile_error_status) : String("");
				failure_details = compile_error != "" ? compile_error : handler + " handler could not be served by wasm";
				failure_trace = "source: " + request.params["SCRIPT_FILENAME"];
			};

			if(stale_mutation)
			{
				// The response above deliberately bypasses all stale application code.
			}
			else if(request.params["BEARER_WS"] == "1")
			{
				// A WS message the broker forwarded here: rebuild the connection
				// context the broker passed as params, then run __bearer_websocket.
				request.resources.websocket_connection_id = request.params["BEARER_WS_CONNECTION_ID"];
				request.resources.websocket_scope = request.params["BEARER_WS_SCOPE"];
				request.resources.websocket_opcode = (u8)int_val(request.params["BEARER_WS_OPCODE"]);
				request.resources.websocket_is_binary = request.params["BEARER_WS_BINARY"] == "1";
				bool msg_ok = false;
				request.in = base64_decode(request.params["BEARER_WS_MESSAGE"], msg_ok);
				for(auto& id : split(request.params["BEARER_WS_CONNECTIONS"], "\n"))
					if(id != "")
						request.resources.websocket_scope_connection_ids.push_back(id);
				bool decoded = false;
				String state_raw = base64_decode(request.params["BEARER_WS_STATE"], decoded);
				if(state_raw != "")
				{
					DValue state; String e;
					if(brb_decode(state_raw, state, &e))
						request.connection = state;
				}
				if(wasm_ready(entry_unit))
					serve_via_wasm(entry_unit, "websocket");
				else
					fail_wasm_unavailable("websocket");
			}
			else if(request.resources.is_cli)
			{
				if(wasm_ready(entry_unit))
					serve_via_wasm(entry_unit, "cli");
				else
					fail_wasm_unavailable("cli");
			}
			else if(request.params["BEARER_SERVE_HTTP"] == "1")
			{
				// W7c: this runs in a normal worker (clean engine) — the custom-server
				// dispatcher forwarded the request here via FastCGI rather than render
				// wasm in its own fork (Wasmtime cannot be re-created across fork).
				if(wasm_ready(entry_unit))
				{
					String fn = request.params["BEARER_SERVE_HTTP_FUNCTION"];
					serve_via_wasm(entry_unit, fn == "" ? String("serve_http") : "serve_http:" + fn);
				}
				else
					fail_wasm_unavailable("serve_http");
			}
			else if(wasm_ready(entry_unit))
				serve_via_wasm(entry_unit, "render");
			else
				fail_wasm_unavailable("render");
		}
		catch(const std::exception& e)
		{
			failure_title = "uncaught exception during request";
			failure_details = e.what();
			failure_trace = backtrace_capture(32, 1);
		}
		catch(...)
		{
			failure_title = "unknown uncaught exception during request";
			failure_trace = backtrace_capture(32, 1);
		}
	}

	request_fault_active = 0;
	request_fault_request = 0;
	restore_request_fault_handlers();

	if(failure_title != "")
		render_request_failure(request, failure_title, failure_details, failure_trace,
			invocation_deadline, invocation_budget_ms, 500);

	for( auto &f : request.uploaded_files)
	{
		file_unlink(f.tmp_name);
	}

	if(failure_title == "" && request.session_id.length() > 0)
		save_session_data(request.session_id, request.session);

	cleanup_mysql_connections();
	cleanup_sqlite_connections();
	restore_active_request(previous_context);

    return request.flags.status;
}


volatile bool termination_signal_received = false;

void on_terminate(int sig)
{
	termination_signal_received = true;
}

void clear_shared_unit_cache(ServerState& state)
{
	for(auto& it : state.units)
		delete it.second;
	state.units.clear();
}

void close_inherited_server_sockets()
{
	for(auto socket_handle : server.server_sockets)
		close(socket_handle);
	server.server_sockets.clear();
	server.server_socket_types.clear();
}

void install_process_fault_handlers()
{
	signal(SIGSEGV, on_segfault);
	signal(SIGABRT, on_segfault);
	signal(SIGBUS, on_segfault);
	signal(SIGILL, on_segfault);
	signal(SIGFPE, on_segfault);
	signal(SIGPIPE, SIG_IGN);
}

String custom_server_safe_key(String key)
{
	return(runtime_safe_key(key, "server key"));
}

String custom_server_config_file(String key)
{
	return(path_join(server_state.config["BIN_DIRECTORY"], "server-" + custom_server_safe_key(key) + ".cfg"));
}

String custom_server_task_key(String key)
{
	return("server:" + custom_server_safe_key(key));
}

String custom_server_config_encode(String key, String type, String bind, String file_name, String function_name)
{
	DValue config;
	config["key"] = key;
	config["type"] = type;
	config["bind"] = bind;
	config["file"] = file_name;
	config["function"] = function_name;
	return(json_encode(config));
}

StringMap custom_server_config_decode(String content)
{
	StringMap result;
	content = trim(content);
	if(content == "")
		return(result);

	if(content[0] == '{')
		return(json_decode(content).to_stringmap());

	// Compatibility for early newline/key=value config files from development.
	for(auto line : split(content, "\n"))
	{
		line = trim(line);
		if(line == "")
			continue;
		String key = trim(nibble(line, "="));
		if(key != "")
			result[key] = json_decode(line).to_string();
	}
	return(result);
}

bool custom_server_is_numeric_port(String value)
{
	value = trim(value);
	if(value == "")
		return(false);
	for(char c : value)
	{
		if(c < '0' || c > '9')
			return(false);
	}
	return(true);
}

u64 custom_server_registry_count()
{
	u64 count = 0;
	for(auto file_name : ls(server_state.config["BIN_DIRECTORY"]))
	{
		if(str_starts_with(file_name, "server-") && str_ends_with(file_name, ".cfg"))
			count++;
	}
	return(count);
}

bool custom_server_wait_for_stop(String task_key, f64 timeout_seconds = 2.0)
{
	f64 deadline = time_precise() + timeout_seconds;
	while(time_precise() < deadline)
	{
		if(task_pid(task_key) == 0)
			return(true);
		usleep(50000);
	}
	return(task_pid(task_key) == 0);
}

int custom_server_bind_http(FastCGIServer& dispatcher, String bind)
{
	bind = trim(bind);
	if(custom_server_is_numeric_port(bind))
	{
		u64 port = int_val(bind);
		u64 min_port = to_u64(server_state.config["CUSTOM_SERVER_MIN_PORT"], 1024);
		u64 max_port = to_u64(server_state.config["CUSTOM_SERVER_MAX_PORT"], 65535);
		if(port < min_port || port > max_port)
			throw std::runtime_error("server_start_http(): TCP port is outside configured custom server range");
		String bind_address = to_bool(server_state.config["CUSTOM_SERVER_ALLOW_PUBLIC_BIND"], false) ? "0.0.0.0" : "127.0.0.1";
		return(dispatcher.listen_http((unsigned)port, bind_address));
	}
	String socket_prefix = first(server_state.config["CUSTOM_SERVER_UNIX_SOCKET_PREFIX"], "/tmp/bearer/custom-servers/");
	if(!str_starts_with(bind, socket_prefix))
		throw std::runtime_error("server_start_http(): Unix socket path is outside configured custom server prefix");
	int socket_handle = dispatcher.listen(bind);
	dispatcher.server_socket_types[socket_handle] = 'H';
	return(socket_handle);
}

StringMap custom_server_read_config(String key)
{
	String config_file = custom_server_config_file(key);
	if(!file_exists(config_file))
		return(StringMap());
	return(custom_server_config_decode(file_get_contents(config_file)));
}

static String custom_server_dispatcher_key = "";

// Forward a request to a normal worker over the FastCGI socket and populate the
// response from the worker's reply. Shared by the connection brokers (the custom
// HTTP server dispatcher and the WS broker): they own a listening socket but
// render through the clean-engine worker pool rather than host a Wasmtime engine
// (which cannot be re-created across fork). The caller sets the routing params
// (BEARER_SERVE_HTTP / BEARER_WS, SCRIPT_FILENAME, etc.) before calling.
int forward_request_to_worker(FastCGIRequest& request, u32 timeout_seconds = 30)
{
	String fcgi_socket = first(server_state.config["FCGI_SOCKET_PATH"], "/run/bearer.sock");
	StringMap forward_params = request.params;
	// The broker accepted raw HTTP, but this hop is FastCGI. Without the CGI
	// marker Request::set_status() emits an HTTP status line that the FastCGI
	// reply parser correctly treats as body; the broker would then wrap that
	// complete response in a second HTTP response.
	forward_params["GATEWAY_INTERFACE"] = "CGI/1.1";
	FcgiForwardResult fwd = fcgi_forward_request(fcgi_socket, forward_params, request.in, timeout_seconds);
	request.ob_start();
	if(!fwd.ok)
	{
		request.set_status(502, "Bad Gateway");
		request.header["Content-Type"] = "text/plain; charset=utf-8";
		(*request.ob) << "worker forward failed: " << fwd.error << "\n";
		return(request.flags.status);
	}
	request.set_status(fwd.status);
	for(auto& h : fwd.headers)
		request.header[h.first] = h.second;
	request.ob->write(fwd.body.data(), fwd.body.size());
	return(request.flags.status);
}

int custom_server_http_complete(FastCGIRequest& request)
{
	String key = first(request.params["BEARER_SERVER_KEY"], custom_server_dispatcher_key);
	StringMap cfg = custom_server_read_config(key);
	if(cfg["type"] != "http" || cfg["file"] == "")
	{
		request.set_status(503, "Service Unavailable");
		request.header["Content-Type"] = "text/plain; charset=utf-8";
		request.ob_start();
		(*request.ob) << "custom HTTP server is not configured\n";
		return(request.flags.status);
	}

	request.params["BEARER_SERVE_HTTP"] = "1";
	request.params["BEARER_SERVE_HTTP_KEY"] = key;
	request.params["BEARER_SERVE_HTTP_BIND"] = cfg["bind"];
	request.params["BEARER_SERVE_HTTP_FUNCTION"] = cfg["function"];
	request.params["SCRIPT_FILENAME"] = cfg["file"];
	u64 timeout = to_u64(server_state.config["CUSTOM_SERVER_HANDLER_TIMEOUT_SECONDS"], 30);
	return(forward_request_to_worker(request, timeout > 0 ? (u32)timeout : 30));
}

// ---- central WS broker -----------------------------------------------------

// Connect to a unix socket (blocking — local, sub-ms) and set it non-blocking.
static int ws_broker_connect_unix(const String& path)
{
	int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if(fd < 0)
		return(-1);
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if(path.size() >= sizeof(addr.sun_path))
	{
		fprintf(stderr, "ws broker unix socket path too long: %s\n", path.c_str());
		::close(fd);
		return(-1);
	}
	strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
	if(::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		::close(fd);
		return(-1);
	}
	int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	return(fd);
}

// Apply a ws_* dispatch batch a worker flushed at workspace teardown. The broker
// owns every connection, so any target (id / scope / broadcast / close) resolves.
void ws_broker_apply_commands(FastCGIRequest& request)
{
	DValue batch;
	String err;
	bool ok = brb_decode(request.in, batch, &err);
	if(ok)
	{
		if(DValue* cmds = batch.key("commands"))
			cmds->each([&](const DValue& cmd_const, String) {
				DValue cmd = cmd_const;   // .each yields const; copy for [] access
				String action = cmd["action"].to_string();
				bool ok = false;
				String msg = base64_decode(cmd["message_b64"].to_string(), ok);
				bool binary = cmd["binary"].to_bool();
				if(action == "broadcast")
					ws_broker.websocket_broadcast(cmd["scope"].to_string(), msg, binary);
				else if(action == "send_to")
					ws_broker.websocket_send_to(cmd["connection_id"].to_string(), msg, binary);
				else if(action == "close")
					ws_broker.websocket_close(cmd["connection_id"].to_string(),
						(u16)cmd["status_code"].to_u64(), cmd["reason"].to_string());
			});
		// persist any updated per-connection state back onto the live connection
		String cid = batch["connection_id"].to_string();
		if(cid != "" && batch.key("connection_state"))
			for(auto& item : ws_broker.client_sockets)
				if(item.second->is_websocket && item.second->websocket_connection_id == cid)
					item.second->websocket_state = batch["connection_state"];
	}
	request.set_status(200);
	request.ob_start();
}

// on_complete for the broker: either a worker's command flush, or an un-upgraded
// HTTP request that hit the WS port (forwarded to the pool via the shared facility).
int ws_broker_complete(FastCGIRequest& request)
{
	if(request.params["BEARER_WS_DISPATCH"] == "1")
	{
		ws_broker_apply_commands(request);
		return(request.flags.status);
	}
	return(forward_request_to_worker(request));
}

// A complete WS message: fire a render to the worker pool WITHOUT blocking the
// broker loop (the connection identity + state ride as params; the message is
// the body). The worker renders __bearer_websocket and flushes ws_* back to us.
int ws_broker_ws_message(FastCGIRequest& request, const String& message, u8 opcode)
{
	StringMap params;
	params["SCRIPT_FILENAME"] = request.params["SCRIPT_FILENAME"];
	params["REQUEST_METHOD"] = "GET";
	// handle_request() rejects any request without REQUEST_URI before on_complete.
	params["REQUEST_URI"] = first(request.params["REQUEST_URI"], request.params["DOCUMENT_URI"],
		request.params["SCRIPT_FILENAME"]);
	params["BEARER_WS"] = "1";
	// The message rides as a param (empty STDIN) so the forwarded request
	// completes cleanly — a STDIN body makes the FastCGI transport flush a
	// premature response before on_complete (handle_complete) ever runs.
	params["BEARER_WS_MESSAGE"] = base64_encode(message);
	params["BEARER_WS_CONNECTION_ID"] = request.resources.websocket_connection_id;
	params["BEARER_WS_SCOPE"] = request.resources.websocket_scope;
	params["BEARER_WS_OPCODE"] = std::to_string((int)opcode);
	params["BEARER_WS_BINARY"] = request.resources.websocket_is_binary ? "1" : "0";
	params["BEARER_WS_CONNECTIONS"] = join(ws_broker.websocket_connection_ids(request.resources.websocket_scope), "\n");
	if(request.resources.websocket_connection_state)
		params["BEARER_WS_STATE"] = base64_encode(brb_encode(*request.resources.websocket_connection_state));

	int fd = ws_broker_connect_unix(first(server_state.config["FCGI_SOCKET_PATH"], "/run/bearer.sock"));
	if(fd < 0)
		return(0);
	ws_broker_outbound[fd] = {fcgi_build_request(params, ""), time_precise()};
	return(0);
}

// Drive in-flight render forwards non-blocking: finish writing the request, then
// drain/discard the reply (the unit's ws_* came back via the command socket).
void ws_broker_drain_outbound(u64 timeout_seconds)
{
	f64 now = time_precise();
	for(auto it = ws_broker_outbound.begin(); it != ws_broker_outbound.end(); )
	{
		int fd = it->first;
		WsBrokerOutbound& outbound = it->second;
		String& pending = outbound.pending;
		bool done = false;
		bool timed_out = timeout_seconds > 0 && now - outbound.started_at > (f64)timeout_seconds;
		if(timed_out)
		{
			fprintf(stderr, "ws broker dropping stale outbound forward fd=%i after %.1fs\n", fd, now - outbound.started_at);
			done = true;
		}
		if(!done && !pending.empty())
		{
			ssize_t n = ::send(fd, pending.data(), pending.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
			if(n > 0)
				pending.erase(0, n);
			else if(n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				done = true;
		}
		if(!done && pending.empty())
		{
			char buf[8192];
			ssize_t r = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
			if(r == 0)
				done = true;   // worker closed the connection: render complete
			else if(r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				done = true;
		}
		if(done)
		{
			::close(fd);
			it = ws_broker_outbound.erase(it);
		}
		else
			++it;
	}
}

void run_ws_broker()
{
	my_pid = getpid();
	close_inherited_server_sockets();   // drop the worker listeners we inherited
	install_process_fault_handlers();
	ws_broker.calls_until_termination = -1;
	ws_broker.http_document_root = first(server_state.config["HTTP_DOCUMENT_ROOT"], path_join(server_state.config["COMPILER_SYS_PATH"], server_state.config["SITE_DIRECTORY"]));
	// The broker renders nothing itself, so accept every request through to
	// on_complete (it either forwards to the pool or applies a ws_* batch).
	ws_broker.on_request = [](FastCGIRequest&) { return 0; };
	ws_broker.on_data = [](FastCGIRequest&) { return 0; };
	ws_broker.on_complete = &ws_broker_complete;
	ws_broker.on_websocket_message = &ws_broker_ws_message;
	if(server_state.config["HTTP_PORT"] != "")
		ws_broker.listen_http(int_val(server_state.config["HTTP_PORT"]));
	u64 ws_broker_outbound_timeout_seconds = to_u64(server_state.config["WS_BROKER_OUTBOUND_TIMEOUT_SECONDS"], 30);
	if(server_state.config["WS_BROKER_SOCKET_PATH"] != "")
	{
		ws_broker.listen(server_state.config["WS_BROKER_SOCKET_PATH"]);
		chmod(server_state.config["WS_BROKER_SOCKET_PATH"].c_str(), S_IRWXU | S_IRGRP | S_IWGRP);
	}
	while(!termination_signal_received)
	{
		ws_broker.process(50);
		ws_broker_drain_outbound(ws_broker_outbound_timeout_seconds);
	}
}

bool ws_broker_alive()
{
	return(ws_broker_pid > 0 && task_kill(ws_broker_pid, 0) == 0);
}

void ensure_ws_broker()
{
	if(ws_broker_alive())
		return;
	pid_t p = fork();
	if(p < 0)
	{
		perror("fork ws_broker");
		return;
	}
	if(p == 0)
	{
		run_ws_broker();
		exit(0);
	}
	ws_broker_pid = p;
	printf("(P) WS broker spawned: PID %i\n", p);
}

void custom_server_http_dispatcher_loop(String key)
{
	my_pid = getpid();
	custom_server_dispatcher_key = key;
	close_inherited_server_sockets();
	install_process_fault_handlers();
	StringMap cfg = custom_server_read_config(key);
	if(cfg["type"] != "http" || cfg["bind"] == "")
	{
		fprintf(stderr, "server_start_http(): missing config for key '%s'\n", key.c_str());
		exit(1);
	}

	try
	{
		FastCGIServer dispatcher;
		custom_server_bind_http(dispatcher, cfg["bind"]);
		dispatcher.calls_until_termination = -1;
		dispatcher.resolve_http_script_filename = false;
		dispatcher.on_complete = &custom_server_http_complete;
		for(;;)
			dispatcher.process(-1);
	}
	catch(const std::exception& e)
	{
		fprintf(stderr, "server_start_http(): dispatcher for key '%s' stopped: %s\n", key.c_str(), e.what());
		exit(1);
	}
	catch(...)
	{
		fprintf(stderr, "server_start_http(): dispatcher for key '%s' stopped with unknown error\n", key.c_str());
		exit(1);
	}
}

pid_t server_start_http(String key, String socket_fn_or_port, String call_bearer_filename, String call_function)
{
	key = trim(key);
	socket_fn_or_port = trim(socket_fn_or_port);
	call_bearer_filename = trim(call_bearer_filename);
	call_function = trim(call_function);
	if(key == "")
		throw std::runtime_error("server_start_http(): key cannot be empty");
	if(socket_fn_or_port == "")
		throw std::runtime_error("server_start_http(): socket_fn_or_port cannot be empty");
	if(call_bearer_filename == "")
		throw std::runtime_error("server_start_http(): call_bearer_filename cannot be empty");
	if(call_bearer_filename[0] != '/')
		call_bearer_filename = expand_path(call_bearer_filename, cwd_get());
	String allowed_root = first(server_state.config["CUSTOM_SERVER_BEARER_ROOT"], path_join(server_state.config["COMPILER_SYS_PATH"], server_state.config["SITE_DIRECTORY"]));
	String real_call_bearer_filename = path_real(call_bearer_filename);
	if(real_call_bearer_filename == "" || !path_is_within(real_call_bearer_filename, allowed_root))
		throw std::runtime_error("server_start_http(): call_bearer_filename is outside configured custom server BEARER root");
	call_bearer_filename = real_call_bearer_filename;

	String config_file = custom_server_config_file(key);
	StringMap previous_config;
	if(file_exists(config_file))
		previous_config = custom_server_config_decode(file_get_contents(config_file));
	String new_config = custom_server_config_encode(key, "http", socket_fn_or_port, call_bearer_filename, call_function);
	String task_key = custom_server_task_key(key);
	u64 max_servers = to_u64(server_state.config["CUSTOM_SERVER_MAX_SERVERS"], 16);
	if(!file_exists(config_file) && max_servers > 0 && custom_server_registry_count() >= max_servers)
		throw std::runtime_error("server_start_http(): custom server quota exceeded");
	pid_t existing_pid = task_pid(task_key);
	if(existing_pid != 0 && previous_config["bind"] != "" && previous_config["bind"] != socket_fn_or_port)
	{
		task_kill(existing_pid, SIGTERM);
		custom_server_wait_for_stop(task_key);
		existing_pid = 0;
	}
	if(!file_put_contents(config_file, new_config))
		throw std::runtime_error("server_start_http(): could not write server config");
	if(existing_pid != 0)
		return(existing_pid);
	return(task(task_key, [key]() {
		custom_server_http_dispatcher_loop(key);
	}, 0));
}

bool server_stop(String key)
{
	key = trim(key);
	if(key == "")
		return(false);
	String task_key = custom_server_task_key(key);
	pid_t pid = task_pid(task_key);
	file_unlink(custom_server_config_file(key));
	if(pid == 0)
		return(true);
	if(task_kill(pid, SIGTERM) != 0)
		return(false);
	return(custom_server_wait_for_stop(task_key));
}

bool proactive_compile_queue_has(StringList& queue, String file_name)
{
	return(std::find(queue.begin(), queue.end(), file_name) != queue.end());
}

u64 bounded_compile_jobs(String value, u64 fallback = 2)
{
	value = trim(value);
	u64 jobs = fallback;
	if(value != "")
	{
		char* end = 0;
		errno = 0;
		long long parsed = strtoll(value.c_str(), &end, 10);
		if(end != value.c_str() && end && *end == '\0' && errno != ERANGE)
			jobs = parsed < 1 ? 1 : (u64)parsed;
	}
	return(std::max<u64>(1, std::min<u64>(jobs, 16)));
}

bool proactive_compile_worker_owns(String file_name, u64 worker, u64 jobs)
{
	u64 hash = 1469598103934665603ull;
	for(unsigned char c : file_name)
	{
		hash ^= c;
		hash *= 1099511628211ull;
	}
	return(hash % jobs == worker);
}

void proactive_compile_queue_push(StringList& queue, String file_name)
{
	if(file_name == "" || proactive_compile_queue_has(queue, file_name))
		return;
	queue.push_back(file_name);
}

bool proactive_compile_unit(Request& context, String file_name, bool& source_missing, bool retry_current_failure = false)
{
	bool failed = false;
	String wasm_path = compiler_unit_wasm_path(&context, file_name);
	if(retry_current_failure || compiler_unit_needs_recompile(&context, file_name, &source_missing))
	{
		printf("(i) proactive compile %s\n", file_name.c_str());
		auto su = retry_current_failure ? compiler_get_shared_unit_internal(&context, file_name, false, true) : get_shared_unit(&context, file_name);
		failed = !su || su->compiler_messages != "";
		if(su)
			wasm_path = su->wasm_name;
	}
	else if(source_missing)
	{
		printf("(i) proactive compiler forget removed unit %s\n", file_name.c_str());
		compiler_untrack_known_unit(&context, file_name);
	}
	if(!source_missing && !failed && wasm_serialized_module_needs_refresh(wasm_path))
	{
		printf("(i) proactive serialize %s\n", file_name.c_str());
		String serialize_error = wasm_serialize_module_artifact(wasm_path);
		if(serialize_error != "")
		{
			printf("(!) proactive serialize failed for %s: %s\n", file_name.c_str(), serialize_error.c_str());
			failed = true;
		}
	}
	context.session.clear();
	context.session_loaded_hash = "";
	clear_shared_unit_cache(server_state);
	return(failed);
}

void run_proactive_compiler(u64 worker, u64 jobs)
{
	Request background_context;
	StringList compile_queue;
	background_context.server = &server_state;
	set_active_request(background_context);
	if(!to_bool(server_state.config["PROACTIVE_COMPILE_ENABLED"], true))
		return;
	f64 check_interval = float_val(server_state.config["PROACTIVE_COMPILE_CHECK_INTERVAL"]);
	f64 failure_retry_interval = 0;
	f64 next_scan_at = 0;
	std::map<String, f64> retry_after;
	if(check_interval < 1)
		check_interval = 1;
	failure_retry_interval = std::max(
		check_interval,
		(f64)std::max((s64)10, (s64)int_val(server_state.config["COMPILE_FAILURE_RETRY_SECONDS"]))
	);

	my_pid = getpid();

	close_inherited_server_sockets();
	signal(SIGSEGV, on_segfault);
	signal(SIGABRT, on_segfault);
	signal(SIGBUS, on_segfault);
	signal(SIGILL, on_segfault);
	signal(SIGFPE, on_segfault);
	signal(SIGPIPE, SIG_IGN);
	setpriority(PRIO_PROCESS, 0, 10);
	printf("(P) proactive compiler worker %llu/%llu ready: PID %i\n",
		(unsigned long long)(worker + 1), (unsigned long long)jobs, getpid());

	try
	{
		auto known_units = compiler_list_known_units(&background_context);
		auto site_units = compiler_scan_site_units(&background_context);
		known_units.insert(known_units.end(), site_units.begin(), site_units.end());
		compiler_set_known_units(&background_context, known_units);
	}
	catch(const std::exception& e)
	{
		printf("(!) proactive compiler initial scan failed: %s\n", e.what());
	}
	catch(...)
	{
		printf("(!) proactive compiler initial scan failed: unknown exception\n");
	}
	next_scan_at = time_precise();

	while(!termination_signal_received)
	{
		try
		{
			if(compile_queue.size() == 0 && time_precise() >= next_scan_at)
			{
				auto tracked_units = compiler_list_known_units(&background_context);
				bool source_generation_changed = false;

				for(auto& file_name : tracked_units)
				{
					if(!proactive_compile_worker_owns(file_name, worker, jobs))
						continue;
					bool source_missing = false;
					auto retry_it = retry_after.find(file_name);
					bool needs_compile = compiler_unit_needs_recompile(&background_context, file_name, &source_missing);
					f64 now = time_precise();
					if(needs_compile && retry_it != retry_after.end())
					{
						retry_after.erase(retry_it);
						retry_it = retry_after.end();
					}
					bool retry_due = retry_it != retry_after.end() && now >= retry_it->second;
					if(!needs_compile && retry_it == retry_after.end() && !source_missing &&
						compiler_unit_needs_recompile(&background_context, file_name, 0, false, true, true))
						retry_after[file_name] = now + failure_retry_interval;
					if(needs_compile || retry_due)
					{
						proactive_compile_queue_push(compile_queue, file_name);
						source_generation_changed = true;
					}
					if(source_missing)
					{
						source_generation_changed = true;
						printf("(i) proactive compiler forget removed unit %s\n", file_name.c_str());
						compiler_untrack_known_unit(&background_context, file_name);
						retry_after.erase(file_name);
						continue;
					}
				}

				if(source_generation_changed)
					compiler_mark_source_generation(&background_context);

			next_scan_at = time_precise() + check_interval;
		}

		if(compile_queue.size() > 0)
		{
			auto file_name = compile_queue.front();
			compile_queue.erase(compile_queue.begin());
			bool source_missing = false;
			auto retry_it = retry_after.find(file_name);
			if(retry_it != retry_after.end() && time_precise() < retry_it->second)
				continue;
			bool normal_compile = compiler_unit_needs_recompile(&background_context, file_name, &source_missing);
			bool retry_due = retry_it != retry_after.end();
			bool retry_current_failure = retry_due && !normal_compile && !source_missing &&
				compiler_unit_needs_recompile(&background_context, file_name, 0, false, true, true);
			bool failed = proactive_compile_unit(background_context, file_name, source_missing, retry_current_failure);
			if(source_missing)
				retry_after.erase(file_name);
			else if(failed)
				retry_after[file_name] = time_precise() + failure_retry_interval;
			else
				retry_after.erase(file_name);
			usleep(250000);
			continue;
		}

		usleep(250000);
		}
		catch(const std::exception& e)
		{
			printf("(!) proactive compiler scan failed: %s\n", e.what());
			next_scan_at = time_precise() + check_interval;
			usleep(250000);
		}
		catch(...)
		{
			printf("(!) proactive compiler scan failed: unknown exception\n");
			next_scan_at = time_precise() + check_interval;
			usleep(250000);
		}
	}
}

void run_priority_compiler()
{
	Request background_context;
	background_context.server = &server_state;
	set_active_request(background_context);
	my_pid = getpid();
	close_inherited_server_sockets();
	signal(SIGSEGV, on_segfault);
	signal(SIGABRT, on_segfault);
	signal(SIGBUS, on_segfault);
	signal(SIGILL, on_segfault);
	signal(SIGFPE, on_segfault);
	signal(SIGPIPE, SIG_IGN);
	setpriority(PRIO_PROCESS, 0, 5);
	while(!termination_signal_received)
	{
		auto units = compiler_take_priority_units(&background_context);
		if(units.empty())
		{
			usleep(100000);
			continue;
		}
		for(auto& file_name : units)
		{
			if(termination_signal_received)
				break;
			bool source_missing = false;
			proactive_compile_unit(background_context, file_name, source_missing);
		}
	}
}

bool proactive_compiler_alive(pid_t pid)
{
	return(pid > 0 && task_kill(pid, 0) == 0);
}

pid_t spawn_compiler(const char* label, void (*runner)())
{
	pid_t p = fork();
	if(p < 0)
	{
		perror((String("fork ") + label).c_str());
		return(0);
	}
	if(p == 0)
	{
		prctl(PR_SET_PDEATHSIG, SIGHUP);
		runner();
		exit(0);
	}
	printf("(P) %s spawned: PID %i\n", label, p);
	return(p);
}

pid_t spawn_proactive_compiler(u64 worker, u64 jobs)
{
	pid_t p = fork();
	if(p < 0)
	{
		perror("fork proactive compiler");
		return(0);
	}
	if(p == 0)
	{
		prctl(PR_SET_PDEATHSIG, SIGHUP);
		run_proactive_compiler(worker, jobs);
		exit(0);
	}
	printf("(P) proactive compiler worker %llu/%llu spawned: PID %i\n",
		(unsigned long long)(worker + 1), (unsigned long long)jobs, p);
	return(p);
}

void ensure_proactive_compiler()
{
	if(!to_bool(server_state.config["PROACTIVE_COMPILE_ENABLED"], true))
		return;
	if(float_val(server_state.config["PROACTIVE_COMPILE_CHECK_INTERVAL"]) <= 0)
		return;

	u64 jobs = bounded_compile_jobs(server_state.config["PROACTIVE_COMPILE_JOBS"]);
	if(proactive_compiler_pids.size() != jobs)
		proactive_compiler_pids.resize(jobs, 0);
	for(u64 worker = 0; worker < jobs; worker++)
		if(!proactive_compiler_alive(proactive_compiler_pids[worker]))
			proactive_compiler_pids[worker] = spawn_proactive_compiler(worker, jobs);
	if(priority_compiler_pid <= 0 || task_kill(priority_compiler_pid, 0) != 0)
		priority_compiler_pid = spawn_compiler("priority compiler", run_priority_compiler);
}

void listen_for_connections()
{
	install_process_fault_handlers();
	// Workers are uniform FastCGI/CLI renderers; the WS broker owns the HTTP/WS
	// port and every connection, so workers never accept raw HTTP themselves.
	server.close_http_listeners();
	// The transport's legacy eight-connection recycle predates persistent
	// Wasmtime engines. Recycling makes every ninth request pay engine/module
	// startup; request-scoped workspaces already isolate and release user state.
	server.calls_until_termination = -1;
	server.on_request = &handle_request;
	server.on_data = &handle_data;
	server.on_complete = &handle_complete;
	server.on_cli_complete = &handle_cli_complete;
	Request startup_context;
	startup_context.server = &server_state;
	f64 wasm_start = time_precise();
	String wasm_error = wasm_backend_start(startup_context);
	f64 wasm_ms = (time_precise() - wasm_start) * 1000.0;
	if(wasm_error == "")
		printf("(P) wasm worker ready: PID %i in %.3f ms\n", getpid(), wasm_ms);
	else
		fprintf(stderr, "(!) wasm worker initialization failed: PID %i in %.3f ms: %s\n", getpid(), wasm_ms, wasm_error.c_str());
	while(!termination_signal_received)
	{
		server.process(-1);
	}
	close_inherited_server_sockets();
	f64 drain_deadline = time_precise() + (f64)to_u64(server_state.config["WORKER_DRAIN_TIMEOUT_SECONDS"], 10);
	while(!server.client_sockets.empty() && time_precise() < drain_deadline)
		server.process(100);
	if(!server.client_sockets.empty())
		fprintf(stderr, "(!) worker PID %i drain deadline reached with %zu client connections\n",
			getpid(), server.client_sockets.size());
	else
		printf("(P) worker PID %i drained cleanly\n", getpid());
	wasm_backend_shutdown();
	exit(server.client_sockets.empty() ? 0 : 1);
}

mode_t configured_socket_mode(String value, mode_t fallback)
{
	value = trim(value);
	if(value == "")
		return(fallback);
	char* end = 0;
	long parsed = strtol(value.c_str(), &end, 8);
	if(end == value.c_str() || *end != '\0' || parsed < 0 || parsed > 0777)
		return(fallback);
	return((mode_t)parsed);
}

void chmod_configured_socket(String path, String mode_value, mode_t fallback)
{
	if(path == "")
		return;
	mode_t mode = configured_socket_mode(mode_value, fallback);
	if(chmod(path.c_str(), mode) != 0)
		fprintf(stderr, "(!) Could not chmod socket %s to %04o: %s\n", path.c_str(), (unsigned int)mode, strerror(errno));
}

int systemd_fastcgi_listener()
{
	const char* listen_pid = getenv("LISTEN_PID");
	const char* listen_fds = getenv("LISTEN_FDS");
	if(!listen_pid || !listen_fds)
		return(-1);
	if(to_u64(listen_pid, 0) != (u64)getpid())
		return(-1);
	if(to_u64(listen_fds, 0) != 1)
		throw std::runtime_error("BEARER requires exactly one systemd FastCGI listener");
	const char* names = getenv("LISTEN_FDNAMES");
	if(names && names[0] != '\0' && String(names) != "fastcgi")
		throw std::runtime_error("systemd listener must be named fastcgi");
	unsetenv("LISTEN_PID");
	unsetenv("LISTEN_FDS");
	unsetenv("LISTEN_FDNAMES");
	return(3);
}

StringMap redacted_server_config_for_log(const StringMap& config)
{
	StringMap redacted = config;
	for(auto& entry : redacted)
	{
		String key = to_upper(entry.first);
		if(key.find("PASSWORD") != String::npos || key.find("SECRET") != String::npos || key.find("TOKEN") != String::npos)
			entry.second = "[redacted]";
	}
	return(redacted);
}

void init_base_process()
{
	printf("(P) Starting parent server PID:%i\n", getpid());

	server_state.config = make_server_settings();
	server_state.config["COMPILER_SYS_PATH"] = cwd_get();
	printf("Compiler base path: %s\n", server_state.config["COMPILER_SYS_PATH"].c_str());
	int inherited_fastcgi = systemd_fastcgi_listener();
	if(inherited_fastcgi >= 0)
		server.adopt_listener(inherited_fastcgi, 'F');

	if(server_state.config["FCGI_PORT"] != "")
		server.listen(int_val(server_state.config["FCGI_PORT"]));

	printf("%s\n", var_dump(redacted_server_config_for_log(server_state.config)).c_str());

	if(inherited_fastcgi < 0 && server_state.config["FCGI_SOCKET_PATH"] != "")
	{
		server.listen(server_state.config["FCGI_SOCKET_PATH"]);
		chmod_configured_socket(server_state.config["FCGI_SOCKET_PATH"], server_state.config["FCGI_SOCKET_MODE"], 0666);
	}

	if(server_state.config["CLI_SOCKET_PATH"] != "")
	{
		server.listen_cli(server_state.config["CLI_SOCKET_PATH"]);
		chmod_configured_socket(server_state.config["CLI_SOCKET_PATH"], server_state.config["CLI_SOCKET_MODE"], 0600);
	}

	// HTTP_PORT (WebSocket + raw HTTP) is owned by the dedicated WS broker, not
	// the worker pool — see ensure_ws_broker(). Workers handle FastCGI + CLI only.

	mkdir(server_state.config["BIN_DIRECTORY"]);
	mkdir(server_state.config["TMP_UPLOAD_PATH"]);
	mkdir(server_state.config["SESSION_PATH"]);

	signal(SIGCHLD, on_child_exit);
	signal(SIGTERM, on_terminate);
	signal(SIGINT, on_terminate);
	signal(SIGHUP, on_terminate);
	signal(SIGPIPE, SIG_IGN);
	srand(time());
}

struct PrecompileWorkerResult
{
	u64 assigned = 0;
	u64 compiled = 0;
	u64 failed = 0;
};

PrecompileWorkerResult precompile_unit_range(Request& background_context, const StringList& files, u64 worker, u64 jobs)
{
	PrecompileWorkerResult result;
	for(u64 i = worker; i < files.size(); i += jobs)
	{
		result.assigned++;
		bool source_missing = false;
		bool needs_compile = compiler_unit_needs_recompile(&background_context, files[i], &source_missing, false, true, true);
		if(needs_compile)
			result.compiled++;
		if(proactive_compile_unit(background_context, files[i], source_missing, needs_compile))
			result.failed++;
	}
	return(result);
}

bool precompile_write_result(int fd, const PrecompileWorkerResult& result)
{
	const char* data = (const char*)&result;
	size_t remaining = sizeof(result);
	while(remaining > 0)
	{
		ssize_t written = write(fd, data, remaining);
		if(written < 0 && errno == EINTR)
			continue;
		if(written <= 0)
			return(false);
		data += written;
		remaining -= (size_t)written;
	}
	return(true);
}

bool precompile_read_result(int fd, PrecompileWorkerResult& result)
{
	char* data = (char*)&result;
	size_t remaining = sizeof(result);
	while(remaining > 0)
	{
		ssize_t received = read(fd, data, remaining);
		if(received < 0 && errno == EINTR)
			continue;
		if(received <= 0)
			return(false);
		data += received;
		remaining -= (size_t)received;
	}
	return(true);
}

int precompile_unit_generation()
{
	f64 started_at = time_precise();
	server_state.config = make_server_settings();
	const char* precompile_files_in = getenv("BEARER_PRECOMPILE_FILES_IN");
	if(precompile_files_in && precompile_files_in[0] != '\0')
		server_state.config["PRECOMPILE_FILES_IN"] = precompile_files_in;
	const char* precompile_bin_directory = getenv("BEARER_PRECOMPILE_BIN_DIRECTORY");
	if(precompile_bin_directory && precompile_bin_directory[0] != '\0')
		server_state.config["BIN_DIRECTORY"] = precompile_bin_directory;
	server_state.config["COMPILER_SYS_PATH"] = cwd_get();
	Request background_context;
	background_context.server = &server_state;
	mkdir(server_state.config["BIN_DIRECTORY"]);
	mkdir(compiler_unit_bin_directory(&background_context));

	set_active_request(background_context);
	auto files = compiler_scan_site_units(&background_context);
	compiler_set_known_units(&background_context, files);
	const char* jobs_env = getenv("BEARER_PRECOMPILE_JOBS");
	String jobs_text = trim(jobs_env && jobs_env[0] != '\0' ? String(jobs_env) : server_state.config["PRECOMPILE_JOBS"]);
	u64 jobs = std::min<u64>(bounded_compile_jobs(jobs_text), files.size() == 0 ? 1 : files.size());
	PrecompileWorkerResult total;
	bool worker_error = false;
	if(jobs == 1)
		total = precompile_unit_range(background_context, files, 0, 1);
	else
	{
		int result_pipe[2];
		if(pipe(result_pipe) != 0)
		{
			printf("(!) cannot create precompile result pipe: %s\n", std::strerror(errno));
			return(1);
		}
		std::vector<pid_t> workers;
		fflush(0);
		for(u64 worker = 0; worker < jobs; worker++)
		{
			pid_t pid = fork();
			if(pid < 0)
			{
				printf("(!) cannot fork precompile worker: %s\n", std::strerror(errno));
				worker_error = true;
				break;
			}
			if(pid == 0)
			{
				close(result_pipe[0]);
				PrecompileWorkerResult result = precompile_unit_range(background_context, files, worker, jobs);
				printf("Precompile worker %llu/%llu: %llu units, %llu compiled, %llu failed\n",
					(unsigned long long)(worker + 1), (unsigned long long)jobs,
					(unsigned long long)result.assigned, (unsigned long long)result.compiled,
					(unsigned long long)result.failed);
				bool reported = precompile_write_result(result_pipe[1], result);
				close(result_pipe[1]);
				_exit(result.failed == 0 && reported ? 0 : 1);
			}
			workers.push_back(pid);
		}
		close(result_pipe[1]);
		for(size_t i = 0; i < workers.size(); i++)
		{
			PrecompileWorkerResult result;
			if(!precompile_read_result(result_pipe[0], result))
			{
				worker_error = true;
				break;
			}
			total.assigned += result.assigned;
			total.compiled += result.compiled;
			total.failed += result.failed;
		}
		close(result_pipe[0]);
		for(auto pid : workers)
		{
			int status = 0;
			pid_t waited;
			do waited = waitpid(pid, &status, 0); while(waited < 0 && errno == EINTR);
			if(waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
				worker_error = true;
		}
		if(total.assigned != files.size())
			worker_error = true;
	}
	auto final_files = compiler_scan_site_units(&background_context);
	bool source_generation_changed = final_files != files;
	if(!source_generation_changed)
	{
		for(auto& file_name : final_files)
		{
			bool source_missing = false;
			if(compiler_unit_needs_recompile(&background_context, file_name, &source_missing, false, true, true) || source_missing)
			{
				source_generation_changed = true;
				break;
			}
		}
	}
	if(source_generation_changed)
	{
		compiler_set_known_units(&background_context, final_files);
		compiler_mark_source_generation(&background_context);
		printf("(!) source generation changed during precompile; candidate generation rejected\n");
	}
	if(worker_error)
		printf("(!) precompile worker or result reporting failed; candidate generation rejected\n");
	bool candidate_rejected = worker_error || source_generation_changed;
	printf("Precompiled unit generation %s with %llu job%s: %zu units, %llu compiled, %llu failed, worker status %s in %.3f s\n",
		compiler_unit_bin_directory(&background_context).c_str(),
		(unsigned long long)jobs, jobs == 1 ? "" : "s", files.size(),
		(unsigned long long)total.compiled, (unsigned long long)total.failed,
		candidate_rejected ? "failed" : "ok",
		time_precise() - started_at);
	return(total.failed == 0 && !candidate_rejected ? 0 : 1);
}

void print_fastcgi_usage(FILE* stream)
{
	fprintf(stream,
		"Usage: bearer_fastcgi [--precompile]\n"
		"       bearer_fastcgi --help\n\n"
		"Without options, start the FastCGI server.\n"
		"  --precompile  Compile the current source generation without starting listeners.\n"
		"  -h, --help    Show this help and exit.\n");
}

int main(int argc, char** argv)
{
	// systemd connects stdout to a pipe, which otherwise block-buffers child
	// diagnostics and assigns stale failures a misleading later journal time.
	setvbuf(stdout, 0, _IOLBF, 0);
	setvbuf(stderr, 0, _IONBF, 0);
	if(argc == 2 && (String(argv[1]) == "--help" || String(argv[1]) == "-h"))
	{
		print_fastcgi_usage(stdout);
		return(0);
	}
	bool precompile = argc == 2 && String(argv[1]) == "--precompile";
	if(argc != 1 && !precompile)
	{
		fprintf(stderr, "invalid arguments\n");
		print_fastcgi_usage(stderr);
		return(2);
	}
	// Warm up libgcc's backtrace state so the first in-handler backtrace()
	// after a fault does not allocate.
	backtrace(request_fault_frames, 4);
	process_start_directory();
	if(precompile)
		return(precompile_unit_generation());

	init_base_process();
	ensure_proactive_compiler();
	ensure_ws_broker();

	while(!termination_signal_received)
	{
		for(auto& pid : proactive_compiler_pids)
			if(!proactive_compiler_alive(pid))
				pid = 0;
		if(priority_compiler_pid > 0 && task_kill(priority_compiler_pid, 0) != 0)
			priority_compiler_pid = 0;
		if(!termination_signal_received)
			ensure_proactive_compiler();

		// One dedicated WS broker owns all connections; respawn it if it dies
		// (live connections are lost on a broker restart, but unit-code crashes
		// happen in workers, not here, so the broker stays up in practice).
		if(!ws_broker_alive())
			ws_broker_pid = 0;
		if(!termination_signal_received)
			ensure_ws_broker();

		while(workers.size() < int_val(server_state.config["WORKER_COUNT"]))
		{
			if(!termination_signal_received)
				spawn_subprocess(listen_for_connections);
		}
		sleep(1);
	}

	printf("(P) draining %zu workers before shutdown\n", workers.size());
	for(auto& worker : workers)
		kill(worker.first, SIGTERM);
	for(auto pid : proactive_compiler_pids)
		if(pid > 0)
			kill(pid, SIGTERM);
	if(priority_compiler_pid > 0)
		kill(priority_compiler_pid, SIGTERM);
	if(ws_broker_pid > 0)
		kill(ws_broker_pid, SIGTERM);
	f64 drain_deadline = time_precise() + (f64)to_u64(server_state.config["WORKER_DRAIN_TIMEOUT_SECONDS"], 10);
	auto background_children_alive = [&]() {
		bool alive = priority_compiler_pid > 0 && task_kill(priority_compiler_pid, 0) == 0;
		alive = alive || (ws_broker_pid > 0 && task_kill(ws_broker_pid, 0) == 0);
		for(auto pid : proactive_compiler_pids)
			alive = alive || proactive_compiler_alive(pid);
		return(alive);
	};
	while((!workers.empty() || background_children_alive()) && time_precise() < drain_deadline)
	{
		on_child_exit(0);
		usleep(10000);
	}
	for(auto& worker : workers)
		kill(worker.first, SIGKILL);
	for(auto pid : proactive_compiler_pids)
		if(proactive_compiler_alive(pid))
			kill(pid, SIGKILL);
	if(priority_compiler_pid > 0 && task_kill(priority_compiler_pid, 0) == 0)
		kill(priority_compiler_pid, SIGKILL);
	if(ws_broker_pid > 0 && task_kill(ws_broker_pid, 0) == 0)
		kill(ws_broker_pid, SIGKILL);
	server.shutdown();

	return 0;
}
