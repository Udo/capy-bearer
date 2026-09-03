#include "lsp.h"

#include "compiler.h"
#include "frontend.h"
#include "stdlib.embedded.h"
#include "../wasm/abi.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace capy::lsp
{
namespace
{
using Clock = std::chrono::steady_clock;

struct Json
{
	using Array = std::vector<Json>;
	using Object = std::map<std::string, Json>;
	std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value = nullptr;
	Json() = default;
	Json(std::nullptr_t) {}
	Json(bool source) : value(source) {}
	Json(int source) : value(static_cast<double>(source)) {}
	Json(std::size_t source) : value(static_cast<double>(source)) {}
	Json(double source) : value(source) {}
	Json(const char* source) : value(std::string(source)) {}
	Json(std::string source) : value(std::move(source)) {}
	Json(Array source) : value(std::move(source)) {}
	Json(Object source) : value(std::move(source)) {}
	bool is_null() const { return std::holds_alternative<std::nullptr_t>(value); }
	bool is_string() const { return std::holds_alternative<std::string>(value); }
	bool is_array() const { return std::holds_alternative<Array>(value); }
	bool is_object() const { return std::holds_alternative<Object>(value); }
	const std::string& string() const { static const std::string empty; auto* result = std::get_if<std::string>(&value); return result ? *result : empty; }
	int integer(int fallback = 0) const { auto* result = std::get_if<double>(&value); return result ? static_cast<int>(*result) : fallback; }
	const Array& array() const { static const Array empty; auto* result = std::get_if<Array>(&value); return result ? *result : empty; }
	const Object& object() const { static const Object empty; auto* result = std::get_if<Object>(&value); return result ? *result : empty; }
	const Json& operator[](std::string_view key) const
	{
		static const Json empty;
		auto* source = std::get_if<Object>(&value);
		if (!source) return empty;
		auto found = source->find(std::string(key));
		return found == source->end() ? empty : found->second;
	}
};

void append_utf8(std::string& output, unsigned value)
{
	if (value <= 0x7f) output.push_back(static_cast<char>(value));
	else if (value <= 0x7ff) { output.push_back(static_cast<char>(0xc0 | value >> 6)); output.push_back(static_cast<char>(0x80 | (value & 0x3f))); }
	else if (value <= 0xffff) { output.push_back(static_cast<char>(0xe0 | value >> 12)); output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f))); output.push_back(static_cast<char>(0x80 | (value & 0x3f))); }
	else { output.push_back(static_cast<char>(0xf0 | value >> 18)); output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f))); output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f))); output.push_back(static_cast<char>(0x80 | (value & 0x3f))); }
}

struct JsonParser
{
	std::string_view source;
	std::size_t position = 0;
	void space() { while (position < source.size() && std::string_view(" \t\r\n").find(source[position]) != std::string_view::npos) ++position; }
	[[noreturn]] void fail() const { throw std::runtime_error("invalid JSON"); }
	bool take(char expected) { space(); if (position < source.size() && source[position] == expected) { ++position; return true; } return false; }
	unsigned hex4()
	{
		unsigned result = 0;
		for (int index = 0; index < 4; ++index)
		{
			if (position == source.size()) fail();
			char c = source[position++];
			result <<= 4;
			if (c >= '0' && c <= '9') result += static_cast<unsigned>(c - '0');
			else if (c >= 'a' && c <= 'f') result += static_cast<unsigned>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') result += static_cast<unsigned>(c - 'A' + 10);
			else fail();
		}
		return result;
	}
	std::string string()
	{
		if (!take('"')) fail();
		std::string result;
		while (position < source.size())
		{
			unsigned char c = static_cast<unsigned char>(source[position++]);
			if (c == '"') return result;
			if (c < 0x20) fail();
			if (c != '\\') { result.push_back(static_cast<char>(c)); continue; }
			if (position == source.size()) fail();
			char escaped = source[position++];
			if (escaped == '"' || escaped == '\\' || escaped == '/') result.push_back(escaped);
			else if (escaped == 'b') result.push_back('\b');
			else if (escaped == 'f') result.push_back('\f');
			else if (escaped == 'n') result.push_back('\n');
			else if (escaped == 'r') result.push_back('\r');
			else if (escaped == 't') result.push_back('\t');
			else if (escaped == 'u')
			{
				unsigned value = hex4();
				if (value >= 0xd800 && value <= 0xdbff)
				{
					if (position + 2 > source.size() || source.substr(position, 2) != "\\u") fail();
					position += 2;
					unsigned low = hex4();
					if (low < 0xdc00 || low > 0xdfff) fail();
					value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
				}
				append_utf8(result, value);
			}
			else fail();
		}
		fail();
	}
	Json parse(unsigned depth = 0)
	{
		if (depth > 64) fail();
		space();
		if (position == source.size()) fail();
		if (source[position] == '"') return string();
		if (take('{'))
		{
			Json::Object result;
			if (take('}')) return result;
			do { std::string key = string(); if (!take(':')) fail(); result.emplace(std::move(key), parse(depth + 1)); } while (take(','));
			if (!take('}')) fail();
			return result;
		}
		if (take('['))
		{
			Json::Array result;
			if (take(']')) return result;
			do result.push_back(parse(depth + 1)); while (take(','));
			if (!take(']')) fail();
			return result;
		}
		for (const auto& [word, result] : std::vector<std::pair<std::string_view, Json>>{{"true", true}, {"false", false}, {"null", nullptr}})
			if (source.substr(position, word.size()) == word) { position += word.size(); return result; }
		std::size_t begin = position;
		if (source[position] == '-') ++position;
		while (position < source.size() && source[position] >= '0' && source[position] <= '9') ++position;
		if (position < source.size() && source[position] == '.') { ++position; while (position < source.size() && source[position] >= '0' && source[position] <= '9') ++position; }
		if (position < source.size() && (source[position] == 'e' || source[position] == 'E')) { ++position; if (position < source.size() && (source[position] == '+' || source[position] == '-')) ++position; while (position < source.size() && source[position] >= '0' && source[position] <= '9') ++position; }
		if (begin == position) fail();
		try { return std::stod(std::string(source.substr(begin, position - begin))); } catch (...) { fail(); }
	}
};

