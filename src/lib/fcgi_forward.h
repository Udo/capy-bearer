#pragma once

// Minimal FastCGI client used by the connection brokers (the custom HTTP server
// dispatcher and the websocket exec child) to render a request through a normal
// worker on FCGI_SOCKET_PATH instead of rendering wasm in the broker's own forked
// process. Wasmtime cannot be safely re-created across fork, so the broker owns
// the connection but forwards the actual unit invocation to a clean-engine
// worker — the "broker holds connections, units respond like RENDER()" model.
//
// Native-only; compiled into the main object. Needs String/StringMap (bearer_lib.h)
// plus the socket headers below.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <climits>
#include <cstddef>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <algorithm>

struct FcgiForwardResult {
	bool ok = false;
	String error;
	int status = 200;          // parsed from a CGI "Status:" header if present
	StringMap headers;         // response headers (excluding Status)
	String body;               // response body
};

// One FastCGI stream (PARAMS/STDIN): data records (<=64KB each) then the
// terminating empty record the protocol requires to close the stream.
inline void fcgi_forward_stream(String& out, unsigned char type, const String& content)
{
	size_t off = 0, len = content.size();
	while(off < len)
	{
		size_t chunk = std::min(len - off, (size_t)0xffff);
		unsigned char hdr[8] = { 1, type, 0, 1, (unsigned char)((chunk >> 8) & 0xff), (unsigned char)(chunk & 0xff), 0, 0 };
		out.append((const char*)hdr, 8);
		out.append(content.data() + off, chunk);
		off += chunk;
	}
	unsigned char term[8] = { 1, type, 0, 1, 0, 0, 0, 0 };
	out.append((const char*)term, 8);
}

// FastCGI name/value pair length prefix: 1 byte if < 128, else 4 bytes (high bit set).
inline void fcgi_forward_put_len(String& out, size_t n)
{
	if(n < 128)
		out.push_back((char)(unsigned char)n);
	else
	{
		out.push_back((char)(unsigned char)(((n >> 24) & 0xff) | 0x80));
		out.push_back((char)(unsigned char)((n >> 16) & 0xff));
		out.push_back((char)(unsigned char)((n >> 8) & 0xff));
		out.push_back((char)(unsigned char)(n & 0xff));
	}
}

// Build the full FastCGI request bytes (BEGIN_REQUEST + PARAMS + STDIN) for a
// RESPONDER request id 1. The broker reuses this to fire renders non-blocking.
inline String fcgi_build_request(const StringMap& params, const String& stdin_body)
{
	String request;
	unsigned char begin[16] = { 1, 1, 0, 1, 0, 8, 0, 0,  0, 1, 0, 0, 0, 0, 0, 0 };
	request.append((const char*)begin, 16);
	String params_encoded;
	for(auto& kv : params)
	{
		fcgi_forward_put_len(params_encoded, kv.first.size());
		fcgi_forward_put_len(params_encoded, kv.second.size());
		params_encoded.append(kv.first);
		params_encoded.append(kv.second);
	}
	fcgi_forward_stream(request, 4 /*FCGI_PARAMS*/, params_encoded);
	fcgi_forward_stream(request, 5 /*FCGI_STDIN*/, stdin_body);
	return(request);
}

