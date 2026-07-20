#pragma once

// Host-side summarizer for Wasmtime trap/error messages (WASM-PROPOSAL
// Phase 4: trap → error-page path). Turns a raw runtime message — which for
// stack exhaustion contains hundreds of identical frames — into a compact,
// error-page-ready summary: headline cause, optional detail (e.g. faulting
// address), and a backtrace with consecutive repeated frames collapsed.
//
// Pure string processing with no wasm-runtime dependency, so it is testable
// from the native suite and reusable by both the phase-4 spike runner and
// the future wasm worker. Tolerant of unrecognized input: an unparsed
// message is passed through as the cause, never dropped.

#include <cstdlib>
#include <string>
#include <vector>

// The demangler is a host-side nicety for trap traces. wasi-libc++ does not
// provide __cxa_demangle, and a wasm unit that merely includes bearer_lib.h must
// not pull it in as an unresolvable import — so it is host-only.
#if defined(__GNUG__) && !defined(__wasm__)
#define BEARER_WASM_TRACE_HAVE_DEMANGLE 1
#include <cxxabi.h>
#endif

struct WasmTraceSummary
{
	std::string cause;               // "wasm trap: ..." headline, or the raw message if unparsed
	std::string detail;              // secondary cause lines, e.g. "memory fault at wasm address ..."
	std::vector<std::string> frames; // collapsed display lines
	size_t total_frames = 0;
	bool parsed = false;             // a wasm backtrace section was recognized
};

inline std::string wasm_trace_demangle(const std::string& name)
{
#if defined(BEARER_WASM_TRACE_HAVE_DEMANGLE)
	// frame names look like "module!symbol"; demangle the symbol part
	auto bang = name.find('!');
	std::string prefix = bang == std::string::npos ? "" : name.substr(0, bang + 1);
	std::string symbol = bang == std::string::npos ? name : name.substr(bang + 1);
	if(symbol.rfind("_Z", 0) == 0)
	{
		int status = 0;
		char* demangled = abi::__cxa_demangle(symbol.c_str(), 0, 0, &status);
		if(status == 0 && demangled)
		{
			std::string result = prefix + demangled;
			free(demangled);
			return(result);
		}
		if(demangled)
			free(demangled);
	}
#endif
	return(name);
}

inline WasmTraceSummary wasm_trace_summarize(const std::string& message, size_t max_frame_lines = 12)
{
	WasmTraceSummary summary;

	struct Frame
	{
		std::string name;
		std::string address;
	};
	std::vector<Frame> raw_frames;
	std::vector<std::string> cause_lines;

	auto trim = [](const std::string& s) {
		size_t a = s.find_first_not_of(" \t\r");
		if(a == std::string::npos)
			return(std::string());
		size_t b = s.find_last_not_of(" \t\r");
		return(s.substr(a, b - a + 1));
	};

	// a frame or numbered-cause line starts with "N:"; strip that prefix
	auto strip_index = [&](const std::string& line, bool& had_index) {
		had_index = false;
		size_t i = 0;
		while(i < line.size() && line[i] >= '0' && line[i] <= '9')
			i++;
		if(i == 0 || i >= line.size() || line[i] != ':')
			return(line);
		had_index = true;
		return(trim(line.substr(i + 1)));
	};

	bool in_backtrace = false;
	bool in_caused_by = false;
	size_t pos = 0;
	while(pos <= message.size())
	{
		size_t eol = message.find('\n', pos);
		std::string line = trim(message.substr(pos, (eol == std::string::npos ? message.size() : eol) - pos));
		pos = eol == std::string::npos ? message.size() + 1 : eol + 1;
		if(line.empty())
			continue;

		if(line.find("wasm backtrace") != std::string::npos)
		{
			summary.parsed = true;
			in_backtrace = true;
			in_caused_by = false;
			continue;
		}
		if(line.rfind("Caused by", 0) == 0)
		{
			in_backtrace = false;
			in_caused_by = true;
			continue;
		}

		bool had_index = false;
		std::string body = strip_index(line, had_index);
		if(in_backtrace && had_index)
		{
			Frame frame;
			// optional "0xADDR - " prefix before the frame name
			if(body.rfind("0x", 0) == 0)
			{
				size_t dash = body.find(" - ");
				if(dash != std::string::npos)
				{
					frame.address = body.substr(0, dash);
					body = trim(body.substr(dash + 3));
				}
			}
			frame.name = wasm_trace_demangle(body);
			raw_frames.push_back(frame);
			continue;
		}
		if(in_caused_by || line.rfind("wasm trap:", 0) == 0)
		{
			cause_lines.push_back(body);
			continue;
		}
	}

	summary.total_frames = raw_frames.size();

	// headline = the "wasm trap:" line; everything else in Caused by → detail
	for(auto& line : cause_lines)
	{
		if(line.rfind("wasm trap:", 0) == 0)
			summary.cause = line;
		else
			summary.detail += (summary.detail.empty() ? "" : "; ") + line;
	}
	if(summary.cause.empty())
		summary.cause = summary.parsed && !cause_lines.empty() ? cause_lines.front() : trim(message);

	// collapse consecutive identical frame names
	struct Group
	{
		std::string name;
		std::string address;
		size_t first = 0;
		size_t count = 0;
	};
	std::vector<Group> groups;
	for(size_t i = 0; i < raw_frames.size(); i++)
	{
		if(!groups.empty() && groups.back().name == raw_frames[i].name)
		{
			groups.back().count++;
			continue;
		}
		Group group;
		group.name = raw_frames[i].name;
		group.address = raw_frames[i].address;
		group.first = i;
		group.count = 1;
		groups.push_back(group);
	}

	auto group_line = [](const Group& g) {
		std::string line;
		if(g.count == 1)
		{
			line = "#" + std::to_string(g.first);
			if(!g.address.empty())
				line += " " + g.address;
			line += " " + g.name;
		}
		else
		{
			line = "#" + std::to_string(g.first) + "..#" + std::to_string(g.first + g.count - 1)
				+ " " + g.name + " ×" + std::to_string(g.count);
		}
		return(line);
	};

	if(groups.size() <= max_frame_lines)
	{
		for(auto& g : groups)
			summary.frames.push_back(group_line(g));
	}
	else
	{
		// keep the top of the stack and the outermost group
		for(size_t i = 0; i + 2 < max_frame_lines && i < groups.size(); i++)
			summary.frames.push_back(group_line(groups[i]));
		summary.frames.push_back("... " + std::to_string(groups.size() - (max_frame_lines - 1)) + " more frame groups ...");
		summary.frames.push_back(group_line(groups.back()));
	}

	return(summary);
}

inline std::string wasm_trace_format(const WasmTraceSummary& summary)
{
	std::string out = summary.cause;
	if(!summary.detail.empty())
		out += "\n  " + summary.detail;
	if(!summary.frames.empty())
	{
		out += "\nbacktrace (" + std::to_string(summary.total_frames) + " frame" + (summary.total_frames == 1 ? "" : "s") + "):";
		for(auto& line : summary.frames)
			out += "\n  " + line;
	}
	return(out);
}

inline std::string wasm_trace_collapse(const std::string& message, size_t max_frame_lines = 12)
{
	return(wasm_trace_format(wasm_trace_summarize(message, max_frame_lines)));
}