Json parse_json(std::string_view source)
{
	JsonParser parser{source};
	Json result = parser.parse();
	parser.space();
	if (parser.position != source.size()) parser.fail();
	return result;
}

void dump_string(std::string& output, std::string_view source)
{
	static constexpr char hex[] = "0123456789abcdef";
	output.push_back('"');
	for (unsigned char c : source)
	{
		if (c == '"' || c == '\\') { output.push_back('\\'); output.push_back(static_cast<char>(c)); }
		else if (c == '\b') output += "\\b";
		else if (c == '\f') output += "\\f";
		else if (c == '\n') output += "\\n";
		else if (c == '\r') output += "\\r";
		else if (c == '\t') output += "\\t";
		else if (c < 0x20) { output += "\\u00"; output.push_back(hex[c >> 4]); output.push_back(hex[c & 15]); }
		else output.push_back(static_cast<char>(c));
	}
	output.push_back('"');
}

void dump_json(std::string& output, const Json& source)
{
	if (source.is_null()) output += "null";
	else if (auto* value = std::get_if<bool>(&source.value)) output += *value ? "true" : "false";
	else if (auto* value = std::get_if<double>(&source.value)) { std::ostringstream text; text.precision(17); text << *value; output += text.str(); }
	else if (auto* value = std::get_if<std::string>(&source.value)) dump_string(output, *value);
	else if (auto* value = std::get_if<Json::Array>(&source.value))
	{
		output.push_back('[');
		for (std::size_t index = 0; index < value->size(); ++index) { if (index) output.push_back(','); dump_json(output, (*value)[index]); }
		output.push_back(']');
	}
	else
	{
		output.push_back('{');
		bool comma = false;
		for (const auto& [key, value] : std::get<Json::Object>(source.value)) { if (comma) output.push_back(','); comma = true; dump_string(output, key); output.push_back(':'); dump_json(output, value); }
		output.push_back('}');
	}
}

std::size_t utf8_size(unsigned char value)
{
	if ((value & 0x80) == 0) return 1;
	if ((value & 0xe0) == 0xc0) return 2;
	if ((value & 0xf0) == 0xe0) return 3;
	if ((value & 0xf8) == 0xf0) return 4;
	return 1;
}

unsigned codepoint(std::string_view text, std::size_t byte)
{
	unsigned char first = static_cast<unsigned char>(text[byte]);
	std::size_t size = std::min(utf8_size(first), text.size() - byte);
	if (size == 1) return first;
	unsigned result = first & ((1u << (7 - size)) - 1);
	for (std::size_t index = 1; index < size; ++index) result = (result << 6) | (static_cast<unsigned char>(text[byte + index]) & 0x3f);
	return result;
}

std::size_t codepoints(std::string_view text)
{
	std::size_t result = 0;
	for (std::size_t byte = 0; byte < text.size(); byte += std::min(utf8_size(static_cast<unsigned char>(text[byte])), text.size() - byte)) ++result;
	return result;
}

struct PositionCodec
{
	bool utf32 = false;
	std::string_view line(std::string_view text, std::size_t wanted) const
	{
		std::size_t start = 0;
		for (std::size_t current = 0; current < wanted; ++current) { start = text.find('\n', start); if (start == std::string_view::npos) return {}; ++start; }
		std::size_t end = text.find('\n', start);
		return text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
	}
	std::size_t units(std::string_view text, std::size_t capy_column) const
	{
		std::size_t result = 0, count = 1;
		for (std::size_t byte = 0; byte < text.size() && count < capy_column; ++count)
		{
			unsigned value = codepoint(text, byte);
			byte += std::min(utf8_size(static_cast<unsigned char>(text[byte])), text.size() - byte);
			result += utf32 || value <= 0xffff ? 1 : 2;
		}
		return result;
	}
	std::size_t column(std::string_view text, std::size_t lsp_units) const
	{
		std::size_t result = 1, used = 0;
		for (std::size_t byte = 0; byte < text.size() && used < lsp_units; ++result)
		{
			unsigned value = codepoint(text, byte);
			std::size_t width = utf32 || value <= 0xffff ? 1 : 2;
			if (used + width > lsp_units) break;
			used += width;
			byte += std::min(utf8_size(static_cast<unsigned char>(text[byte])), text.size() - byte);
		}
		return result;
	}
	Json position(std::string_view text, const Location& location) const
	{
		return Json::Object{{"line", location.line ? location.line - 1 : 0}, {"character", units(line(text, location.line ? location.line - 1 : 0), location.column)}};
	}
};

struct Declaration
{
	enum class Kind { function, structure, alias };
	std::string name, signature, uri;
	Location location, name_location;
	Kind kind = Kind::function;
};

std::string function_signature(const Function& function)
{
	std::string result = "function " + function.name + "(";
	for (std::size_t index = 0; index < function.parameters.size(); ++index)
	{
		if (index) result += ", ";
		const Parameter& parameter = function.parameters[index];
		if (parameter.variadic) result += "...";
		result += parameter.name + " : ";
		if (parameter.convert) result += "as ";
		result += type_name(*parameter.type_expr);
		if (parameter.default_value) result += " = ...";
	}
	result += ")";
	if (function.return_type) result += " " + type_name(*function.return_type);
	return result;
}