// Forward `params` + `stdin_body` to the FastCGI responder at unix `socket_path`
// and return the parsed CGI response. Times out (recv) at `timeout_seconds`.
inline FcgiForwardResult fcgi_forward_request(const String& socket_path,
	const StringMap& params, const String& stdin_body, u32 timeout_seconds = 30,
	u64 max_response_bytes = 8 * 1024 * 1024)
{
	FcgiForwardResult result;

	auto monotonic_ms = []() { timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now); return((u64)now.tv_sec * 1000 + (u64)now.tv_nsec / 1000000); };
	u64 started = monotonic_ms();
	u64 timeout_ms = std::max<u64>(1, timeout_seconds) * 1000;
	u64 deadline = started > UINT64_MAX - timeout_ms ? UINT64_MAX : started + timeout_ms;
	auto wait_for = [&](int fd, short events) {
		for(;;)
		{
			u64 now = monotonic_ms();
			if(now >= deadline) return(false);
			pollfd item{fd, events, 0};
			int rc = poll(&item, 1, (int)std::min<u64>(INT_MAX, deadline - now));
			if(rc > 0) return((item.revents & events) != 0 && (item.revents & (POLLERR|POLLNVAL)) == 0);
			if(rc == 0) return(false);
			if(errno != EINTR) return(false);
		}
	};

	int fd = ::socket(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0);
	if(fd < 0)
	{
		result.error = "fcgi_forward: socket() failed";
		return(result);
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if(socket_path.empty() || socket_path.size() >= sizeof(addr.sun_path))
	{
		::close(fd);
		result.error = "fcgi_forward: invalid Unix socket path";
		return(result);
	}
	memcpy(addr.sun_path, socket_path.data(), socket_path.size());
	int connected = ::connect(fd, (struct sockaddr*)&addr, offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
	if(connected < 0 && errno == EINPROGRESS && wait_for(fd, POLLOUT))
	{
		int socket_error = 0;
		socklen_t error_size = sizeof(socket_error);
		connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == 0 && socket_error == 0 ? 0 : -1;
	}
	if(connected < 0)
	{
		::close(fd);
		result.error = "fcgi_forward: connect(" + socket_path + ") failed or timed out";
		return(result);
	}

	String request = fcgi_build_request(params, stdin_body);
	for(size_t offset = 0; offset < request.size();)
	{
		ssize_t sent = ::send(fd, request.data() + offset, request.size() - offset, MSG_NOSIGNAL);
		if(sent < 0 && errno == EINTR)
			continue;
		if(sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_for(fd, POLLOUT))
			continue;
		if(sent <= 0)
		{
			::close(fd);
			result.error = "fcgi_forward: write to responder failed";
			return(result);
		}
		offset += (size_t)sent;
	}

	// Read records; collect FCGI_STDOUT content until FCGI_END_REQUEST or EOF.
	String inbuf, stdout_data;
	char buf[65536];
	bool ended = false;
	while(!ended)
	{
		ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
		if(n < 0 && errno == EINTR)
			continue;
		if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_for(fd, POLLIN))
			continue;
		if(n <= 0)
			break;
		inbuf.append(buf, n);
		while(inbuf.size() >= 8)
		{
			const unsigned char* h = (const unsigned char*)inbuf.data();
			size_t content = ((size_t)h[4] << 8) + h[5];
			size_t padding = h[6];
			size_t record_len = 8 + content + padding;
			if(inbuf.size() < record_len)
				break;
			unsigned char type = h[1];
			if(type == 6 /*FCGI_STDOUT*/)
			{
				if(stdout_data.size() > max_response_bytes || content > max_response_bytes - stdout_data.size())
				{
					::close(fd);
					result.error = "fcgi_forward: response exceeded configured output limit";
					return(result);
				}
				stdout_data.append(inbuf.data() + 8, content);
			}
			else if(type == 3 /*FCGI_END_REQUEST*/)
				ended = true;
			inbuf.erase(0, record_len);
		}
	}
	::close(fd);
	if(!ended)
	{
		result.error = "fcgi_forward: responder ended before FCGI_END_REQUEST";
		return(result);
	}

	// Parse the CGI response: header block, then body. Status: header (if any)
	// sets the HTTP status; everything else is a response header.
	size_t sep = stdout_data.find("\r\n\r\n");
	size_t sep_len = 4;
	if(sep == String::npos)
	{
		sep = stdout_data.find("\n\n");
		sep_len = 2;
	}
	String header_block = sep == String::npos ? String() : stdout_data.substr(0, sep);
	if(sep == String::npos)
		result.body = std::move(stdout_data);
	else
		result.body = stdout_data.substr(sep + sep_len);

	for(String line : split_strings(header_block, "\n"))
	{
		line = trim(line);
		if(line == "")
			continue;
		size_t colon = line.find(":");
		if(colon == String::npos)
			continue;
		String name = trim(line.substr(0, colon));
		String value = trim(line.substr(colon + 1));
		if(to_lower(name) == "status")
			result.status = (int)int_val(value);
		else
			result.headers[name] = value;
	}

	result.ok = true;
	return(result);
}
