#ifndef __UCE_WASM_CORE__
#include <sys/file.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dlfcn.h>
#endif
#include <stdlib.h>
#include <iostream>
#include <filesystem>
#include <ctype.h>
#include <fstream>
#include <ctime>
#include <limits.h>
#include <algorithm>
#include <iostream>

#include "types.h"

// Single definition of context for the native split build (the wasm core/unit
// builds keep the in-place definition from types.h — see the guard there).
#if !defined(__UCE_WASM_CORE__) && !defined(__UCE_WASM_UNIT__)
Request* context = 0;
#endif

#ifndef __UCE_WASM_UNIT__
void * operator new(decltype(sizeof(0)) n) noexcept(false)
{
	void* ptr = malloc(n);
	if(context)
	{
		context->stats.mem_alloc += n;
		if(context->stats.mem_alloc > context->stats.mem_high)
			context->stats.mem_high = context->stats.mem_alloc;
	}
	return(ptr);
}

void operator delete(void * p) throw()
{
	//TO DO: track deallocations
	free(p);
}
#endif

namespace {

String http_status_reason(s32 code)
{
	switch(code)
	{
		case 200: return("OK");
		case 201: return("Created");
		case 202: return("Accepted");
		case 204: return("No Content");
		case 301: return("Moved Permanently");
		case 302: return("Found");
		case 303: return("See Other");
		case 304: return("Not Modified");
		case 307: return("Temporary Redirect");
		case 308: return("Permanent Redirect");
		case 400: return("Bad Request");
		case 401: return("Unauthorized");
		case 403: return("Forbidden");
		case 404: return("Not Found");
		case 405: return("Method Not Allowed");
		case 409: return("Conflict");
		case 422: return("Unprocessable Content");
		case 429: return("Too Many Requests");
		case 500: return("Internal Server Error");
		case 501: return("Not Implemented");
		case 502: return("Bad Gateway");
		case 503: return("Service Unavailable");
		case 504: return("Gateway Timeout");
		default: return("");
	}
}

}

SharedUnit::~SharedUnit()
{
}

String nibble(String div, String& haystack)
{
	auto pos = haystack.find(div);
	if(pos == String::npos)
	{
		auto result = haystack;
		haystack.clear();
		return(result);
	}
	else
	{
		auto result = haystack.substr(0, pos);
		haystack.erase(0, pos+div.length());
		return(result);
	}
}

void Request::ob_start()
{
	ob_stack.push_back(new ByteStream());
	ob = ob_stack.back();
}

void Request::set_status(s32 code, String reason)
{
	if(reason == "")
		reason = http_status_reason(code);
	String prefix = params["GATEWAY_INTERFACE"] != "" ? "Status: " : "HTTP/1.1 ";
	response_code = prefix + std::to_string(code);
	if(reason != "")
		response_code += " " + reason;
	flags.status = code;
}

Request::~Request()
{
	for(auto* stream : ob_stack)
		delete stream;
	ob_stack.clear();
	ob = 0;
#ifndef __UCE_WASM_CORE__
	for(auto& sockfd : resources.sockets)
		close(sockfd);
#endif
}