Location name_location(const Expr& expression, std::string_view name, const std::vector<Token>& tokens)
{
	for (const Token& token : tokens)
		if (token.location.offset >= expression.location.offset && token.kind == TokenKind::identifier && token.text == name)
			return token.location;
	return expression.location;
}

std::vector<Declaration> declarations(const Program& program, const std::vector<Token>& tokens, const std::string& uri)
{
	std::vector<Declaration> result;
	for (Expr* item : program.items)
	{
		Declaration declaration;
		declaration.uri = uri;
		declaration.location = item->location;
		if (item->kind == ExprKind::Function)
		{
			auto& function = *static_cast<Function*>(item);
			if (function.name.starts_with("__")) continue;
			declaration.name = function.name;
			declaration.signature = function_signature(function);
			declaration.kind = Declaration::Kind::function;
		}
		else if (item->kind == ExprKind::Struct)
		{
			auto& structure = *static_cast<Struct*>(item);
			declaration.name = structure.name;
			declaration.signature = "struct " + structure.name;
			declaration.kind = Declaration::Kind::structure;
		}
		else if (item->kind == ExprKind::TypeAlias)
		{
			auto& alias = *static_cast<TypeAlias*>(item);
			declaration.name = alias.name;
			declaration.signature = "type " + alias.name + " = " + type_name(*alias.value);
			declaration.kind = Declaration::Kind::alias;
		}
		else continue;
		declaration.name_location = name_location(*item, declaration.name, tokens);
		result.push_back(std::move(declaration));
	}
	return result;
}

struct Diagnostic
{
	Location location;
	std::string message;
};

struct Analysis
{
	std::string uri, text;
	std::size_t revision = 0;
	std::vector<Token> tokens;
	std::vector<Declaration> declarations;
	std::optional<Diagnostic> diagnostic;
};

struct Task
{
	std::string uri, text;
	std::size_t revision = 0;
	std::shared_ptr<std::atomic<bool>> cancelled;
};

class AnalysisWorker
{
	std::mutex mutex_;
	std::condition_variable condition_;
	std::deque<Task> queued_;
	std::vector<Analysis> results_;
	bool stopping_ = false;
	std::shared_ptr<std::atomic<bool>> active_cancel_;
	std::thread thread_;
	int notify_[2] = {-1, -1};
	ParsedSourceCache cache_;

	Analysis analyze(const Task& task)
	{
		Analysis result;
		result.uri = task.uri;
		result.text = task.text;
		result.revision = task.revision;
		auto cancelled = [flag = task.cancelled] { return flag->load(); };
		try
		{
			result.tokens = Lexer(task.text, task.uri, 1, 1, 0, cancelled).tokens();
			Program program = Parser(result.tokens, cancelled).parse();
			result.declarations = declarations(program, result.tokens, task.uri);
			CompileOptions options;
			options.source_path = task.uri;
			options.module_name = "lsp.wasm";
			options.abi_version = BEARER_WASM_CORE_ABI_VERSION;
			options.cancelled = cancelled;
			options.canonical_source_identity = task.uri;
			options.parsed_source_cache = &cache_;
			compile_bearer_unit(task.text, options);
		}
		catch (const Error& error)
		{
			if (!task.cancelled->load() && error.message != "Capy compilation cancelled") result.diagnostic = Diagnostic{error.location, error.message};
		}
		catch (const std::exception& error)
		{
			if (!task.cancelled->load()) result.diagnostic = Diagnostic{{task.uri, 1, 1, 0}, error.what()};
		}
		return result;
	}

	void loop()
	{
		for (;;)
		{
			Task task;
			{
				std::unique_lock lock(mutex_);
				condition_.wait(lock, [&] { return stopping_ || !queued_.empty(); });
				if (stopping_) return;
				task = std::move(queued_.front());
				queued_.pop_front();
				active_cancel_ = task.cancelled;
			}
			Analysis result = analyze(task);
			{
				std::lock_guard lock(mutex_);
				active_cancel_.reset();
				if (!task.cancelled->load()) results_.push_back(std::move(result));
			}
			const char byte = 1;
			if (::write(notify_[1], &byte, 1) < 0 && errno != EAGAIN) {}
		}
	}

public:
	AnalysisWorker()
	{
		if (::pipe(notify_) != 0) throw std::runtime_error("could not create LSP worker pipe");
		thread_ = std::thread([this] { loop(); });
	}
	~AnalysisWorker()
	{
		{
			std::lock_guard lock(mutex_);
			stopping_ = true;
			if (active_cancel_) active_cancel_->store(true);
			for (Task& task : queued_) task.cancelled->store(true);
		}
		condition_.notify_one();
		if (thread_.joinable()) thread_.join();
		::close(notify_[0]);
		::close(notify_[1]);
	}
	int notify_fd() const { return notify_[0]; }
	void submit(Task task)
	{
		std::lock_guard lock(mutex_);
		for (auto iterator = queued_.begin(); iterator != queued_.end();)
			if (iterator->uri == task.uri) { iterator->cancelled->store(true); iterator = queued_.erase(iterator); }
			else ++iterator;
		queued_.push_back(std::move(task));
		condition_.notify_one();
	}
	void cancel()
	{
		std::lock_guard lock(mutex_);
		if (active_cancel_) active_cancel_->store(true);
	}
	std::vector<Analysis> take()
	{
		char bytes[64];
		if (::read(notify_[0], bytes, sizeof(bytes)) < 0 && errno != EINTR) {}
		std::lock_guard lock(mutex_);
		return std::exchange(results_, {});
	}
};

class Frames
{
	std::string buffer_;
public:
	bool read_from(int fd)
	{
		char data[8192];
		ssize_t size = ::read(fd, data, sizeof(data));
		if (size > 0) { buffer_.append(data, static_cast<std::size_t>(size)); return true; }
		if (size < 0 && errno == EINTR) return true;
		return false;
	}
	std::optional<Json> pop()
	{
		std::size_t split = buffer_.find("\r\n\r\n");
		if (split == std::string::npos)
		{
			if (buffer_.size() > 65536) throw std::runtime_error("LSP headers are too large");
			return {};
		}
		if (split > 65536) throw std::runtime_error("LSP headers are too large");
		std::size_t length = std::string::npos;
		std::istringstream headers(buffer_.substr(0, split));
		for (std::string line; std::getline(headers, line);)
		{
			if (!line.empty() && line.back() == '\r') line.pop_back();
			auto colon = line.find(':');
			std::string name = line.substr(0, colon);
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
			if (name == "content-length" && colon != std::string::npos)
			{
				if (length != std::string::npos) throw std::runtime_error("duplicate Content-Length header");
				length = std::stoull(line.substr(colon + 1));
			}
		}
		if (length == std::string::npos || length > 16 * 1024 * 1024) throw std::runtime_error("invalid Content-Length header");
		std::size_t body = split + 4;
		if (buffer_.size() - body < length) return {};
		Json result = parse_json(std::string_view(buffer_).substr(body, length));
		buffer_.erase(0, body + length);
		return result;
	}
};

void write_all(int fd, std::string_view text)
{
	while (!text.empty())
	{
		ssize_t size = ::write(fd, text.data(), text.size());
		if (size < 0 && errno == EINTR) continue;
		if (size <= 0) throw std::runtime_error("could not write LSP response");
		text.remove_prefix(static_cast<std::size_t>(size));
	}
}

std::string uri_path(std::string uri)
{
	if (!uri.starts_with("file://")) return {};
	uri.erase(0, 7);
	std::string result;
	for (std::size_t index = 0; index < uri.size(); ++index)
	{
		if (uri[index] == '%' && index + 2 < uri.size())
		{
			auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
			int high = digit(uri[index + 1]), low = digit(uri[index + 2]);
			if (high >= 0 && low >= 0) { result.push_back(static_cast<char>(high * 16 + low)); index += 2; continue; }
		}
		result.push_back(uri[index]);
	}
	return result;
}

std::string path_uri(const std::filesystem::path& path)
{
	std::string result = "file://";
	for (unsigned char c : path.string())
	{
		if (std::isalnum(c) || std::string_view("/-._~").find(static_cast<char>(c)) != std::string_view::npos) result.push_back(static_cast<char>(c));
		else { static constexpr char hex[] = "0123456789ABCDEF"; result.push_back('%'); result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
	}
	return result;
}

struct Document
{
	std::string text;
	std::size_t revision = 0;
	Clock::time_point due;
	bool pending = false;
	std::shared_ptr<std::atomic<bool>> cancelled;
	std::optional<Analysis> analysis;
};

class Server
{
	int input_, output_;
	Frames frames_;
	AnalysisWorker worker_;
	std::map<std::string, Document> documents_;
	std::vector<Declaration> standard_;
	PositionCodec positions_;
	std::filesystem::path workspace_;
	std::string site_directory_, compiler_sys_path_;
	bool explicit_workspace_ = false, shutdown_ = false, exit_ = false;

	void send(const Json& message)
	{
		std::string body;
		dump_json(body, message);
		write_all(output_, "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
	}
	void respond(const Json& id, Json result) { send(Json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}); }
	void error(const Json& id, int code, std::string message) { send(Json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json::Object{{"code", code}, {"message", std::move(message)}}}}); }
	void notify(std::string method, Json params) { send(Json::Object{{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}}); }

	Json range(std::string_view text, const Location& start, std::size_t length = 1) const
	{
		Location end = start;
		end.column += std::max<std::size_t>(1, length);
		return Json::Object{{"start", positions_.position(text, start)}, {"end", positions_.position(text, end)}};
	}
	std::size_t token_length(const Analysis& analysis, const Token& token) const
	{
		if (token.kind == TokenKind::directive) return codepoints(token.text) + 1;
		if (token.kind != TokenKind::string) return std::max<std::size_t>(1, codepoints(token.text));
		std::string_view line = positions_.line(analysis.text, token.location.line - 1);
		std::size_t byte = 0;
		for (std::size_t column = 1; byte < line.size() && column < token.location.column; ++column)
			byte += std::min(utf8_size(static_cast<unsigned char>(line[byte])), line.size() - byte);
		std::size_t start = byte;
		bool escaped = false;
		while (byte < line.size())
		{
			char value = line[byte];
			byte += std::min(utf8_size(static_cast<unsigned char>(line[byte])), line.size() - byte);
			if (value == '"' && byte > start + 1 && !escaped) break;
			escaped = value == '\\' && !escaped;
			if (value != '\\') escaped = false;
		}
		return std::max<std::size_t>(1, codepoints(line.substr(start, byte - start)));
	}
	Json diagnostic_range(const Analysis& analysis) const
	{
		const Location& location = analysis.diagnostic->location;
		for (const Token& token : analysis.tokens)
		{
			std::size_t length = token_length(analysis, token);
			if (token.location.offset <= location.offset && location.offset < token.location.offset + length)
				return range(analysis.text, token.location, length);
		}
		return range(analysis.text, location);
	}
	void publish(const Analysis& analysis)
	{
		Json::Array diagnostics;
		if (analysis.diagnostic)
		{
			const Diagnostic& diagnostic = *analysis.diagnostic;
			diagnostics.push_back(Json::Object{{"range", diagnostic_range(analysis)}, {"severity", 1}, {"source", "capyc"}, {"message", diagnostic.message}});
		}
		notify("textDocument/publishDiagnostics", Json::Object{{"uri", analysis.uri}, {"diagnostics", diagnostics}});
	}
	void schedule(const std::string& uri, std::string text)
	{
		Document& document = documents_[uri];
		if (document.cancelled) document.cancelled->store(true);
		document.text = std::move(text);
		++document.revision;
		document.pending = true;
		document.due = Clock::now() + std::chrono::milliseconds(150);
		document.cancelled = std::make_shared<std::atomic<bool>>(false);
	}
	void submit_due()
	{
		auto now = Clock::now();
		for (auto& [uri, document] : documents_)
			if (document.pending && document.due <= now)
			{
				document.pending = false;
				worker_.submit({uri, document.text, document.revision, document.cancelled});
			}
	}
	int timeout() const
	{
		std::optional<Clock::time_point> due;
		for (const auto& [uri, document] : documents_) { (void)uri; if (document.pending && (!due || document.due < *due)) due = document.due; }
		if (!due) return -1;
		auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*due - Clock::now()).count();
		return static_cast<int>(std::clamp<long long>(remaining, 0, 1000));
	}
	void results()
	{
		for (Analysis& analysis : worker_.take())
		{
			auto found = documents_.find(analysis.uri);
			if (found == documents_.end() || found->second.revision != analysis.revision) continue;
			publish(analysis);
			found->second.analysis = std::move(analysis);
		}
	}
	const Analysis* analysis(const std::string& uri) const
	{
		auto found = documents_.find(uri);
		return found == documents_.end() || !found->second.analysis ? nullptr : &*found->second.analysis;
	}
	std::string word_at(const Analysis& source, const Json& position) const
	{
		std::size_t line = static_cast<std::size_t>(position["line"].integer());
		std::size_t column = positions_.column(positions_.line(source.text, line), static_cast<std::size_t>(position["character"].integer()));
		for (const Token& token : source.tokens)
			if (token.kind == TokenKind::identifier && token.location.line == line + 1 && column >= token.location.column && column <= token.location.column + codepoints(token.text))
				return token.text;
		return {};
	}
	std::vector<const Declaration*> named(const Analysis& source, std::string_view name) const
	{
		std::vector<const Declaration*> result;
		for (const Declaration& declaration : source.declarations) if (declaration.name == name) result.push_back(&declaration);
		for (const Declaration& declaration : standard_) if (declaration.name == name) result.push_back(&declaration);
		return result;
	}
	Json declaration_location(const Declaration& declaration, std::string_view text) const
	{
		return Json::Object{{"uri", declaration.uri}, {"range", range(text, declaration.name_location, codepoints(declaration.name))}};
	}
	void refresh_workspace()
	{
		if (explicit_workspace_) return;
		std::filesystem::path root = compiler_sys_path_.empty() ? std::filesystem::current_path() : std::filesystem::path(compiler_sys_path_);
		workspace_ = site_directory_.empty() ? root : root / site_directory_;
	}
	void configuration(const Json& value)
	{
		const Json& settings = value["settings"].is_object() ? value["settings"] : value;
		if (settings["SITE_DIRECTORY"].is_string()) site_directory_ = settings["SITE_DIRECTORY"].string();
		if (settings["COMPILER_SYS_PATH"].is_string()) compiler_sys_path_ = settings["COMPILER_SYS_PATH"].string();
		const Json& capy_settings = settings["capy"];
		if (capy_settings["siteDirectory"].is_string()) site_directory_ = capy_settings["siteDirectory"].string();
		if (capy_settings["compilerSysPath"].is_string()) compiler_sys_path_ = capy_settings["compilerSysPath"].string();
		refresh_workspace();
	}
	void read_settings()
	{
		std::ifstream input("etc/bearer/settings.cfg");
		for (std::string line; std::getline(input, line);)
		{
			auto equal = line.find('=');
			if (equal == std::string::npos || line.starts_with('#')) continue;
			if (line.substr(0, equal) == "SITE_DIRECTORY") site_directory_ = line.substr(equal + 1);
			if (line.substr(0, equal) == "COMPILER_SYS_PATH") compiler_sys_path_ = line.substr(equal + 1);
		}
	}
	Json capabilities() const
	{
		Json::Array token_types = {"namespace", "type", "class", "function", "variable", "string", "number", "keyword", "operator", "html_text", "html_attribute", "javascript_value", "css_value"};
		return Json::Object{{"positionEncoding", positions_.utf32 ? "utf-32" : "utf-16"}, {"textDocumentSync", Json::Object{{"openClose", true}, {"change", 1}}}, {"documentSymbolProvider", true}, {"workspaceSymbolProvider", true}, {"hoverProvider", true}, {"completionProvider", Json::Object{}}, {"signatureHelpProvider", Json::Object{{"triggerCharacters", Json::Array{"(", ","}}}}, {"definitionProvider", true}, {"semanticTokensProvider", Json::Object{{"legend", Json::Object{{"tokenTypes", token_types}, {"tokenModifiers", Json::Array{}}}}, {"full", true}}}};
	}
	void initialize(const Json& id, const Json& params)
	{
		for (const Json& encoding : params["capabilities"]["general"]["positionEncodings"].array()) if (encoding.string() == "utf-32") positions_.utf32 = true;
		std::string root = params["rootUri"].string();
		if (root.empty() && !params["workspaceFolders"].array().empty()) root = params["workspaceFolders"].array().front()["uri"].string();
		workspace_ = uri_path(root);
		explicit_workspace_ = !workspace_.empty();
		configuration(params["initializationOptions"]);
		respond(id, Json::Object{{"capabilities", capabilities()}, {"serverInfo", Json::Object{{"name", "capyc"}}}});
	}
	void did_open(const Json& params) { schedule(params["textDocument"]["uri"].string(), params["textDocument"]["text"].string()); }
	void did_change(const Json& params)
	{
		const auto& changes = params["contentChanges"].array();
		if (!changes.empty()) schedule(params["textDocument"]["uri"].string(), changes.back()["text"].string());
	}
	void did_close(const Json& params)
	{
		std::string uri = params["textDocument"]["uri"].string();
		auto found = documents_.find(uri);
		if (found != documents_.end() && found->second.cancelled) found->second.cancelled->store(true);
		documents_.erase(uri);
		notify("textDocument/publishDiagnostics", Json::Object{{"uri", uri}, {"diagnostics", Json::Array{}}});
	}
	void document_symbols(const Json& id, const Json& params)
	{
		std::string uri = params["textDocument"]["uri"].string();
		const Analysis* source = analysis(uri);
		Json::Array result;
		if (source) for (const Declaration& declaration : source->declarations)
		{
			int kind = declaration.kind == Declaration::Kind::function ? 12 : declaration.kind == Declaration::Kind::structure ? 23 : 5;
			result.push_back(Json::Object{{"name", declaration.name}, {"detail", declaration.signature}, {"kind", kind}, {"range", range(source->text, declaration.location, codepoints(declaration.name) + 8)}, {"selectionRange", range(source->text, declaration.name_location, codepoints(declaration.name))}});
		}
		respond(id, result);
	}
	void workspace_symbols(const Json& id, const Json& params)
	{
		std::string query = params["query"].string();
		std::transform(query.begin(), query.end(), query.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
		Json::Array result;
		if (!workspace_.empty() && std::filesystem::is_directory(workspace_))
		{
			std::error_code error_code;
			std::size_t files = 0, bytes = 0;
			auto deadline = Clock::now() + std::chrono::milliseconds(250);
			for (std::filesystem::recursive_directory_iterator iterator(workspace_, std::filesystem::directory_options::skip_permission_denied, error_code), end;
				 iterator != end && result.size() < 10000 && files < 4096 && bytes < 64 * 1024 * 1024 && Clock::now() < deadline; iterator.increment(error_code))
			{
				if (error_code) { error_code.clear(); continue; }
				if (iterator->is_symlink(error_code)) { if (iterator->is_directory(error_code)) iterator.disable_recursion_pending(); continue; }
				if (!iterator->is_regular_file(error_code) || iterator->path().extension() != ".capy") continue;
				std::uintmax_t file_size = iterator->file_size(error_code);
				if (error_code || file_size > 4 * 1024 * 1024 || file_size > 64 * 1024 * 1024 - bytes) { error_code.clear(); continue; }
				++files;
				bytes += static_cast<std::size_t>(file_size);
				try
				{
					std::ifstream input(iterator->path(), std::ios::binary);
					std::string text{std::istreambuf_iterator<char>(input), {}};
					std::string uri = path_uri(iterator->path());
					auto tokens = Lexer(text, uri).tokens();
					auto declarations_value = declarations(Parser(tokens).parse(), tokens, uri);
					for (const Declaration& declaration : declarations_value)
					{
						std::string name = declaration.name;
						std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
						if (!query.empty() && name.find(query) == std::string::npos) continue;
						result.push_back(Json::Object{{"name", declaration.name}, {"kind", declaration.kind == Declaration::Kind::function ? 12 : declaration.kind == Declaration::Kind::structure ? 23 : 5}, {"location", declaration_location(declaration, text)}});
					}
				}
				catch (...) {}
			}
		}
		respond(id, result);
	}
	void hover(const Json& id, const Json& params)
	{
		std::string uri = params["textDocument"]["uri"].string();
		const Analysis* source = analysis(uri);
		if (!source) { respond(id, nullptr); return; }
		std::string word = word_at(*source, params["position"]);
		auto matches = named(*source, word);
		if (matches.empty()) { respond(id, nullptr); return; }
		std::string text;
		for (const Declaration* declaration : matches) { if (!text.empty()) text += "\n\n"; text += "```capy\n" + declaration->signature + "\n```"; }
		if (std::any_of(matches.begin(), matches.end(), [](const Declaration* declaration) { return declaration->uri == "capy://stdlib.capy"; }))
		{
			std::replace(word.begin(), word.end(), '_', '-');
			text += "\n\n[API documentation](/doc/api/" + word + "/)";
		}
		respond(id, Json::Object{{"contents", Json::Object{{"kind", "markdown"}, {"value", text}}}});
	}
	void completion(const Json& id, const Json& params)
	{
		std::string uri = params["textDocument"]["uri"].string();
		const Analysis* source = analysis(uri);
		Json::Array result;
		std::set<std::string> seen;
		auto add = [&](const Declaration& declaration) { if (seen.insert(declaration.name).second) result.push_back(Json::Object{{"label", declaration.name}, {"kind", declaration.kind == Declaration::Kind::function ? 3 : declaration.kind == Declaration::Kind::structure ? 7 : 25}, {"detail", declaration.signature}}); };
		if (source) for (const Declaration& declaration : source->declarations) add(declaration);
		for (const Declaration& declaration : standard_) add(declaration);
		respond(id, result);
	}
	void definition(const Json& id, const Json& params)
	{
		std::string uri = params["textDocument"]["uri"].string();
		const Analysis* source = analysis(uri);
		if (!source) { respond(id, nullptr); return; }
		std::string word = word_at(*source, params["position"]);
		Json::Array result;
		for (const Declaration* declaration : named(*source, word))
		{
			std::string_view text = declaration->uri == source->uri ? std::string_view(source->text) : stdlib::text;
			result.push_back(declaration_location(*declaration, text));
		}
		respond(id, result.empty() ? Json(nullptr) : Json(result));
	}
	void signature_help(const Json& id, const Json& params)
	{
		std::string uri = params["textDocument"]["uri"].string();
		const Analysis* source = analysis(uri);
		if (!source) { respond(id, nullptr); return; }
		std::size_t line_number = static_cast<std::size_t>(params["position"]["line"].integer());
		std::string_view line = positions_.line(source->text, line_number);
		std::size_t capy_column = positions_.column(line, static_cast<std::size_t>(params["position"]["character"].integer()));
		std::size_t byte = 0;
		for (std::size_t column = 1; byte < line.size() && column < capy_column; ++column) byte += std::min(utf8_size(static_cast<unsigned char>(line[byte])), line.size() - byte);
		std::string prefix(line.substr(0, byte));
		auto open = prefix.rfind('(');
		if (open == std::string::npos) { respond(id, nullptr); return; }
		std::size_t end = open;
		while (end && std::isspace(static_cast<unsigned char>(prefix[end - 1]))) --end;
		std::size_t begin = end;
		while (begin && (std::isalnum(static_cast<unsigned char>(prefix[begin - 1])) || prefix[begin - 1] == '_')) --begin;
		std::string name = prefix.substr(begin, end - begin);
		Json::Array signatures;
		for (const Declaration* declaration : named(*source, name)) if (declaration->kind == Declaration::Kind::function) signatures.push_back(Json::Object{{"label", declaration->signature}});
		if (signatures.empty()) respond(id, nullptr); else respond(id, Json::Object{{"signatures", signatures}, {"activeSignature", 0}, {"activeParameter", static_cast<int>(std::count(prefix.begin() + static_cast<std::ptrdiff_t>(open), prefix.end(), ','))}});
	}
	int semantic_kind(const Token& token) const
	{
		if (token.kind == TokenKind::string) return 5;
		if (token.kind == TokenKind::integer || token.kind == TokenKind::floating) return 6;
		if (token.kind == TokenKind::directive) return 7;
		if (token.kind == TokenKind::symbol) return 8;
		if (token.kind == TokenKind::identifier)
		{
			static const std::set<std::string> keywords = {"function", "struct", "type", "host", "trace", "var", "return", "if", "else", "while", "for", "break", "continue", "none"};
			return keywords.contains(token.text) ? 7 : 4;
		}
		return -1;
	}
	int markup_kind(bearer::MarkupContext context) const
	{
		if (context == bearer::MarkupContext::html_attribute) return 10;
		if (context == bearer::MarkupContext::javascript_value) return 11;
		if (context == bearer::MarkupContext::css_value) return 12;
		return 9;
	}
	void semantic_tokens(const Json& id, const Json& params)
	{
		std::string uri = params["textDocument"]["uri"].string();
		const Analysis* source = analysis(uri);
		struct Item { std::size_t line, column, length; int kind; };
		std::vector<Item> items;
		if (source) for (const Token& token : source->tokens)
		{
			if (token.kind == TokenKind::markup)
			{
				for (const MarkupTokenPart& part : token.markup)
				{
					std::size_t line = part.location.line, column = part.location.column;
					std::size_t start = 0;
					for (;;)
					{
						auto newline = part.source.find('\n', start);
						std::string_view piece(part.source.data() + start, (newline == std::string::npos ? part.source.size() : newline) - start);
						if (!piece.empty()) items.push_back({line - 1, positions_.units(positions_.line(source->text, line - 1), column), positions_.utf32 ? codepoints(piece) : positions_.units(piece, codepoints(piece) + 1), markup_kind(part.context)});
						if (newline == std::string::npos) break;
						start = newline + 1; ++line; column = 1;
					}
				}
				continue;
			}
			int kind = semantic_kind(token);
			if (kind < 0 || token.kind == TokenKind::newline || token.kind == TokenKind::eof) continue;
			std::size_t length = token_length(*source, token);
			if (length)
			{
				std::string_view line = positions_.line(source->text, token.location.line - 1);
				std::size_t start = positions_.units(line, token.location.column);
				std::size_t end = positions_.units(line, token.location.column + length);
				items.push_back({token.location.line - 1, start, end - start, kind});
			}
		}
		std::sort(items.begin(), items.end(), [](const Item& left, const Item& right) { return std::pair(left.line, left.column) < std::pair(right.line, right.column); });
		Json::Array data;
		std::size_t previous_line = 0, previous_column = 0;
		for (const Item& item : items)
		{
			std::size_t delta_line = item.line - previous_line;
			std::size_t delta_column = delta_line ? item.column : item.column - previous_column;
			data.insert(data.end(), {delta_line, delta_column, item.length, item.kind, 0});
			previous_line = item.line; previous_column = item.column;
		}
		respond(id, Json::Object{{"data", data}});
	}
	void message(const Json& message)
	{
		std::string method = message["method"].string();
		const Json& params = message["params"];
		const Json& id = message["id"];
		if (method == "initialize") initialize(id, params);
		else if (method == "initialized") {}
		else if (method == "shutdown") { shutdown_ = true; respond(id, nullptr); }
		else if (method == "exit") exit_ = true;
		else if (method == "$/cancelRequest") worker_.cancel();
		else if (method == "workspace/didChangeConfiguration") configuration(params);
		else if (method == "textDocument/didOpen") did_open(params);
		else if (method == "textDocument/didChange") did_change(params);
		else if (method == "textDocument/didClose") did_close(params);
		else if (method == "textDocument/documentSymbol") document_symbols(id, params);
		else if (method == "workspace/symbol") workspace_symbols(id, params);
		else if (method == "textDocument/hover") hover(id, params);
		else if (method == "textDocument/completion") completion(id, params);
		else if (method == "textDocument/definition") definition(id, params);
		else if (method == "textDocument/signatureHelp") signature_help(id, params);
		else if (method == "textDocument/semanticTokens/full") semantic_tokens(id, params);
		else if (!id.is_null()) error(id, -32601, "method not found: " + method);
	}

public:
	Server(int input, int output) : input_(input), output_(output)
	{
		read_settings();
		auto tokens = Lexer(stdlib::text, "capy://stdlib.capy").tokens();
		standard_ = declarations(Parser(tokens).parse(), tokens, "capy://stdlib.capy");
	}
	int loop()
	{
		while (!exit_)
		{
			submit_due();
			pollfd descriptors[] = {{input_, POLLIN, 0}, {worker_.notify_fd(), POLLIN, 0}};
			int count = ::poll(descriptors, 2, timeout());
			if (count < 0 && errno == EINTR) continue;
			if (count < 0) throw std::runtime_error("LSP poll failed");
			if (descriptors[1].revents & POLLIN) results();
			if (descriptors[0].revents & (POLLIN | POLLHUP))
			{
				if (!frames_.read_from(input_)) break;
				while (auto message_value = frames_.pop()) message(*message_value);
			}
		}
		return shutdown_ ? 0 : 1;
	}
};

std::string read_source(const std::string& path)
{
	std::istream* input = &std::cin;
	std::ifstream file;
	if (path != "-") { file.open(path, std::ios::binary); if (!file) throw std::runtime_error("could not read " + path); input = &file; }
	return {std::istreambuf_iterator<char>(*input), {}};
}

int check(const std::string& path)
{
	std::string diagnostic_path = path == "-" ? "-" : std::filesystem::absolute(path).string();
	try
	{
		std::string source = read_source(path);
		CompileOptions options;
		options.source_path = diagnostic_path;
		options.module_name = "check.wasm";
		options.abi_version = BEARER_WASM_CORE_ABI_VERSION;
		compile_bearer_unit(source, options);
		return 0;
	}
	catch (const Error& error)
	{
		std::cerr << error.location.file << ':' << error.location.line << ':' << error.location.column << ": " << error.message << '\n';
		return 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "capyc: " << error.what() << '\n';
		return 1;
	}
}

int socket_server(const std::string& path)
{
	struct SocketPath
	{
		const std::string& path;
		dev_t device = 0;
		ino_t inode = 0;
		bool active = false;
		~SocketPath()
		{
			struct stat current {};
			if (active && ::lstat(path.c_str(), &current) == 0 && current.st_dev == device && current.st_ino == inode) ::unlink(path.c_str());
		}
	} cleanup{path};
	if (path.size() >= sizeof(sockaddr_un::sun_path)) throw std::runtime_error("LSP socket path is too long");
	struct stat status {};
	if (::lstat(path.c_str(), &status) == 0)
	{
		if (!S_ISSOCK(status.st_mode)) throw std::runtime_error("LSP socket path exists and is not a socket: " + path);
		int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
		sockaddr_un existing{};
		existing.sun_family = AF_UNIX;
		std::memcpy(existing.sun_path, path.c_str(), path.size() + 1);
		if (probe >= 0 && ::connect(probe, reinterpret_cast<sockaddr*>(&existing), sizeof(existing)) == 0)
		{
			::close(probe);
			throw std::runtime_error("LSP socket is already in use: " + path);
		}
		if (probe >= 0) ::close(probe);
		if (::unlink(path.c_str()) != 0) throw std::runtime_error("could not remove stale LSP socket: " + path);
	}
	int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener < 0) throw std::runtime_error("could not create LSP socket");
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
	if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
	{
		int saved = errno; ::close(listener); throw std::runtime_error("could not bind LSP socket: " + std::string(std::strerror(saved)));
	}
	if (::lstat(path.c_str(), &status) != 0)
	{
		int saved = errno; ::close(listener); throw std::runtime_error("could not inspect LSP socket: " + std::string(std::strerror(saved)));
	}
	cleanup.device = status.st_dev;
	cleanup.inode = status.st_ino;
	cleanup.active = true;
	if (::chmod(path.c_str(), 0600) != 0 || ::listen(listener, 1) != 0)
	{
		int saved = errno; ::close(listener); throw std::runtime_error("could not listen on LSP socket: " + std::string(std::strerror(saved)));
	}
	int connection = ::accept(listener, nullptr, nullptr);
	::close(listener);
	if (connection < 0) throw std::runtime_error("could not accept LSP socket connection");
	int result = Server(connection, connection).loop();
	::close(connection);
	return result;
}

} // namespace

int run(int argc, char** argv)
{
	if (argc >= 2 && std::string_view(argv[1]) == "--check")
	{
		if (argc != 3) { std::cerr << "capyc: --check requires one file or -\n"; return 2; }
		return check(argv[2]);
	}
	if (argc < 2 || std::string_view(argv[1]) != "--lsp") return -1;
	try
	{
		if (argc == 2) return Server(STDIN_FILENO, STDOUT_FILENO).loop();
		if (argc == 4 && std::string_view(argv[2]) == "--socket") return socket_server(argv[3]);
		std::cerr << "capyc: usage: capyc --lsp [--socket PATH]\n";
		return 2;
	}
	catch (const std::exception& error)
	{
		std::cerr << "capyc: " << error.what() << '\n';
		return 1;
	}
}

} // namespace capy::lsp
