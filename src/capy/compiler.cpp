#include "compiler.h"

#include "frontend.h"
#include "stdlib.embedded.h"
#include "../wasm/abi.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unistd.h>

#ifndef CAPY_COMPILER_BUILD_ID
#define CAPY_COMPILER_BUILD_ID "unversioned"
#endif

namespace capy
{

struct ParsedSourceCache::State
{
	struct Key
	{
		std::uint64_t digest;
		std::string canonical_identity, diagnostic_identity, parser_compiler_identity;
		unsigned abi_version;
		bool operator==(const Key&) const = default;
		std::size_t charged_bytes() const
		{
			return sizeof(digest) + canonical_identity.size() + diagnostic_identity.size() + parser_compiler_identity.size() + sizeof(abi_version);
		}
	};
	struct Entry
	{
		Key key;
		std::string source;
		std::shared_ptr<const Program> program;
		std::size_t charged = 0;
		bool pinned = false;
	};
	pid_t owner_pid;
	mutable std::mutex mutex;
	std::list<Entry> entries;
	std::size_t user_entries = 0;
	ParsedSourceCacheStats stats;
	explicit State(pid_t owner) : owner_pid(owner) {}
};

ParsedSourceCache::ParsedSourceCache(std::size_t max_entries, std::size_t max_charged_bytes, std::size_t max_source_bytes)
	: state_(std::make_shared<State>(getpid())), max_entries_(max_entries), max_charged_bytes_(max_charged_bytes), max_source_bytes_(max_source_bytes)
{
}
ParsedSourceCache::~ParsedSourceCache() = default;

static void cache_check_cancelled(const CancellationCallback& cancelled, const std::string& identity)
{
	if (cancelled && cancelled())
		throw Error({identity, 1, 1, 0}, "Capy compilation cancelled");
}

static std::uint64_t cache_digest(std::string_view source, const CancellationCallback& cancelled, const std::string& identity)
{
	std::uint64_t hash = 1469598103934665603ull;
	for (std::size_t index = 0; index < source.size(); ++index)
	{
		if ((index & 4095) == 0)
			cache_check_cancelled(cancelled, identity);
		hash = (hash ^ static_cast<unsigned char>(source[index])) * 1099511628211ull;
	}
	cache_check_cancelled(cancelled, identity);
	return hash;
}

std::shared_ptr<const Program> ParsedSourceCache::acquire(std::string_view source, const std::string& canonical_identity,
	const std::string& diagnostic_identity, const std::string& parser_compiler_identity, unsigned abi_version,
	CancellationCallback cancelled, bool pinned)
{
	cache_check_cancelled(cancelled, diagnostic_identity);
	if (canonical_identity.empty())
	{
		auto parsed = std::make_shared<const Program>(parse(source, diagnostic_identity, cancelled));
		cache_check_cancelled(cancelled, diagnostic_identity);
		return parsed;
	}
	const State::Key key{cache_digest(source, cancelled, diagnostic_identity), canonical_identity, diagnostic_identity, parser_compiler_identity, abi_version};
	auto state = state_.load();
	while (state->owner_pid != getpid())
	{
		auto replacement = std::make_shared<State>(getpid());
		state_.compare_exchange_strong(state, replacement);
		state = state_.load();
	}
	std::shared_ptr<const Program> hit;
	{
		std::lock_guard lock(state->mutex);
		for (auto it = state->entries.begin(); it != state->entries.end(); ++it)
			if (it->key == key && it->source == source)
			{
				hit = it->program;
				state->entries.splice(state->entries.begin(), state->entries, it);
				++state->stats.hits;
				break;
			}
		if (!hit)
			++state->stats.misses;
	}
	if (hit)
	{
		cache_check_cancelled(cancelled, diagnostic_identity);
		return hit;
	}
	auto parsed = std::make_shared<const Program>(parse(source, diagnostic_identity, cancelled));
	const std::size_t charged = source.size() + key.charged_bytes() + parsed->storage.size();
	cache_check_cancelled(cancelled, diagnostic_identity);
	if (!pinned && (source.size() > max_source_bytes_ || charged > max_charged_bytes_))
	{
		{
			std::lock_guard lock(state->mutex);
			++state->stats.oversize;
		}
		cache_check_cancelled(cancelled, diagnostic_identity);
		return parsed;
	}
	std::vector<std::shared_ptr<const Program>> released;
	std::shared_ptr<const Program> published;
	{
		std::lock_guard lock(state->mutex);
		for (auto it = state->entries.begin(); it != state->entries.end(); ++it)
			if (it->key == key && it->source == source)
			{
				published = it->program;
				break;
			}
		if (!published)
		{
			state->entries.push_front({key, std::string(source), parsed, pinned ? 0 : charged, pinned});
			if (pinned)
			{
				++state->stats.pinned_entries;
				state->stats.pinned_source_bytes += source.size();
			}
			else
			{
				++state->user_entries;
				state->stats.charged_bytes += charged;
			}
			while ((state->user_entries > max_entries_ || state->stats.charged_bytes > max_charged_bytes_) && !state->entries.empty())
			{
				auto victim = std::prev(state->entries.end());
				if (victim->pinned)
				{
					auto candidate = victim;
					while (candidate != state->entries.begin() && candidate->pinned)
						--candidate;
					if (candidate->pinned)
						break;
					victim = candidate;
				}
				state->stats.charged_bytes -= victim->charged;
				--state->user_entries;
				released.push_back(std::move(victim->program));
				state->entries.erase(victim);
				++state->stats.evictions;
			}
		}
	}
	cache_check_cancelled(cancelled, diagnostic_identity);
	return published ? published : parsed;
}

ParsedSourceCacheStats ParsedSourceCache::stats() const
{
	auto state = state_.load();
	while (state->owner_pid != getpid())
	{
		auto replacement = std::make_shared<State>(getpid());
		state_.compare_exchange_strong(state, replacement);
		state = state_.load();
	}
	std::lock_guard lock(state->mutex);
	auto result = state->stats;
	result.entries = state->user_entries;
	return result;
}

void ParsedSourceCache::clear()
{
	auto state = state_.load();
	while (state->owner_pid != getpid())
	{
		auto replacement = std::make_shared<State>(getpid());
		state_.compare_exchange_strong(state, replacement);
		state = state_.load();
	}
	std::vector<std::shared_ptr<const Program>> released;
	{
		std::lock_guard lock(state->mutex);
		for (auto& entry : state->entries)
			released.push_back(std::move(entry.program));
		state->entries.clear();
		state->user_entries = 0;
		state->stats.entries = state->stats.charged_bytes = state->stats.pinned_entries = state->stats.pinned_source_bytes = 0;
	}
}

namespace detail
{
struct ParsedSourceCacheAccess
{
	static std::shared_ptr<const Program> acquire(ParsedSourceCache& cache, std::string_view source, const std::string& canonical_identity,
		const std::string& diagnostic_identity, const std::string& parser_compiler_identity, unsigned abi_version, CancellationCallback cancelled, bool pinned)
	{
		return cache.acquire(source, canonical_identity, diagnostic_identity, parser_compiler_identity, abi_version, std::move(cancelled), pinned);
	}
};
} // namespace detail

namespace
{

using wasm::Bytes;

bool is_scalar(const std::string& type)
{
	return type == "s8" || type == "s16" || type == "s32" || type == "s64" || type == "u8" || type == "u16" || type == "u32" || type == "u64" || type == "f32" || type == "f64" || type == "bool";
}

bool can_convert(const std::string& source, const std::string& target)
{
	return source == target || (is_scalar(source) && is_scalar(target)) || (is_scalar(source) && target == "string") ||
		(source == "dval" && (is_scalar(target) || target == "string" || target.rfind("function#", 0) == 0));
}

bool primitive_constructor_name(const std::string& name)
{
	return name == "s8" || name == "s16" || name == "s32" || name == "s64" || name == "u8" || name == "u16" || name == "u32" || name == "u64" || name == "f32" || name == "f64" || name == "bool" || name == "string";
}

static_assert(BEARER_WASM_OBJECT_OWNER_OFFSET == BEARER_WASM_OBJECT_REFS_OFFSET + BEARER_WASM_OBJECT_WORD_SIZE);
static_assert(BEARER_WASM_OBJECT_TYPE_OFFSET == BEARER_WASM_OBJECT_OWNER_OFFSET + BEARER_WASM_OBJECT_WORD_SIZE);
static_assert(BEARER_WASM_OBJECT_HEADER_SIZE_OFFSET == BEARER_WASM_OBJECT_TYPE_OFFSET + BEARER_WASM_OBJECT_WORD_SIZE);
static_assert(BEARER_WASM_OBJECT_LENGTH_OFFSET == BEARER_WASM_OBJECT_HEADER_SIZE_OFFSET + BEARER_WASM_OBJECT_WORD_SIZE);
static_assert(BEARER_WASM_OBJECT_CAPACITY_OFFSET == BEARER_WASM_OBJECT_LENGTH_OFFSET + BEARER_WASM_OBJECT_WORD_SIZE);
static_assert(BEARER_WASM_OBJECT_PAYLOAD_OFFSET == BEARER_WASM_OBJECT_CAPACITY_OFFSET + BEARER_WASM_OBJECT_WORD_SIZE);
static_assert(BEARER_WASM_OBJECT_HANDLE_SIZE == BEARER_WASM_OBJECT_PAYLOAD_OFFSET + BEARER_WASM_OBJECT_WORD_SIZE);
static_assert(BEARER_WASM_OBJECT_BLOB_HEADER_SIZE == BEARER_WASM_OBJECT_LENGTH_OFFSET + BEARER_WASM_OBJECT_WORD_SIZE);


std::string sink_format_helper(const std::string& symbol, const std::string& type)
{
	return "sink_format:" + symbol + ":" + type;
}

std::uint8_t wasm_value_type(const std::string& type)
{
	return type == "s64" || type == "u64" ? 0x7e : type == "f32" ? 0x7d : type == "f64" ? 0x7c : 0x7f;
}

bool wide_scalar(const std::string& type)
{
	return type == "s64" || type == "u64" || type == "f64";
}

unsigned array_element_size(const std::string& type)
{
	return wide_scalar(type) ? 8 : 4;
}

std::uint8_t array_load_opcode(const std::string& type)
{
	return type == "s64" || type == "u64" ? 0x29 : type == "f32" ? 0x2a : type == "f64" ? 0x2b : 0x28;
}

std::uint8_t array_store_opcode(const std::string& type)
{
	return type == "s64" || type == "u64" ? 0x37 : type == "f32" ? 0x38 : type == "f64" ? 0x39 : 0x36;
}

struct AggregateLayout
{
	std::vector<unsigned> offsets;
	unsigned size;
};

unsigned align_to(unsigned value, unsigned alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

AggregateLayout aggregate_layout(const std::vector<std::string>& fields, unsigned first_offset)
{
	AggregateLayout result{{}, first_offset};
	for (const std::string& field : fields)
	{
		const unsigned size = array_element_size(field);
		result.size = align_to(result.size, size);
		result.offsets.push_back(result.size);
		result.size += size;
	}
	result.size = align_to(result.size, 8);
	return result;
}

void load_field(Bytes& code, const std::string& type, unsigned offset)
{
	code.push_back(array_load_opcode(type));
	code.push_back(static_cast<std::uint8_t>(array_element_size(type) == 8 ? 3 : 2));
	wasm::append_uleb(code, offset);
}

void store_field(Bytes& code, const std::string& type, unsigned offset)
{
	code.push_back(array_store_opcode(type));
	code.push_back(static_cast<std::uint8_t>(array_element_size(type) == 8 ? 3 : 2));
	wasm::append_uleb(code, offset);
}

void store_i32_constant(Bytes& code, unsigned object, std::int32_t value, unsigned offset)
{
	code.push_back(0x20);
	wasm::append_uleb(code, object);
	code.push_back(0x41);
	wasm::append_sleb32(code, value);
	store_field(code, "s32", offset);
}

void managed_payload_pointer(Bytes& code, unsigned local, const std::string& type)
{
	code.push_back(0x20);
	wasm::append_uleb(code, local);
	if (type == "dval")
		code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET});
	else
		code.insert(code.end(), {0x41, BEARER_WASM_OBJECT_BLOB_HEADER_SIZE, 0x6a});
}

void managed_payload_length(Bytes& code, unsigned local)
{
	code.push_back(0x20);
	wasm::append_uleb(code, local);
	code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET});
}

void managed_payload_span(Bytes& code, unsigned local, const std::string& type)
{
	managed_payload_pointer(code, local, type);
	managed_payload_length(code, local);
}

void append_u32_le(std::string& out, std::uint32_t value)
{
	out.push_back(static_cast<char>(value));
	out.push_back(static_cast<char>(value >> 8));
	out.push_back(static_cast<char>(value >> 16));
	out.push_back(static_cast<char>(value >> 24));
}

bool managed_type(const std::string& type)
{
	return type == "string" || type == "dval" || type.rfind("array<", 0) == 0 || type.rfind("struct:", 0) == 0 ||
		   type.rfind("function#", 0) == 0;
}

std::string normalize_spread_type(std::string type)
{
	if (type.rfind("spread<array<", 0) == 0)
		return type.substr(13, type.size() - 15);
	if (type.rfind("spread<", 0) == 0)
		return type.substr(7, type.size() - 8);
	return type;
}

bool task_suffix_is_valid(std::string_view suffix)
{
	if (suffix.empty() || !(std::isalpha(static_cast<unsigned char>(suffix.front())) || suffix.front() == '_'))
		return false;
	return std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

bool is_handler(const std::string& name, std::string& exported, bool& invalid_task_name)
{
	static const std::map<std::string, std::string> names = {
		{"RENDER", "__bearer_render"}, {"COMPONENT", "__bearer_component"},	  {"CLI", "__bearer_cli"}, {"WS", "__bearer_websocket"}, {"ONCE", "__bearer_once"},
		{"INIT", "__bearer_init"},	   {"SERVE_HTTP", "__bearer_serve_http"}, {"TASK", "__bearer_task"},
	};
	if (auto it = names.find(name); it != names.end())
	{
		exported = it->second;
		return true;
	}
	if (name.rfind("TASK:", 0) == 0)
	{
		const std::string_view suffix(name.data() + 5, name.size() - 5);
		invalid_task_name = !task_suffix_is_valid(suffix);
		if (!invalid_task_name)
			exported = "__bearer_task_" + std::string(suffix);
		return true;
	}
	for (const auto& prefix : {std::pair<std::string_view, std::string_view>{"RENDER:", "__bearer_render_"},
							   {"COMPONENT:", "__bearer_component_"},
							   {"SERVE_HTTP:", "__bearer_serve_http_"}})
	{
		if (name.rfind(prefix.first, 0) == 0 && name.size() > prefix.first.size())
		{
			exported = std::string(prefix.second) + name.substr(prefix.first.size());
			return true;
		}
	}
	return false;
}

std::string type_of_expression(const Expr* expression, bool allow_void = false)
{
	if (!expression)
	{
		if (allow_void)
			return "void";
		throw Error({"<input>", 1, 1, 0}, "function return type cannot be inferred yet; declare it explicitly");
	}
	if (auto array = dynamic_cast<const ArrayLiteral*>(expression))
	{
		if (array->items.size() != 1)
			throw Error(expression->location, "array type requires exactly one element type");
		return "array<" + type_of_expression(array->items[0]) + ">";
	}
	if (auto function = dynamic_cast<const FunctionType*>(expression))
	{
		std::string type = "function(";
		for (std::size_t i = 0; i < function->parameters.size(); ++i)
		{
			if (i)
				type += ',';
			type += type_of_expression(function->parameters[i].type_expr);
		}
		return type + ") " + type_of_expression(function->return_type, true);
	}
	std::string name = type_name(*expression);
	if (name == "any" || name.find("::type") != std::string::npos)
		throw Error(expression->location, "compile-time any and dependent types are only valid in a generic function declaration");
	if (name == "request")
		throw Error(expression->location, "type request was removed. Use dval for Bearer handler input.");
	if (name == "s8" || name == "s16" || name == "s32" || name == "s64" || name == "u8" || name == "u16" || name == "u32" || name == "u64" || name == "f32" || name == "f64" || name == "bool" || name == "string" || name == "dval" ||
		name == "module" || name == "void")
		return name;
	return "struct:" + name;
}

bool integer_type(const std::string& type)
{
	return type == "s8" || type == "s16" || type == "s32" || type == "s64" || type == "u8" || type == "u16" || type == "u32" || type == "u64";
}

bool integer_fits(const Integer& literal, const std::string& type)
{
	if (type == "s8")
		return literal.magnitude <= (literal.negative ? std::uint64_t{1} << 7 : 127);
	if (type == "s16")
		return literal.magnitude <= (literal.negative ? std::uint64_t{1} << 15 : 32767);
	if (type == "s32")
		return literal.magnitude <= (literal.negative ? std::uint64_t{1} << 31 : static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()));
	if (type == "u8") return !literal.negative && literal.magnitude <= 255;
	if (type == "u16") return !literal.negative && literal.magnitude <= 65535;
	if (type == "u32") return !literal.negative && literal.magnitude <= std::numeric_limits<std::uint32_t>::max();
	if (type == "s64")
		return literal.magnitude <= (literal.negative ? std::uint64_t{1} << 63 : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
	if (type == "u64")
		return !literal.negative;
	return false;
}

std::int64_t signed_integer_value(const Integer& literal)
{
	if (!literal.negative)
		return static_cast<std::int64_t>(literal.magnitude);
	if (literal.magnitude == (std::uint64_t{1} << 63))
		return std::numeric_limits<std::int64_t>::min();
	return -static_cast<std::int64_t>(literal.magnitude);
}

std::string literal_type(const Expr* expression)
{
	if (dynamic_cast<const Integer*>(expression)) return "s64";
	if (dynamic_cast<const SignedInteger*>(expression)) return "s64";
	if (dynamic_cast<const UnsignedInteger*>(expression)) return "u64";
	if (dynamic_cast<const Float*>(expression)) return "f64";
	if (dynamic_cast<const String*>(expression)) return "string";
	if (auto name = dynamic_cast<const Name*>(expression); name && (name->value == "true" || name->value == "false")) return "bool";
	if (dynamic_cast<const MapLiteral*>(expression) || dynamic_cast<const ArrayLiteral*>(expression)) return "dval";
	return "";
}

std::string literal_type(const Expr* expression, const std::string& expected)
{
	if (auto integer = dynamic_cast<const Integer*>(expression); integer && integer_type(expected))
	{
		if (!integer_fits(*integer, expected))
			throw Error(integer->location, "integer literal is outside the " + expected + " range");
		return expected;
	}
	if (dynamic_cast<const Float*>(expression) && (expected == "f32" || expected == "f64")) return expected;
	return literal_type(expression);
}

std::unique_ptr<Expr> callsite_default_literal(const Expr* value, const Location& location, const std::string& expected)
{
	if (auto literal = dynamic_cast<const Integer*>(value))
	{
		if (expected != "dval" && !integer_fits(*literal, expected))
			throw Error(literal->location, "integer literal is outside the " + expected + " range");
		if (expected == "s64") return std::make_unique<SignedInteger>(location, signed_integer_value(*literal));
		if (expected == "u64") return std::make_unique<UnsignedInteger>(location, literal->magnitude);
		return std::make_unique<Integer>(location, literal->magnitude, literal->negative);
	}
	if (auto literal = dynamic_cast<const SignedInteger*>(value)) return std::make_unique<SignedInteger>(location, literal->value);
	if (auto literal = dynamic_cast<const UnsignedInteger*>(value)) return std::make_unique<UnsignedInteger>(location, literal->value);
	if (auto literal = dynamic_cast<const Float*>(value)) return std::make_unique<Float>(location, literal->value);
	if (auto literal = dynamic_cast<const String*>(value)) return std::make_unique<String>(location, literal->value);
	if (auto literal = dynamic_cast<const Name*>(value); literal && (literal->value == "true" || literal->value == "false")) return std::make_unique<Name>(location, literal->value);
	if (auto literal = dynamic_cast<const ArrayLiteral*>(value))
	{
		auto copy = std::make_unique<ArrayLiteral>(location);
		for (const Expr* item : literal->items)
			copy->items.push_back(callsite_default_literal(item, location, "dval").release());
		return copy;
	}
	if (auto literal = dynamic_cast<const MapLiteral*>(value))
	{
		auto copy = std::make_unique<MapLiteral>(location);
		for (const auto& [key, item] : literal->entries)
			copy->entries.emplace_back(key, callsite_default_literal(item, location, "dval").release());
		return copy;
	}
	throw std::runtime_error("default parameter is not a literal");
}

struct Definition
{
	Function* function = nullptr;
	std::vector<std::string> parameters;
	std::vector<bool> convert;
	std::vector<Expr*> default_values;
	bool variadic = false;
	std::string variadic_element;
	bool variadic_convert = false;
	std::string result;
	std::string exported;
	unsigned index = 0;
	unsigned type = 0;
	unsigned thunk_target = 0xffffffffu;
	bool handler_adapter = false;
	bool first_parameter_used = false;
	bool closure_body = false;
	bool body_omitted = false;
	bool inline_only = false;
	Expr* inline_value = nullptr;
	std::vector<std::pair<std::string, std::string>> captures;
};

struct GenericDefinition
{
	Function* function = nullptr;
	std::vector<std::string> patterns;
	std::vector<bool> convert;
	std::vector<int> dependent_parameters;
	std::string fixed_result;
	int dependent_result = -1;
};

bool generic_matches(const GenericDefinition& generic, const std::vector<std::string>& types, unsigned* exact = nullptr)
{
	if (generic.patterns.size() != types.size())
		return false;
	unsigned score = 0;
	for (std::size_t i = 0; i < types.size(); ++i)
	{
		const int dependency = generic.dependent_parameters[i];
		if (dependency < 0 && generic.patterns[i] == "any")
			continue;
		if ((dependency >= 0 && types[i] != types[static_cast<std::size_t>(dependency)]) ||
			(dependency < 0 && generic.patterns[i] != types[i]))
			return false;
		++score;
	}
	if (exact)
		*exact = score;
	return true;
}

std::optional<std::string> dependent_type_parameter(const Expr* expression)
{
	if (auto lookup = dynamic_cast<const ScopeLookup*>(expression); lookup && lookup->member == "type")
		if (auto name = dynamic_cast<const Name*>(lookup->value)) return name->value;
	return std::nullopt;
}

// A parser-level member call can be either an extension-style ordinary call or
// an indirect invocation of a function-valued struct field.  Keep that syntax
// distinction intact and classify only at a type-aware compiler boundary.
const Member* member_call(const Call* call)
{
	return dynamic_cast<const Member*>(call->function);
}

bool references_function_value(Expr* expression, const std::string& target)
{
	if (!expression) return false;
	if (auto name = dynamic_cast<Name*>(expression)) return name->value == target;
	if (auto call = dynamic_cast<Call*>(expression))
	{
		if (!dynamic_cast<Name*>(call->function) && references_function_value(call->function, target)) return true;
		for (Expr* argument : call->arguments)
			if (references_function_value(argument, target)) return true;
		return false;
	}
	if (auto block = dynamic_cast<Block*>(expression))
	{
		for (Expr* item : block->items) if (references_function_value(item, target)) return true;
		return false;
	}
	if (auto function = dynamic_cast<Function*>(expression)) return references_function_value(function->body, target);
	if (auto lambda = dynamic_cast<Lambda*>(expression)) return references_function_value(lambda->body, target);
	if (auto variable = dynamic_cast<Variable*>(expression)) return references_function_value(variable->value, target);
	if (auto annotation = dynamic_cast<Annotation*>(expression)) return references_function_value(annotation->value, target);
	if (auto binary = dynamic_cast<Binary*>(expression)) return references_function_value(binary->left, target) || references_function_value(binary->right, target);
	if (auto lookup = dynamic_cast<ScopeLookup*>(expression)) return references_function_value(lookup->value, target);
	if (auto returned = dynamic_cast<Return*>(expression)) return references_function_value(returned->value, target);
	if (auto yielded = dynamic_cast<Yield*>(expression)) return references_function_value(yielded->value, target);
	if (auto conditional = dynamic_cast<If*>(expression))
		return references_function_value(conditional->condition, target) || references_function_value(conditional->then_body, target) || references_function_value(conditional->else_body, target);
	if (auto loop = dynamic_cast<While*>(expression)) return references_function_value(loop->condition, target) || references_function_value(loop->body, target);
	if (auto loop = dynamic_cast<For*>(expression)) return references_function_value(loop->iterable, target) || references_function_value(loop->body, target);
	if (auto index = dynamic_cast<Index*>(expression)) return references_function_value(index->value, target) || references_function_value(index->index, target);
	if (auto member = dynamic_cast<Member*>(expression)) return references_function_value(member->value, target);
	if (auto array = dynamic_cast<ArrayLiteral*>(expression))
	{
		for (Expr* item : array->items) if (references_function_value(item, target)) return true;
		return false;
	}
	if (auto spread = dynamic_cast<Spread*>(expression)) return references_function_value(spread->value, target);
	if (auto map = dynamic_cast<MapLiteral*>(expression))
	{
		for (const auto& [key, item] : map->entries) if (references_function_value(item, target)) return true;
		return false;
	}
	if (auto markup = dynamic_cast<Markup*>(expression))
	{
		for (Expr* item : markup->parts) if (references_function_value(item, target)) return true;
		return false;
	}
	if (auto field = dynamic_cast<MarkupField*>(expression)) return references_function_value(field->value, target);
	return false;
}

struct Module;

struct FunctionLowerer
{
	FunctionLowerer(Module& module, Definition& definition);
	Bytes lower();
	Module& module_;
	Definition& definition_;
	std::vector<std::unordered_map<std::string, std::pair<unsigned, std::string>>> scopes_;
	std::vector<std::vector<std::pair<unsigned, std::string>>> owned_scopes_;
	std::set<unsigned> borrowed_managed_slots_;
	std::unordered_map<unsigned, unsigned> borrowed_managed_rebind_flags_;
	std::set<unsigned> owned_local_dval_slots_;
	unsigned local_count_ = 0;
	std::vector<std::string> local_types_;
	bool yielded_result_ = false;
	std::set<const Expr*> owned_expression_results_;
	std::unordered_map<const Expr*, std::string> inferred_types_;
	std::optional<std::size_t> repeated_condition_scope_;
	struct Loop
	{
		unsigned break_depth, continue_depth, ownership_boundary;
		Bytes break_edge, continue_edge;
	};
	std::vector<Loop> loops_;
	unsigned control_depth_ = 0;

	std::pair<Bytes, std::string> expression(Expr* value, bool value_required = true);
	std::pair<Bytes, std::string> conversion(Bytes code, const std::string& source, const std::string& target, const Location& location, bool source_owned = false);
	std::string infer(Expr* value);
	std::string infer_uncached(Expr* value);
	std::string infer_integer(Integer* value, const std::string& expected = "") const;
	std::pair<Bytes, std::string> integer_expression(Integer* value, const std::string& expected = "") const;
	std::pair<Bytes, std::string> float_expression(Float* value, const std::string& expected = "") const;
	std::optional<std::pair<unsigned, std::string>> compatible_local_callable(const std::string& name, const std::vector<std::string>& arguments) const;
	std::vector<std::pair<std::string, std::string>> lambda_captures(Lambda* value) const;
	std::tuple<std::string, unsigned, unsigned, Definition*, std::vector<std::pair<std::string, std::string>>> register_lambda(Lambda* value);
	Bytes markup_escape_length(unsigned source, unsigned total, bearer::MarkupContext context, const Location& location);
	Bytes markup_escape_write(unsigned source, unsigned cursor, bearer::MarkupContext context, const Location& location);
	Bytes markup_unicode_separator(unsigned source, unsigned index, unsigned length, unsigned target);
	Bytes markup_s32_length(unsigned source, unsigned total, const Location& location);
	Bytes markup_s32_write(unsigned source, unsigned cursor, const Location& location);
	Bytes markup_write_bytes(unsigned cursor, std::string_view text);
	std::pair<Bytes, std::string> dval_value(Expr* value);
	std::pair<Bytes, std::string> dval_lookup(Expr* value, Expr* key, bool require_present);
	std::pair<Bytes, std::string> dval_presence(Expr* value);
	std::pair<Bytes, std::string> dval_set_path(unsigned root, const std::vector<Expr*>& selectors, Expr* replacement, const Location& location);
	Bytes dval_replace(unsigned target, unsigned replacement, const Location& location, bool replacement_owned);
	void retain_dval_callables(Bytes& code, unsigned value, const Location& location);
	std::pair<Bytes, std::string> array_method(Call* call, const Member* member);
	Bytes array_ensure_capacity(unsigned slot, const std::string& array_type, unsigned required, const Location& location, const Bytes& failure_cleanup = {});
	std::pair<Bytes, unsigned> allocate_array(const std::string& array_type, unsigned length, const Location& location, const Bytes& failure_cleanup = {});
	std::pair<Bytes, unsigned> allocate_dval(unsigned length, const Location& location, const Bytes& failure_cleanup = {});
	std::pair<Bytes, unsigned> allocate_blob(const std::string& type, unsigned type_id, unsigned length, const Location& location, const Bytes& failure_cleanup = {});
	std::pair<Bytes, std::string> byte_conversion(Call* call, bool to_string);
	Bytes format_wide_scalar(Bytes code, const std::string& type, const Location& location);
	void narrow_s64_index(Bytes& code, unsigned value, unsigned target, const Location& location, const Bytes& failure_cleanup = {}, bool invalid_is_missing = false);
	struct BlockValue
	{
		Bytes code;
		std::string type;
		bool falls_through;
	};
	Bytes block(Block* block, bool new_scope = true);
	BlockValue value_block(Block* block);
	std::string infer_block(Block* block);
	Bytes cleanup_scopes(unsigned first = 0) const;
	bool condition_is_bool(const std::string& type) const;
	Expr* coerce_condition(Expr* condition, const char* form, std::vector<std::unique_ptr<Expr>>& own);
	bool expression_is_owned(const Expr* value);
	std::pair<unsigned, std::string> lookup(const Name* name) const;
	unsigned add_local(const std::string& name, const std::string& type, const Location& location);
	static void append(Bytes& target, const Bytes& source)
	{
		target.insert(target.end(), source.begin(), source.end());
	}
	static void append(Bytes& target, Bytes&& source)
	{
		if (target.empty()) target = std::move(source);
		else target.insert(target.end(), source.begin(), source.end());
	}
};

struct Module
{
	Module(std::vector<Expr*> items, std::vector<std::string> sources, std::string source, std::string artifact_source, std::string module,
		unsigned abi, CancellationCallback cancelled, std::function<std::vector<std::string>(const std::string&)> import_type_metadata = {})
		: items_(std::move(items)), sources_(std::move(sources)), source_(std::move(source)), artifact_source_(std::move(artifact_source)),
		  module_(std::move(module)), abi_(abi), cancelled_(std::move(cancelled)), import_type_metadata_(std::move(import_type_metadata))
	{
	}

	struct Capabilities
	{
		bool format_s64 = false, format_u64 = false, format_f64 = false;
		bool alloc = false, retain = false, release = false, clone = false, arc_live = false;
	};

	Capabilities discover_capabilities();
	CompileResult compile();
	void check_cancelled() const
	{
		if (cancelled_ && cancelled_())
			throw Error({source_, 1, 1, 0}, "Capy compilation cancelled");
	}
	unsigned add_data(const std::string& text)
	{
		unsigned offset = static_cast<unsigned>(data_.size());
		data_.insert(data_.end(), text.begin(), text.end());
		return offset;
	}
	unsigned add_static_closure(unsigned slot, unsigned function_type)
	{
		while (data_.size() % 8)
			data_.push_back(0);
		const unsigned offset = static_cast<unsigned>(data_.size());
		const std::uint32_t header[] = {0xffffffffu, 0xffffffffu, 0x3fffffffu, BEARER_WASM_OBJECT_CAPACITY_OFFSET + BEARER_WASM_OBJECT_WORD_SIZE, slot, function_type};
		for (std::uint32_t value : header)
			for (unsigned byte = 0; byte != 4; ++byte)
				data_.push_back(static_cast<std::uint8_t>(value >> (8 * byte)));
		return offset;
	}
	unsigned add_static_string(const std::string& text)
	{
		while (data_.size() % 8)
			data_.push_back(0);
		const unsigned offset = static_cast<unsigned>(data_.size());
		const std::uint32_t header[] = {0xffffffffu, 0xffffffffu, 1u, static_cast<std::uint32_t>(BEARER_WASM_OBJECT_BLOB_HEADER_SIZE + text.size()), static_cast<std::uint32_t>(text.size())};
		for (std::uint32_t value : header)
			for (unsigned byte = 0; byte != 4; ++byte)
				data_.push_back(static_cast<std::uint8_t>(value >> (8 * byte)));
		data_.insert(data_.end(), text.begin(), text.end());
		return offset;
	}
	unsigned reflection_type_descriptor(const std::string& type, const Location& location);
	void prepare_reflection_descriptors();
	unsigned import_index(const std::string& name) const
	{
		auto found = imports_.find(name);
		if (found == imports_.end())
			throw std::runtime_error("missing native Capy import " + name + " while compiling " + source_);
		return found->second;
	}
	unsigned helper_index(const std::string& name) const
	{
		auto found = helpers_.find(name);
		if (found == helpers_.end())
			throw std::runtime_error("missing native Capy helper " + name);
		return found->second;
	}
	unsigned format_scalar_index(const std::string& type) const
	{
		const Definition* definition = exact_definition("__bearer_format_" + type + "_capy", {type});
		if (!definition)
			throw std::runtime_error("missing Capy " + type + " formatter while compiling " + source_);
		return definition->index;
	}
	unsigned retain_index() const
	{
		return helper_index("retain");
	}
	unsigned release_index() const
	{
		return helper_index("release");
	}
	unsigned clone_index() const
	{
		return helper_index("clone");
	}
	struct Aggregate
	{
		unsigned type_id;
		std::vector<std::pair<std::string, std::string>> fields;
	};
	bool has_struct(const std::string& name) const
	{
		return structs_.contains(name);
	}
	const Aggregate& struct_type(const std::string& name, const Location& location) const
	{
		auto found = structs_.find(name);
		if (found == structs_.end())
			throw Error(location, "unknown struct '" + name + "'");
		return found->second;
	}
	bool has_alias(const std::string& name) const
	{
		return aliases_.contains(name);
	}
	std::string alias_type(const std::string& name, const Location& location)
	{
		if (auto resolved = resolved_aliases_.find(name); resolved != resolved_aliases_.end())
			return resolved->second;
		auto found = aliases_.find(name);
		if (found == aliases_.end())
			throw Error(location, "unknown type '" + name + "'");
		if (!resolving_aliases_.insert(name).second)
			throw Error(found->second->location, "cyclic type alias '" + name + "'");
		const std::string resolved = value_type(found->second->value);
		resolving_aliases_.erase(name);
		resolved_aliases_[name] = resolved;
		return resolved;
	}
	std::string imported_type(const std::string& name_space, const std::string& name, const Location& location) const
	{
		auto found_space = imported_types_.find(name_space);
		if (found_space == imported_types_.end())
			throw Error(location, "unknown import namespace '" + name_space + "'");
		auto found = found_space->second.find(name);
		if (found == found_space->second.end())
			throw Error(location, "import namespace '" + name_space + "' has no type '" + name + "'");
		return found->second;
	}
	std::string named_type(const std::string& name, const Location& location)
	{
		if (has_alias(name))
			return alias_type(name, location);
		if (name == "request")
			throw Error(location, "type request was removed. Use dval for Bearer handler input.");
		if (name == "s8" || name == "s16" || name == "s32" || name == "s64" || name == "u8" || name == "u16" || name == "u32" || name == "u64" || name == "f32" || name == "f64" || name == "bool" || name == "string" ||
			name == "dval" || name == "module" || name == "void")
			return name;
		if (has_struct(name))
			return "struct:" + name;
		throw Error(location, "unknown type '" + name + "'");
	}
	std::string constructor_name(const std::string& name, const Location& location)
	{
		const std::string type = named_type(name, location);
		if (type.rfind("struct:", 0) == 0)
			return type.substr(7);
		if (primitive_constructor_name(type) || type == "dval")
			return type;
		throw Error(location, "type '" + name + "' has no constructor call");
	}
	std::pair<std::string, unsigned> reference_function(const std::string& name, const Location& location)
	{
		std::vector<std::size_t> candidates;
		if (auto found = definitions_by_name_.find(name); found != definitions_by_name_.end())
			for (std::size_t index : found->second)
				if (definitions_[index].exported.empty())
					candidates.push_back(index);
		if (auto generic = generics_.find(name); generic != generics_.end() && !generic->second.empty())
			throw Error(location, "generic function value '" + name + "' requires an explicit concrete function type");
		if (candidates.empty())
			throw Error(location, "unknown local '" + name + "'");
		if (candidates.size() != 1)
			throw Error(location, "function value '" + name + "' requires exactly one concrete overload; found more than one overload");
		Definition& target = definitions_[candidates.front()];
		auto value_parameters = target.parameters;
		value_parameters.insert(value_parameters.begin(), "s32");
		const std::string contract = target.variadic ? "variadic:" + target.variadic_element + (target.variadic_convert ? ":convert" : "") : "";
		const unsigned value_type_index = wasm_type(value_parameters, target.result, contract);
		if (target.variadic) variadic_function_types_[value_type_index] = {target.parameters.size() - 1, target.variadic_element, target.variadic_convert};
		const std::string value_type = "function#" + std::to_string(value_type_index);
		const std::string cache = key(name, target.parameters);
		if (auto found = function_values_.find(cache); found != function_values_.end())
			return {value_type, found->second};
		Definition thunk;
		thunk.parameters = target.parameters;
		thunk.parameters.insert(thunk.parameters.begin(), "s32");
		thunk.result = target.result;
		thunk.index = first_user_index_ + static_cast<unsigned>(std::count_if(definitions_.begin(), definitions_.end(), [](const Definition& value) { return !value.inline_only; }));
		thunk.type = value_type_index;
		thunk.thunk_target = static_cast<unsigned>(candidates.front());
		definitions_.push_back(std::move(thunk));
		const unsigned slot = static_cast<unsigned>(table_functions_.size());
		table_functions_.push_back(definitions_.back().index);
		function_values_[cache] = slot;
		return {value_type, slot};
	}
	struct HostDeclaration
	{
		std::vector<std::string> parameters;
		std::string result;
		std::string symbol;
		Function* function = nullptr;
		bool trace = false;
	};
	const HostDeclaration* host(const std::string& name, const std::vector<std::string>& types) const
	{
		auto found = hosts_.find(key(name, types));
		return found == hosts_.end() ? nullptr : &found->second;
	}
	std::optional<std::vector<std::string>> contextual_argument_types(const std::string& name, const std::vector<Expr*>& arguments,
		const std::vector<std::string>& inferred, const Location& location) const
	{
		struct Candidate { std::vector<std::string> types; unsigned rank; };
		std::vector<Candidate> candidates;
		auto add = [&](const std::vector<std::string>& parameters, bool variadic, const std::string& element,
			const std::vector<Expr*>& defaults)
		{
			const std::size_t fixed = variadic ? parameters.size() - 1 : parameters.size();
			if ((!variadic && arguments.size() > parameters.size()) || (variadic && arguments.size() < fixed))
				return;
			if (!variadic && arguments.size() < parameters.size())
				for (std::size_t i = arguments.size(); i < parameters.size(); ++i)
					if (i >= defaults.size() || !defaults[i]) return;
			std::vector<std::string> types;
			for (std::size_t i = 0; i < arguments.size(); ++i)
			{
				const std::string expected = variadic && i >= fixed ? element : parameters[i];
				if (auto integer = dynamic_cast<Integer*>(arguments[i]))
				{
					if (!integer_type(expected) || !integer_fits(*integer, expected)) return;
					types.push_back(expected);
				}
				else if (dynamic_cast<Float*>(arguments[i]) && (expected == "f32" || expected == "f64"))
					types.push_back(expected);
				else if (auto array = dynamic_cast<ArrayLiteral*>(arguments[i]); array && expected.rfind("array<", 0) == 0)
				{
					const std::string element = expected.substr(6, expected.size() - 7);
					for (Expr* item : array->items)
					{
						const std::string item_type = dynamic_cast<Integer*>(item) ? literal_type(item, element) : inferred[i];
						if (item_type != element) return;
					}
					types.push_back(expected);
				}
				else
				{
					if (inferred[i] != expected) return;
					types.push_back(inferred[i]);
				}
			}
			const unsigned rank = variadic ? 2 : arguments.size() == parameters.size() ? 0 : 1;
			candidates.push_back({std::move(types), rank});
		};
		for (const auto& entry : hosts_)
		{
			const HostDeclaration& declaration = entry.second;
			if (declaration.function && declaration.function->name == name && declaration.parameters.size() == arguments.size())
				add(declaration.parameters, false, "", {});
		}
		if (auto found = definitions_by_name_.find(name); found != definitions_by_name_.end())
			for (std::size_t index : found->second)
			{
				const Definition& definition = definitions_[index];
				add(definition.parameters, definition.variadic, definition.variadic_element, definition.default_values);
			}
		if (candidates.empty()) return std::nullopt;
		const unsigned rank = std::min_element(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right)
		{
			return left.rank < right.rank;
		})->rank;
		std::vector<std::string> selected;
		for (const Candidate& candidate : candidates)
			if (candidate.rank == rank && candidate.types == inferred)
				return candidate.types;
		for (const Candidate& candidate : candidates)
			if (candidate.rank == rank)
			{
				if (selected.empty()) selected = candidate.types;
				else if (selected != candidate.types)
					throw Error(location, "ambiguous contextual literal overload " + name);
			}
		return selected;
	}
	const HostDeclaration* fused_variadic_sink(const Definition& target) const
	{
		if (!target.variadic || target.parameters.size() != 1 || target.result != "void" || !target.function || !target.function->body || target.function->body->items.size() != 1)
			return nullptr;
		auto loop = dynamic_cast<For*>(target.function->body->items[0]);
		auto iterable = loop ? dynamic_cast<Name*>(loop->iterable) : nullptr;
		if (!loop || !iterable || target.function->parameters.size() != 1 || iterable->value != target.function->parameters[0].name || loop->names.size() != 1 || loop->body->items.size() != 1)
			return nullptr;
		auto sink_call = dynamic_cast<Call*>(loop->body->items[0]);
		auto sink_name = sink_call ? dynamic_cast<Name*>(sink_call->function) : nullptr;
		auto sink_value = sink_call && sink_call->arguments.size() == 1 ? dynamic_cast<Name*>(sink_call->arguments[0]) : nullptr;
		if (!sink_name || !sink_value || sink_value->value != loop->names[0])
			return nullptr;
		const HostDeclaration* sink = host(sink_name->value, {target.variadic_element});
		return sink && sink->result == "void" && !sink->trace && target.variadic_element == "string" ? sink : nullptr;
	}
	Definition* exact_definition(const std::string& name, const std::vector<std::string>& types)
	{
		auto found = definitions_by_key_.find(key(name, types));
		return found == definitions_by_key_.end() ? nullptr : &definitions_[found->second];
	}
	const Definition* exact_definition(const std::string& name, const std::vector<std::string>& types) const
	{
		auto found = definitions_by_key_.find(key(name, types));
		return found == definitions_by_key_.end() ? nullptr : &definitions_[found->second];
	}
	bool constructor_available(const std::string& source, const std::string& target) const
	{
		if (can_convert(source, target))
			return true;
		const std::string name = target.rfind("struct:", 0) == 0 ? target.substr(7) : target;
		if (target == "dval" && (source == "string" || is_scalar(source) || source == "dval"))
			return true;
		if (target.rfind("struct:", 0) == 0)
		{
			auto structure = structs_.find(name);
			if (structure != structs_.end() && structure->second.fields.size() == 1 && structure->second.fields[0].second == source)
				return true;
		}
		if (const Definition* exact = exact_definition(name, {source}))
			return exact->result == target;
		static thread_local std::set<std::pair<std::string, std::string>> resolving_defaults;
		const auto request = std::pair{source, target};
		if (!resolving_defaults.insert(request).second)
			return false;
		if (const Definition* defaulted = default_definition(name, {source}, {}))
		{
			resolving_defaults.erase(request);
			return defaulted->result == target;
		}
		resolving_defaults.erase(request);
		if (auto found = generics_.find(name); found != generics_.end())
			for (const GenericDefinition& generic : found->second)
				if (generic.patterns.size() == 1 && (generic.patterns[0] == "any" || generic.patterns[0] == source))
				{
					const std::string result = generic.dependent_result == 0 ? source : generic.fixed_result;
					if (result == target)
						return true;
				}
		return false;
	}
	const Definition* converted_definition(const std::string& name, const std::vector<std::string>& types, const Location& location) const
	{
		if (definitions_by_key_.contains(key(name, types)))
			return nullptr;
		if (auto found = generics_.find(name); found != generics_.end())
			for (const auto& generic : found->second)
			{
				if (generic_matches(generic, types))
					return nullptr;
			}
		const Definition* selected = nullptr;
		unsigned best = std::numeric_limits<unsigned>::max();
		bool ambiguous = false;
		auto found = definitions_by_name_.find(name);
		if (found == definitions_by_name_.end())
			return nullptr;
		for (std::size_t index : found->second)
		{
			const Definition& definition = definitions_[index];
			if (definition.parameters.size() != types.size() || definition.convert.size() != types.size())
				continue;
			unsigned conversions = 0;
			bool matches = true;
			for (std::size_t i = 0; i < types.size(); ++i)
				if (types[i] != definition.parameters[i])
				{
					if (!definition.convert[i] || !constructor_available(types[i], definition.parameters[i])) { matches = false; break; }
					++conversions;
				}
			if (!matches) continue;
			if (conversions < best) { selected = &definition; best = conversions; ambiguous = false; }
			else if (conversions == best) ambiguous = true;
		}
		if (ambiguous)
		{
			std::string rendered;
			for (std::size_t i = 0; i < types.size(); ++i)
				rendered += (i ? ", " : "") + types[i];
			throw Error(location, "ambiguous converted overload " + name + "(" + rendered + ")");
		}
		return selected;
	}
	const Definition* default_definition(const std::string& name, const std::vector<std::string>& types, const Location& location) const
	{
		const Definition* selected = nullptr;
		unsigned best_conversions = std::numeric_limits<unsigned>::max();
		bool ambiguous = false;
		auto found = definitions_by_name_.find(name);
		if (found == definitions_by_name_.end())
			return nullptr;
		for (std::size_t index : found->second)
		{
			const Definition& definition = definitions_[index];
			if (definition.variadic || types.size() >= definition.parameters.size() || definition.default_values.size() != definition.parameters.size())
				continue;
			unsigned conversions = 0;
			bool matches = true;
			for (std::size_t i = 0; i < types.size(); ++i)
				if (types[i] != definition.parameters[i])
				{
					if (i >= definition.convert.size() || !definition.convert[i] || !constructor_available(types[i], definition.parameters[i])) { matches = false; break; }
					++conversions;
				}
			for (std::size_t i = types.size(); matches && i < definition.parameters.size(); ++i)
				if (!definition.default_values[i]) matches = false;
			if (!matches)
				continue;
			if (!selected || conversions < best_conversions) { selected = &definition; best_conversions = conversions; ambiguous = false; }
			else if (conversions == best_conversions)
				ambiguous = true;
		}
		if (ambiguous)
			throw Error(location, "ambiguous default overload " + name);
		return selected;
	}
	const Definition* variadic_definition(const std::string& name, const std::vector<std::string>& types, const Location& location) const
	{
		const Definition* selected = nullptr;
		unsigned best_conversions = std::numeric_limits<unsigned>::max(), best_fixed = 0;
		bool ambiguous = false;
		auto found = definitions_by_name_.find(name);
		if (found == definitions_by_name_.end())
			return nullptr;
		for (std::size_t index : found->second)
		{
			const Definition& definition = definitions_[index];
			if (!definition.variadic || definition.parameters.empty())
				continue;
			const std::size_t fixed = definition.parameters.size() - 1;
			if (types.size() < fixed)
				continue;
			unsigned conversions = 0;
			bool matches = true;
			for (std::size_t i = 0; i < fixed; ++i)
				if (types[i] != definition.parameters[i])
				{
					if (i >= definition.convert.size() || !definition.convert[i] || !constructor_available(types[i], definition.parameters[i]))
					{
						matches = false;
						break;
					}
					++conversions;
				}
			if (!matches)
				continue;
			for (std::size_t i = fixed; i < types.size(); ++i)
			{
				const std::string source = normalize_spread_type(types[i]);
				if (source != definition.variadic_element)
				{
					if (!definition.variadic_convert || !constructor_available(source, definition.variadic_element))
					{
						matches = false;
						break;
					}
					++conversions;
				}
			}
			if (!matches)
				continue;
			if (!selected || fixed > best_fixed || (fixed == best_fixed && conversions < best_conversions))
			{
				selected = &definition;
				best_fixed = static_cast<unsigned>(fixed);
				best_conversions = conversions;
				ambiguous = false;
			}
			else if (fixed == best_fixed && conversions == best_conversions)
				ambiguous = true;
		}
		if (ambiguous)
		{
			std::string rendered;
			for (std::size_t i = 0; i < types.size(); ++i)
				rendered += (i ? ", " : "") + types[i];
			throw Error(location, "ambiguous variadic overload " + name + "(" + rendered + ")");
		}
		return selected;
	}
	std::optional<std::string> compatible_result(const std::string& name, const std::vector<std::string>& types, const Location& location) const
	{
		if (auto it = definitions_by_key_.find(key(name, types)); it != definitions_by_key_.end())
			return definitions_[it->second].result;
		if (const Definition* defaulted = default_definition(name, types, location);
			defaulted && std::equal(types.begin(), types.end(), defaulted->parameters.begin()))
			return defaulted->result;
		std::vector<const GenericDefinition*> candidates;
		unsigned best = 0;
		if (auto found = generics_.find(name); found != generics_.end())
			for (const auto& generic : found->second)
			{
				if (generic.patterns.size() != types.size())
					continue;
				unsigned exact = 0;
				if (!generic_matches(generic, types, &exact)) continue;
				if (candidates.empty() || exact > best) { candidates = {&generic}; best = exact; }
				else if (exact == best) candidates.push_back(&generic);
			}
		if (candidates.size() > 1)
			throw Error(location, "ambiguous generic overload " + name);
		if (candidates.size() == 1)
			return candidates[0]->dependent_result >= 0 ? types[static_cast<unsigned>(candidates[0]->dependent_result)] : candidates[0]->fixed_result;
		if (const Definition* converted = converted_definition(name, types, location))
			return converted->result;
		if (const Definition* variadic = variadic_definition(name, types, location))
			return variadic->result;
		if (const Definition* defaulted = default_definition(name, types, location))
			return defaulted->result;
		return std::nullopt;
	}
	Definition* compatible_definition(const std::string& name, const std::vector<std::string>& types, const Location& location)
	{
		if (auto it = definitions_by_key_.find(key(name, types)); it != definitions_by_key_.end())
			return &definitions_[it->second];
		if (const Definition* defaulted = default_definition(name, types, location);
			defaulted && std::equal(types.begin(), types.end(), defaulted->parameters.begin()))
			return const_cast<Definition*>(defaulted);
		std::vector<const GenericDefinition*> candidates;
		unsigned best = 0;
		for (const auto& generic : generics_[name])
		{
			if (generic.patterns.size() != types.size())
				continue;
			unsigned exact = 0;
			if (!generic_matches(generic, types, &exact))
				continue;
			if (candidates.empty() || exact > best)
			{
				candidates = {&generic};
				best = exact;
			}
			else if (exact == best)
				candidates.push_back(&generic);
		}
		std::string rendered;
		for (std::size_t i = 0; i < types.size(); ++i)
			rendered += (i ? ", " : "") + types[i];
		if (candidates.empty())
		{
			if (const Definition* converted = converted_definition(name, types, location))
				return const_cast<Definition*>(converted);
			if (const Definition* variadic = variadic_definition(name, types, location))
				return const_cast<Definition*>(variadic);
			return const_cast<Definition*>(default_definition(name, types, location));
		}
		if (candidates.size() != 1)
			throw Error(location, "ambiguous generic overload " + name + "(" + rendered + ")");
		const GenericDefinition& generic = *candidates.front();
		Definition definition;
		definition.function = generic.function;
		definition.parameters = types;
		definition.convert = generic.convert;
		definition.result = generic.dependent_result >= 0 ? types[static_cast<unsigned>(generic.dependent_result)] : generic.fixed_result;
		definition.index = first_user_index_ + static_cast<unsigned>(std::count_if(definitions_.begin(), definitions_.end(), [](const Definition& value) { return !value.inline_only; }));
		definition.type = wasm_type(definition.parameters, definition.result);
		definitions_by_key_[key(name, types)] = definitions_.size();
		definitions_by_name_[name].push_back(definitions_.size());
		definitions_.push_back(std::move(definition));
		return &definitions_.back();
	}
	Definition& resolve(const std::string& name, const std::vector<std::string>& types, const Location& location)
	{
		if (Definition* definition = compatible_definition(name, types, location))
			return *definition;
		static const std::map<std::string, std::string> request_migrations{
			{"request_context", "request_context was removed. Use the handler dval parameter directly."},
			{"request_param", "request_param was removed. Read the applicable method, headers, route, url, or server field from the handler input."},
			{"request_get", "request_get was removed. Read the query field from the handler input."},
			{"request_post", "request_post was removed. Read the form field from the handler input."},
			{"request_cookie", "request_cookie was removed. Read the cookies field from the handler input."},
			{"request_session", "request_session was removed. Read the entry session or capture the state returned by a session effect."},
			{"request_body", "request_body was removed. Read the body field from the handler input."},
			{"request_base_url", "request_base_url was removed. Read request.url.base from the handler input."},
			{"request_script_url", "request_script_url was removed. Read request.url.script from the handler input."},
			{"request_query_path", "request_query_path was removed. Read request.route.path from the handler input."},
			{"request_query_route", "request_query_route was removed. Read request.route from the handler input."},
			{"request_perf", "request_perf was renamed to runtime_perf."},
			{"cli_input", "cli_input was removed. Read query, form, and body from the handler input."},
			{"cli_arg", "cli_arg was removed. Read query or form from the handler input."},
			{"ws_message", "ws_message was removed. Read request.body from the handler input."},
			{"ws_connection_id", "ws_connection_id was removed. Read request.websocket.connection_id from the handler input."},
			{"ws_scope", "ws_scope was removed. Read request.websocket.scope from the handler input."},
			{"ws_opcode", "ws_opcode was removed. Read request.websocket.opcode from the handler input."},
			{"ws_is_binary", "ws_is_binary was removed. Read request.websocket.binary from the handler input."},
			{"ws_connections", "ws_connections was removed. Read request.websocket.connections from the handler input."},
			{"ws_connection_count", "ws_connection_count was removed. Use length(request.websocket.connections)."},
			{"to_bool", "to_bool was removed. Use the bool constructor."},
			{"to_f64", "to_f64 was removed. Use the f64 constructor."},
			{"to_s64", "to_s64 was removed. Use the s64 constructor."},
			{"to_u64", "to_u64 was removed. Use the u64 constructor."},
			{"to_lower", "to_lower was removed. Use lower."},
			{"to_upper", "to_upper was removed. Use upper."},
			{"dval_to_json", "dval_to_json was removed. Use json_encode."},
			{"dval_to_stringmap", "dval_to_stringmap was removed. Construct a dval map and convert its values with string."},
			{"password_needs_rehash", "password_needs_rehash was removed. Compare the password encoding parameter prefix after password_verify succeeds."},
			{"ascii_safe_name", "ascii_safe_name was removed. Use safe_name."},
			{"component_exists", "component_exists was removed. Use length(component_resolve(name)) > 0."},
			{"memcache_escape_key", "memcache_escape_key was removed. Memcache functions normalize keys."},
			{"sha256_hex", "sha256_hex was removed. Use hex(sha256(value))."},
			{"hmac_sha256_hex", "hmac_sha256_hex was removed. Use hex(hmac_sha256(key, value))."},
			{"shell_spawn", "shell_spawn was removed. Use shell_exec(command, flags)."},
		};
		if (auto migration = request_migrations.find(name); migration != request_migrations.end())
			throw Error(location, migration->second);
		std::string rendered;
		for (std::size_t i = 0; i < types.size(); ++i)
			rendered += (i ? ", " : "") + types[i];
		throw Error(location, "no overload " + name + "(" + rendered + ")");
	}
	Bytes marker(const Location& location)
	{
		const std::int32_t value = static_cast<std::int32_t>(0x5a000000u + markers_.size());
		markers_.push_back(location);
		Bytes code{0x01, 0x01, 0x01, 0x41};
		wasm::append_sleb32(code, value);
		code.push_back(0x1a);
		return code;
	}
	const std::string& source_path() const
	{
		return source_;
	}
	const std::string& module_name() const
	{
		return module_;
	}
	const std::string& artifact_path(const std::string& path) const
	{
		return path == source_ ? artifact_source_ : path;
	}
	const std::vector<Expr*> items_;
	const std::vector<std::string> sources_;
	std::string source_, artifact_source_, module_;
	unsigned abi_;
	CancellationCallback cancelled_;
	std::deque<Definition> definitions_;
	std::deque<Function> lambda_functions_;
	std::unordered_map<std::string, std::size_t> definitions_by_key_;
	std::unordered_map<std::string, std::vector<std::size_t>> definitions_by_name_;
	std::map<std::string, std::vector<GenericDefinition>> generics_;
	unsigned first_user_index_ = 0;
	std::map<std::string, unsigned> function_values_;
	std::unordered_map<const Lambda*, std::tuple<std::string, unsigned, unsigned, Definition*, std::vector<std::pair<std::string, std::string>>>> lambdas_;
	std::map<unsigned, std::vector<std::string>> closure_types_;
	std::vector<unsigned> table_functions_;
	std::unordered_map<std::string, unsigned> imports_;
	std::set<std::string> runtime_imports_;
	std::unordered_map<std::string, HostDeclaration> hosts_;
	std::map<std::string, TypeAlias*> aliases_;
	std::map<std::string, std::map<std::string, std::string>> imported_types_;
	std::function<std::vector<std::string>(const std::string&)> import_type_metadata_;
	std::map<std::string, std::string> resolved_aliases_;
	std::set<std::string> resolving_aliases_;
	std::set<std::string> used_hosts_;
	std::map<std::string, unsigned> host_types_;
	std::unordered_map<std::string, unsigned> helpers_;
	std::set<std::pair<std::string, std::string>> fused_sink_formats_;
	std::set<std::string> string_format_types_;
	Bytes data_;
	bool dval_ = false, trace_host_ = false, use_trace_global_ = false;
	unsigned trace_stack_offset_ = 0;
	std::vector<std::pair<std::string, Definition*>> custom_exports_;
	std::vector<std::string> function_exports_;
	std::vector<std::string> type_exports_;
	bool use_retain_ = false, use_release_ = false, use_clone_ = false, use_arc_global_ = false;
	std::vector<Location> markers_;
	std::vector<std::pair<std::vector<std::string>, std::string>> types_;
	struct VariadicFunctionType { std::size_t fixed; std::string element; bool convert; };
	std::map<unsigned, VariadicFunctionType> variadic_function_types_;
	std::unordered_map<std::string, unsigned> type_indices_;
	std::map<std::string, unsigned> reflection_type_descriptors_;
	std::map<std::string, unsigned> reflection_struct_descriptors_;
	std::map<std::string, Aggregate> structs_;
	unsigned next_aggregate_type_ = 5;

	static std::string key(const std::string& name, const std::vector<std::string>& types)
	{
		std::string value = name + "\x1f";
		for (const auto& type : types)
			value += type + "\x1e";
		return value;
	}
	std::string value_type(const Expr* expression, bool allow_void = false);
	unsigned wasm_type(const std::vector<std::string>& parameters, const std::string& result, std::string_view source_contract = {})
	{
		std::string signature = result + "|" + std::string(source_contract) + "|";
		for (const auto& p : parameters)
			signature += p + ",";
		auto [it, inserted] = type_indices_.emplace(signature, static_cast<unsigned>(types_.size()));
		if (inserted)
			types_.push_back({parameters, result});
		return it->second;
	}
	void collect();
	std::vector<Bytes> runtime_bodies() const;
	Bytes custom_export_body(const Definition& target);
};

namespace
{
void patch_u32_le(Bytes& data, unsigned offset, std::uint32_t value)
{
	for (unsigned byte = 0; byte != 4; ++byte)
		data[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
}
}

unsigned Module::reflection_type_descriptor(const std::string& type, const Location& location)
{
	const std::function<std::string(const std::string&)> canonical_type = [&](const std::string& value) -> std::string
	{
		if (value.rfind("struct:", 0) == 0) return value.substr(7);
		if (value.rfind("array<", 0) == 0) return "[" + canonical_type(value.substr(6, value.size() - 7)) + "]";
		if (value.rfind("function#", 0) == 0)
		{
			const unsigned index = static_cast<unsigned>(std::stoul(value.substr(9)));
			if (index < types_.size())
			{
				std::string result = "function(";
				for (std::size_t i = 1; i < types_[index].first.size(); ++i)
				{
					if (i > 1) result += ',';
					result += canonical_type(types_[index].first[i]);
				}
				return result + ") " + canonical_type(types_[index].second);
			}
		}
		return value;
	};
	if (auto found = reflection_type_descriptors_.find(type); found != reflection_type_descriptors_.end())
		return found->second;
	while (data_.size() % 4) data_.push_back(0);
	const unsigned descriptor = static_cast<unsigned>(data_.size());
	reflection_type_descriptors_[type] = descriptor;
	data_.resize(data_.size() + BEARER_CAPY_REFLECTION_TYPE_SIZE, 0);
	const auto primitive_kind = [](const std::string& value) -> unsigned
	{
		static const std::map<std::string, unsigned> kinds = {
			{"s8", BEARER_CAPY_REFLECTION_S8}, {"s16", BEARER_CAPY_REFLECTION_S16}, {"s32", BEARER_CAPY_REFLECTION_S32},
			{"s64", BEARER_CAPY_REFLECTION_S64}, {"u8", BEARER_CAPY_REFLECTION_U8}, {"u16", BEARER_CAPY_REFLECTION_U16},
			{"u32", BEARER_CAPY_REFLECTION_U32}, {"u64", BEARER_CAPY_REFLECTION_U64}, {"f32", BEARER_CAPY_REFLECTION_F32},
			{"f64", BEARER_CAPY_REFLECTION_F64}, {"bool", BEARER_CAPY_REFLECTION_BOOL}, {"string", BEARER_CAPY_REFLECTION_STRING},
			{"dval", BEARER_CAPY_REFLECTION_DVAL}, {"module", BEARER_CAPY_REFLECTION_OPAQUE}};
		auto found = kinds.find(value);
		return found == kinds.end() ? 0 : found->second;
	};
	unsigned kind = primitive_kind(type), detail = 0;
	std::string canonical = type;
	if (type.rfind("struct:", 0) == 0)
	{
		kind = BEARER_CAPY_REFLECTION_STRUCT;
		canonical = type.substr(7);
		detail = reflection_struct_descriptors_.contains(canonical) ? reflection_struct_descriptors_.at(canonical) : 0;
		if (!detail)
		{
			while (data_.size() % 4) data_.push_back(0);
			detail = static_cast<unsigned>(data_.size());
			reflection_struct_descriptors_[canonical] = detail;
			data_.resize(data_.size() + BEARER_CAPY_REFLECTION_STRUCT_SIZE, 0);
			const auto& aggregate = struct_type(canonical, location);
			std::vector<std::string> field_types;
			for (const auto& field : aggregate.fields) field_types.push_back(field.second);
			const AggregateLayout layout = aggregate_layout(field_types, BEARER_WASM_OBJECT_STRUCT_FIELDS_OFFSET);
			while (data_.size() % 4) data_.push_back(0);
			const unsigned fields = static_cast<unsigned>(data_.size());
			data_.resize(data_.size() + aggregate.fields.size() * BEARER_CAPY_REFLECTION_FIELD_SIZE, 0);
			for (std::size_t i = 0; i < aggregate.fields.size(); ++i)
			{
				const unsigned field_descriptor = fields + static_cast<unsigned>(i * BEARER_CAPY_REFLECTION_FIELD_SIZE);
				const unsigned name = add_static_string(aggregate.fields[i].first);
				const unsigned field_type = reflection_type_descriptor(aggregate.fields[i].second, location);
				patch_u32_le(data_, field_descriptor + BEARER_CAPY_REFLECTION_FIELD_NAME_OFFSET, name);
				patch_u32_le(data_, field_descriptor + BEARER_CAPY_REFLECTION_FIELD_TYPE_OFFSET, field_type);
				patch_u32_le(data_, field_descriptor + BEARER_CAPY_REFLECTION_FIELD_VALUE_OFFSET, layout.offsets[i]);
			}
			patch_u32_le(data_, detail + BEARER_CAPY_REFLECTION_STRUCT_TYPE_ID_OFFSET, aggregate.type_id);
			patch_u32_le(data_, detail + BEARER_CAPY_REFLECTION_STRUCT_FIELD_COUNT_OFFSET, aggregate.fields.size());
			patch_u32_le(data_, detail + BEARER_CAPY_REFLECTION_STRUCT_FIELDS_OFFSET, fields);
		}
	}
	else if (type.rfind("array<", 0) == 0)
	{
		kind = BEARER_CAPY_REFLECTION_ARRAY;
		const std::string element = type.substr(6, type.size() - 7);
		canonical = canonical_type(type);
		detail = reflection_type_descriptor(element, location);
	}
	else if (type.rfind("function#", 0) == 0)
	{
		kind = BEARER_CAPY_REFLECTION_FUNCTION;
		const unsigned index = static_cast<unsigned>(std::stoul(type.substr(9)));
		canonical = type;
		if (index < types_.size())
		{
			canonical = canonical_type(type);
		}
		detail = index;
	}
	else if (!kind)
		throw Error(location, "cannot emit reflection descriptor for " + type);
	const unsigned name = add_static_string(canonical);
	patch_u32_le(data_, descriptor + BEARER_CAPY_REFLECTION_TYPE_KIND_OFFSET, kind);
	patch_u32_le(data_, descriptor + BEARER_CAPY_REFLECTION_TYPE_NAME_OFFSET, name);
	patch_u32_le(data_, descriptor + BEARER_CAPY_REFLECTION_TYPE_DETAIL_OFFSET, detail);
	if (type.rfind("struct:", 0) == 0)
		patch_u32_le(data_, descriptor + BEARER_CAPY_REFLECTION_TYPE_DETAIL_OFFSET, reflection_struct_descriptors_.at(type.substr(7)));
	return descriptor;
}

void Module::prepare_reflection_descriptors()
{
	for (const auto& [name, aggregate] : structs_)
		reflection_type_descriptor("struct:" + name, {source_, 1, 1, 0});
}

std::string Module::value_type(const Expr* expression, bool allow_void)
{
	if (auto array = dynamic_cast<const ArrayLiteral*>(expression))
	{
		if (array->items.size() != 1)
			throw Error(expression->location, "array type requires exactly one element type");
		const std::string element = value_type(array->items[0]);
		if (element == "module")
			throw Error(array->items[0]->location, "module is opaque and cannot be stored in array layouts");
		return "array<" + element + ">";
	}
	if (auto function = dynamic_cast<const FunctionType*>(expression))
	{
		std::vector<std::string> parameters{"s32"};
		bool variadic = false, convert = false;
		std::string element;
		for (const auto& parameter : function->parameters)
		{
			if (parameter.default_value)
				throw Error(parameter.default_value->location, "function types cannot use default parameters");
			std::string type = value_type(parameter.type_expr);
			if (parameter.convert && !parameter.variadic)
				throw Error(parameter.type_expr->location, "only variadic function-type parameters may request conversion");
			if (parameter.variadic)
			{
				variadic = true;
				convert = parameter.convert;
				element = type;
				type = "array<" + type + ">";
			}
			parameters.push_back(std::move(type));
		}
		const std::string result = value_type(function->return_type, true);
		const std::string contract = variadic ? "variadic:" + element + (convert ? ":convert" : "") : "";
		const unsigned type = wasm_type(parameters, result, contract);
		if (variadic) variadic_function_types_[type] = {parameters.size() - 2, element, convert};
		return "function#" + std::to_string(type);
	}
	if (auto name = dynamic_cast<const Name*>(expression))
		return named_type(name->value, name->location);
	if (auto lookup = dynamic_cast<const ScopeLookup*>(expression))
		if (auto name_space = dynamic_cast<const Name*>(lookup->value))
			return imported_type(name_space->value, lookup->member, lookup->location);
	if (auto member = dynamic_cast<const Member*>(expression))
		if (auto name_space = dynamic_cast<const Name*>(member->value))
			return imported_type(name_space->value, member->member, member->location);
	return type_of_expression(expression, allow_void);
}

FunctionLowerer::FunctionLowerer(Module& module, Definition& definition) : module_(module), definition_(definition)
{
	if (!definition.function)
		return;
	scopes_.push_back({});
	owned_scopes_.push_back({});
	unsigned parameter = 0;
	if (definition.closure_body)
	{
		parameter = 1;
		for (const auto& value : definition.function->parameters)
		{
			const std::string& type = definition.parameters[parameter];
			scopes_.back()[value.name] = {parameter, type};
			if (managed_type(type))
				borrowed_managed_slots_.insert(parameter);
			++parameter;
		}
		for (const auto& [name, type] : definition.captures)
		{
			const unsigned slot = add_local(name, type, definition.function->location);
			if (managed_type(type))
				borrowed_managed_slots_.insert(slot);
		}
	}
	else
		for (const auto& value : definition.function->parameters)
		{
			if (parameter >= definition.parameters.size() && definition.exported.empty())
				break;
			const std::string type = definition.parameters[parameter];
			scopes_.back()[value.name] = {parameter, type};
			if (managed_type(type))
				borrowed_managed_slots_.insert(parameter);
			++parameter;
		}
}

// Conditions use the same bool constructor as `as bool` parameters.
Expr* FunctionLowerer::coerce_condition(Expr* condition, const char* form, std::vector<std::unique_ptr<Expr>>& own)
{
	const std::string type = infer(condition);
	if (type == "bool")
		return condition;
	if (!condition_is_bool(type))
		throw Error(condition->location, type == "module"
			? "module is opaque and cannot be used as a condition"
			: std::string(form) + " condition must be bool");
	own.push_back(std::make_unique<Name>(condition->location, "bool"));
	auto call = std::make_unique<Call>(condition->location, own.back().get());
	call->arguments.push_back(condition);
	own.push_back(std::move(call));
	return own.back().get();
}

bool FunctionLowerer::condition_is_bool(const std::string& type) const
{
	return type == "bool" || (type != "module" && module_.constructor_available(type, "bool"));
}

Bytes FunctionLowerer::cleanup_scopes(unsigned first) const
{
	Bytes code;
	for (auto scope = owned_scopes_.rbegin(); scope != owned_scopes_.rend() - first; ++scope)
		for (auto value = scope->rbegin(); value != scope->rend(); ++value)
		{
			code.push_back(0x20);
			wasm::append_uleb(code, value->first);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.release_index());
		}
	if (first == 0)
		for (const auto& [slot, flag] : borrowed_managed_rebind_flags_)
		{
			code.push_back(0x20);
			wasm::append_uleb(code, flag);
			code.insert(code.end(), {0x04, 0x40, 0x20});
			wasm::append_uleb(code, slot);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.release_index());
			code.push_back(0x0b);
		}
	return code;
}

bool expression_always_returns(const Expr* value)
{
	if (dynamic_cast<const Return*>(value))
		return true;
	if (auto block = dynamic_cast<const Block*>(value))
	{
		for (const Expr* item : block->items)
			if (expression_always_returns(item))
				return true;
		return false;
	}
	if (auto conditional = dynamic_cast<const If*>(value))
		return conditional->else_body && expression_always_returns(conditional->then_body) && expression_always_returns(conditional->else_body);
	return false;
}

bool expression_falls_through(const Expr* value)
{
	if (dynamic_cast<const Return*>(value) || dynamic_cast<const Break*>(value) || dynamic_cast<const Continue*>(value))
		return false;
	if (auto block = dynamic_cast<const Block*>(value))
	{
		for (const Expr* item : block->items)
			if (!expression_falls_through(item))
				return false;
		return true;
	}
	if (auto conditional = dynamic_cast<const If*>(value))
		return !conditional->else_body || expression_falls_through(conditional->then_body) || expression_falls_through(conditional->else_body);
	return true;
}

bool FunctionLowerer::expression_is_owned(const Expr* value)
{
	if (owned_expression_results_.contains(value))
		return true;
	if (auto lambda = dynamic_cast<const Lambda*>(value))
		return !std::get<4>(register_lambda(const_cast<Lambda*>(lambda))).empty();
	if (auto name = dynamic_cast<const Name*>(value); name && name->value == "none")
		return true;
	if (dynamic_cast<const ArrayLiteral*>(value) || dynamic_cast<const MapLiteral*>(value) ||
		dynamic_cast<const Markup*>(value))
		return true;
	if (auto index = dynamic_cast<const Index*>(value))
	{
		if (infer(index->value) == "dval")
			return true;
		return managed_type(infer(const_cast<Index*>(index))) && expression_is_owned(index->value);
	}
	if (auto member = dynamic_cast<const Member*>(value))
		return infer(member->value) == "dval" || (managed_type(infer(member->value)) && expression_is_owned(member->value));
	if (auto binary = dynamic_cast<const Binary*>(value))
		return binary->operator_ == "+" && infer(binary->left) == "string" && infer(binary->right) == "string";
	if (auto call = dynamic_cast<const Call*>(value))
	{
		if (auto name = dynamic_cast<const Name*>(call->function); name && name->value == "__bearer_dval_replace")
			return false;
		return managed_type(infer(const_cast<Call*>(call)));
	}
	if (auto scope = dynamic_cast<const ScopeLookup*>(value))
		return scope->member == "items";
	return false;
}

std::pair<unsigned, std::string> FunctionLowerer::lookup(const Name* name) const
{
	for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
		if (auto it = scope->find(name->value); it != scope->end())
			return it->second;
	throw Error(name->location, "unknown local '" + name->value + "'");
}

unsigned FunctionLowerer::add_local(const std::string& name, const std::string& type, const Location& location)
{
	if (scopes_.back().contains(name))
		throw Error(location, "local '" + name + "' is already declared in this scope");
	unsigned parameter_count = definition_.exported.empty() ? static_cast<unsigned>(definition_.parameters.size()) : 1;
	unsigned slot = parameter_count + local_count_++;
	local_types_.push_back(type);
	if (!name.empty())
		scopes_.back()[name] = {slot, type};
	return slot;
}

std::vector<std::pair<std::string, std::string>> FunctionLowerer::lambda_captures(Lambda* lambda) const
{
	std::vector<std::set<std::string>> scopes{{}};
	for (const auto& parameter : lambda->parameters)
		scopes.back().insert(parameter.name);
	std::vector<std::pair<std::string, std::string>> captures;
	std::set<std::string> seen;
	std::function<void(Expr*)> visit = [&](Expr* value)
	{
		if (auto nested = dynamic_cast<Lambda*>(value))
		{
			scopes.push_back({});
			for (const auto& parameter : nested->parameters)
				scopes.back().insert(parameter.name);
			for (Expr* item : nested->body->items)
				visit(item);
			scopes.pop_back();
		}
		else if (auto name = dynamic_cast<Name*>(value))
		{
			if (name->value == "true" || name->value == "false")
				return;
			if (std::any_of(scopes.rbegin(), scopes.rend(), [&](const auto& scope) { return scope.contains(name->value); }))
				return;
			for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
				if (auto found = scope->find(name->value); found != scope->end())
				{
					if (seen.insert(name->value).second)
						captures.push_back({name->value, found->second.second});
					return;
				}
		}
		else if (auto variable = dynamic_cast<Variable*>(value))
		{
			visit(variable->value);
			scopes.back().insert(variable->name);
		}
		else if (auto block = dynamic_cast<Block*>(value))
		{
			scopes.push_back({});
			for (Expr* item : block->items)
				visit(item);
			scopes.pop_back();
		}
		else if (auto call = dynamic_cast<Call*>(value))
		{
			visit(call->function);
			for (Expr* item : call->arguments)
				visit(item);
		}
		else if (auto binary = dynamic_cast<Binary*>(value))
		{
			if (binary->operator_ == ":=")
			{
				visit(binary->right);
				if (auto name = dynamic_cast<Name*>(binary->left))
					scopes.back().insert(name->value);
			}
			else if (binary->operator_ == "=")
			{
				if (auto name = dynamic_cast<Name*>(binary->left))
				{
					const bool local = std::any_of(scopes.rbegin(), scopes.rend(), [&](const auto& scope) { return scope.contains(name->value); });
					if (!local)
						for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
							if (scope->contains(name->value))
								throw Error(name->location, "cannot assign to captured binding '" + name->value + "'. Captures are immutable");
				}
				else
					visit(binary->left);
				visit(binary->right);
			}
			else if (binary->operator_ == "&&" || binary->operator_ == "||")
			{
				visit(binary->left);
				scopes.push_back({});
				visit(binary->right);
				scopes.pop_back();
			}
			else
			{
				visit(binary->left);
				visit(binary->right);
			}
		}
		else if (auto index = dynamic_cast<Index*>(value))
		{
			visit(index->value);
			visit(index->index);
		}
		else if (auto member = dynamic_cast<Member*>(value))
			visit(member->value);
		else if (auto returned = dynamic_cast<Return*>(value))
		{
			if (returned->value)
				visit(returned->value);
		}
		else if (auto yielded = dynamic_cast<Yield*>(value))
			visit(yielded->value);
		else if (auto conditional = dynamic_cast<If*>(value))
		{
			visit(conditional->condition);
			visit(conditional->then_body);
			if (conditional->else_body)
				visit(conditional->else_body);
		}
		else if (auto loop = dynamic_cast<While*>(value))
		{
			visit(loop->condition);
			visit(loop->body);
		}
		else if (auto loop = dynamic_cast<For*>(value))
		{
			visit(loop->iterable);
			scopes.push_back({});
			for (const auto& name : loop->names)
				scopes.back().insert(name);
			visit(loop->body);
			scopes.pop_back();
		}
		else if (auto array = dynamic_cast<ArrayLiteral*>(value))
			for (Expr* item : array->items)
				visit(item);
		else if (auto spread = dynamic_cast<Spread*>(value))
			visit(spread->value);
		else if (auto map = dynamic_cast<MapLiteral*>(value))
			for (const auto& [key, item] : map->entries)
				visit(item);
		else if (auto markup = dynamic_cast<Markup*>(value))
			for (Expr* item : markup->parts)
				if (auto field = dynamic_cast<MarkupField*>(item))
					visit(field->value);
	};
	for (Expr* item : lambda->body->items)
		visit(item);
	return captures;
}

std::tuple<std::string, unsigned, unsigned, Definition*, std::vector<std::pair<std::string, std::string>>> FunctionLowerer::register_lambda(Lambda* lambda)
{
	if (auto found = module_.lambdas_.find(lambda); found != module_.lambdas_.end())
		return found->second;
	std::vector<std::string> parameters;
	bool variadic = false, variadic_convert = false;
	std::string variadic_element;
	for (const auto& parameter : lambda->parameters)
	{
		if (parameter.default_value)
			throw Error(parameter.default_value->location, "anonymous functions cannot use default parameters");
		if (parameter.convert && !parameter.variadic)
			throw Error(parameter.type_expr->location, "only variadic anonymous-function parameters may request conversion");
		std::string type = module_.value_type(parameter.type_expr);
		if (parameter.variadic)
		{
			variadic = true;
			variadic_convert = parameter.convert;
			variadic_element = type;
			type = "array<" + type + ">";
		}
		parameters.push_back(std::move(type));
	}
	const std::string result = module_.value_type(lambda->return_type, true);
	const auto captures = lambda_captures(lambda);
	for (const auto& [name, type] : captures)
		if (type == "module")
			throw Error(lambda->location, "module is opaque and cannot be captured by a closure");
	auto indirect_parameters = parameters;
	indirect_parameters.insert(indirect_parameters.begin(), "s32");
	const std::string contract = variadic ? "variadic:" + variadic_element + (variadic_convert ? ":convert" : "") : "";
	const unsigned type = module_.wasm_type(indirect_parameters, result, contract);
	if (variadic) module_.variadic_function_types_[type] = {parameters.size() - 1, variadic_element, variadic_convert};
	const std::string value_type = "function#" + std::to_string(type);
	module_.lambda_functions_.emplace_back(lambda->location, "__lambda_" + std::to_string(module_.lambdas_.size()));
	Function& function = module_.lambda_functions_.back();
	function.parameters = lambda->parameters;
	function.return_type = lambda->return_type;
	function.body = lambda->body;
	Definition definition;
	definition.function = &function;
	definition.parameters = std::move(indirect_parameters);
	definition.variadic = variadic;
	definition.variadic_element = variadic_element;
	definition.variadic_convert = variadic_convert;
	definition.result = result;
	definition.index = module_.first_user_index_ + static_cast<unsigned>(std::count_if(module_.definitions_.begin(), module_.definitions_.end(), [](const Definition& value) { return !value.inline_only; }));
	definition.type = type;
	definition.closure_body = true;
	definition.captures = captures;
	module_.definitions_by_name_[function.name].push_back(module_.definitions_.size());
	module_.definitions_.push_back(std::move(definition));
	const unsigned slot = static_cast<unsigned>(module_.table_functions_.size());
	module_.table_functions_.push_back(module_.definitions_.back().index);
	const unsigned type_id = 0x40000000u + static_cast<unsigned>(module_.lambdas_.size());
	std::vector<std::string> capture_types;
	for (const auto& [name, capture_type] : captures)
		capture_types.push_back(capture_type);
	module_.closure_types_[type_id] = std::move(capture_types);
	auto record = std::make_tuple(value_type, slot, type_id, &module_.definitions_.back(), captures);
	module_.lambdas_[lambda] = record;
	return record;
}

std::optional<std::pair<unsigned, std::string>> FunctionLowerer::compatible_local_callable(const std::string& name,
	const std::vector<std::string>& arguments) const
{
	for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
		if (auto found = scope->find(name); found != scope->end() && found->second.second.rfind("function#", 0) == 0)
		{
			const unsigned type = static_cast<unsigned>(std::stoul(found->second.second.substr(9)));
			if (type >= module_.types_.size())
				throw Error({module_.source_path(), 1, 1, 0}, "invalid function value type");
			const auto& signature = module_.types_[type];
			if (signature.first.size() != arguments.size() + 1)
				return std::nullopt;
			for (std::size_t i = 0; i < arguments.size(); ++i)
				if (signature.first[i + 1] != arguments[i])
					return std::nullopt;
			return found->second;
		}
	return std::nullopt;
}

std::string FunctionLowerer::infer_block(Block* block_value)
{
	const auto saved_scopes = scopes_;
	scopes_.push_back({});
	std::string result = "void";
	for (std::size_t i = 0; i < block_value->items.size(); ++i)
	{
		inferred_types_.clear();
		Expr* item = block_value->items[i];
		const bool final = i + 1 == block_value->items.size();
		if (auto yielded = dynamic_cast<Yield*>(item))
		{
			if (!final)
				throw Error(yielded->location, "block yield must be the final item in its block");
			result = infer(yielded->value);
		}
		else if (dynamic_cast<Return*>(item) || dynamic_cast<Break*>(item) || dynamic_cast<Continue*>(item))
			result = "never";
		else if (auto conditional = dynamic_cast<If*>(item); conditional && !final)
		{
			if (!condition_is_bool(infer(conditional->condition)))
				throw Error(conditional->condition->location, "if condition must be bool");
			result = "void";
		}
		else
		{
			result = infer(item);
			if (final)
				result = "void";
		}
		if (auto variable = dynamic_cast<Variable*>(item))
			scopes_.back()[variable->name] = {0, result};
		else if (auto binary = dynamic_cast<Binary*>(item); binary && binary->operator_ == ":=")
			if (auto name = dynamic_cast<Name*>(binary->left))
				scopes_.back()[name->value] = {0, result};
		auto register_condition_declaration = [&](Expr* condition)
		{
			if (auto variable = dynamic_cast<Variable*>(condition))
				scopes_.back()[variable->name] = {0, "bool"};
			else if (auto binary = dynamic_cast<Binary*>(condition); binary && binary->operator_ == ":=")
				if (auto name = dynamic_cast<Name*>(binary->left))
					scopes_.back()[name->value] = {0, "bool"};
		};
		if (auto conditional = dynamic_cast<If*>(item)) register_condition_declaration(conditional->condition);
		if (auto loop = dynamic_cast<While*>(item)) register_condition_declaration(loop->condition);
		if (!expression_falls_through(item))
			break;
	}
	scopes_ = saved_scopes;
	return result;
}

std::string FunctionLowerer::infer_integer(Integer* value, const std::string& expected) const
{
	const std::string type = expected.empty() ? "s64" : expected;
	if (!integer_type(type))
		throw Error(value->location, "expected " + type + ", found s64");
	if (!integer_fits(*value, type))
		throw Error(value->location, "integer literal is outside the " + type + " range");
	return type;
}

std::pair<Bytes, std::string> FunctionLowerer::integer_expression(Integer* value, const std::string& expected) const
{
	const std::string type = infer_integer(value, expected);
	if (wasm_value_type(type) == 0x7f)
	{
		Bytes code{0x41};
		wasm::append_sleb32(code, static_cast<std::int32_t>(signed_integer_value(*value)));
		return {code, type};
	}
	Bytes code{0x42};
	const std::int64_t encoded = type == "u64" ? std::bit_cast<std::int64_t>(value->magnitude) : signed_integer_value(*value);
	wasm::append_sleb64(code, encoded);
	return {code, type};
}

std::pair<Bytes, std::string> FunctionLowerer::float_expression(Float* value, const std::string& expected) const
{
	const std::string type = expected.empty() ? "f64" : expected;
	if (type != "f32" && type != "f64")
		throw Error(value->location, "expected " + type + ", found f64");
	Bytes code{static_cast<std::uint8_t>(type == "f32" ? 0x43 : 0x44)};
	if (type == "f32")
	{
		const float narrowed = static_cast<float>(value->value);
		if (!std::isfinite(narrowed)) throw Error(value->location, "float literal is outside the f32 range");
		wasm::append_f32(code, narrowed);
	}
	else wasm::append_f64(code, value->value);
	return {code, type};
}

std::string FunctionLowerer::infer(Expr* value)
{
	if (!value->program_owned)
		return infer_uncached(value);
	if (auto found = inferred_types_.find(value); found != inferred_types_.end())
		return found->second;
	std::string type = infer_uncached(value);
	inferred_types_.emplace(value, type);
	return type;
}

std::string FunctionLowerer::infer_uncached(Expr* value)
{
	if (auto integer = dynamic_cast<Integer*>(value))
		return infer_integer(integer);
	if (dynamic_cast<UnsignedInteger*>(value))
		return "u64";
	if (dynamic_cast<SignedInteger*>(value))
		return "s64";
	if (dynamic_cast<Float*>(value))
		return "f64";
	if (dynamic_cast<String*>(value))
		return "string";
	if (dynamic_cast<Markup*>(value))
		return "string";
	if (auto lambda = dynamic_cast<Lambda*>(value))
		return std::get<0>(register_lambda(lambda));
	if (auto block = dynamic_cast<Block*>(value))
		return infer_block(block);
	if (auto conditional = dynamic_cast<If*>(value))
	{
		const auto saved_scopes = scopes_;
		const std::string condition = infer(conditional->condition);
		if (!condition_is_bool(condition))
			throw Error(conditional->condition->location, "if condition must be bool");
		if (auto variable = dynamic_cast<Variable*>(conditional->condition))
			scopes_.back()[variable->name] = {0, condition};
		else if (auto binary = dynamic_cast<Binary*>(conditional->condition); binary && binary->operator_ == ":=")
			if (auto name = dynamic_cast<Name*>(binary->left))
				scopes_.back()[name->value] = {0, condition};
		const std::string then_type = infer_block(conditional->then_body);
		const std::string else_type = conditional->else_body ? infer_block(conditional->else_body) : "void";
		scopes_ = saved_scopes;
		if (!conditional->else_body)
			return "void";
		if (then_type == "never")
			return else_type;
		if (else_type == "never")
			return then_type;
		if (then_type != else_type)
			throw Error(conditional->location, "if branches produce " + then_type + " and " + else_type);
		return then_type;
	}
	if (dynamic_cast<Yield*>(value))
		throw Error(value->location, "block yield is only valid as a block item");
	if (dynamic_cast<Return*>(value) || dynamic_cast<Break*>(value) || dynamic_cast<Continue*>(value))
		return "never";
	if (dynamic_cast<While*>(value) || dynamic_cast<For*>(value))
		return "void";
	if (auto name = dynamic_cast<Name*>(value))
	{
		if (name->value == "true" || name->value == "false")
			return "bool";
		for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
			if (auto found = scope->find(name->value); found != scope->end())
				return found->second.second;
		if (name->value == "none")
			return "dval";
		return module_.reference_function(name->value, name->location).first;
	}
	if (auto variable = dynamic_cast<Variable*>(value))
	{
		const std::string declared = variable->annotation ? module_.value_type(variable->annotation) : "";
		std::string actual;
		if (variable->annotation && dynamic_cast<ArrayLiteral*>(variable->value) && declared.rfind("array<", 0) == 0)
		{
			const std::string element = declared.substr(6, declared.size() - 7);
			for (Expr* item : static_cast<ArrayLiteral*>(variable->value)->items)
			{
				const std::string item_type = dynamic_cast<Integer*>(item) ? infer_integer(static_cast<Integer*>(item), element) : infer(item);
				if (item_type != element) throw Error(item->location, "array literal elements must have one type");
			}
			actual = declared;
		}
		else
			actual = variable->annotation && dynamic_cast<Integer*>(variable->value)
				? infer_integer(static_cast<Integer*>(variable->value), declared) : infer(variable->value);
		const std::string resolved = variable->annotation ? declared : actual;
		if (resolved != actual)
			throw Error(value->location, "expected " + resolved + ", found " + actual);
		return resolved;
	}
	if (auto spread = dynamic_cast<Spread*>(value))
	{
		const std::string source = infer(spread->value);
		if (source.rfind("array<", 0) == 0)
			return "spread<" + source.substr(6);
		throw Error(spread->location, "spread requires an array");
	}
	if (dynamic_cast<MapLiteral*>(value))
		return "dval";
	if (auto array = dynamic_cast<ArrayLiteral*>(value))
	{
		if (array->items.empty() && !array->explicit_element_type)
			throw Error(value->location, "empty array literal needs an explicit element type");
		std::string element = array->explicit_element_type ? module_.value_type(array->explicit_element_type) : "";
		for (Expr* item : array->items)
		{
			std::string item_type;
			if (auto spread = dynamic_cast<Spread*>(item))
			{
				const std::string source = infer(spread->value);
				if (source.rfind("array<", 0) != 0)
					throw Error(spread->location, "array literal spread requires an array");
				item_type = spread->target_element_type ? module_.value_type(spread->target_element_type) : source.substr(6, source.size() - 7);
			}
			else if (auto integer = dynamic_cast<Integer*>(item); integer && !element.empty())
				item_type = infer_integer(integer, element);
			else
				item_type = infer(item);
			if (element.empty()) element = item_type;
			if (item_type != element) throw Error(item->location, "array literal elements must have one type");
		}
		if (element == "module")
			throw Error(value->location, "module is opaque and cannot be stored in array layouts");
		return "array<" + element + ">";
	}
	if (auto scope = dynamic_cast<ScopeLookup*>(value))
	{
		if (scope->member == "type")
			throw Error(scope->location, "value::type is valid only in dependent type declarations");
		if (scope->member == "type_name")
		{
			const std::string object = infer(scope->value);
			if (object == "void" || object == "never") throw Error(scope->location, "value::type_name requires a value");
			return "string";
		}
		if (scope->member == "size")
		{
			const std::string object = infer(scope->value);
			if (object.rfind("struct:", 0) != 0) throw Error(scope->location, "value::size requires a struct");
			return "s64";
		}
		if (scope->member == "items")
		{
			const std::string object = infer(scope->value);
			if (object.rfind("struct:", 0) != 0) throw Error(scope->location, "value::items requires a struct");
			return "dval";
		}
		throw Error(scope->location, "unknown scope member '" + scope->member + "'");
	}
	if (auto member = dynamic_cast<Member*>(value))
	{
		const std::string object = infer(member->value);
		if (object == "dval" || object == "module")
			return "dval";
		if (object.rfind("struct:", 0) != 0)
			throw Error(member->location, "member access requires a struct");
		for (const auto& field : module_.struct_type(object.substr(7), member->location).fields)
			if (field.first == member->member)
				return field.second;
		throw Error(member->location, "struct has no member '" + member->member + "'");
	}
	if (auto index = dynamic_cast<Index*>(value))
	{
		const std::string object = infer(index->value);
		if (object == "dval")
		{
			const std::string key = infer(index->index);
			if (key != "string" && key != "s64")
				throw Error(index->index->location, "dval index must be string or s64");
			return "dval";
		}
		if (object.rfind("array<", 0) == 0)
			return object.substr(6, object.size() - 7);
		throw Error(index->location, "indexing requires an array");
	}
	if (auto call = dynamic_cast<Call*>(value))
	{
		if (const Member* member = member_call(call))
		{
			const std::string receiver = infer(member->value);
			if (receiver == "module" && member->member != "call")
			{
				if (call->arguments.size() > 1)
					throw Error(call->location, "dynamic module member call accepts at most one dval input");
				return "dval";
			}
			if (receiver.rfind("array<", 0) == 0)
			{
				const std::string element = receiver.substr(6, receiver.size() - 7);
				const std::size_t count = call->arguments.size();
				auto argument_type = [&](std::size_t index, const std::string& expected)
				{
					if (auto integer = dynamic_cast<Integer*>(call->arguments[index]); integer && integer_type(expected)) return infer_integer(integer, expected);
					return infer(call->arguments[index]);
				};
				if (member->member == "capacity")
				{
					if (count != 0) throw Error(call->location, "array capacity expects no arguments");
					return "s64";
				}
				if (member->member == "push")
				{
					if (count != 1 || argument_type(0, element) != element) throw Error(call->location, "array push expects one " + element + " value");
					return "void";
				}
				if (member->member == "pop")
				{
					if (count != 0) throw Error(call->location, "array pop expects no arguments");
					return element;
				}
				if (member->member == "insert")
				{
					if (count != 2 || argument_type(0, "s64") != "s64" || argument_type(1, element) != element)
						throw Error(call->location, "array insert expects an s64 index and " + element + " value");
					return "void";
				}
				if (member->member == "remove")
				{
					if (count != 1 || infer(call->arguments[0]) != "s64") throw Error(call->location, "array remove expects one s64 index");
					return element;
				}
				if (member->member == "clear" || member->member == "reserve")
				{
					if (count != (member->member == "reserve" ? 1u : 0u) || (count && infer(call->arguments[0]) != "s64"))
						throw Error(call->location, "array " + member->member + (count ? " expects an s64 capacity" : " expects no arguments"));
					return "void";
				}
				if (member->member == "resize")
				{
					if (count != 2 || argument_type(0, "s64") != "s64" || argument_type(1, element) != element)
						throw Error(call->location, "array resize expects an s64 length and " + element + " fill value");
					return "void";
				}
			}
			std::vector<std::string> arguments{receiver};
			for (Expr* argument : call->arguments)
				arguments.push_back(infer(argument));
			if (auto local = compatible_local_callable(member->member, arguments))
			{
				const auto& signature = module_.types_.at(static_cast<unsigned>(std::stoul(local->second.substr(9))));
				return signature.second;
			}
			if (const Module::HostDeclaration* host = module_.host(member->member, arguments))
				return host->result;
			if (Definition* definition = module_.compatible_definition(member->member, arguments, call->location))
				return definition->result;
		}
		auto name = dynamic_cast<Name*>(call->function);
		auto function_value_result = [&](unsigned type) -> std::string
		{
			if (type >= module_.types_.size()) throw Error(call->location, "invalid function value type");
			const auto& signature = module_.types_[type];
			std::vector<std::pair<std::string, Location>> argument_types;
			for (Expr* argument : call->arguments)
				argument_types.push_back({infer(argument), argument->location});
			auto variadic = module_.variadic_function_types_.find(type);
			if (call->arguments.size() == argument_types.size())
				for (std::size_t i = 0; i < call->arguments.size(); ++i)
					if (auto integer = dynamic_cast<Integer*>(call->arguments[i]))
					{
						if (variadic == module_.variadic_function_types_.end() && i + 1 >= signature.first.size()) continue;
						const std::string expected = variadic == module_.variadic_function_types_.end() || i < variadic->second.fixed
							? signature.first[i + 1] : variadic->second.element;
						if (integer_type(expected)) argument_types[i].first = infer_integer(integer, expected);
					}
			if (variadic == module_.variadic_function_types_.end())
			{
				if (signature.first.size() != argument_types.size() + 1) throw Error(call->location, "function value argument count does not match signature");
				for (std::size_t i = 0; i < argument_types.size(); ++i)
					if (argument_types[i].first != signature.first[i + 1]) throw Error(argument_types[i].second, "function value argument type does not match signature");
				return signature.second;
			}
			const auto& contract = variadic->second;
			if (argument_types.size() < contract.fixed) throw Error(call->location, "variadic function value has too few arguments");
			for (std::size_t i = 0; i < contract.fixed; ++i)
				if (argument_types[i].first != signature.first[i + 1]) throw Error(argument_types[i].second, "function value argument type does not match signature");
			for (std::size_t i = contract.fixed; i < argument_types.size(); ++i)
			{
				std::string source = normalize_spread_type(argument_types[i].first);
				if (source != contract.element && (!contract.convert || !module_.constructor_available(source, contract.element)))
					throw Error(argument_types[i].second, "function value variadic argument cannot construct " + contract.element + " from " + source);
			}
			return signature.second;
		};
		if (!name)
		{
			const std::string function = infer(call->function);
			if (function.rfind("function#", 0) != 0)
				throw Error(call->function->location, "call target is not a function value");
			return function_value_result(static_cast<unsigned>(std::stoul(function.substr(9))));
		}
		for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
			if (auto found = scope->find(name->value); found != scope->end() && found->second.second.rfind("function#", 0) == 0)
			{
				const unsigned type = static_cast<unsigned>(std::stoul(found->second.second.substr(9)));
				return function_value_result(type);
			}
		const std::string type_callee = module_.has_alias(name->value) ? module_.constructor_name(name->value, name->location) : name->value;
		if (type_callee == "dval")
		{
			if (call->arguments.size() != 1)
				throw Error(call->location, "dval expects one scalar, map, or list");
			if (dynamic_cast<MapLiteral*>(call->arguments[0]) || dynamic_cast<ArrayLiteral*>(call->arguments[0]))
				return "dval";
			const std::string argument = infer(call->arguments[0]);
			if (argument != "string" && !is_scalar(argument) && argument != "dval" && !argument.starts_with("function#"))
				throw Error(call->arguments[0]->location, "cannot construct dval from " + argument);
			return "dval";
		}
		std::vector<std::string> arguments;
		for (Expr* argument : call->arguments)
			if (auto integer = dynamic_cast<Integer*>(argument); integer && primitive_constructor_name(type_callee) && call->arguments.size() == 1 && integer_type(type_callee))
				arguments.push_back(infer_integer(integer, type_callee));
			else if (dynamic_cast<Integer*>(argument))
				arguments.push_back("s64");
			else
				arguments.push_back(infer(argument));
		if (primitive_constructor_name(type_callee) && arguments.size() == 1)
		{
			if (Definition* exact = module_.exact_definition(type_callee, arguments))
				return exact->result;
			if (arguments[0] == type_callee)
				return type_callee;
			if (can_convert(arguments[0], type_callee))
				return type_callee;
			return module_.resolve(type_callee, arguments, call->location).result;
		}
		if (module_.has_struct(type_callee))
		{
			std::vector<std::string> fields;
			for (const auto& field : module_.struct_type(type_callee, call->location).fields)
				fields.push_back(field.second);
			std::vector<std::string> contextual = arguments;
			if (contextual.size() == fields.size() && call->arguments.size() == fields.size())
				for (std::size_t i = 0; i < contextual.size(); ++i)
					if (auto integer = dynamic_cast<Integer*>(call->arguments[i]); integer && integer_fits(*integer, fields[i]))
						contextual[i] = fields[i];
			if (contextual == fields)
				return "struct:" + type_callee;
		}
		if (name->value == "clone")
			return infer(call->arguments.at(0));
		if (name->value == "length" || name->value == "arc_live") return "s64";
		if (name->value == "__bearer_byte") return "u8";
		if (name->value == "string_from_bytes") return "string";
		if (name->value == "bytes_of") return "array<u8>";
		if (name->value == "has") return "bool";
		if (name->value == "trap")
			return "void";
		if (auto contextual = module_.contextual_argument_types(name->value, call->arguments, arguments, call->location))
		{
			if (const Module::HostDeclaration* declaration = module_.host(name->value, *contextual))
				return declaration->result;
			return module_.resolve(name->value, *contextual, call->location).result;
		}
		if (const Module::HostDeclaration* declaration = module_.host(name->value, arguments))
			return declaration->result;
		if (Definition* definition = module_.compatible_definition(name->value, arguments, call->location))
			return definition->result;
		return module_.resolve(name->value, arguments, call->location).result;
	}
	if (auto binary = dynamic_cast<Binary*>(value))
	{
		if (binary->operator_ == "postfix?")
		{
			if (infer(binary->left) != "dval")
				throw Error(binary->location, "postfix '?' requires dval");
			return "bool";
		}
		if (binary->operator_ == "..")
			return "range";
		if (binary->operator_ == "=" || binary->operator_ == ":=")
		{
			if (binary->operator_ == ":=")
				return infer(binary->right);
			std::string target_type;
			if (auto target = dynamic_cast<Name*>(binary->left))
				target_type = lookup(target).second;
			else if (auto member = dynamic_cast<Member*>(binary->left))
			{
				const std::string receiver = infer(member->value);
				if (receiver == "dval")
					target_type = "dval";
				else
				{
					if (receiver.rfind("struct:", 0) != 0)
						throw Error(member->location, "member assignment requires a struct or dval receiver");
					for (const auto& field : module_.struct_type(receiver.substr(7), member->location).fields)
						if (field.first == member->member) target_type = field.second;
					if (target_type.empty())
						throw Error(member->location, "struct has no member '" + member->member + "'");
				}
			}
			else if (auto index = dynamic_cast<Index*>(binary->left))
			{
				const std::string receiver = infer(index->value);
				if (receiver.rfind("array<", 0) == 0)
					target_type = receiver.substr(6, receiver.size() - 7);
				else if (receiver == "dval")
					target_type = "dval";
				else
					throw Error(index->location, "indexed assignment requires an array or dval receiver");
			}
			else
				throw Error(binary->left->location, "assignment target must be a binding, array element, DValue path, or struct field");
			std::string actual;
			if (auto array = dynamic_cast<ArrayLiteral*>(binary->right); target_type.rfind("array<", 0) == 0)
			{
				const std::string element = target_type.substr(6, target_type.size() - 7);
				for (Expr* item : array->items)
				{
					const std::string item_type = dynamic_cast<Integer*>(item) ? infer_integer(static_cast<Integer*>(item), element) : infer(item);
					if (item_type != element) throw Error(item->location, "array literal elements must have one type");
				}
				actual = target_type;
			}
			else
				actual = dynamic_cast<Integer*>(binary->right) && target_type != "dval"
					? infer_integer(static_cast<Integer*>(binary->right), target_type) : infer(binary->right);
			if (target_type == "dval")
			{
				if (actual != "dval" && actual != "string" && !is_scalar(actual) && !actual.starts_with("function#"))
					throw Error(binary->right->location, "cannot construct dval from " + actual);
				return "dval";
			}
			if (actual != target_type)
				throw Error(binary->right->location, "expected " + target_type + ", found " + actual);
			return target_type;
		}
		if (binary->operator_ == "&&" || binary->operator_ == "||" || binary->operator_ == "unary!")
		{
			if (infer(binary->right) != "bool" || (binary->operator_ != "unary!" && infer(binary->left) != "bool"))
				throw Error(binary->location, "logical operators require bool operands");
			return "bool";
		}
		const std::string inferred_left = dynamic_cast<Integer*>(binary->left) ? "s64" : infer(binary->left);
		const std::string inferred_right = dynamic_cast<Integer*>(binary->right) ? "s64" : infer(binary->right);
		if (inferred_left == "string" || inferred_right == "string")
		{
			if (inferred_left != "string" || inferred_right != "string")
				throw Error(binary->location, "string operators require string operands");
			if (binary->operator_ == "+") return "string";
			if (binary->operator_ == "==" || binary->operator_ == "!=") return "bool";
			throw Error(binary->location, "strings support only +, ==, and != operators");
		}
		if (binary->operator_ == "unary-")
		{
			const std::string operand = infer(binary->right);
			if (operand != "s8" && operand != "s16" && operand != "s32" && operand != "s64" && operand != "f32" && operand != "f64")
				throw Error(binary->location, "unary - requires a signed integer or float operand");
			return operand;
		}
		const std::string left = dynamic_cast<Integer*>(binary->left) && !dynamic_cast<Integer*>(binary->right)
			? infer_integer(static_cast<Integer*>(binary->left), inferred_right) : inferred_left;
		const std::string right = dynamic_cast<Integer*>(binary->right) && !dynamic_cast<Integer*>(binary->left)
			? infer_integer(static_cast<Integer*>(binary->right), inferred_left) : inferred_right;
		if (left != right)
			throw Error(binary->location, "expected " + left + ", found " + right);
		const bool equality = binary->operator_ == "==" || binary->operator_ == "!=";
		if (!is_scalar(left) || (left == "bool" && !equality))
			throw Error(binary->location, "unsupported operator " + binary->operator_ + " for " + left);
		const bool comparison = binary->operator_ == "==" || binary->operator_ == "!=" || binary->operator_ == "<" || binary->operator_ == ">" ||
								binary->operator_ == "<=" || binary->operator_ == ">=";
		if (left == "f64" && binary->operator_ == "%")
			throw Error(binary->location, "operator % is not defined for f64");
		return comparison ? "bool" : left;
	}
	throw Error(value->location, "cannot infer type of expression");
}

Bytes FunctionLowerer::markup_write_bytes(unsigned cursor, std::string_view text)
{
	Bytes code;
	for (unsigned char byte : text)
	{
		code.push_back(0x20);
		wasm::append_uleb(code, cursor);
		code.push_back(0x41);
		wasm::append_sleb32(code, byte);
		code.insert(code.end(), {0x3a, 0x00, 0x00, 0x20});
		wasm::append_uleb(code, cursor);
		code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
		wasm::append_uleb(code, cursor);
	}
	return code;
}

Bytes FunctionLowerer::markup_unicode_separator(unsigned source, unsigned index, unsigned length, unsigned target)
{
	Bytes code{0x41, 0x00, 0x21};
	wasm::append_uleb(code, target);
	code.push_back(0x20); wasm::append_uleb(code, index);
	code.insert(code.end(), {0x41, 0x02, 0x6a, 0x20}); wasm::append_uleb(code, length);
	code.insert(code.end(), {0x49, 0x04, 0x40});
	auto load = [&](unsigned offset)
	{
		code.push_back(0x20); wasm::append_uleb(code, source);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20}); wasm::append_uleb(code, index);
		if (offset) { code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(offset)); code.push_back(0x6a); }
		code.insert(code.end(), {0x6a, 0x2d, 0x00, 0x00});
	};
	load(0); code.push_back(0x41); wasm::append_sleb32(code, 0xe2); code.push_back(0x46);
	load(1); code.push_back(0x41); wasm::append_sleb32(code, 0x80); code.insert(code.end(), {0x46, 0x71});
	load(2); code.push_back(0x22); wasm::append_uleb(code, target);
	code.push_back(0x41); wasm::append_sleb32(code, 0xa8); code.push_back(0x46);
	code.push_back(0x20); wasm::append_uleb(code, target);
	code.push_back(0x41); wasm::append_sleb32(code, 0xa9); code.insert(code.end(), {0x46, 0x72, 0x71, 0x45, 0x04, 0x40, 0x41, 0x00, 0x21});
	wasm::append_uleb(code, target);
	code.insert(code.end(), {0x0b, 0x0b});
	return code;
}

std::vector<std::pair<std::int32_t, std::string>> markup_escape_sequences(bearer::MarkupContext context)
{
	if (context == bearer::MarkupContext::html_text || context == bearer::MarkupContext::html_attribute)
		return {{'&', "&amp;"}, {'<', "&lt;"}, {'>', "&gt;"}, {'"', "&quot;"}, {'\'', "&#39;"}};
	static const char hex[] = "0123456789ABCDEF";
	std::vector<std::pair<std::int32_t, std::string>> result;
	if (context == bearer::MarkupContext::javascript_value)
	{
		result = {{'"', "\\\""}, {'\\', "\\\\"}, {'/', "\\/"}, {'<', "\\u003C"}, {'>', "\\u003E"}, {'&', "\\u0026"}, {'\'', "\\u0027"}};
		for (int value = 0; value < 32; ++value)
		{
			std::string escaped = "\\u00";
			escaped += hex[value >> 4];
			escaped += hex[value & 15];
			result.emplace_back(value, std::move(escaped));
		}
	}
	else
	{
		result = {{'"', "\\\""}, {'\\', "\\\\"}, {'<', "\\3C "}, {'>', "\\3E "}, {'&', "\\26 "}, {127, "\\7F "}};
		for (int value = 0; value < 32; ++value)
		{
			std::string escaped = "\\";
			if (value > 15) escaped += hex[value >> 4];
			escaped += hex[value & 15];
			escaped += ' ';
			result.emplace_back(value, std::move(escaped));
		}
	}
	return result;
}

Bytes FunctionLowerer::markup_escape_length(unsigned source, unsigned total, bearer::MarkupContext context, const Location& location)
{
	const unsigned index = add_local("", "s32", location), length = add_local("", "s32", location), byte = add_local("", "s32", location);
	const bool contextual = context == bearer::MarkupContext::javascript_value || context == bearer::MarkupContext::css_value;
	const unsigned separator = contextual ? add_local("", "s32", location) : 0;
	Bytes code;
	if (contextual)
	{
		code.push_back(0x20); wasm::append_uleb(code, total);
		code.insert(code.end(), {0x41, 0x02, 0x6a, 0x21}); wasm::append_uleb(code, total);
	}
	code.push_back(0x20);
	wasm::append_uleb(code, source);
	code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21});
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x00, 0x21});
	wasm::append_uleb(code, index);
	code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20});
	wasm::append_uleb(code, index);
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x4f, 0x0d, 0x01});
	code.push_back(0x20);
	wasm::append_uleb(code, source);
	code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
	wasm::append_uleb(code, index);
	code.insert(code.end(), {0x6a, 0x2d, 0x00, 0x00, 0x21});
	wasm::append_uleb(code, byte);
	if (contextual)
	{
		append(code, markup_unicode_separator(source, index, length, separator));
		code.push_back(0x20); wasm::append_uleb(code, separator);
		code.insert(code.end(), {0x04, 0x40, 0x20}); wasm::append_uleb(code, total);
		code.insert(code.end(), {0x41, 0x06, 0x6a, 0x21}); wasm::append_uleb(code, total);
		code.push_back(0x20); wasm::append_uleb(code, index);
		code.insert(code.end(), {0x41, 0x03, 0x6a, 0x21}); wasm::append_uleb(code, index);
		code.push_back(0x05);
	}
	code.push_back(0x20);
	wasm::append_uleb(code, total);
	code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
	wasm::append_uleb(code, total);
	for (const auto& [character, escaped] : markup_escape_sequences(context))
	{
		const std::int32_t extra = static_cast<std::int32_t>(escaped.size() - 1);
		code.push_back(0x20);
		wasm::append_uleb(code, byte);
		code.push_back(0x41);
		wasm::append_sleb32(code, character);
		code.insert(code.end(), {0x46, 0x04, 0x40, 0x20});
		wasm::append_uleb(code, total);
		code.push_back(0x41);
		wasm::append_sleb32(code, extra);
		code.insert(code.end(), {0x6a, 0x21});
		wasm::append_uleb(code, total);
		code.push_back(0x0b);
	}
	code.push_back(0x20);
	wasm::append_uleb(code, index);
	code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
	wasm::append_uleb(code, index);
	if (contextual) code.push_back(0x0b);
	code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
	return code;
}

Bytes FunctionLowerer::markup_escape_write(unsigned source, unsigned cursor, bearer::MarkupContext context, const Location& location)
{
	const unsigned index = add_local("", "s32", location), length = add_local("", "s32", location), byte = add_local("", "s32", location);
	const bool contextual = context == bearer::MarkupContext::javascript_value || context == bearer::MarkupContext::css_value;
	const unsigned separator = contextual ? add_local("", "s32", location) : 0;
	Bytes code;
	if (contextual)
		append(code, markup_write_bytes(cursor, "\""));
	code.push_back(0x20);
	wasm::append_uleb(code, source);
	code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21});
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x00, 0x21});
	wasm::append_uleb(code, index);
	code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20});
	wasm::append_uleb(code, index);
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x4f, 0x0d, 0x01});
	code.push_back(0x20);
	wasm::append_uleb(code, source);
	code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
	wasm::append_uleb(code, index);
	code.insert(code.end(), {0x6a, 0x2d, 0x00, 0x00, 0x21});
	wasm::append_uleb(code, byte);
	if (contextual)
	{
		append(code, markup_unicode_separator(source, index, length, separator));
		code.push_back(0x20); wasm::append_uleb(code, separator);
		code.insert(code.end(), {0x04, 0x40, 0x20}); wasm::append_uleb(code, separator);
		code.push_back(0x41); wasm::append_sleb32(code, 0xa8);
		code.insert(code.end(), {0x46, 0x04, 0x40});
		append(code, markup_write_bytes(cursor, context == bearer::MarkupContext::javascript_value ? "\\u2028" : "\\2028 "));
		code.push_back(0x05);
		append(code, markup_write_bytes(cursor, context == bearer::MarkupContext::javascript_value ? "\\u2029" : "\\2029 "));
		code.push_back(0x0b);
		code.push_back(0x20); wasm::append_uleb(code, index);
		code.insert(code.end(), {0x41, 0x03, 0x6a, 0x21}); wasm::append_uleb(code, index);
		code.push_back(0x05);
	}
	code.insert(code.end(), {0x02, 0x40});
	for (const auto& [character, escaped] : markup_escape_sequences(context))
	{
		code.push_back(0x20);
		wasm::append_uleb(code, byte);
		code.push_back(0x41);
		wasm::append_sleb32(code, character);
		code.insert(code.end(), {0x46, 0x04, 0x40});
		append(code, markup_write_bytes(cursor, escaped));
		code.insert(code.end(), {0x0c, 0x01, 0x0b});
	}
	code.push_back(0x20);
	wasm::append_uleb(code, cursor);
	code.push_back(0x20);
	wasm::append_uleb(code, byte);
	code.insert(code.end(), {0x3a, 0x00, 0x00, 0x20});
	wasm::append_uleb(code, cursor);
	code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
	wasm::append_uleb(code, cursor);
	code.push_back(0x0b);
	code.push_back(0x20);
	wasm::append_uleb(code, index);
	code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
	wasm::append_uleb(code, index);
	if (contextual) code.push_back(0x0b);
	code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
	if (contextual)
		append(code, markup_write_bytes(cursor, "\""));
	return code;
}

Bytes FunctionLowerer::markup_s32_length(unsigned source, unsigned total, const Location& location)
{
	const unsigned value = add_local("", "s32", location), digits = add_local("", "s32", location);
	Bytes code{0x20};
	wasm::append_uleb(code, source);
	code.push_back(0x21);
	wasm::append_uleb(code, value);
	code.push_back(0x20);
	wasm::append_uleb(code, source);
	code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40, 0x20});
	wasm::append_uleb(code, total);
	code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
	wasm::append_uleb(code, total);
	code.push_back(0x0b);
	code.insert(code.end(), {0x41, 0x00, 0x21});
	wasm::append_uleb(code, digits);
	code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20});
	wasm::append_uleb(code, digits);
	code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
	wasm::append_uleb(code, digits);
	code.push_back(0x20);
	wasm::append_uleb(code, value);
	code.insert(code.end(), {0x41, 0x0a, 0x6d, 0x22});
	wasm::append_uleb(code, value);
	code.insert(code.end(), {0x0d, 0x00, 0x0b, 0x0b});
	code.push_back(0x20);
	wasm::append_uleb(code, total);
	code.push_back(0x20);
	wasm::append_uleb(code, digits);
	code.insert(code.end(), {0x6a, 0x21});
	wasm::append_uleb(code, total);
	return code;
}

Bytes FunctionLowerer::markup_s32_write(unsigned source, unsigned cursor, const Location& location)
{
	const unsigned value = add_local("", "s32", location), divisor = add_local("", "s32", location), digit = add_local("", "s32", location);
	Bytes code{0x20};
	wasm::append_uleb(code, source);
	code.push_back(0x21);
	wasm::append_uleb(code, value);
	code.push_back(0x20);
	wasm::append_uleb(code, value);
	code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
	append(code, markup_write_bytes(cursor, "-"));
	code.push_back(0x05);
	code.insert(code.end(), {0x41, 0x00, 0x20});
	wasm::append_uleb(code, value);
	code.insert(code.end(), {0x6b, 0x21});
	wasm::append_uleb(code, value);
	code.push_back(0x0b);
	code.insert(code.end(), {0x41, 0x01, 0x21});
	wasm::append_uleb(code, divisor);
	code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20});
	wasm::append_uleb(code, value);
	code.push_back(0x20);
	wasm::append_uleb(code, divisor);
	code.insert(code.end(), {0x6d, 0x41, 0x76, 0x4c, 0x45, 0x0d, 0x01, 0x20});
	wasm::append_uleb(code, divisor);
	code.insert(code.end(), {0x41, 0x0a, 0x6c, 0x21});
	wasm::append_uleb(code, divisor);
	code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
	code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x41, 0x00, 0x20});
	wasm::append_uleb(code, value);
	code.push_back(0x20);
	wasm::append_uleb(code, divisor);
	code.insert(code.end(), {0x6d, 0x6b, 0x21});
	wasm::append_uleb(code, digit);
	code.push_back(0x20);
	wasm::append_uleb(code, cursor);
	code.push_back(0x20);
	wasm::append_uleb(code, digit);
	code.insert(code.end(), {0x41, 0x30, 0x6a, 0x3a, 0x00, 0x00, 0x20});
	wasm::append_uleb(code, cursor);
	code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
	wasm::append_uleb(code, cursor);
	code.push_back(0x20);
	wasm::append_uleb(code, value);
	code.push_back(0x20);
	wasm::append_uleb(code, divisor);
	code.insert(code.end(), {0x6f, 0x21});
	wasm::append_uleb(code, value);
	code.push_back(0x20);
	wasm::append_uleb(code, divisor);
	code.insert(code.end(), {0x41, 0x0a, 0x6d, 0x22});
	wasm::append_uleb(code, divisor);
	code.insert(code.end(), {0x0d, 0x00, 0x0b, 0x0b});
	return code;
}

std::pair<Bytes, unsigned> FunctionLowerer::allocate_dval(unsigned length, const Location& location, const Bytes& failure_cleanup)
{
	const unsigned capacity = add_local("", "s32", location), payload = add_local("", "s32", location), handle = add_local("", "dval", location);
	Bytes code{0x20}; wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40}); append(code, failure_cleanup); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b});
	code.push_back(0x20); wasm::append_uleb(code, length);
	code.insert(code.end(), {0x45, 0x04, 0x7f, 0x41, 0x01, 0x05, 0x20}); wasm::append_uleb(code, length);
	code.insert(code.end(), {0x0b, 0x22}); wasm::append_uleb(code, capacity);
	code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_alloc"));
	code.push_back(0x22); wasm::append_uleb(code, payload);
	code.insert(code.end(), {0x45, 0x04, 0x40}); append(code, failure_cleanup); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b});
	code.push_back(0x41); wasm::append_sleb32(code, BEARER_WASM_OBJECT_HANDLE_SIZE); code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_alloc"));
	code.push_back(0x22); wasm::append_uleb(code, handle); code.insert(code.end(), {0x45, 0x04, 0x40});
	code.push_back(0x20); wasm::append_uleb(code, payload); code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_free"));
	append(code, failure_cleanup); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b});
	auto store = [&](unsigned offset, std::optional<std::int32_t> constant, unsigned local)
	{
		code.push_back(0x20); wasm::append_uleb(code, handle);
		if (constant) { code.push_back(0x41); wasm::append_sleb32(code, *constant); }
		else { code.push_back(0x20); wasm::append_uleb(code, local); }
		code.insert(code.end(), {0x36, 0x02}); wasm::append_uleb(code, offset);
	};
	store(BEARER_WASM_OBJECT_REFS_OFFSET, 1, 0);
	store(BEARER_WASM_OBJECT_OWNER_OFFSET, 1, 0);
	store(BEARER_WASM_OBJECT_TYPE_OFFSET, 4, 0);
	store(BEARER_WASM_OBJECT_HEADER_SIZE_OFFSET, BEARER_WASM_OBJECT_HANDLE_SIZE, 0);
	store(BEARER_WASM_OBJECT_LENGTH_OFFSET, std::nullopt, length);
	store(BEARER_WASM_OBJECT_CAPACITY_OFFSET, std::nullopt, capacity);
	store(BEARER_WASM_OBJECT_PAYLOAD_OFFSET, std::nullopt, payload);
	code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
	return {std::move(code), handle};
}

void FunctionLowerer::retain_dval_callables(Bytes& code, unsigned value, const Location& location)
{
	if (!module_.runtime_imports_.contains("bearer_dv_callable_at_brrb"))
		return;
	const unsigned ordinal = add_local("", "s32", location), closure = add_local("", "function#0", location);
	code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, ordinal);
	code.insert(code.end(), {0x02, 0x40, 0x03, 0x40});
	managed_payload_span(code, value, "dval");
	code.push_back(0x20); wasm::append_uleb(code, ordinal);
	code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_dv_callable_at_brrb"));
	code.push_back(0x22); wasm::append_uleb(code, closure);
	code.insert(code.end(), {0x45, 0x0d, 0x01, 0x20}); wasm::append_uleb(code, closure);
	code.push_back(0x10); wasm::append_uleb(code, module_.retain_index());
	code.push_back(0x20); wasm::append_uleb(code, ordinal);
	code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, ordinal);
	code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
}

std::pair<Bytes, std::string> FunctionLowerer::byte_conversion(Call* call, bool to_string)
{
	if (call->arguments.size() != 1)
		throw Error(call->location, to_string ? "string_from_bytes expects one [u8] value" : "bytes_of expects one string");
	const std::string expected = to_string ? "array<u8>" : "string";
	auto source = expression(call->arguments[0]);
	if (source.second != expected)
		throw Error(call->arguments[0]->location, (to_string ? "string_from_bytes expects [u8], found " : "bytes_of expects string, found ") + source.second);
	const unsigned input = add_local("", expected, call->arguments[0]->location);
	Bytes code = std::move(source.first);
	code.push_back(0x21);
	wasm::append_uleb(code, input);
	const unsigned length = add_local("", "s32", call->location);
	code.push_back(0x20);
	wasm::append_uleb(code, input);
	code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21});
	wasm::append_uleb(code, length);
	Bytes release_input;
	if (expression_is_owned(call->arguments[0]))
	{
		release_input.push_back(0x20);
		wasm::append_uleb(release_input, input);
		release_input.push_back(0x10);
		wasm::append_uleb(release_input, module_.release_index());
	}
	const unsigned source_pointer = add_local("", "s32", call->location), destination = add_local("", "s32", call->location), index = add_local("", "s32", call->location);
	auto copy_bytes = [&](bool compact_source)
	{
		code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, index);
		code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, index);
		code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4f, 0x0d, 0x01});
		code.push_back(0x20); wasm::append_uleb(code, destination); code.push_back(0x20); wasm::append_uleb(code, index);
		if (compact_source) code.insert(code.end(), {0x41, BEARER_WASM_OBJECT_WORD_SIZE, 0x6c});
		code.push_back(0x6a);
		code.push_back(0x20); wasm::append_uleb(code, source_pointer); code.push_back(0x20); wasm::append_uleb(code, index);
		if (!compact_source) code.insert(code.end(), {0x41, BEARER_WASM_OBJECT_WORD_SIZE, 0x6c});
		code.insert(code.end(), {0x6a, 0x2d, 0x00, 0x00});
		code.insert(code.end(), {static_cast<std::uint8_t>(compact_source ? 0x36 : 0x3a), static_cast<std::uint8_t>(compact_source ? 2 : 0), 0x00});
		code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, index);
		code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
	};
	if (to_string)
	{
		auto allocation = allocate_blob("string", 1, length, call->location, release_input);
		append(code, allocation.first);
		code.push_back(0x20); wasm::append_uleb(code, input); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x21); wasm::append_uleb(code, source_pointer);
		managed_payload_pointer(code, allocation.second, "string"); code.push_back(0x21); wasm::append_uleb(code, destination);
		copy_bytes(false);
		append(code, release_input);
		code.push_back(0x20); wasm::append_uleb(code, allocation.second);
		return {code, "string"};
	}
	auto allocation = allocate_array("array<u8>", length, call->location, release_input);
	append(code, allocation.first);
	managed_payload_pointer(code, input, "string"); code.push_back(0x21); wasm::append_uleb(code, source_pointer);
	code.push_back(0x20); wasm::append_uleb(code, allocation.second); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x21); wasm::append_uleb(code, destination);
	copy_bytes(true);
	append(code, release_input);
	code.push_back(0x20); wasm::append_uleb(code, allocation.second);
	return {code, "array<u8>"};
}

std::pair<Bytes, unsigned> FunctionLowerer::allocate_blob(const std::string& type, unsigned type_id, unsigned length, const Location& location, const Bytes& failure_cleanup)
{
	if (type == "dval")
		return allocate_dval(length, location, failure_cleanup);
	const unsigned pointer = add_local("", type, location);
	Bytes code{0x20};
	wasm::append_uleb(code, length);
	code.push_back(0x41);
	wasm::append_sleb32(code, std::numeric_limits<std::int32_t>::max() - BEARER_WASM_OBJECT_BLOB_HEADER_SIZE);
	code.insert(code.end(), {0x4b, 0x04, 0x40});
	append(code, failure_cleanup);
	append(code, module_.marker(location));
	code.insert(code.end(), {0x00, 0x0b, 0x20});
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, BEARER_WASM_OBJECT_BLOB_HEADER_SIZE, 0x6a, 0x10});
	wasm::append_uleb(code, module_.import_index("bearer_alloc"));
	code.push_back(0x21);
	wasm::append_uleb(code, pointer);
	code.push_back(0x20);
	wasm::append_uleb(code, pointer);
	code.insert(code.end(), {0x45, 0x04, 0x40});
	append(code, failure_cleanup);
	append(code, module_.marker(location));
	code.insert(code.end(), {0x00, 0x0b});
	for (const auto [header, offset] : {std::pair<std::int32_t, unsigned>{1, BEARER_WASM_OBJECT_REFS_OFFSET}, {1, BEARER_WASM_OBJECT_OWNER_OFFSET}, {static_cast<std::int32_t>(type_id), BEARER_WASM_OBJECT_TYPE_OFFSET}})
		store_i32_constant(code, pointer, header, offset);
	code.push_back(0x20);
	wasm::append_uleb(code, pointer);
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x14, 0x6a, 0x36, 0x02, BEARER_WASM_OBJECT_HEADER_SIZE_OFFSET, 0x20});
	wasm::append_uleb(code, pointer);
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x36, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
	return {std::move(code), pointer};
}

Bytes FunctionLowerer::format_wide_scalar(Bytes code, const std::string& type, const Location&)
{
	code.push_back(0x10);
	wasm::append_uleb(code, module_.format_scalar_index(type));
	return code;
}

void FunctionLowerer::narrow_s64_index(Bytes& code, unsigned value, unsigned target, const Location& location, const Bytes& failure_cleanup, bool invalid_is_missing)
{
	if (invalid_is_missing)
	{
		code.push_back(0x20); wasm::append_uleb(code, value); code.insert(code.end(), {0x42, 0x00, 0x53, 0x20});
		wasm::append_uleb(code, value); code.insert(code.end(), {0x42, 0xff, 0xff, 0xff, 0xff, 0x07, 0x55, 0x72, 0x04, 0x40, 0x41, 0x7f, 0x21});
		wasm::append_uleb(code, target); code.push_back(0x05); code.push_back(0x20); wasm::append_uleb(code, value); code.insert(code.end(), {0xa7, 0x21});
		wasm::append_uleb(code, target); code.push_back(0x0b);
		return;
	}
	for (const Bytes& check : {
		Bytes{0x42, 0x00, 0x53},
		Bytes{0x42, 0xff, 0xff, 0xff, 0xff, 0x07, 0x55}})
	{
		code.push_back(0x20);
		wasm::append_uleb(code, value);
		append(code, check);
		code.insert(code.end(), {0x04, 0x40});
		append(code, failure_cleanup);
		append(code, module_.marker(location));
		code.insert(code.end(), {0x00, 0x0b});
	}
	code.push_back(0x20);
	wasm::append_uleb(code, value);
	code.insert(code.end(), {0xa7, 0x21});
	wasm::append_uleb(code, target);
}

std::pair<Bytes, std::string> FunctionLowerer::dval_lookup(Expr* value, Expr* key, bool require_present)
{
	auto [object_code, object_type] = expression(value);
	if (object_type != "dval")
		throw Error(value->location, "expected dval, found " + object_type);
	auto [key_code, key_type] = expression(key);
	if (key_type != "string" && key_type != "s64")
		throw Error(key->location, "dval index must be string or s64");
	const unsigned object = add_local("", "dval", value->location), key_local = add_local("", key_type, key->location),
				   index = key_type == "s64" ? add_local("", "s32", key->location) : key_local, length = add_local("", "s32", key->location);
	Bytes code = std::move(object_code);
	code.push_back(0x21);
	wasm::append_uleb(code, object);
	append(code, key_code);
	code.push_back(0x21);
	wasm::append_uleb(code, key_local);
	if (key_type == "s64")
		narrow_s64_index(code, key_local, index, key->location, {}, true);
	auto append_call = [&](bool output, unsigned pointer)
	{
		managed_payload_span(code, object, "dval");
		if (key_type == "string")
		{
			code.insert(code.end(), {0x41, 0x00, 0x20});
			wasm::append_uleb(code, key_local);
			code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
			wasm::append_uleb(code, key_local);
			code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x41, 0x00});
		}
		else
		{
			code.insert(code.end(), {0x41, 0x01, 0x41, 0x00, 0x41, 0x00, 0x20});
			wasm::append_uleb(code, index);
		}
		if (output)
		{
			managed_payload_pointer(code, pointer, "dval");
			code.push_back(0x20);
			wasm::append_uleb(code, length);
		}
		else
		{
			code.insert(code.end(), {0x41, 0x00, 0x41, 0x00});
		}
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index(require_present ? "bearer_dv_read_brrb" : "bearer_dv_get_brrb"));
	};
	append_call(false, 0);
	code.push_back(0x21);
	wasm::append_uleb(code, length);
	if (!require_present)
	{
		const unsigned result = add_local("", "bool", key->location);
		code.push_back(0x20);
		wasm::append_uleb(code, length);
		code.insert(code.end(), {0x41, 0x00, 0x4e, 0x21});
		wasm::append_uleb(code, result);
		if (expression_is_owned(key))
		{
			code.push_back(0x20);
			wasm::append_uleb(code, key_local);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.release_index());
		}
		if (expression_is_owned(value))
		{
			code.push_back(0x20);
			wasm::append_uleb(code, object);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.release_index());
		}
		code.push_back(0x20);
		wasm::append_uleb(code, result);
		return {code, "bool"};
	}
	code.push_back(0x20); wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
	if (expression_is_owned(key)) { code.push_back(0x20); wasm::append_uleb(code, key_local); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
	if (expression_is_owned(value)) { code.push_back(0x20); wasm::append_uleb(code, object); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
	append(code, module_.marker(key->location)); code.insert(code.end(), {0x00, 0x0b});
	Bytes allocation_cleanup;
	if (expression_is_owned(key)) { allocation_cleanup.push_back(0x20); wasm::append_uleb(allocation_cleanup, key_local); allocation_cleanup.push_back(0x10); wasm::append_uleb(allocation_cleanup, module_.release_index()); }
	if (expression_is_owned(value)) { allocation_cleanup.push_back(0x20); wasm::append_uleb(allocation_cleanup, object); allocation_cleanup.push_back(0x10); wasm::append_uleb(allocation_cleanup, module_.release_index()); }
	auto [allocation, pointer] = allocate_blob("dval", 4, length, key->location, allocation_cleanup);
	append(code, allocation);
	append_call(true, pointer);
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x47, 0x04, 0x40});
	code.push_back(0x20); wasm::append_uleb(code, pointer); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
	if (expression_is_owned(key)) { code.push_back(0x20); wasm::append_uleb(code, key_local); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
	if (expression_is_owned(value)) { code.push_back(0x20); wasm::append_uleb(code, object); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
	append(code, module_.marker(key->location)); code.insert(code.end(), {0x00, 0x0b});
	retain_dval_callables(code, pointer, key->location);
	if (expression_is_owned(key))
	{
		code.push_back(0x20);
		wasm::append_uleb(code, key_local);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.release_index());
	}
	if (expression_is_owned(value))
	{
		code.push_back(0x20);
		wasm::append_uleb(code, object);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.release_index());
	}
	code.push_back(0x20);
	wasm::append_uleb(code, pointer);
	return {code, "dval"};
}


std::pair<Bytes, std::string> FunctionLowerer::dval_presence(Expr* value)
{
	auto [source, type] = expression(value);
	if (type != "dval")
		throw Error(value->location, "postfix '?' requires dval");
	const unsigned input = add_local("", "dval", value->location);
	Bytes code = std::move(source);
	code.push_back(0x21);
	wasm::append_uleb(code, input);
	managed_payload_span(code, input, "dval");
	code.push_back(0x10);
	wasm::append_uleb(code, module_.import_index("bearer_dv_is_none_brrb"));
	const unsigned state = add_local("", "s32", value->location);
	code.push_back(0x21); wasm::append_uleb(code, state);
	code.push_back(0x20); wasm::append_uleb(code, state);
	code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
	if (expression_is_owned(value)) { code.push_back(0x20); wasm::append_uleb(code, input); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
	append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b, 0x20}); wasm::append_uleb(code, state);
	code.insert(code.end(), {0x45});
	if (expression_is_owned(value))
	{
		const unsigned result = add_local("", "bool", value->location);
		code.push_back(0x21);
		wasm::append_uleb(code, result);
		code.push_back(0x20);
		wasm::append_uleb(code, input);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.release_index());
		code.push_back(0x20);
		wasm::append_uleb(code, result);
	}
	return {code, "bool"};
}

Bytes FunctionLowerer::dval_replace(unsigned target, unsigned replacement, const Location& location, bool replacement_owned)
{
	const unsigned old_length = add_local("", "s32", location), old_capacity = add_local("", "s32", location), old_payload = add_local("", "s32", location);
	Bytes code{0x20}; wasm::append_uleb(code, target); code.push_back(0x20); wasm::append_uleb(code, replacement);
	code.insert(code.end(), {0x46, 0x04, 0x7f});
	if (replacement_owned) { code.push_back(0x20); wasm::append_uleb(code, replacement); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
	code.push_back(0x20); wasm::append_uleb(code, target); code.push_back(0x05);
	auto load = [&](unsigned object, unsigned offset, unsigned local)
	{
		code.push_back(0x20); wasm::append_uleb(code, object); code.insert(code.end(), {0x28, 0x02}); wasm::append_uleb(code, offset);
		code.push_back(0x21); wasm::append_uleb(code, local);
	};
	load(target, BEARER_WASM_OBJECT_LENGTH_OFFSET, old_length);
	load(target, BEARER_WASM_OBJECT_CAPACITY_OFFSET, old_capacity);
	load(target, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, old_payload);
	auto copy_field = [&](unsigned destination, unsigned source, unsigned offset)
	{
		code.push_back(0x20); wasm::append_uleb(code, destination); code.push_back(0x20); wasm::append_uleb(code, source);
		code.insert(code.end(), {0x28, 0x02}); wasm::append_uleb(code, offset); code.insert(code.end(), {0x36, 0x02}); wasm::append_uleb(code, offset);
	};
	copy_field(target, replacement, BEARER_WASM_OBJECT_LENGTH_OFFSET);
	copy_field(target, replacement, BEARER_WASM_OBJECT_CAPACITY_OFFSET);
	copy_field(target, replacement, BEARER_WASM_OBJECT_PAYLOAD_OFFSET);
	auto store = [&](unsigned offset, unsigned local)
	{
		code.push_back(0x20); wasm::append_uleb(code, replacement); code.push_back(0x20); wasm::append_uleb(code, local);
		code.insert(code.end(), {0x36, 0x02}); wasm::append_uleb(code, offset);
	};
	store(BEARER_WASM_OBJECT_LENGTH_OFFSET, old_length);
	store(BEARER_WASM_OBJECT_CAPACITY_OFFSET, old_capacity);
	store(BEARER_WASM_OBJECT_PAYLOAD_OFFSET, old_payload);
	code.push_back(0x20); wasm::append_uleb(code, replacement); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
	code.push_back(0x20); wasm::append_uleb(code, target); code.push_back(0x0b);
	return code;
}

std::pair<Bytes, std::string> FunctionLowerer::dval_set_path(unsigned root, const std::vector<Expr*>& selectors, Expr* replacement, const Location& location)
{
	std::vector<std::unique_ptr<Name>> selector_names;
	std::vector<bool> selector_owned;
	const unsigned snapshot = add_local("", "dval", location);
	Bytes code{0x20}; wasm::append_uleb(code, root); code.push_back(0x21); wasm::append_uleb(code, snapshot);
	code.push_back(0x20); wasm::append_uleb(code, snapshot); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index());
	for (Expr* selector : selectors)
	{
		auto [selector_code, selector_type] = expression(selector);
		if (selector_type != "string" && selector_type != "s64")
			throw Error(selector->location, "dval index must be string or s64");
		const unsigned local = add_local("", selector_type, selector->location);
		append(code, selector_code);
		code.push_back(0x21);
		wasm::append_uleb(code, local);
		unsigned path_local = local;
		std::string path_type = selector_type;
		if (selector_type == "s64")
		{
			path_local = add_local("", "s32", selector->location);
			narrow_s64_index(code, local, path_local, selector->location);
			path_type = "s32";
		}
		const std::string name = std::string(1, '\x1f') + "dval_path_" + std::to_string(path_local);
		scopes_.back()[name] = {path_local, path_type};
		selector_names.push_back(std::make_unique<Name>(selector->location, name));
		selector_owned.push_back(expression_is_owned(selector));
	}
	auto [replacement_code, replacement_type] = dval_value(replacement);
	const bool replacement_owned = infer(replacement) != "dval" || expression_is_owned(replacement);
	const unsigned replacement_local = add_local("", "dval", replacement->location);
	append(code, replacement_code);
	code.push_back(0x21);
	wasm::append_uleb(code, replacement_local);
	ArrayLiteral path(location);
	for (const auto& name : selector_names)
		path.items.push_back(name.get());
	auto [path_code, path_type] = dval_value(&path);
	const unsigned path_local = add_local("", "dval", location);
	append(code, path_code);
	code.push_back(0x21);
	wasm::append_uleb(code, path_local);
	for (std::size_t index = 0; index < selector_names.size(); ++index)
	{
		const auto found = scopes_.back().find(selector_names[index]->value);
		if (selector_owned[index])
		{
			code.push_back(0x20);
			wasm::append_uleb(code, found->second.first);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.release_index());
		}
		scopes_.back().erase(found);
	}
	const unsigned length = add_local("", "s32", location);
	auto append_call = [&](bool output, unsigned pointer)
	{
		for (unsigned input : {snapshot, path_local, replacement_local})
			managed_payload_span(code, input, "dval");
		if (output)
		{
			managed_payload_pointer(code, pointer, "dval");
			code.push_back(0x20); wasm::append_uleb(code, length);
		}
		else
			code.insert(code.end(), {0x41, 0x00, 0x41, 0x00});
		code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_dv_set_path_brrb"));
	};
	append_call(false, 0);
	code.push_back(0x21); wasm::append_uleb(code, length);
	code.push_back(0x20); wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
	code.push_back(0x20); wasm::append_uleb(code, path_local); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
	if (replacement_owned) { code.push_back(0x20); wasm::append_uleb(code, replacement_local); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
	code.push_back(0x20); wasm::append_uleb(code, snapshot); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
	append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b});
	Bytes allocation_cleanup;
	allocation_cleanup.push_back(0x20); wasm::append_uleb(allocation_cleanup, path_local);
	allocation_cleanup.push_back(0x10); wasm::append_uleb(allocation_cleanup, module_.release_index());
	if (replacement_owned) { allocation_cleanup.push_back(0x20); wasm::append_uleb(allocation_cleanup, replacement_local); allocation_cleanup.push_back(0x10); wasm::append_uleb(allocation_cleanup, module_.release_index()); }
	allocation_cleanup.push_back(0x20); wasm::append_uleb(allocation_cleanup, snapshot);
	allocation_cleanup.push_back(0x10); wasm::append_uleb(allocation_cleanup, module_.release_index());
	auto [allocation, result] = allocate_blob("dval", 4, length, location, allocation_cleanup);
	append(code, allocation);
	append_call(true, result);
	code.push_back(0x20); wasm::append_uleb(code, length);
	code.insert(code.end(), {0x47, 0x04, 0x40});
	code.push_back(0x20); wasm::append_uleb(code, result); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
	code.push_back(0x20); wasm::append_uleb(code, path_local); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
	if (replacement_owned) { code.push_back(0x20); wasm::append_uleb(code, replacement_local); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
	code.push_back(0x20); wasm::append_uleb(code, snapshot); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
	append(code, module_.marker(location));
	code.insert(code.end(), {0x00, 0x0b});
	retain_dval_callables(code, result, location);
	code.push_back(0x20); wasm::append_uleb(code, path_local); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
	if (replacement_owned) { code.push_back(0x20); wasm::append_uleb(code, replacement_local); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
	append(code, dval_replace(snapshot, result, location, true));
	return {code, "dval"};
}

std::pair<Bytes, std::string> FunctionLowerer::dval_value(Expr* value)
{
	if (!dynamic_cast<MapLiteral*>(value) && !dynamic_cast<ArrayLiteral*>(value))
	{
		const std::string type = infer(value);
		if (type == "dval")
			return expression(value);
		auto callable_value = [&](Bytes source, const std::string& actual) {
			const unsigned function_type = static_cast<unsigned>(std::stoul(actual.substr(9)));
			const unsigned closure = add_local("", actual, value->location), length = add_local("", "s32", value->location);
			Bytes code = std::move(source);
			code.push_back(0x21); wasm::append_uleb(code, closure);
			code.push_back(0x20); wasm::append_uleb(code, closure); code.push_back(0x41); wasm::append_sleb32(code, function_type);
			code.insert(code.end(), {0x41, 0x00, 0x41, 0x00, 0x10}); wasm::append_uleb(code, module_.import_index("bearer_dv_callable_brrb"));
			code.push_back(0x21); wasm::append_uleb(code, length);
			auto [allocation, pointer] = allocate_blob("dval", 4, length, value->location);
			append(code, allocation);
			code.push_back(0x20); wasm::append_uleb(code, closure); code.push_back(0x41); wasm::append_sleb32(code, function_type);
			managed_payload_pointer(code, pointer, "dval"); code.push_back(0x20); wasm::append_uleb(code, length);
			code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_dv_callable_brrb"));
			code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x47, 0x04, 0x40, 0x20}); wasm::append_uleb(code, pointer);
			code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b});
			retain_dval_callables(code, pointer, value->location);
			if (expression_is_owned(value)) { code.push_back(0x20); wasm::append_uleb(code, closure); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
			code.push_back(0x20); wasm::append_uleb(code, pointer);
			return std::pair{std::move(code), std::string("dval")};
		};
		auto [source, actual] = expression(value);
		if (actual.starts_with("function#"))
			return callable_value(std::move(source), actual);
		if (actual != "string" && !is_scalar(actual))
			throw Error(value->location, "cannot construct dval from " + actual);
		std::string scalar = actual == "s8" || actual == "s16" || actual == "u8" || actual == "u16" ? "s32" : actual == "u32" ? "u64" : actual == "f32" ? "f64" : actual;
		if (dynamic_cast<Integer*>(value) && (actual == "s64" || actual == "u64") && integer_fits(*static_cast<Integer*>(value), "s32")) scalar = "s32";
		const unsigned input = add_local("", scalar, value->location), length = add_local("", "s32", value->location);
		const char* import = scalar == "string" ? "bearer_dv_string_to_brrb"
							 : scalar == "s32"	? "bearer_dv_s32_to_brrb"
							 : scalar == "s64"	? "bearer_dv_s64_to_brrb"
							 : scalar == "u64"	? "bearer_dv_u64_to_brrb"
							 : scalar == "f64"	? "bearer_dv_f64_to_brrb"
												: "bearer_dv_bool_to_brrb";
		Bytes code = std::move(source);
		if (actual == "u32") code.push_back(0xad);
		else if ((actual == "s64" || actual == "u64") && scalar == "s32") code.push_back(0xa7);
		else if (actual == "f32") code.push_back(0xbb);
		code.push_back(0x21);
		wasm::append_uleb(code, input);
		code.push_back(0x20);
		wasm::append_uleb(code, input);
		if (scalar == "string")
		{
			code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
			wasm::append_uleb(code, input);
			code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET});
		}
		code.insert(code.end(), {0x41, 0x00, 0x41, 0x00, 0x10});
		wasm::append_uleb(code, module_.import_index(import));
		code.push_back(0x21);
		wasm::append_uleb(code, length);
		Bytes input_cleanup;
		if (scalar == "string" && expression_is_owned(value))
		{
			input_cleanup.push_back(0x20); wasm::append_uleb(input_cleanup, input);
			input_cleanup.push_back(0x10); wasm::append_uleb(input_cleanup, module_.release_index());
		}
		auto [allocation, pointer] = allocate_blob("dval", 4, length, value->location, input_cleanup);
		append(code, allocation);
		code.push_back(0x20);
		wasm::append_uleb(code, input);
		if (scalar == "string")
		{
			code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
			wasm::append_uleb(code, input);
			code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET});
		}
		managed_payload_pointer(code, pointer, "dval");
		code.push_back(0x20);
		wasm::append_uleb(code, length);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index(import));
		code.push_back(0x20);
		wasm::append_uleb(code, length);
		code.insert(code.end(), {0x47, 0x04, 0x40, 0x20}); wasm::append_uleb(code, pointer);
		code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
		append(code, input_cleanup); append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b});
		append(code, input_cleanup);
		code.push_back(0x20);
		wasm::append_uleb(code, pointer);
		return {code, "dval"};
	}
	std::vector<std::pair<std::string, Expr*>> entries;
	bool list = false;
	if (auto map = dynamic_cast<MapLiteral*>(value))
		entries = map->entries;
	else
	{
		list = true;
		auto array = static_cast<ArrayLiteral*>(value);
		for (unsigned i = 0; i < array->items.size(); ++i)
			entries.push_back({std::to_string(i), array->items[i]});
	}
	std::set<std::string> keys;
	for (const auto& [key, item] : entries)
		if (!list && !keys.insert(key).second)
			throw Error(value->location, "dval map literal contains a duplicate key");
	Bytes code;
	std::vector<std::pair<unsigned, bool>> values;
	for (const auto& [key, item] : entries)
	{
		auto [part, type] = dval_value(item);
		const unsigned local = add_local("", "dval", item->location);
		append(code, part);
		code.push_back(0x21);
		wasm::append_uleb(code, local);
		values.push_back({local, expression_is_owned(item) || infer(item) != "dval"});
	}
	const unsigned descriptor = add_local("", "s32", value->location);
	const unsigned size = static_cast<unsigned>(entries.size() * 16);
	auto release_values = [&](Bytes& target)
	{
		for (auto it = values.rbegin(); it != values.rend(); ++it)
			if (it->second)
			{
				target.push_back(0x20); wasm::append_uleb(target, it->first);
				target.push_back(0x10); wasm::append_uleb(target, module_.release_index());
			}
	};
	if (size)
	{
		code.push_back(0x41);
		wasm::append_sleb32(code, size);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index("bearer_alloc"));
		code.push_back(0x22);
		wasm::append_uleb(code, descriptor);
		code.insert(code.end(), {0x45, 0x04, 0x40}); release_values(code);
		append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b});
	}
	else
	{
		code.insert(code.end(), {0x41, 0x00, 0x21});
		wasm::append_uleb(code, descriptor);
	}
	for (unsigned i = 0; i < entries.size(); ++i)
	{
		const unsigned offset = module_.add_data(entries[i].first);
		const unsigned base = i * 16;
		code.push_back(0x20);
		wasm::append_uleb(code, descriptor);
		code.insert(code.end(), {0x23, 0x00, 0x41});
		wasm::append_sleb32(code, offset);
		code.insert(code.end(), {0x6a, 0x36, 0x02});
		wasm::append_uleb(code, base);
		code.push_back(0x20);
		wasm::append_uleb(code, descriptor);
		code.push_back(0x41);
		wasm::append_sleb32(code, static_cast<std::int32_t>(entries[i].first.size()));
		code.insert(code.end(), {0x36, 0x02});
		wasm::append_uleb(code, base + 4);
		code.push_back(0x20);
		wasm::append_uleb(code, descriptor);
		managed_payload_pointer(code, values[i].first, "dval");
		code.insert(code.end(), {0x36, 0x02});
		wasm::append_uleb(code, base + 8);
		code.push_back(0x20);
		wasm::append_uleb(code, descriptor);
		managed_payload_length(code, values[i].first);
		code.insert(code.end(), {0x36, 0x02});
		wasm::append_uleb(code, base + 12);
	}
	const unsigned length = add_local("", "s32", value->location);
	code.push_back(0x41);
	wasm::append_sleb32(code, list);
	code.push_back(0x20);
	wasm::append_uleb(code, descriptor);
	code.push_back(0x41);
	wasm::append_sleb32(code, entries.size());
	code.insert(code.end(), {0x41, 0x00, 0x41, 0x00, 0x10});
	wasm::append_uleb(code, module_.import_index("bearer_dv_build_brrb"));
	code.push_back(0x21);
	wasm::append_uleb(code, length);
	Bytes build_cleanup;
	if (size)
	{
		build_cleanup.push_back(0x20); wasm::append_uleb(build_cleanup, descriptor);
		build_cleanup.push_back(0x10); wasm::append_uleb(build_cleanup, module_.import_index("bearer_free"));
	}
	release_values(build_cleanup);
	auto [allocation, pointer] = allocate_blob("dval", 4, length, value->location, build_cleanup);
	append(code, allocation);
	code.push_back(0x41);
	wasm::append_sleb32(code, list);
	code.push_back(0x20);
	wasm::append_uleb(code, descriptor);
	code.push_back(0x41);
	wasm::append_sleb32(code, entries.size());
	managed_payload_pointer(code, pointer, "dval");
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.push_back(0x10);
	wasm::append_uleb(code, module_.import_index("bearer_dv_build_brrb"));
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x47, 0x04, 0x40, 0x20}); wasm::append_uleb(code, pointer);
	code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
	append(code, build_cleanup); append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b});
	retain_dval_callables(code, pointer, value->location);
	append(code, build_cleanup);
	code.push_back(0x20);
	wasm::append_uleb(code, pointer);
	return {code, "dval"};
}

std::pair<Bytes, unsigned> FunctionLowerer::allocate_array(const std::string& array_type, unsigned length, const Location& location, const Bytes& failure_cleanup)
{
	const std::string element = array_type.substr(6, array_type.size() - 7);
	const unsigned element_size = array_element_size(element);
	const unsigned bytes = add_local("", "s32", location), backing = add_local("", "s32", location);
	const unsigned handle = add_local("", array_type, location);
	Bytes code{0x20}; wasm::append_uleb(code, length);
	code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(std::numeric_limits<std::int32_t>::max() / element_size));
	code.insert(code.end(), {0x4b, 0x04, 0x40}); append(code, failure_cleanup); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b});
	code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x45, 0x04, 0x40, 0x05, 0x20}); wasm::append_uleb(code, length);
	code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x22}); wasm::append_uleb(code, bytes);
	code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_alloc")); code.push_back(0x22); wasm::append_uleb(code, backing);
	code.insert(code.end(), {0x45, 0x04, 0x40}); append(code, failure_cleanup); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b, 0x0b});
	code.push_back(0x41); wasm::append_sleb32(code, BEARER_WASM_OBJECT_HANDLE_SIZE); code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_alloc"));
	code.push_back(0x22); wasm::append_uleb(code, handle); code.insert(code.end(), {0x45, 0x04, 0x40, 0x20}); wasm::append_uleb(code, backing);
	code.insert(code.end(), {0x45, 0x04, 0x40, 0x05, 0x20}); wasm::append_uleb(code, backing); code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_free")); code.push_back(0x0b);
	append(code, failure_cleanup); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b});
	auto store = [&](unsigned offset, std::optional<std::int32_t> constant, unsigned local)
	{
		code.push_back(0x20); wasm::append_uleb(code, handle);
		if (constant) { code.push_back(0x41); wasm::append_sleb32(code, *constant); } else { code.push_back(0x20); wasm::append_uleb(code, local); }
		code.insert(code.end(), {0x36, 0x02}); wasm::append_uleb(code, offset);
	};
	store(BEARER_WASM_OBJECT_REFS_OFFSET, 1, 0);
	store(BEARER_WASM_OBJECT_OWNER_OFFSET, 1, 0);
	store(BEARER_WASM_OBJECT_TYPE_OFFSET, managed_type(element) ? 3 : 2, 0);
	store(BEARER_WASM_OBJECT_HEADER_SIZE_OFFSET, BEARER_WASM_OBJECT_HANDLE_SIZE, 0);
	store(BEARER_WASM_OBJECT_LENGTH_OFFSET, std::nullopt, length);
	store(BEARER_WASM_OBJECT_CAPACITY_OFFSET, std::nullopt, length);
	store(BEARER_WASM_OBJECT_PAYLOAD_OFFSET, std::nullopt, backing);
	code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
	return {code, handle};
}

std::pair<Bytes, std::string> FunctionLowerer::array_method(Call* call, const Member* member)
{
	const std::string array_type = infer(member->value);
	const std::string element = array_type.substr(6, array_type.size() - 7);
	const unsigned element_size = array_element_size(element);
	if (member->member == "capacity")
	{
		if (!call->arguments.empty()) throw Error(call->location, "array capacity expects no arguments");
		auto [code, type] = expression(member->value);
		const unsigned array = add_local("", type, member->value->location), result = add_local("", "s32", call->location);
		code.push_back(0x21); wasm::append_uleb(code, array);
		code.push_back(0x20); wasm::append_uleb(code, array); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_CAPACITY_OFFSET, 0x21}); wasm::append_uleb(code, result);
		if (expression_is_owned(member->value)) { code.push_back(0x20); wasm::append_uleb(code, array); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
		code.push_back(0x20); wasm::append_uleb(code, result); code.push_back(0xad);
		return {code, "s64"};
	}
	infer(call);
	auto receiver = expression(member->value);
	if (receiver.second != array_type)
		throw Error(member->value->location, "array receiver type changed during mutation");
	const bool owned_receiver = expression_is_owned(member->value);
	const bool stable_borrow = !owned_receiver && dynamic_cast<Name*>(member->value);
	const unsigned slot = add_local("", array_type, member->value->location);
	Bytes receiver_code = std::move(receiver.first); receiver_code.push_back(0x21); wasm::append_uleb(receiver_code, slot);
	if (!owned_receiver && !stable_borrow) { receiver_code.push_back(0x20); wasm::append_uleb(receiver_code, slot); receiver_code.push_back(0x10); wasm::append_uleb(receiver_code, module_.retain_index()); }
	auto release_receiver = [&](Bytes& code)
	{
		if (!owned_receiver && stable_borrow) return;
		code.push_back(0x20); wasm::append_uleb(code, slot); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
	};
	Bytes receiver_cleanup; release_receiver(receiver_cleanup);
	auto address = [&](Bytes& code, unsigned array, unsigned index)
	{
		code.push_back(0x20); wasm::append_uleb(code, array);
		code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET});
		code.push_back(0x20); wasm::append_uleb(code, index);
		code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
		code.insert(code.end(), {0x6c, 0x6a});
	};
	auto load_length = [&](Bytes& code, unsigned target)
	{
		code.push_back(0x20); wasm::append_uleb(code, slot);
		code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21}); wasm::append_uleb(code, target);
	};
	auto store_length = [&](Bytes& code, unsigned source)
	{
		code.push_back(0x20); wasm::append_uleb(code, slot);
		code.push_back(0x20); wasm::append_uleb(code, source);
		code.insert(code.end(), {0x36, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET});
	};
	if (member->member == "push")
	{
		auto compiled = dynamic_cast<Integer*>(call->arguments[0]) && integer_type(element)
			? integer_expression(static_cast<Integer*>(call->arguments[0]), element) : expression(call->arguments[0]);
		auto value_code = std::move(compiled.first);
		const std::string value_type = std::move(compiled.second);
		const unsigned item = add_local("", element, call->arguments[0]->location), length = add_local("", "s32", call->location), required = add_local("", "s32", call->location);
		Bytes code = receiver_code; append(code, value_code); code.push_back(0x21); wasm::append_uleb(code, item); load_length(code, length);
		Bytes failure_cleanup = receiver_cleanup;
		if (managed_type(element) && expression_is_owned(call->arguments[0])) { failure_cleanup.push_back(0x20); wasm::append_uleb(failure_cleanup, item); failure_cleanup.push_back(0x10); wasm::append_uleb(failure_cleanup, module_.release_index()); }
		code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x41, 0x01, 0x6a, 0x22}); wasm::append_uleb(code, required);
		code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4d, 0x04, 0x40}); append(code, failure_cleanup); append(code, module_.marker(call->location)); code.insert(code.end(), {0x00, 0x0b});
		append(code, array_ensure_capacity(slot, array_type, required, call->location, failure_cleanup));
		if (managed_type(element) && !expression_is_owned(call->arguments[0])) { code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
		address(code, slot, length); code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(array_store_opcode(element)); code.push_back(element_size == 8 ? 3 : 2); code.push_back(0x00);
		store_length(code, required); release_receiver(code);
		return {code, "void"};
	}
	if (member->member == "pop")
	{
		const unsigned length = add_local("", "s32", call->location), index = add_local("", "s32", call->location), result = add_local("", element, call->location);
		Bytes code = receiver_code; load_length(code, length); code.push_back(0x20); wasm::append_uleb(code, length);
		code.insert(code.end(), {0x45, 0x04, 0x40}); append(code, receiver_cleanup); append(code, module_.marker(call->location)); code.insert(code.end(), {0x00, 0x0b});
		code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x41, 0x01, 0x6b, 0x21}); wasm::append_uleb(code, index);
		address(code, slot, index); code.push_back(array_load_opcode(element)); code.push_back(element_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x21}); wasm::append_uleb(code, result);
		store_length(code, index); release_receiver(code); code.push_back(0x20); wasm::append_uleb(code, result);
		return {code, element};
	}
	if (member->member == "reserve")
	{
		auto [required_code, required_type] = expression(call->arguments[0]);
		const unsigned requested = add_local("", "s64", call->arguments[0]->location), required = add_local("", "s32", call->arguments[0]->location), capacity = add_local("", "s32", call->location);
		Bytes code = receiver_code; append(code, required_code); code.push_back(0x21); wasm::append_uleb(code, requested); narrow_s64_index(code, requested, required, call->arguments[0]->location, receiver_cleanup);
		code.push_back(0x20); wasm::append_uleb(code, slot); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_CAPACITY_OFFSET, 0x21}); wasm::append_uleb(code, capacity);
		code.push_back(0x20); wasm::append_uleb(code, required); code.push_back(0x20); wasm::append_uleb(code, capacity); code.insert(code.end(), {0x4b, 0x04, 0x40});
		append(code, array_ensure_capacity(slot, array_type, required, call->location, receiver_cleanup)); code.push_back(0x0b); release_receiver(code);
		return {code, "void"};
	}
	if (member->member == "clear")
	{
		const unsigned length = add_local("", "s32", call->location), index = add_local("", "s32", call->location);
		Bytes code = receiver_code; load_length(code, length);
		if (managed_type(element))
		{
			code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, index);
			code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, index); code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4f, 0x0d, 0x01});
			address(code, slot, index); code.insert(code.end(), {0x28, 0x02, 0x00, 0x10}); wasm::append_uleb(code, module_.release_index());
			code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, index); code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
		}
		code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, length); store_length(code, length); release_receiver(code);
		return {code, "void"};
	}
	if (member->member == "insert" || member->member == "remove")
	{
		auto [index_code, index_type] = expression(call->arguments[0]);
		const unsigned requested = add_local("", "s64", call->arguments[0]->location), index = add_local("", "s32", call->arguments[0]->location), length = add_local("", "s32", call->location), required = add_local("", "s32", call->location);
		const unsigned item = add_local("", element, call->location);
		Bytes code = receiver_code; append(code, index_code); code.push_back(0x21); wasm::append_uleb(code, requested); narrow_s64_index(code, requested, index, call->arguments[0]->location, receiver_cleanup);
		bool insert = member->member == "insert";
		bool item_owned = false;
		if (insert)
		{
			auto value = dynamic_cast<Integer*>(call->arguments[1]) && integer_type(element)
				? integer_expression(static_cast<Integer*>(call->arguments[1]), element) : expression(call->arguments[1]);
			append(code, value.first); code.push_back(0x21); wasm::append_uleb(code, item); item_owned = expression_is_owned(call->arguments[1]);
		}
		Bytes failure_cleanup = receiver_cleanup;
		if (insert && managed_type(element) && item_owned) { failure_cleanup.push_back(0x20); wasm::append_uleb(failure_cleanup, item); failure_cleanup.push_back(0x10); wasm::append_uleb(failure_cleanup, module_.release_index()); }
		load_length(code, length); code.push_back(0x20); wasm::append_uleb(code, index); code.push_back(0x20); wasm::append_uleb(code, length); code.push_back(insert ? 0x4b : 0x4f);
		code.insert(code.end(), {0x04, 0x40}); append(code, failure_cleanup); append(code, module_.marker(call->arguments[0]->location)); code.insert(code.end(), {0x00, 0x0b});
		if (insert) { code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, required); }
		else { code.push_back(0x20); wasm::append_uleb(code, length); code.push_back(0x21); wasm::append_uleb(code, required); }
		if (insert) append(code, array_ensure_capacity(slot, array_type, required, call->location, failure_cleanup));
		if (!insert)
		{
			address(code, slot, index); code.push_back(array_load_opcode(element)); code.push_back(element_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x21}); wasm::append_uleb(code, item);
		}
		code.push_back(0x20); wasm::append_uleb(code, slot); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x20); wasm::append_uleb(code, index);
		if (insert) code.insert(code.end(), {0x41, 0x01, 0x6a});
		code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a});
		code.push_back(0x20); wasm::append_uleb(code, slot); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x20); wasm::append_uleb(code, index);
		if (!insert) code.insert(code.end(), {0x41, 0x01, 0x6a});
		code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a, 0x20}); wasm::append_uleb(code, length); code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x6b});
		if (!insert) code.insert(code.end(), {0x41, 0x01, 0x6b});
		code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0xfc, 0x0a, 0x00, 0x00});
		if (insert)
		{
			if (managed_type(element) && !item_owned) { code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
			address(code, slot, index); code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(array_store_opcode(element)); code.push_back(element_size == 8 ? 3 : 2); code.push_back(0x00); store_length(code, required); release_receiver(code);
			return {code, "void"};
		}
		code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x41, 0x01, 0x6b, 0x21}); wasm::append_uleb(code, required); store_length(code, required); release_receiver(code); code.push_back(0x20); wasm::append_uleb(code, item);
		return {code, element};
	}
	if (member->member == "resize")
	{
		auto [size_code, size_type] = expression(call->arguments[0]);
		auto fill_result = dynamic_cast<Integer*>(call->arguments[1]) && integer_type(element)
			? integer_expression(static_cast<Integer*>(call->arguments[1]), element) : expression(call->arguments[1]);
		auto fill_code = std::move(fill_result.first);
		const std::string fill_type = std::move(fill_result.second);
		const unsigned requested = add_local("", "s64", call->arguments[0]->location), desired = add_local("", "s32", call->arguments[0]->location), fill = add_local("", element, call->arguments[1]->location);
		const unsigned length = add_local("", "s32", call->location), index = add_local("", "s32", call->location);
		Bytes code = receiver_code; append(code, size_code); code.push_back(0x21); wasm::append_uleb(code, requested); append(code, fill_code); code.push_back(0x21); wasm::append_uleb(code, fill);
		Bytes failure_cleanup = receiver_cleanup;
		if (managed_type(element) && expression_is_owned(call->arguments[1])) { failure_cleanup.push_back(0x20); wasm::append_uleb(failure_cleanup, fill); failure_cleanup.push_back(0x10); wasm::append_uleb(failure_cleanup, module_.release_index()); }
		narrow_s64_index(code, requested, desired, call->arguments[0]->location, failure_cleanup); load_length(code, length);
		code.push_back(0x20); wasm::append_uleb(code, desired); code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4b, 0x04, 0x40}); append(code, array_ensure_capacity(slot, array_type, desired, call->location, failure_cleanup)); code.push_back(0x0b);
		code.push_back(0x20); wasm::append_uleb(code, desired); code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x49, 0x04, 0x40});
		if (managed_type(element))
		{
			code.push_back(0x20); wasm::append_uleb(code, desired); code.push_back(0x21); wasm::append_uleb(code, index);
			code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, index); code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4f, 0x0d, 0x01});
			address(code, slot, index); code.insert(code.end(), {0x28, 0x02, 0x00, 0x10}); wasm::append_uleb(code, module_.release_index());
			code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, index); code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
		}
		code.push_back(0x05); code.push_back(0x20); wasm::append_uleb(code, length); code.push_back(0x21); wasm::append_uleb(code, index);
		code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, index); code.push_back(0x20); wasm::append_uleb(code, desired); code.insert(code.end(), {0x4f, 0x0d, 0x01});
		if (managed_type(element)) { code.push_back(0x20); wasm::append_uleb(code, fill); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
		address(code, slot, index); code.push_back(0x20); wasm::append_uleb(code, fill); code.push_back(array_store_opcode(element)); code.push_back(element_size == 8 ? 3 : 2); code.push_back(0x00);
		code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, index); code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b, 0x0b});
		store_length(code, desired);
		if (managed_type(element) && expression_is_owned(call->arguments[1])) { code.push_back(0x20); wasm::append_uleb(code, fill); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
		release_receiver(code);
		return {code, "void"};
	}
	throw Error(call->location, "unknown array method '" + member->member + "'");
}

Bytes FunctionLowerer::array_ensure_capacity(unsigned slot, const std::string& array_type, unsigned required, const Location& location, const Bytes& failure_cleanup)
{
	const unsigned element_size = array_element_size(array_type.substr(6, array_type.size() - 7));
	const unsigned length = add_local("", "s32", location), capacity = add_local("", "s32", location);
	const unsigned new_capacity = add_local("", "s32", location), bytes = add_local("", "s32", location);
	const unsigned old_backing = add_local("", "s32", location), new_backing = add_local("", "s32", location);
	Bytes code{0x20};
	wasm::append_uleb(code, slot); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21}); wasm::append_uleb(code, length);
	code.push_back(0x20); wasm::append_uleb(code, slot); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_CAPACITY_OFFSET, 0x21}); wasm::append_uleb(code, capacity);
	code.push_back(0x20); wasm::append_uleb(code, required); code.push_back(0x20); wasm::append_uleb(code, capacity); code.insert(code.end(), {0x4b, 0x04, 0x40});
	code.push_back(0x20); wasm::append_uleb(code, capacity); code.insert(code.end(), {0x41, 0x02, 0x6c, 0x21}); wasm::append_uleb(code, new_capacity);
	code.push_back(0x20); wasm::append_uleb(code, required); code.push_back(0x20); wasm::append_uleb(code, new_capacity); code.insert(code.end(), {0x4b, 0x04, 0x40, 0x20}); wasm::append_uleb(code, required); code.push_back(0x21); wasm::append_uleb(code, new_capacity); code.push_back(0x0b);
	code.push_back(0x20); wasm::append_uleb(code, new_capacity); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(std::numeric_limits<std::int32_t>::max() / element_size)); code.insert(code.end(), {0x4b, 0x04, 0x40}); append(code, failure_cleanup); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b});
	code.push_back(0x20); wasm::append_uleb(code, new_capacity); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x22}); wasm::append_uleb(code, bytes);
	code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_alloc")); code.push_back(0x22); wasm::append_uleb(code, new_backing); code.insert(code.end(), {0x45, 0x04, 0x40}); append(code, failure_cleanup); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b});
	code.push_back(0x20); wasm::append_uleb(code, slot); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x21}); wasm::append_uleb(code, old_backing);
	code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x45, 0x04, 0x40, 0x05, 0x20}); wasm::append_uleb(code, new_backing);
	code.push_back(0x20); wasm::append_uleb(code, old_backing); code.push_back(0x20); wasm::append_uleb(code, length); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0xfc, 0x0a, 0x00, 0x00, 0x0b});
	code.push_back(0x20); wasm::append_uleb(code, slot); code.push_back(0x20); wasm::append_uleb(code, new_backing); code.insert(code.end(), {0x36, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET});
	code.push_back(0x20); wasm::append_uleb(code, slot); code.push_back(0x20); wasm::append_uleb(code, new_capacity); code.insert(code.end(), {0x36, 0x02, BEARER_WASM_OBJECT_CAPACITY_OFFSET});
	code.push_back(0x20); wasm::append_uleb(code, old_backing); code.insert(code.end(), {0x45, 0x04, 0x40, 0x05, 0x20}); wasm::append_uleb(code, old_backing); code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_free")); code.insert(code.end(), {0x0b, 0x0b});
	return code;
}

std::pair<Bytes, std::string> FunctionLowerer::conversion(Bytes code, const std::string& source, const std::string& target,
														 const Location& location, bool source_owned)
{
	if (!can_convert(source, target))
		throw Error(location, "no explicit conversion from " + source + " to " + target);
	if (source == target || (source == "bool" && target == "s32"))
		return {std::move(code), target};
	if (source == "dval" && target.rfind("function#", 0) == 0)
	{
		const unsigned input = add_local("", "dval", location), result = add_local("", target, location);
		const unsigned function_type = static_cast<unsigned>(std::stoul(target.substr(9)));
		code.push_back(0x21); wasm::append_uleb(code, input);
		managed_payload_pointer(code, input, "dval"); managed_payload_length(code, input);
		code.push_back(0x41); wasm::append_sleb32(code, function_type); code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index("bearer_dv_callable_extract_brrb")); code.push_back(0x22); wasm::append_uleb(code, result);
		code.insert(code.end(), {0x45, 0x04, 0x40}); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b, 0x20}); wasm::append_uleb(code, result);
		code.push_back(0x10); wasm::append_uleb(code, module_.retain_index());
		if (source_owned) { code.push_back(0x20); wasm::append_uleb(code, input); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
		code.push_back(0x20); wasm::append_uleb(code, result);
		return {std::move(code), target};
	}
	if (source == "dval")
	{
		const unsigned input = add_local("", "dval", location);
		code.push_back(0x21);
		wasm::append_uleb(code, input);
		const std::string input_name = std::string(1, '\x1f') + "dval_conversion_" + std::to_string(input);
		scopes_.back()[input_name] = {input, "dval"};
		Name input_value(location, input_name), host_name(location, "__bearer_dv_extract_" + target);
		std::unique_ptr<Expr> fallback;
		if (target == "string") fallback = std::make_unique<String>(location, "");
		else if (target == "bool") fallback = std::make_unique<Name>(location, "false");
		else if (target == "s32") fallback = std::make_unique<Integer>(location, 0);
		else if (target == "s64") fallback = std::make_unique<SignedInteger>(location, 0);
		else if (target == "u64") fallback = std::make_unique<UnsignedInteger>(location, 0);
		else fallback = std::make_unique<Float>(location, 0.0);
		Call extraction(location, &host_name);
		extraction.arguments = {&input_value, fallback.get()};
		auto converted = expression(&extraction);
		scopes_.back().erase(input_name);
		append(code, converted.first);
		if (source_owned)
		{
			const unsigned result = add_local("", target, location);
			code.push_back(0x21); wasm::append_uleb(code, result);
			code.push_back(0x20); wasm::append_uleb(code, input);
			code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
			code.push_back(0x20); wasm::append_uleb(code, result);
		}
		return {std::move(code), target};
	}
	if (target == "string")
	{
		if (source == "bool")
		{
			const unsigned value = add_local("", "bool", location);
			code.push_back(0x21);
			wasm::append_uleb(code, value);
			const unsigned true_offset = module_.add_static_string("true"), false_offset = module_.add_static_string("false");
			code.push_back(0x20);
			wasm::append_uleb(code, value);
			code.insert(code.end(), {0x04, 0x7f, 0x23, 0x00, 0x41});
			wasm::append_sleb32(code, static_cast<std::int32_t>(true_offset));
			code.insert(code.end(), {0x6a, 0x05, 0x23, 0x00, 0x41});
			wasm::append_sleb32(code, static_cast<std::int32_t>(false_offset));
			code.insert(code.end(), {0x6a, 0x0b});
			return {std::move(code), "string"};
		}
		if (source == "s8" || source == "s16" || source == "s32")
		{
			code.push_back(0xac);
			return {format_wide_scalar(std::move(code), "s64", location), "string"};
		}
		if (source == "u8" || source == "u16" || source == "u32")
		{
			code.push_back(0xad);
			return {format_wide_scalar(std::move(code), "u64", location), "string"};
		}
		if (source == "f32")
		{
			code.push_back(0xbb);
			return {format_wide_scalar(std::move(code), "f64", location), "string"};
		}
		return {format_wide_scalar(std::move(code), source, location), "string"};
	}
	if (target == "bool")
	{
		if (source == "s8" || source == "s16" || source == "s32" || source == "u8" || source == "u16" || source == "u32")
			code.insert(code.end(), {0x45, 0x45});
		else if (source == "s64" || source == "u64")
			code.insert(code.end(), {0x50, 0x45});
		else if (source == "f32")
			code.insert(code.end(), {0x43, 0x00, 0x00, 0x00, 0x00, 0x5c});
		else
		{
			code.push_back(0x44);
			wasm::append_f64(code, 0.0);
			code.push_back(0x62);
		}
		return {std::move(code), "bool"};
	}
	const bool source_i32 = source == "bool" || source == "s8" || source == "s16" || source == "s32" || source == "u8" || source == "u16" || source == "u32";
	const bool target_i32 = target == "s8" || target == "s16" || target == "s32" || target == "u8" || target == "u16" || target == "u32";
	const bool source_unsigned = source == "u8" || source == "u16" || source == "u32" || source == "u64";
	const bool target_unsigned = target == "u8" || target == "u16" || target == "u32" || target == "u64";
	if (source_i32)
	{
		if (target == "s64") code.push_back(source_unsigned ? 0xad : 0xac);
		else if (target == "u64") code.push_back(source_unsigned || source == "bool" ? 0xad : 0xac);
		else if (target == "f32") code.push_back(source_unsigned ? 0xb3 : 0xb2);
		else if (target == "f64") code.push_back(source_unsigned ? 0xb8 : 0xb7);
	}
	else if (source == "s64" || source == "u64")
	{
		if (target_i32) code.push_back(0xa7);
		else if (target == "f32") code.push_back(source == "u64" ? 0xb5 : 0xb4);
		else if (target == "f64") code.push_back(source == "u64" ? 0xba : 0xb9);
	}
	else if (source == "f32")
	{
		append(code, module_.marker(location));
		if (target_i32) code.push_back(target_unsigned ? 0xa9 : 0xa8);
		else if (target == "s64") code.push_back(0xae);
		else if (target == "u64") code.push_back(0xaf);
		else if (target == "f64") code.push_back(0xbb);
	}
	else if (source == "f64")
	{
		append(code, module_.marker(location));
		if (target_i32) code.push_back(target_unsigned ? 0xab : 0xaa);
		else if (target == "s64") code.push_back(0xb0);
		else if (target == "u64") code.push_back(0xb1);
		else if (target == "f32") code.push_back(0xb6);
	}
	return {std::move(code), target};
}

std::pair<Bytes, std::string> FunctionLowerer::expression(Expr* value, bool value_required)
{
	module_.check_cancelled();
	if (auto integer = dynamic_cast<Integer*>(value))
		return integer_expression(integer);
	if (auto integer = dynamic_cast<UnsignedInteger*>(value))
	{
		Bytes code{0x42};
		wasm::append_sleb64(code, std::bit_cast<std::int64_t>(integer->value));
		return {code, "u64"};
	}
	if (auto integer = dynamic_cast<SignedInteger*>(value))
	{
		Bytes code{0x42};
		wasm::append_sleb64(code, integer->value);
		return {code, "s64"};
	}
	if (auto floating = dynamic_cast<Float*>(value))
		return float_expression(floating);
	if (auto string = dynamic_cast<String*>(value))
	{
		unsigned offset = module_.add_static_string(string->value);
		Bytes code{0x23, 0x00, 0x41};
		wasm::append_sleb32(code, static_cast<std::int32_t>(offset));
		code.push_back(0x6a);
		return {code, "string"};
	}
	if (auto lambda = dynamic_cast<Lambda*>(value))
	{
		auto [type, slot, type_id, definition, captures] = register_lambda(lambda);
		if (captures.empty())
		{
			const unsigned offset = module_.add_static_closure(slot, static_cast<unsigned>(std::stoul(type.substr(9))));
			Bytes code{0x23, 0x00, 0x41};
			wasm::append_sleb32(code, static_cast<std::int32_t>(offset));
			code.push_back(0x6a);
			return {code, type};
		}
		std::vector<std::string> capture_types;
		for (const auto& [name, capture_type] : captures)
			capture_types.push_back(capture_type);
		const AggregateLayout layout = aggregate_layout(capture_types, BEARER_WASM_OBJECT_CLOSURE_CAPTURES_OFFSET);
		const unsigned pointer = add_local("", type, value->location), size = layout.size;
		Bytes code{0x41};
		wasm::append_sleb32(code, static_cast<std::int32_t>(size));
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index("bearer_alloc"));
		code.push_back(0x21);
		wasm::append_uleb(code, pointer);
		code.push_back(0x20);
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x45, 0x04, 0x40});
		append(code, module_.marker(value->location));
		code.insert(code.end(), {0x00, 0x0b});
		for (const auto [header, offset] : {std::pair<std::int32_t, unsigned>{1, 0},
											{1, 4},
											{static_cast<std::int32_t>(type_id), 8},
											{static_cast<std::int32_t>(size), 12},
											{static_cast<std::int32_t>(slot), 16},
											{static_cast<std::int32_t>(std::stoul(type.substr(9))), 20}})
			store_i32_constant(code, pointer, header, offset);
		code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
		for (std::size_t i = 0; i < captures.size(); ++i)
		{
			Name name(value->location, captures[i].first);
			auto [local, actual] = lookup(&name);
			if (actual != captures[i].second)
				throw Error(value->location, "captured local type changed while lowering");
			if (managed_type(actual))
			{
				code.push_back(0x20);
				wasm::append_uleb(code, local);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.retain_index());
			}
			code.push_back(0x20);
			wasm::append_uleb(code, pointer);
			code.push_back(0x20);
			wasm::append_uleb(code, local);
			store_field(code, actual, layout.offsets[i]);
		}
		code.push_back(0x20);
		wasm::append_uleb(code, pointer);
		return {code, type};
	}
	if (auto map = dynamic_cast<MapLiteral*>(value))
		return dval_value(map);
	if (auto array = dynamic_cast<ArrayLiteral*>(value))
	{
		if (array->items.empty() && !array->explicit_element_type)
			throw Error(value->location, "empty array literal needs an explicit element type");
		std::vector<std::pair<Bytes, bool>> items;
		std::string element_type = array->explicit_element_type ? module_.value_type(array->explicit_element_type) : "";
		const bool has_spread = std::any_of(array->items.begin(), array->items.end(), [](Expr* item) { return dynamic_cast<Spread*>(item); });
		if (has_spread)
		{
			struct Part { bool spread; unsigned local; unsigned length; bool owned; std::string source_element; };
			std::vector<Part> parts;
			auto release_parts = [&](Bytes& target)
			{
				for (auto it = parts.rbegin(); it != parts.rend(); ++it)
					if (it->spread || (it->owned && managed_type(it->source_element)))
					{
						target.push_back(0x20); wasm::append_uleb(target, it->local);
						target.push_back(0x10); wasm::append_uleb(target, module_.release_index());
					}
			};
			const unsigned total = add_local("", "s32", value->location), previous = add_local("", "s32", value->location);
			Bytes code{0x41, 0x00, 0x21}; wasm::append_uleb(code, total);
			for (Expr* item : array->items)
			{
				if (auto spread = dynamic_cast<Spread*>(item))
				{
					auto compiled = expression(spread->value);
					if (compiled.second.rfind("array<", 0) != 0) throw Error(spread->location, "array literal spread requires an array");
					const std::string source_element = compiled.second.substr(6, compiled.second.size() - 7);
					const std::string target_element = spread->target_element_type ? module_.value_type(spread->target_element_type) : source_element;
					if (element_type.empty()) element_type = target_element;
					if (target_element != element_type) throw Error(spread->location, "array literal spread element type does not match");
					if (source_element != element_type && !module_.constructor_available(source_element, element_type))
						throw Error(spread->location, "no constructor for converted array spread from " + source_element + " to " + element_type);
					const bool owned_source = expression_is_owned(spread->value);
					const unsigned source = add_local("", compiled.second, spread->location), length = add_local("", "s32", spread->location);
					append(code, compiled.first); code.push_back(0x21); wasm::append_uleb(code, source);
					code.push_back(0x20); wasm::append_uleb(code, source); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21}); wasm::append_uleb(code, length);
					Bytes snapshot_cleanup;
					if (owned_source) { snapshot_cleanup.push_back(0x20); wasm::append_uleb(snapshot_cleanup, source); snapshot_cleanup.push_back(0x10); wasm::append_uleb(snapshot_cleanup, module_.release_index()); }
					auto [snapshot_code, snapshot] = allocate_array(compiled.second, length, spread->location, snapshot_cleanup);
					append(code, snapshot_code);
					const unsigned source_size = array_element_size(source_element);
					if (!managed_type(source_element))
					{
						code.push_back(0x20); wasm::append_uleb(code, snapshot); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x20}); wasm::append_uleb(code, source);
						code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x20}); wasm::append_uleb(code, length); code.push_back(0x41);
						wasm::append_sleb32(code, static_cast<std::int32_t>(source_size)); code.insert(code.end(), {0x6c, 0xfc, 0x0a, 0x00, 0x00});
					}
					else
					{
						const unsigned copy_index = add_local("", "s32", spread->location), copied = add_local("", source_element, spread->location);
						code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, copy_index);
						code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, copy_index);
						code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4f, 0x0d, 0x01, 0x20}); wasm::append_uleb(code, source);
						code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x20}); wasm::append_uleb(code, copy_index); code.push_back(0x41);
						wasm::append_sleb32(code, static_cast<std::int32_t>(source_size)); code.insert(code.end(), {0x6c, 0x6a});
						code.push_back(array_load_opcode(source_element)); code.push_back(source_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x22}); wasm::append_uleb(code, copied);
						code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); code.push_back(0x20); wasm::append_uleb(code, snapshot);
						code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x20}); wasm::append_uleb(code, copy_index); code.push_back(0x41);
						wasm::append_sleb32(code, static_cast<std::int32_t>(source_size)); code.insert(code.end(), {0x6c, 0x6a, 0x20}); wasm::append_uleb(code, copied);
						code.push_back(array_store_opcode(source_element)); code.push_back(source_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x20}); wasm::append_uleb(code, copy_index);
						code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, copy_index); code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
					}
					if (owned_source) { code.push_back(0x20); wasm::append_uleb(code, source); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
					parts.push_back({true, snapshot, length, true, source_element});
				}
				else
				{
					auto compiled = expression(item);
					if (element_type.empty()) element_type = compiled.second;
					if (compiled.second != element_type) throw Error(item->location, "array literal elements must have one type");
					const unsigned local = add_local("", element_type, item->location), length = add_local("", "s32", item->location);
					append(code, compiled.first); code.push_back(0x21); wasm::append_uleb(code, local);
					code.insert(code.end(), {0x41, 0x01, 0x21}); wasm::append_uleb(code, length);
					parts.push_back({false, local, length, expression_is_owned(item), element_type});
				}
				code.push_back(0x20); wasm::append_uleb(code, total); code.push_back(0x21); wasm::append_uleb(code, previous);
				code.push_back(0x20); wasm::append_uleb(code, previous); code.push_back(0x20); wasm::append_uleb(code, parts.back().length); code.insert(code.end(), {0x6a, 0x22}); wasm::append_uleb(code, total);
				code.push_back(0x20); wasm::append_uleb(code, previous); code.insert(code.end(), {0x49, 0x04, 0x40}); release_parts(code); append(code, module_.marker(item->location)); code.insert(code.end(), {0x00, 0x0b});
			}
			if (element_type.empty()) throw Error(value->location, "empty array literal needs an explicit element type");
			const unsigned element_size = array_element_size(element_type), cursor = add_local("", "s32", value->location);
			Bytes allocation_cleanup; release_parts(allocation_cleanup);
			auto [allocation, pointer] = allocate_array("array<" + element_type + ">", total, value->location, allocation_cleanup);
			append(code, allocation);
			code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, cursor);
			for (const Part& part : parts)
			{
				if (part.spread && part.source_element == element_type && !managed_type(element_type))
				{
					code.push_back(0x20); wasm::append_uleb(code, pointer); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x20}); wasm::append_uleb(code, cursor);
					code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a, 0x20}); wasm::append_uleb(code, part.local);
					code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x20}); wasm::append_uleb(code, part.length); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
					code.insert(code.end(), {0x6c, 0xfc, 0x0a, 0x00, 0x00});
				}
				else if (part.spread && part.source_element == element_type)
				{
					const unsigned index = add_local("", "s32", value->location), copied = add_local("", element_type, value->location);
					code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, index); code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, index);
					code.push_back(0x20); wasm::append_uleb(code, part.length); code.insert(code.end(), {0x4f, 0x0d, 0x01, 0x20}); wasm::append_uleb(code, part.local); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x20); wasm::append_uleb(code, index);
					code.insert(code.end(), {0x41, 0x04, 0x6c, 0x6a, 0x28, 0x02, 0x00, 0x22}); wasm::append_uleb(code, copied); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index());
					code.push_back(0x20); wasm::append_uleb(code, pointer); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x20); wasm::append_uleb(code, cursor); code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x6a, 0x41, 0x04, 0x6c, 0x6a, 0x20}); wasm::append_uleb(code, copied); code.insert(code.end(), {0x36, 0x02, 0x00, 0x20}); wasm::append_uleb(code, index);
					code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, index); code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
				}
				else if (part.spread)
				{
					const unsigned source_size = array_element_size(part.source_element);
					const unsigned index = add_local("", "s32", value->location), source_item = add_local("", part.source_element, value->location);
					const unsigned converted_item = add_local("", element_type, value->location);
					const std::string temporary_name = std::string(1, '\x1f') + "array_spread_" + std::to_string(source_item);
					scopes_.back()[temporary_name] = {source_item, part.source_element};
					Name input(value->location, temporary_name), constructor(value->location, element_type.rfind("struct:", 0) == 0 ? element_type.substr(7) : element_type);
					Call conversion_call(value->location, &constructor); conversion_call.arguments.push_back(&input);
					auto converted = expression(&conversion_call);
					scopes_.back().erase(temporary_name);
					code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, index); code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, index);
					code.push_back(0x20); wasm::append_uleb(code, part.length); code.insert(code.end(), {0x4f, 0x0d, 0x01, 0x20}); wasm::append_uleb(code, part.local); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x20); wasm::append_uleb(code, index);
					code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(source_size)); code.insert(code.end(), {0x6c, 0x6a}); code.push_back(array_load_opcode(part.source_element));
					code.push_back(source_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x21}); wasm::append_uleb(code, source_item); append(code, converted.first); code.push_back(0x21); wasm::append_uleb(code, converted_item);
					code.push_back(0x20); wasm::append_uleb(code, pointer); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x20); wasm::append_uleb(code, cursor); code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x6a});
					code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a, 0x20}); wasm::append_uleb(code, converted_item);
					code.push_back(array_store_opcode(element_type)); code.push_back(element_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x20}); wasm::append_uleb(code, index);
					code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, index); code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
				}
				else
				{
					if (managed_type(element_type) && !part.owned) { code.push_back(0x20); wasm::append_uleb(code, part.local); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
					code.push_back(0x20); wasm::append_uleb(code, pointer); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x20); wasm::append_uleb(code, cursor); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a, 0x20}); wasm::append_uleb(code, part.local);
					code.push_back(array_store_opcode(element_type)); code.push_back(element_size == 8 ? 3 : 2); code.push_back(0x00);
				}
				code.push_back(0x20); wasm::append_uleb(code, cursor); code.push_back(0x20); wasm::append_uleb(code, part.length); code.insert(code.end(), {0x6a, 0x21}); wasm::append_uleb(code, cursor);
				if (part.spread && part.owned) { code.push_back(0x20); wasm::append_uleb(code, part.local); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
			}
			code.push_back(0x20); wasm::append_uleb(code, pointer);
			return {code, "array<" + element_type + ">"};
		}
		for (Expr* item : array->items)
		{
			auto compiled = dynamic_cast<Integer*>(item) && !element_type.empty()
				? integer_expression(static_cast<Integer*>(item), element_type) : expression(item);
			if (element_type.empty())
				element_type = compiled.second;
			if (compiled.second != element_type)
				throw Error(item->location, "array literal elements must have one type");
			if (element_type == "module")
				throw Error(item->location, "module is opaque and cannot be stored in array layouts");
			if (!is_scalar(element_type) && !managed_type(element_type))
				throw Error(item->location, "array element type is unsupported");
			items.push_back({std::move(compiled.first), expression_is_owned(item)});
		}
		const unsigned element_size = array_element_size(element_type);
		const unsigned length = add_local("", "s32", value->location);
		Bytes code{0x41}; wasm::append_sleb32(code, static_cast<std::int32_t>(items.size())); code.push_back(0x21); wasm::append_uleb(code, length);
		auto [allocation, pointer] = allocate_array("array<" + element_type + ">", length, value->location);
		append(code, allocation);
		for (std::size_t i = 0; i < items.size(); ++i)
		{
			if (managed_type(element_type))
			{
				const unsigned item = add_local("", element_type, value->location);
				append(code, items[i].first);
				code.push_back(0x21);
				wasm::append_uleb(code, item);
				if (!items[i].second)
				{
					code.push_back(0x20);
					wasm::append_uleb(code, item);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.retain_index());
				}
				code.push_back(0x20);
				wasm::append_uleb(code, pointer);
				code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET});
				code.push_back(0x20);
				wasm::append_uleb(code, item);
			}
			else
			{
				code.push_back(0x20);
				wasm::append_uleb(code, pointer);
				code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET});
				append(code, items[i].first);
			}
			code.push_back(array_store_opcode(element_type));
			code.push_back(static_cast<std::uint8_t>(element_size == 8 ? 3 : 2));
			wasm::append_uleb(code, static_cast<unsigned>(element_size * i));
		}
		code.push_back(0x20);
		wasm::append_uleb(code, pointer);
		return {code, "array<" + element_type + ">"};
	}
	if (auto index = dynamic_cast<Index*>(value))
	{
		if (infer(index->value) == "dval")
			return dval_lookup(index->value, index->index, true);
		auto [array_code, array_type] = expression(index->value);
		if (array_type.rfind("array<", 0) != 0)
			throw Error(value->location, "indexing requires an array");
		auto [index_code, index_type] = expression(index->index);
		if (index_type != "s64")
			throw Error(index->index->location, "expected s64, found " + index_type);
		const unsigned array_local = add_local("", array_type, index->value->location);
		const unsigned requested_index = add_local("", "s64", index->index->location), index_local = add_local("", "s32", index->index->location);
		Bytes code = std::move(array_code);
		code.push_back(0x21);
		wasm::append_uleb(code, array_local);
		append(code, index_code);
		code.push_back(0x21);
		wasm::append_uleb(code, requested_index);
		narrow_s64_index(code, requested_index, index_local, value->location);
		code.push_back(0x20);
		wasm::append_uleb(code, index_local);
		code.push_back(0x20);
		wasm::append_uleb(code, array_local);
		code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x4f, 0x04, 0x40});
		append(code, module_.marker(value->location));
		code.insert(code.end(), {0x00, 0x0b});
		const std::string element_type = array_type.substr(6, array_type.size() - 7);
		const unsigned element_size = array_element_size(element_type);
		code.push_back(0x20);
		wasm::append_uleb(code, array_local);
		code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET});
		code.push_back(0x20);
		wasm::append_uleb(code, index_local);
		code.push_back(0x41);
		wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
		code.insert(code.end(), {0x6c, 0x6a});
		code.push_back(array_load_opcode(element_type));
		code.push_back(static_cast<std::uint8_t>(element_size == 8 ? 3 : 2));
		code.push_back(0x00);
		if (expression_is_owned(index->value))
		{
			const unsigned result = add_local("", element_type, value->location);
			code.push_back(0x21);
			wasm::append_uleb(code, result);
			if (managed_type(element_type))
			{
				code.push_back(0x20);
				wasm::append_uleb(code, result);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.retain_index());
			}
			code.push_back(0x20);
			wasm::append_uleb(code, array_local);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.release_index());
			code.push_back(0x20);
			wasm::append_uleb(code, result);
		}
		return {code, element_type};
	}
	if (auto scope = dynamic_cast<ScopeLookup*>(value))
	{
		const std::string receiver_type = infer(scope->value);
		if (scope->member == "type")
			throw Error(scope->location, "value::type is valid only in dependent type declarations");
		if (scope->member != "type_name" && scope->member != "size" && scope->member != "items")
			throw Error(scope->location, "unknown scope member '" + scope->member + "'");
		if (scope->member != "type_name" && receiver_type.rfind("struct:", 0) != 0)
			throw Error(scope->location, "value::" + scope->member + " requires a struct");
		auto [receiver_code, actual] = expression(scope->value);
		if (actual != receiver_type) throw Error(scope->location, "reflection receiver type changed while lowering");
		const unsigned receiver = add_local("", receiver_type, scope->value->location);
		Bytes code = std::move(receiver_code);
		code.push_back(0x21); wasm::append_uleb(code, receiver);
		const unsigned descriptor = module_.reflection_type_descriptor(receiver_type, scope->location);
		if (scope->member == "type_name")
		{
			code.insert(code.end(), {0x23, 0x00, 0x41}); wasm::append_sleb32(code, static_cast<std::int32_t>(descriptor));
			code.insert(code.end(), {0x6a, 0x28, 0x02, BEARER_CAPY_REFLECTION_TYPE_NAME_OFFSET, 0x23, 0x00, 0x6a});
			if (expression_is_owned(scope->value))
			{
				code.push_back(0x20); wasm::append_uleb(code, receiver);
				code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
			}
			return {code, "string"};
		}
		if (scope->member == "size")
		{
			code.insert(code.end(), {0x23, 0x00, 0x41}); wasm::append_sleb32(code, static_cast<std::int32_t>(descriptor));
			code.insert(code.end(), {0x6a, 0x28, 0x02, BEARER_CAPY_REFLECTION_TYPE_DETAIL_OFFSET, 0x23, 0x00, 0x6a, 0x28, 0x02, BEARER_CAPY_REFLECTION_STRUCT_FIELD_COUNT_OFFSET, 0xad});
			if (expression_is_owned(scope->value))
			{
				code.push_back(0x20); wasm::append_uleb(code, receiver);
				code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
			}
			return {code, "s64"};
		}
		auto reflect = [&](unsigned output, unsigned capacity)
		{
			code.push_back(0x20); wasm::append_uleb(code, receiver);
			code.insert(code.end(), {0x23, 0x00, 0x23, 0x00, 0x41}); wasm::append_sleb32(code, static_cast<std::int32_t>(descriptor));
			code.insert(code.end(), {0x6a, 0x28, 0x02, BEARER_CAPY_REFLECTION_TYPE_DETAIL_OFFSET});
			if (output)
				managed_payload_pointer(code, output, "dval");
			else
			{
				code.push_back(0x41); wasm::append_sleb32(code, 0);
			}
			if (capacity)
			{
				code.push_back(0x20); wasm::append_uleb(code, capacity);
			}
			else
			{
				code.push_back(0x41); wasm::append_sleb32(code, 0);
			}
			code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_capy_reflect_struct_brrb"));
		};
		Bytes release_receiver;
		if (expression_is_owned(scope->value))
		{
			release_receiver.push_back(0x20); wasm::append_uleb(release_receiver, receiver);
			release_receiver.push_back(0x10); wasm::append_uleb(release_receiver, module_.release_index());
		}
		const unsigned length = add_local("", "s32", scope->location);
		reflect(0, 0);
		code.push_back(0x22); wasm::append_uleb(code, length);
		code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
		append(code, release_receiver); append(code, module_.marker(scope->location)); code.insert(code.end(), {0x00, 0x0b});
		auto [allocation, output] = allocate_dval(length, scope->location, release_receiver);
		append(code, allocation);
		const unsigned written = add_local("", "s32", scope->location);
		reflect(output, length);
		code.push_back(0x22); wasm::append_uleb(code, written);
		code.push_back(0x20); wasm::append_uleb(code, length);
		code.insert(code.end(), {0x47, 0x04, 0x40});
		code.push_back(0x20); wasm::append_uleb(code, output); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
		append(code, release_receiver); append(code, module_.marker(scope->location)); code.insert(code.end(), {0x00, 0x0b});
		code.push_back(0x20); wasm::append_uleb(code, output); code.push_back(0x20); wasm::append_uleb(code, written);
		code.insert(code.end(), {0x36, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET});
		retain_dval_callables(code, output, scope->location);
		append(code, release_receiver);
		code.push_back(0x20); wasm::append_uleb(code, output);
		return {code, "dval"};
	}
	if (auto member = dynamic_cast<Member*>(value))
	{
		const std::string receiver_type = infer(member->value);
		if (receiver_type == "dval")
		{
			String key(member->location, member->member);
			return dval_lookup(member->value, &key, true);
		}
		if (receiver_type == "module")
			throw Error(member->location, "module member access must be called");
		auto [object_code, object_type] = expression(member->value);
		if (object_type.rfind("struct:", 0) != 0)
			throw Error(member->location, "member access requires a struct");
		const auto& fields = module_.struct_type(object_type.substr(7), member->location).fields;
		auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& field) { return field.first == member->member; });
		if (found == fields.end())
			throw Error(member->location, "struct has no member '" + member->member + "'");
		const unsigned field_index = static_cast<unsigned>(found - fields.begin()), object = add_local("", object_type, member->value->location);
		std::vector<std::string> field_types;
		for (const auto& [name, field_type] : fields)
			field_types.push_back(field_type);
		const AggregateLayout layout = aggregate_layout(field_types, 16);
		Bytes code = std::move(object_code);
		code.push_back(0x21);
		wasm::append_uleb(code, object);
		code.push_back(0x20);
		wasm::append_uleb(code, object);
		load_field(code, found->second, layout.offsets[field_index]);
		if (expression_is_owned(member->value))
		{
			const unsigned result = add_local("", found->second, value->location);
			code.push_back(0x21);
			wasm::append_uleb(code, result);
			if (managed_type(found->second))
			{
				code.push_back(0x20);
				wasm::append_uleb(code, result);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.retain_index());
			}
			code.push_back(0x20);
			wasm::append_uleb(code, object);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.release_index());
			code.push_back(0x20);
			wasm::append_uleb(code, result);
		}
		return {code, found->second};
	}
	if (auto markup = dynamic_cast<Markup*>(value))
	{
		std::vector<MarkupField*> fields;
		for (Expr* part : markup->parts)
			if (auto field = dynamic_cast<MarkupField*>(part))
				fields.push_back(field);
		if (fields.empty())
		{
			std::string text;
			for (Expr* part : markup->parts)
				text += static_cast<MarkupText*>(part)->value;
			const unsigned offset = module_.add_static_string(text);
			Bytes code{0x23, 0x00, 0x41};
			wasm::append_sleb32(code, static_cast<std::int32_t>(offset));
			code.push_back(0x6a);
			return {code, "string"};
		}

		struct Field
		{
			MarkupField* node;
			unsigned local;
			std::string type;
			bool owned;
			bool formatted_scalar;
		};
		std::vector<Field> compiled;
		Bytes code;
		for (MarkupField* field : fields)
		{
			auto [field_code, type] = expression(field->value);
			bool owned = expression_is_owned(field->value);
			bool formatted_scalar = false;
			if ((type == "f32" || type == "f64") && (field->context == bearer::MarkupContext::javascript_value || field->context == bearer::MarkupContext::css_value))
				throw Error(field->location, "f64 markup interpolation is not supported in script or style elements");
			if (is_scalar(type) && type != "bool" && type != "s32")
			{
				auto formatted = conversion(std::move(field_code), type, "string", field->location);
				field_code = std::move(formatted.first);
				type = std::move(formatted.second);
				owned = true;
				formatted_scalar = true;
			}
			if (type != "string" && type != "s32" && type != "bool")
				throw Error(field->location, "markup interpolation does not support " + type);
			const unsigned local = add_local("", type, field->location);
			append(code, field_code);
			code.push_back(0x21);
			wasm::append_uleb(code, local);
			compiled.push_back({field, local, std::move(type), owned, formatted_scalar});
		}

		const unsigned total = add_local("", "s32", markup->location);
		std::size_t static_length = 0;
		for (Expr* part : markup->parts)
			if (auto text = dynamic_cast<MarkupText*>(part))
				static_length += text->value.size();
		code.push_back(0x41);
		wasm::append_sleb32(code, static_cast<std::int32_t>(static_length));
		code.push_back(0x21);
		wasm::append_uleb(code, total);
		for (const Field& field : compiled)
		{
			if (field.node->escaped && field.type == "string" && !field.formatted_scalar)
				append(code, markup_escape_length(field.local, total, field.node->context, field.node->location));
			else if (field.type == "s32")
				append(code, markup_s32_length(field.local, total, field.node->location));
			else if (field.type == "bool")
			{
				code.insert(code.end(), {0x20});
				wasm::append_uleb(code, total);
				code.insert(code.end(), {0x41, 0x04, 0x6a, 0x21});
				wasm::append_uleb(code, total);
				code.insert(code.end(), {0x20});
				wasm::append_uleb(code, field.local);
				code.insert(code.end(), {0x45, 0x04, 0x40, 0x20});
				wasm::append_uleb(code, total);
				code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
				wasm::append_uleb(code, total);
				code.push_back(0x0b);
			}
			else
			{
				code.insert(code.end(), {0x20});
				wasm::append_uleb(code, total);
				code.insert(code.end(), {0x20});
				wasm::append_uleb(code, field.local);
				code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x6a, 0x21});
				wasm::append_uleb(code, total);
			}
		}

		const unsigned pointer = add_local("", "string", markup->location);
		code.insert(code.end(), {0x20});
		wasm::append_uleb(code, total);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x10});
		wasm::append_uleb(code, module_.import_index("bearer_alloc"));
		code.push_back(0x21);
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x20});
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x45, 0x04, 0x40});
		append(code, module_.marker(markup->location));
		code.push_back(0x00);
		code.push_back(0x0b);
		for (const auto [header, offset] : {std::pair<std::int32_t, unsigned>{1, 0}, {1, 4}, {1, 8}})
			store_i32_constant(code, pointer, header, offset);
		code.insert(code.end(), {0x20});
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x20});
		wasm::append_uleb(code, total);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x36, 0x02, BEARER_WASM_OBJECT_HEADER_SIZE_OFFSET});
		code.insert(code.end(), {0x20});
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x20});
		wasm::append_uleb(code, total);
		code.insert(code.end(), {0x36, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
		const unsigned cursor = add_local("", "s32", markup->location);
		code.insert(code.end(), {0x20});
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x21});
		wasm::append_uleb(code, cursor);
		for (Expr* part : markup->parts)
		{
			if (auto text = dynamic_cast<MarkupText*>(part))
			{
				if (text->value.empty())
					continue;
				const unsigned offset = module_.add_data(text->value);
				code.push_back(0x20);
				wasm::append_uleb(code, cursor);
				code.insert(code.end(), {0x23, 0x00, 0x41});
				wasm::append_sleb32(code, static_cast<std::int32_t>(offset));
				code.insert(code.end(), {0x6a, 0x41});
				wasm::append_sleb32(code, static_cast<std::int32_t>(text->value.size()));
				code.insert(code.end(), {0xfc, 0x0a, 0x00, 0x00, 0x20});
				wasm::append_uleb(code, cursor);
				code.push_back(0x41);
				wasm::append_sleb32(code, static_cast<std::int32_t>(text->value.size()));
				code.push_back(0x6a);
				code.push_back(0x21);
				wasm::append_uleb(code, cursor);
				continue;
			}
			auto field = std::find_if(compiled.begin(), compiled.end(), [&](const Field& item) { return item.node == part; });
			if (field->node->escaped && field->type == "string" && !field->formatted_scalar)
				append(code, markup_escape_write(field->local, cursor, field->node->context, field->node->location));
			else if (field->type == "s32")
				append(code, markup_s32_write(field->local, cursor, field->node->location));
			else if (field->type == "bool")
			{
				code.insert(code.end(), {0x20});
				wasm::append_uleb(code, field->local);
				code.insert(code.end(), {0x04, 0x40});
				append(code, markup_write_bytes(cursor, "true"));
				code.push_back(0x05);
				append(code, markup_write_bytes(cursor, "false"));
				code.push_back(0x0b);
			}
			else
			{
				code.insert(code.end(), {0x20});
				wasm::append_uleb(code, cursor);
				code.insert(code.end(), {0x20});
				wasm::append_uleb(code, field->local);
				code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
				wasm::append_uleb(code, field->local);
				code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0xfc, 0x0a, 0x00, 0x00, 0x20});
				wasm::append_uleb(code, cursor);
				code.insert(code.end(), {0x20});
				wasm::append_uleb(code, field->local);
				code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x6a, 0x21});
				wasm::append_uleb(code, cursor);
			}
			if (field->owned)
			{
				code.insert(code.end(), {0x20});
				wasm::append_uleb(code, field->local);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.release_index());
			}
		}
		code.push_back(0x20);
		wasm::append_uleb(code, pointer);
		return {code, "string"};
	}
	if (auto name = dynamic_cast<Name*>(value))
	{
		if (name->value == "true" || name->value == "false")
			return {{0x41, static_cast<std::uint8_t>(name->value == "true")}, "bool"};
		for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
			if (auto found = scope->find(name->value); found != scope->end())
			{
				Bytes code{0x20};
				wasm::append_uleb(code, found->second.first);
				return {code, found->second.second};
			}
		if (name->value == "none")
		{
			const unsigned length = add_local("", "s32", value->location);
			Bytes code{0x41, 0x00, 0x41, 0x00, 0x10};
			wasm::append_uleb(code, module_.import_index("bearer_dv_none_brrb"));
			code.push_back(0x21);
			wasm::append_uleb(code, length);
			code.push_back(0x20); wasm::append_uleb(code, length);
			code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
			append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b});
			auto [allocation, pointer] = allocate_blob("dval", 4, length, value->location);
			append(code, allocation);
			managed_payload_pointer(code, pointer, "dval");
			code.push_back(0x20);
			wasm::append_uleb(code, length);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.import_index("bearer_dv_none_brrb"));
			code.push_back(0x20);
			wasm::append_uleb(code, length);
			code.insert(code.end(), {0x47, 0x04, 0x40, 0x20}); wasm::append_uleb(code, pointer);
			code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
			append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b, 0x20});
			wasm::append_uleb(code, pointer);
			return {code, "dval"};
		}
		auto [type, slot] = module_.reference_function(name->value, name->location);
		const unsigned offset = module_.add_static_closure(slot, static_cast<unsigned>(std::stoul(type.substr(9))));
		Bytes code{0x23, 0x00, 0x41};
		wasm::append_sleb32(code, static_cast<std::int32_t>(offset));
		code.push_back(0x6a);
		return {code, type};
	}
	if (auto variable = dynamic_cast<Variable*>(value))
	{
		ArrayLiteral typed_array(variable->value->location);
		Expr* initializer = variable->value;
		if (variable->annotation)
			if (auto value_array = dynamic_cast<ArrayLiteral*>(variable->value))
				if (auto type_array = dynamic_cast<ArrayLiteral*>(variable->annotation); type_array && type_array->items.size() == 1)
				{
					typed_array.items = value_array->items;
					typed_array.explicit_element_type = type_array->items[0];
					initializer = &typed_array;
				}
		std::string declared = variable->annotation ? module_.value_type(variable->annotation) : "";
		auto initialized = variable->annotation && dynamic_cast<Integer*>(initializer)
			? integer_expression(static_cast<Integer*>(initializer), declared) : expression(initializer);
		auto code = std::move(initialized.first);
		std::string type = std::move(initialized.second);
		if (!variable->annotation)
			declared = type;
		if (declared != type && can_convert(type, declared))
		{
			auto converted = conversion(std::move(code), type, declared, value->location, expression_is_owned(initializer));
			code = std::move(converted.first);
			type = std::move(converted.second);
		}
		if (declared != type)
			throw Error(value->location, "expected " + declared + ", found " + type);
		unsigned slot = add_local(variable->name, declared, value->location);
		if (declared == "dval")
			owned_local_dval_slots_.insert(slot);
		const bool managed = managed_type(declared);
		const bool replace = managed && repeated_condition_scope_ && owned_scopes_.size() == *repeated_condition_scope_;
		if (replace)
		{
			const unsigned replacement = add_local("", declared, value->location);
			code.push_back(0x21);
			wasm::append_uleb(code, replacement);
			if (!expression_is_owned(variable->value))
			{
				code.push_back(0x20);
				wasm::append_uleb(code, replacement);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.retain_index());
			}
			code.push_back(0x20);
			wasm::append_uleb(code, slot);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.release_index());
			code.push_back(0x20);
			wasm::append_uleb(code, replacement);
		}
		code.push_back(value_required ? 0x22 : 0x21);
		wasm::append_uleb(code, slot);
		if (managed)
		{
			if (!replace && !expression_is_owned(variable->value))
			{
				code.push_back(0x20);
				wasm::append_uleb(code, slot);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.retain_index());
			}
			owned_scopes_.back().push_back({slot, declared});
		}
		return {code, value_required ? declared : "void"};
	}
	if (auto binary = dynamic_cast<Binary*>(value))
	{
		if (binary->operator_ == "postfix?")
			return dval_presence(binary->left);
		if (binary->operator_ == "=")
		{
			if (auto target_member = dynamic_cast<Member*>(binary->left); target_member && infer(target_member->value).rfind("struct:", 0) == 0)
			{
				const std::string object_type = infer(target_member->value);
				const auto& fields = module_.struct_type(object_type.substr(7), target_member->location).fields;
				auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& field) { return field.first == target_member->member; });
				if (found == fields.end()) throw Error(target_member->location, "struct has no member '" + target_member->member + "'");
				const unsigned field_index = static_cast<unsigned>(found - fields.begin());
				std::vector<std::string> field_types;
				for (const auto& field : fields) field_types.push_back(field.second);
				const AggregateLayout layout = aggregate_layout(field_types, 16);
				auto receiver = expression(target_member->value);
				auto replacement = dynamic_cast<Integer*>(binary->right)
					? integer_expression(static_cast<Integer*>(binary->right), found->second) : expression(binary->right);
				if (replacement.second != found->second) throw Error(binary->right->location, "expected " + found->second + ", found " + replacement.second);
				const bool owned_receiver = expression_is_owned(target_member->value);
				const unsigned object = add_local("", object_type, target_member->value->location);
				const unsigned value_slot = add_local("", found->second, binary->right->location);
				Bytes code = std::move(receiver.first); code.push_back(0x21); wasm::append_uleb(code, object);
				if (!owned_receiver) { code.push_back(0x20); wasm::append_uleb(code, object); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
				append(code, replacement.first); code.push_back(0x21); wasm::append_uleb(code, value_slot);
				if (managed_type(found->second))
				{
					if (!expression_is_owned(binary->right)) { code.push_back(0x20); wasm::append_uleb(code, value_slot); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
					code.push_back(0x20); wasm::append_uleb(code, object); load_field(code, found->second, layout.offsets[field_index]); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
				}
				code.push_back(0x20); wasm::append_uleb(code, object); code.push_back(0x20); wasm::append_uleb(code, value_slot); store_field(code, found->second, layout.offsets[field_index]);
				if (managed_type(found->second) && owned_receiver) { code.push_back(0x20); wasm::append_uleb(code, value_slot); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); owned_expression_results_.insert(value); }
				code.push_back(0x20); wasm::append_uleb(code, object); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
				code.push_back(0x20); wasm::append_uleb(code, value_slot);
				return {code, found->second};
			}
			std::vector<std::unique_ptr<String>> member_selectors;
			std::vector<Expr*> selectors;
			Expr* root = binary->left;
			while (true)
			{
				if (auto member = dynamic_cast<Member*>(root))
				{
					member_selectors.push_back(std::make_unique<String>(member->location, member->member));
					selectors.push_back(member_selectors.back().get());
					root = member->value;
				}
				else if (auto index = dynamic_cast<Index*>(root))
				{
					selectors.push_back(index->index);
					root = index->value;
				}
				else
					break;
			}
			if (!selectors.empty())
			{
				std::reverse(selectors.begin(), selectors.end());
				if (auto target_index = dynamic_cast<Index*>(binary->left); target_index && infer(target_index->value).rfind("array<", 0) == 0)
					selectors.clear();
				else
				{
					auto receiver = dynamic_cast<Name*>(root);
					if (!receiver)
						throw Error(binary->left->location, "nested DValue assignment requires a local, parameter, or captured dval root");
					auto [slot, type] = lookup(receiver);
					if (type != "dval" || (!owned_local_dval_slots_.contains(slot) && !borrowed_managed_slots_.contains(slot)))
						throw Error(binary->left->location, "nested DValue assignment requires a local, parameter, or captured dval root");
					auto result = dval_set_path(slot, selectors, binary->right, binary->location);
					owned_expression_results_.insert(value);
					return result;
				}
			}
			if (auto target_index = dynamic_cast<Index*>(binary->left))
			{
				const std::string array_type = infer(target_index->value);
				if (array_type.rfind("array<", 0) != 0)
					throw Error(target_index->value->location, "indexed assignment requires an array");
				const std::string element = array_type.substr(6, array_type.size() - 7);
				const unsigned element_size = array_element_size(element);
				auto receiver = expression(target_index->value);
				auto [index_code, index_type] = expression(target_index->index);
				if (index_type != "s64") throw Error(target_index->index->location, "array index must be s64");
				auto replacement_result = dynamic_cast<Integer*>(binary->right)
					? integer_expression(static_cast<Integer*>(binary->right), element) : expression(binary->right);
				if (replacement_result.second != element) throw Error(binary->right->location, "expected " + element + ", found " + replacement_result.second);
				const bool owned_receiver = expression_is_owned(target_index->value);
				const unsigned array = add_local("", array_type, target_index->value->location), requested_index = add_local("", "s64", target_index->index->location), index = add_local("", "s32", target_index->index->location);
				const unsigned replacement = add_local("", element, binary->right->location), length = add_local("", "s32", binary->location);
				Bytes code = std::move(receiver.first); code.push_back(0x21); wasm::append_uleb(code, array);
				if (!owned_receiver) { code.push_back(0x20); wasm::append_uleb(code, array); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
				append(code, index_code); code.push_back(0x21); wasm::append_uleb(code, requested_index);
				narrow_s64_index(code, requested_index, index, target_index->location);
				append(code, replacement_result.first); code.push_back(0x21); wasm::append_uleb(code, replacement);
				code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40}); append(code, module_.marker(target_index->location)); code.insert(code.end(), {0x00, 0x0b});
				code.push_back(0x20); wasm::append_uleb(code, array); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21}); wasm::append_uleb(code, length);
				code.push_back(0x20); wasm::append_uleb(code, index); code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4f, 0x04, 0x40}); append(code, module_.marker(target_index->location)); code.insert(code.end(), {0x00, 0x0b});
				if (managed_type(element))
				{
					if (!expression_is_owned(binary->right)) { code.push_back(0x20); wasm::append_uleb(code, replacement); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
					code.push_back(0x20); wasm::append_uleb(code, array); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x20); wasm::append_uleb(code, index);
					code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
					code.insert(code.end(), {0x6c, 0x6a, 0x28, 0x02, 0x00, 0x10}); wasm::append_uleb(code, module_.release_index());
				}
				code.push_back(0x20); wasm::append_uleb(code, array); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x20); wasm::append_uleb(code, index);
				code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a, 0x20}); wasm::append_uleb(code, replacement);
				code.push_back(array_store_opcode(element)); code.push_back(element_size == 8 ? 3 : 2); code.push_back(0x00);
				if (managed_type(element) && owned_receiver) { code.push_back(0x20); wasm::append_uleb(code, replacement); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); owned_expression_results_.insert(value); }
				code.push_back(0x20); wasm::append_uleb(code, array); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
				code.push_back(0x20); wasm::append_uleb(code, replacement);
				return {code, element};
			}
			auto target = dynamic_cast<Name*>(binary->left);
			if (!target)
				throw Error(binary->left->location, "assignment target must be a binding, array element, DValue path, or struct field");
			auto [slot, expected] = lookup(target);
			ArrayLiteral typed_array(binary->right->location);
			Name element_type(binary->right->location, "");
			Expr* replacement_value = binary->right;
			if (auto array = dynamic_cast<ArrayLiteral*>(replacement_value); expected.rfind("array<", 0) == 0)
			{
				element_type.value = expected.substr(6, expected.size() - 7);
				typed_array.items = array->items;
				typed_array.explicit_element_type = &element_type;
				replacement_value = &typed_array;
			}
			auto replacement = dynamic_cast<Integer*>(replacement_value)
				? integer_expression(static_cast<Integer*>(replacement_value), expected) : expression(replacement_value);
			auto code = std::move(replacement.first);
			const std::string actual = std::move(replacement.second);
			if (actual != expected)
				throw Error(value->location, "expected " + expected + ", found " + actual);
			if (managed_type(expected))
			{
				const auto borrowed = borrowed_managed_slots_.contains(slot);
				const unsigned temporary = add_local("", expected, value->location);
				unsigned rebind_flag = 0;
				if (borrowed)
				{
					auto [entry, inserted] = borrowed_managed_rebind_flags_.emplace(slot, 0);
					if (inserted) entry->second = add_local("", "bool", value->location);
					rebind_flag = entry->second;
				}
				code.push_back(0x21);
				wasm::append_uleb(code, temporary);
				if (!expression_is_owned(binary->right))
				{
					code.push_back(0x20);
					wasm::append_uleb(code, temporary);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.retain_index());
				}
				if (borrowed)
				{
					code.push_back(0x20);
					wasm::append_uleb(code, rebind_flag);
					code.insert(code.end(), {0x04, 0x40, 0x20});
					wasm::append_uleb(code, slot);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.release_index());
					code.push_back(0x0b);
				}
				else
				{
					code.push_back(0x20);
					wasm::append_uleb(code, slot);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.release_index());
				}
				code.push_back(0x20);
				wasm::append_uleb(code, temporary);
				code.push_back(0x22);
				wasm::append_uleb(code, slot);
				if (borrowed)
				{
					code.push_back(0x41);
					code.push_back(0x01);
					code.push_back(0x21);
					wasm::append_uleb(code, rebind_flag);
				}
				return {code, actual};
			}
			code.push_back(0x22);
			wasm::append_uleb(code, slot);
			return {code, actual};
		}
		if (binary->operator_ == ":=")
		{
			auto target = dynamic_cast<Name*>(binary->left);
			if (!target)
				throw Error(binary->left->location, "inferred declaration target must be a local name");
			auto [code, actual] = expression(binary->right);
			const unsigned slot = add_local(target->value, actual, target->location);
			code.push_back(0x21);
			wasm::append_uleb(code, slot);
			if (managed_type(actual))
			{
				if (!expression_is_owned(binary->right))
				{
					code.push_back(0x20);
					wasm::append_uleb(code, slot);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.retain_index());
				}
				owned_scopes_.back().push_back({slot, actual});
			}
			code.push_back(0x20);
			wasm::append_uleb(code, slot);
			return {code, actual};
		}
		if (binary->operator_ == "&&" || binary->operator_ == "||")
		{
			auto [left, left_type] = expression(binary->left);
			scopes_.push_back({});
			owned_scopes_.push_back({});
			auto [right, right_type] = expression(binary->right);
			if (left_type != "bool" || right_type != "bool")
				throw Error(value->location, "logical operators require bool operands");
			const unsigned right_result = add_local("", "bool", binary->right->location);
			right.push_back(0x21);
			wasm::append_uleb(right, right_result);
			append(right, cleanup_scopes(owned_scopes_.size() - 1));
			right.push_back(0x20);
			wasm::append_uleb(right, right_result);
			owned_scopes_.pop_back();
			scopes_.pop_back();
			left.insert(left.end(), {0x04, 0x7f});
			if (binary->operator_ == "&&")
			{
				append(left, right);
				left.insert(left.end(), {0x05, 0x41, 0x00});
			}
			else
			{
				left.insert(left.end(), {0x41, 0x01, 0x05});
				append(left, right);
			}
			left.push_back(0x0b);
			return {left, "bool"};
		}
		if (binary->operator_ == "unary-" || binary->operator_ == "unary!")
		{
			auto [right, right_type] = expression(binary->right);
			if (binary->operator_ == "unary-")
			{
				if (right_type == "s8" || right_type == "s16" || right_type == "s32")
				{
					Bytes code{0x41, 0x00};
					append(code, right);
					code.push_back(0x6b);
					return {code, "s32"};
				}
				if (right_type == "s64")
				{
					Bytes code{0x42, 0x00};
					append(code, right);
					code.push_back(0x7d);
					return {code, "s64"};
				}
				if (right_type == "f32")
				{
					right.push_back(0x8c);
					return {right, "f32"};
				}
				if (right_type == "f64")
				{
					right.push_back(0x9a);
					return {right, "f64"};
				}
				throw Error(value->location, "unary - requires an s32, s64, or f64 operand");
			}
			if (right_type != "bool")
				throw Error(value->location, "unary ! requires a bool operand");
			right.push_back(0x45);
			return {right, "bool"};
		}
		const std::string inferred_left = dynamic_cast<Integer*>(binary->left) ? "s64" : infer(binary->left);
		const std::string inferred_right = dynamic_cast<Integer*>(binary->right) ? "s64" : infer(binary->right);
		if (inferred_left == "string" || inferred_right == "string")
		{
			auto [left_code, left_type] = expression(binary->left);
			auto [right_code, right_type] = expression(binary->right);
			if (left_type != "string" || right_type != "string")
				throw Error(value->location, "string operators require string operands");
			if (binary->operator_ != "+" && binary->operator_ != "==" && binary->operator_ != "!=")
				throw Error(value->location, "strings support only +, ==, and != operators");
			const unsigned left = add_local("", "string", binary->left->location), right = add_local("", "string", binary->right->location);
			const unsigned left_length = add_local("", "s32", binary->left->location), right_length = add_local("", "s32", binary->right->location);
			Bytes code = std::move(left_code);
			code.push_back(0x21);
			wasm::append_uleb(code, left);
			append(code, right_code);
			code.push_back(0x21);
			wasm::append_uleb(code, right);
			for (const auto [object, length] : {std::pair<unsigned, unsigned>{left, left_length}, {right, right_length}})
			{
				code.push_back(0x20);
				wasm::append_uleb(code, object);
				code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21});
				wasm::append_uleb(code, length);
			}
			unsigned result = 0;
			if (binary->operator_ == "+")
			{
				const unsigned total = add_local("", "s32", value->location);
				code.push_back(0x20);
				wasm::append_uleb(code, left_length);
				code.push_back(0x20);
				wasm::append_uleb(code, right_length);
				code.insert(code.end(), {0x6a, 0x22});
				wasm::append_uleb(code, total);
				code.push_back(0x20);
				wasm::append_uleb(code, left_length);
				code.insert(code.end(), {0x49, 0x04, 0x40});
				append(code, module_.marker(value->location));
				code.insert(code.end(), {0x00, 0x0b});
				auto [allocation, pointer] = allocate_blob("string", 1, total, value->location);
				append(code, allocation);
				auto append_copy = [&](unsigned destination_offset, unsigned source, unsigned length)
				{
					code.push_back(0x20);
					wasm::append_uleb(code, pointer);
					code.insert(code.end(), {0x41, 0x14, 0x6a});
					if (destination_offset != 0xffffffffu)
					{
						code.push_back(0x20);
						wasm::append_uleb(code, destination_offset);
						code.push_back(0x6a);
					}
					code.push_back(0x20);
					wasm::append_uleb(code, source);
					code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
					wasm::append_uleb(code, length);
					code.insert(code.end(), {0xfc, 0x0a, 0x00, 0x00});
				};
				append_copy(0xffffffffu, left, left_length);
				append_copy(left_length, right, right_length);
				result = pointer;
			}
			else
			{
				const unsigned index = add_local("", "s32", value->location);
				result = add_local("", "bool", value->location);
				code.insert(code.end(), {0x41, 0x00, 0x21});
				wasm::append_uleb(code, result);
				code.push_back(0x20);
				wasm::append_uleb(code, left_length);
				code.push_back(0x20);
				wasm::append_uleb(code, right_length);
				code.insert(code.end(), {0x46, 0x04, 0x40, 0x41, 0x01, 0x21});
				wasm::append_uleb(code, result);
				code.insert(code.end(), {0x41, 0x00, 0x21});
				wasm::append_uleb(code, index);
				code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20});
				wasm::append_uleb(code, index);
				code.push_back(0x20);
				wasm::append_uleb(code, left_length);
				code.insert(code.end(), {0x4f, 0x0d, 0x01});
				for (unsigned object : {left, right})
				{
					code.push_back(0x20);
					wasm::append_uleb(code, object);
					code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
					wasm::append_uleb(code, index);
					code.insert(code.end(), {0x6a, 0x2d, 0x00, 0x00});
				}
				code.insert(code.end(), {0x47, 0x04, 0x40, 0x41, 0x00, 0x21});
				wasm::append_uleb(code, result);
				code.insert(code.end(), {0x0c, 0x02, 0x0b, 0x20});
				wasm::append_uleb(code, index);
				code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
				wasm::append_uleb(code, index);
				code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b, 0x0b});
				if (binary->operator_ == "!=")
				{
					code.push_back(0x20);
					wasm::append_uleb(code, result);
					code.insert(code.end(), {0x45, 0x21});
					wasm::append_uleb(code, result);
				}
			}
			for (const auto& [expression_value, local] : {std::pair<Expr*, unsigned>{binary->left, left}, {binary->right, right}})
				if (expression_is_owned(expression_value))
				{
					code.push_back(0x20);
					wasm::append_uleb(code, local);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.release_index());
				}
			code.push_back(0x20);
			wasm::append_uleb(code, result);
			return {code, binary->operator_ == "+" ? "string" : "bool"};
		}
		auto left_result = dynamic_cast<Integer*>(binary->left) && !dynamic_cast<Integer*>(binary->right)
			? integer_expression(static_cast<Integer*>(binary->left), inferred_right) : expression(binary->left);
		auto right_result = dynamic_cast<Integer*>(binary->right) && !dynamic_cast<Integer*>(binary->left)
			? integer_expression(static_cast<Integer*>(binary->right), inferred_left) : expression(binary->right);
		auto left = std::move(left_result.first);
		const std::string left_type = std::move(left_result.second);
		auto right = std::move(right_result.first);
		const std::string right_type = std::move(right_result.second);
		if (left_type != right_type)
			throw Error(value->location, "expected " + left_type + ", found " + right_type);
		static const std::map<std::string, std::map<std::string, std::uint8_t>> ops = {
			{"s32",
			 {{"+", 0x6a},
			  {"-", 0x6b},
			  {"*", 0x6c},
			  {"/", 0x6d},
			  {"%", 0x6f},
			  {"==", 0x46},
			  {"!=", 0x47},
			  {"<", 0x48},
			  {">", 0x4a},
			  {"<=", 0x4c},
			  {">=", 0x4e}}},
			{"u32", {{"+", 0x6a}, {"-", 0x6b}, {"*", 0x6c}, {"/", 0x6e}, {"%", 0x70}, {"==", 0x46}, {"!=", 0x47}, {"<", 0x49}, {">", 0x4b}, {"<=", 0x4d}, {">=", 0x4f}}},
			{"bool", {{"==", 0x46}, {"!=", 0x47}}},
			{"s64",
			 {{"+", 0x7c},
			  {"-", 0x7d},
			  {"*", 0x7e},
			  {"/", 0x7f},
			  {"%", 0x81},
			  {"==", 0x51},
			  {"!=", 0x52},
			  {"<", 0x53},
			  {">", 0x55},
			  {"<=", 0x57},
			  {">=", 0x59}}},
			{"u64",
			 {{"+", 0x7c},
			  {"-", 0x7d},
			  {"*", 0x7e},
			  {"/", 0x80},
			  {"%", 0x82},
			  {"==", 0x51},
			  {"!=", 0x52},
			  {"<", 0x54},
			  {">", 0x56},
			  {"<=", 0x58},
			  {">=", 0x5a}}},
			{"f32", {{"+", 0x92}, {"-", 0x93}, {"*", 0x94}, {"/", 0x95}, {"==", 0x5b}, {"!=", 0x5c}, {"<", 0x5d}, {">", 0x5e}, {"<=", 0x5f}, {">=", 0x60}}},
			{"f64", {{"+", 0xa0}, {"-", 0xa1}, {"*", 0xa2}, {"/", 0xa3}, {"==", 0x61}, {"!=", 0x62}, {"<", 0x63}, {">", 0x64}, {"<=", 0x65}, {">=", 0x66}}}};
		const std::string opcode_type = left_type == "s8" || left_type == "s16" ? "s32" : left_type == "u8" || left_type == "u16" ? "u32" : left_type;
		auto type_ops = ops.find(opcode_type);
		if (type_ops == ops.end())
			throw Error(value->location, "unsupported operator " + binary->operator_ + " for " + left_type);
		auto found = type_ops->second.find(binary->operator_);
		if (found == type_ops->second.end())
			throw Error(value->location, "unsupported operator " + binary->operator_ + " for " + left_type);
		left.insert(left.end(), right.begin(), right.end());
		left.push_back(found->second);
		const bool comparison = binary->operator_ == "==" || binary->operator_ == "!=" || binary->operator_ == "<" || binary->operator_ == ">" ||
								binary->operator_ == "<=" || binary->operator_ == ">=";
		return {left, comparison ? "bool" : left_type};
	}
	if (auto call = dynamic_cast<Call*>(value))
	{
		const Member* member = member_call(call);
		const std::string member_receiver_type = member ? infer(member->value) : "";
		if (member && member_receiver_type.rfind("array<", 0) == 0 &&
			(member->member == "capacity" || member->member == "push" || member->member == "pop" || member->member == "insert" ||
			 member->member == "remove" || member->member == "clear" || member->member == "reserve" || member->member == "resize"))
			return array_method(call, member);
		std::vector<Expr*> method_arguments;
		std::vector<std::unique_ptr<Expr>> module_call_synthetic;
		const std::vector<Expr*>* arguments = &call->arguments;
		bool dynamic_module_member = false;
		std::optional<std::pair<unsigned, std::string>> method_local;
		const Module::HostDeclaration* method_host = nullptr;
		Definition* method_definition = nullptr;
		if (member)
		{
			method_arguments.push_back(member->value);
			method_arguments.insert(method_arguments.end(), call->arguments.begin(), call->arguments.end());
			std::vector<std::string> types;
			for (Expr* argument : method_arguments)
				types.push_back(infer(argument));
			method_local = compatible_local_callable(member->member, types);
			if (!method_local)
				method_host = module_.host(member->member, types);
			if (!method_local && !method_host)
				method_definition = module_.compatible_definition(member->member, types, call->location);
			if (!method_local && !method_host && !method_definition && member_receiver_type == "module" && member->member != "call")
			{
				if (call->arguments.size() > 1)
					throw Error(call->location, "dynamic module member call accepts at most one dval input");
				method_arguments.clear();
				method_arguments.push_back(member->value);
				module_call_synthetic.push_back(std::make_unique<String>(member->location, member->member));
				method_arguments.push_back(module_call_synthetic.back().get());
				method_arguments.insert(method_arguments.end(), call->arguments.begin(), call->arguments.end());
				types.clear();
				for (Expr* argument : method_arguments)
					types.push_back(infer(argument));
				method_definition = module_.compatible_definition("call", types, call->location);
				dynamic_module_member = true;
			}
			if (method_local || method_host || method_definition || dynamic_module_member)
				arguments = &method_arguments;
		}
		auto named = dynamic_cast<Name*>(call->function);
		std::vector<Expr*> indirect_arguments;
		std::vector<std::unique_ptr<Expr>> indirect_synthetic;
		ArrayLiteral indirect_pack(value->location);
		std::unique_ptr<Name> indirect_element_type;
		auto prepare_indirect_arguments = [&](unsigned type) -> const std::vector<Expr*>*
		{
			auto variadic = module_.variadic_function_types_.find(type);
			if (variadic == module_.variadic_function_types_.end()) return arguments;
			const auto& contract = variadic->second;
			for (std::size_t i = 0; i < contract.fixed; ++i) indirect_arguments.push_back((*arguments)[i]);
			const std::string element_name = contract.element.rfind("struct:", 0) == 0 ? contract.element.substr(7) : contract.element;
			indirect_element_type = std::make_unique<Name>(value->location, element_name);
			indirect_pack.explicit_element_type = indirect_element_type.get();
			for (std::size_t i = contract.fixed; i < arguments->size(); ++i)
			{
				Expr* item = (*arguments)[i];
				std::string source = infer(item);
				const bool spread = source.rfind("spread<", 0) == 0;
				if (spread) source = normalize_spread_type(source);
				if (source != contract.element)
				{
					if (spread)
					{
						auto converted_spread = std::make_unique<Spread>(item->location, static_cast<Spread*>(item)->value);
						converted_spread->target_element_type = indirect_element_type.get();
						item = converted_spread.get();
						indirect_synthetic.push_back(std::move(converted_spread));
					}
					else
					{
						auto constructor = std::make_unique<Name>(item->location, element_name);
						auto converted = std::make_unique<Call>(item->location, constructor.get()); converted->arguments.push_back(item); item = converted.get();
						indirect_synthetic.push_back(std::move(constructor)); indirect_synthetic.push_back(std::move(converted));
					}
				}
				indirect_pack.items.push_back(item);
			}
			indirect_arguments.push_back(&indirect_pack);
			return &indirect_arguments;
		};
		if (!named && (!member || (!method_local && !method_host && !method_definition && !dynamic_module_member)))
		{
			auto [function_code, function_type] = expression(call->function);
			if (function_type.rfind("function#", 0) != 0)
				throw Error(call->function->location, "call target is not a function value");
			const unsigned type = static_cast<unsigned>(std::stoul(function_type.substr(9)));
			const auto& signature = module_.types_.at(type);
			const std::vector<Expr*>* indirect = prepare_indirect_arguments(type);
			if (signature.first.size() != indirect->size() + 1)
				throw Error(call->location, "function value argument count does not match signature");
			const unsigned closure = add_local("", function_type, call->function->location);
			Bytes code = std::move(function_code);
			code.push_back(0x21);
			wasm::append_uleb(code, closure);
			code.push_back(0x20);
			wasm::append_uleb(code, closure);
			std::vector<unsigned> owned_arguments;
			for (std::size_t i = 0; i < indirect->size(); ++i)
			{
				auto compiled = dynamic_cast<Integer*>((*indirect)[i]) && integer_type(signature.first[i + 1])
					? integer_expression(static_cast<Integer*>((*indirect)[i]), signature.first[i + 1]) : expression((*indirect)[i]);
				auto argument = std::move(compiled.first);
				const std::string actual = std::move(compiled.second);
				if (actual != signature.first[i + 1])
					throw Error((*indirect)[i]->location, "function value argument type does not match signature");
				if (managed_type(actual) && expression_is_owned((*indirect)[i]))
				{
					const unsigned temporary = add_local("", actual, (*indirect)[i]->location);
					append(code, argument);
					code.push_back(0x21);
					wasm::append_uleb(code, temporary);
					code.push_back(0x20);
					wasm::append_uleb(code, temporary);
					owned_arguments.push_back(temporary);
				}
				else
					append(code, argument);
			}
			code.push_back(0x20);
			wasm::append_uleb(code, closure);
			code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x11});
			wasm::append_uleb(code, type);
			code.push_back(0);
			const bool owned_closure = expression_is_owned(call->function);
			if (managed_type(signature.second))
			{
				const unsigned result = add_local("", signature.second, value->location);
				code.push_back(0x21);
				wasm::append_uleb(code, result);
				for (auto it = owned_arguments.rbegin(); it != owned_arguments.rend(); ++it)
				{
					code.push_back(0x20);
					wasm::append_uleb(code, *it);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.release_index());
				}
				if (owned_closure)
				{
					code.push_back(0x20);
					wasm::append_uleb(code, closure);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.release_index());
				}
				code.push_back(0x20);
				wasm::append_uleb(code, result);
			}
			else
			{
				for (auto it = owned_arguments.rbegin(); it != owned_arguments.rend(); ++it)
				{
					code.push_back(0x20);
					wasm::append_uleb(code, *it);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.release_index());
				}
				if (owned_closure)
				{
					code.push_back(0x20);
					wasm::append_uleb(code, closure);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.release_index());
				}
			}
			return {code, signature.second};
		}
		std::string callee = dynamic_module_member ? "call" : member ? member->member : named->value;
		if (!member || method_local)
		for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
			if (auto found = scope->find(callee); found != scope->end() && found->second.second.rfind("function#", 0) == 0)
			{
				const unsigned type = static_cast<unsigned>(std::stoul(found->second.second.substr(9)));
				if (type >= module_.types_.size())
					throw Error(value->location, "invalid function value type");
				const auto& signature = module_.types_[type];
				arguments = prepare_indirect_arguments(type);
				if (signature.first.size() != arguments->size() + 1)
					throw Error(value->location, "function value argument count does not match signature");
				Bytes code{0x20};
				wasm::append_uleb(code, found->second.first);
				std::vector<unsigned> owned_arguments;
				for (std::size_t i = 0; i < arguments->size(); ++i)
				{
					auto compiled = dynamic_cast<Integer*>((*arguments)[i]) && integer_type(signature.first[i + 1])
						? integer_expression(static_cast<Integer*>((*arguments)[i]), signature.first[i + 1]) : expression((*arguments)[i]);
					auto argument = std::move(compiled.first);
					const std::string actual = std::move(compiled.second);
					if (actual != signature.first[i + 1])
						throw Error((*arguments)[i]->location, "function value argument type does not match signature");
					if (managed_type(actual) && expression_is_owned((*arguments)[i]))
					{
						const unsigned temporary = add_local("", actual, (*arguments)[i]->location);
						append(code, argument);
						code.push_back(0x21);
						wasm::append_uleb(code, temporary);
						code.push_back(0x20);
						wasm::append_uleb(code, temporary);
						owned_arguments.push_back(temporary);
					}
					else
						append(code, argument);
				}
				code.push_back(0x20);
				wasm::append_uleb(code, found->second.first);
				code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x11});
				wasm::append_uleb(code, type);
				code.push_back(0);
				if (managed_type(signature.second))
				{
					const unsigned result = add_local("", signature.second, value->location);
					code.push_back(0x21);
					wasm::append_uleb(code, result);
					for (auto argument = owned_arguments.rbegin(); argument != owned_arguments.rend(); ++argument)
					{
						code.push_back(0x20);
						wasm::append_uleb(code, *argument);
						code.push_back(0x10);
						wasm::append_uleb(code, module_.release_index());
					}
					code.push_back(0x20);
					wasm::append_uleb(code, result);
				}
				else
					for (auto argument = owned_arguments.rbegin(); argument != owned_arguments.rend(); ++argument)
					{
						code.push_back(0x20);
						wasm::append_uleb(code, *argument);
						code.push_back(0x10);
						wasm::append_uleb(code, module_.release_index());
					}
				return {code, signature.second};
			}
		if (!member && module_.has_alias(callee))
			callee = module_.constructor_name(callee, call->location);
		if (!member && (callee == "sqrt" || callee == "abs" || callee == "neg" || callee == "floor" || callee == "ceil" ||
			callee == "trunc" || callee == "nearest" || callee == "min" || callee == "max" || callee == "copysign" ||
			callee == "is_nan" || callee == "is_infinite" || callee == "is_finite" || callee == "bit_and" || callee == "bit_or" ||
			callee == "bit_xor" || callee == "bit_not" || callee == "bit_shl" || callee == "bit_shr" || callee == "bit_rotate_left" ||
			callee == "bit_rotate_right" || callee == "bit_count_lz" || callee == "bit_count_tz" || callee == "bit_count_1" ||
			callee == "bits_of" || callee == "f32_from_bits" || callee == "f64_from_bits" || callee == "clamp"))
		{
			std::vector<std::string> types;
			for (Expr* argument : *arguments)
				types.push_back(dynamic_cast<Integer*>(argument) ? "s64" : infer(argument));
			if (auto contextual = module_.contextual_argument_types(callee, *arguments, types, call->location))
				types = *contextual;
			auto integer_opcode_type = [](const std::string& type) -> std::string { return type == "s8" || type == "s16" || type == "s32" || type == "u8" || type == "u16" || type == "u32" ? "i32" : "i64"; };
			auto integer = [](const std::string& type) { return type == "s8" || type == "s16" || type == "s32" || type == "s64" || type == "u8" || type == "u16" || type == "u32" || type == "u64"; };
			auto emit_arguments = [&]()
			{
				Bytes code;
				for (std::size_t i = 0; i < arguments->size(); ++i)
				{
					auto compiled = dynamic_cast<Integer*>((*arguments)[i]) ? integer_expression(static_cast<Integer*>((*arguments)[i]), types[i]) : expression((*arguments)[i]);
					if (compiled.second != types[i]) throw Error((*arguments)[i]->location, "expected " + types[i] + ", found " + compiled.second);
					append(code, compiled.first);
				}
				return code;
			};
			auto normalize = [&](Bytes& code, const std::string& type)
			{
				if (type == "s8") code.push_back(0xc0);
				else if (type == "s16") code.push_back(0xc1);
				else if (type == "u8") { code.push_back(0x41); code.push_back(0xff); code.push_back(0x01); code.push_back(0x71); }
				else if (type == "u16") { code.push_back(0x41); code.push_back(0xff); code.push_back(0xff); code.push_back(0x03); code.push_back(0x71); }
			};
			if (callee == "bits_of" || callee == "f32_from_bits" || callee == "f64_from_bits")
			{
				Bytes code = emit_arguments();
				if (callee == "bits_of") code.push_back(types[0] == "f32" ? 0xbc : 0xbd);
				else code.push_back(types[0] == "u32" ? 0xbe : 0xbf);
				return {code, callee == "bits_of" ? (types[0] == "f32" ? "u32" : "u64") : (types[0] == "u32" ? "f32" : "f64")};
			}
			if (callee == "is_nan" || callee == "is_infinite" || callee == "is_finite")
			{
				if (types.size() != 1 || (types[0] != "f32" && types[0] != "f64")) throw Error(call->location, callee + " expects one float");
				Bytes code = emit_arguments();
				const bool f32 = types[0] == "f32";
				if (callee == "is_nan")
				{
					const unsigned temp = add_local("", types[0], call->location);
					code.push_back(0x21); wasm::append_uleb(code, temp);
					code.push_back(0x20); wasm::append_uleb(code, temp);
					code.push_back(0x20); wasm::append_uleb(code, temp);
					code.push_back(f32 ? 0x5c : 0x62);
					return {code, "bool"};
				}
				else
				{
					const unsigned temp = add_local("", types[0], call->location); code.push_back(0x21); wasm::append_uleb(code, temp);
					code.push_back(0x20); wasm::append_uleb(code, temp); code.push_back(f32 ? 0x8b : 0x99);
					code.push_back(f32 ? 0x43 : 0x44); if (f32) wasm::append_f32(code, std::numeric_limits<float>::infinity()); else wasm::append_f64(code, std::numeric_limits<double>::infinity());
					code.push_back(callee == "is_infinite" ? (f32 ? 0x5b : 0x61) : (f32 ? 0x5d : 0x63));
					return {code, "bool"};
				}
				code.push_back(f32 ? 0x5c : 0x62); return {code, "bool"};
			}
			if (callee == "clamp")
			{
				if (types.size() != 3 || types[0] != types[1] || types[1] != types[2]) throw Error(call->location, "clamp expects three values of one numeric type");
				Bytes code = emit_arguments();
				const std::string type = types[0]; const bool f32 = type == "f32", is_int = integer(type);
				const unsigned high = add_local("", type, call->location), low = add_local("", type, call->location), value_local = add_local("", type, call->location);
				code.push_back(0x21); wasm::append_uleb(code, high); code.push_back(0x21); wasm::append_uleb(code, low); code.push_back(0x21); wasm::append_uleb(code, value_local);
				auto select = [&](unsigned first, unsigned second, std::uint8_t compare)
				{
					code.push_back(0x20); wasm::append_uleb(code, first);
					code.push_back(0x20); wasm::append_uleb(code, second);
					code.push_back(0x20); wasm::append_uleb(code, first);
					code.push_back(0x20); wasm::append_uleb(code, second);
					code.push_back(compare);
					code.push_back(0x1b);
				};
				const bool signed_type = type[0] == 's';
				const bool wide = integer_opcode_type(type) == "i64";
				const std::uint8_t greater = is_int ? (signed_type ? (wide ? 0x55 : 0x4a) : (wide ? 0x56 : 0x4b)) : (f32 ? 0x5e : 0x64);
				const std::uint8_t less = is_int ? (signed_type ? (wide ? 0x53 : 0x48) : (wide ? 0x54 : 0x49)) : (f32 ? 0x5d : 0x63);
				select(low, value_local, greater);
				const unsigned bounded = add_local("", type, call->location);
				code.push_back(0x21); wasm::append_uleb(code, bounded);
				select(high, bounded, less);
				normalize(code, type);
				return {code, type};
			}
			const bool float_type = !types.empty() && (types[0] == "f32" || types[0] == "f64");
			if (float_type)
			{
				Bytes code = emit_arguments();
				const bool f32 = types[0] == "f32";
				static const std::map<std::string, std::pair<std::uint8_t, std::uint8_t>> unary = {{"sqrt", {0x91, 0x9f}}, {"abs", {0x8b, 0x99}}, {"neg", {0x8c, 0x9a}}, {"floor", {0x8e, 0x9c}}, {"ceil", {0x8d, 0x9b}}, {"trunc", {0x8f, 0x9d}}, {"nearest", {0x90, 0x9e}}};
				if (auto op = unary.find(callee); op != unary.end()) { code.push_back(f32 ? op->second.first : op->second.second); return {code, types[0]}; }
				if (callee == "min" || callee == "max") { code.push_back(f32 ? (callee == "min" ? 0x96 : 0x97) : (callee == "min" ? 0xa4 : 0xa5)); return {code, types[0]}; }
				if (callee == "copysign") { code.push_back(f32 ? 0x98 : 0xa6); return {code, types[0]}; }
				throw Error(call->location, "unsupported float builtin " + callee);
			}
			if (types.empty() || !integer(types[0])) throw Error(call->location, callee + " expects integer values");
			const std::string type = types[0], wasm_type = integer_opcode_type(type); const bool wide = wasm_type == "i64", signed_type = type[0] == 's';
			Bytes code = emit_arguments();
			const std::uint8_t bit_and = wide ? 0x83 : 0x71, bit_or = wide ? 0x84 : 0x72, bit_xor = wide ? 0x85 : 0x73;
			if (callee == "abs")
			{
				if (!signed_type) return {code, type};
				const unsigned temp = add_local("", type, call->location); code.push_back(0x21); wasm::append_uleb(code, temp);
				code.push_back(wide ? 0x42 : 0x41); if (wide) wasm::append_sleb64(code, 0); else code.push_back(0x00);
				code.push_back(0x20); wasm::append_uleb(code, temp); code.push_back(wide ? 0x7d : 0x6b);
				code.push_back(0x20); wasm::append_uleb(code, temp);
				code.push_back(0x20); wasm::append_uleb(code, temp); code.push_back(wide ? 0x42 : 0x41); if (wide) wasm::append_sleb64(code, 0); else code.push_back(0x00); code.push_back(wide ? 0x53 : 0x48); code.push_back(0x1b); normalize(code, type); return {code, type};
			}
			if (callee == "bit_and" || callee == "bit_or" || callee == "bit_xor") code.push_back(callee == "bit_and" ? bit_and : callee == "bit_or" ? bit_or : bit_xor);
			else if (callee == "bit_not") { code.push_back(wide ? 0x42 : 0x41); if (wide) wasm::append_sleb64(code, -1); else { code.push_back(0x7f); } code.push_back(bit_xor); }
			else if (callee == "bit_shl" || callee == "bit_shr") code.push_back(callee == "bit_shl" ? (wide ? 0x86 : 0x74) : signed_type ? (wide ? 0x87 : 0x75) : (wide ? 0x88 : 0x76));
			else if (callee == "bit_count_lz" || callee == "bit_count_tz" || callee == "bit_count_1")
			{
				if (!wide && (type == "s8" || type == "u8" || type == "s16" || type == "u16")) { code.push_back(0x41); if (type.ends_with("8")) { code.push_back(0xff); code.push_back(0x01); } else { code.push_back(0xff); code.push_back(0xff); code.push_back(0x03); } code.push_back(0x71); }
				if (callee == "bit_count_lz" && !wide && (type == "s8" || type == "u8" || type == "s16" || type == "u16"))
				{
					code.push_back(0x67);
					code.push_back(0x41); code.push_back(type.ends_with("8") ? 24 : 16);
					code.push_back(0x6b);
				}
				else
					code.push_back(callee == "bit_count_lz" ? (wide ? 0x79 : 0x67) : callee == "bit_count_tz" ? (wide ? 0x7a : 0x68) : (wide ? 0x7b : 0x69));
				if (callee == "bit_count_tz" && !wide && (type == "s8" || type == "u8" || type == "s16" || type == "u16"))
				{
					const unsigned count = add_local("", "s32", call->location);
					code.push_back(0x21); wasm::append_uleb(code, count);
					code.push_back(0x41); code.push_back(type.ends_with("8") ? 8 : 16);
					code.push_back(0x20); wasm::append_uleb(code, count);
					code.push_back(0x20); wasm::append_uleb(code, count);
					code.push_back(0x41); code.push_back(32);
					code.push_back(0x46);
					code.push_back(0x1b);
				}
				if (!wide) code.push_back(0xac);
				return {code, "s64"};
			}
			else if (callee == "bit_rotate_left" || callee == "bit_rotate_right")
			{
				const unsigned width = wide ? 64 : type.ends_with("8") ? 8 : type.ends_with("16") ? 16 : 32;
				const unsigned value_local = add_local("", type, call->location), count_local = add_local("", type, call->location);
				code.push_back(0x21); wasm::append_uleb(code, count_local); code.push_back(0x21); wasm::append_uleb(code, value_local);
				if (!wide && (type == "s8" || type == "u8" || type == "s16" || type == "u16")) { code.push_back(0x20); wasm::append_uleb(code, value_local); code.push_back(0x41); if (type.ends_with("8")) { code.push_back(0xff); code.push_back(0x01); } else { code.push_back(0xff); code.push_back(0xff); code.push_back(0x03); } code.push_back(0x71); code.push_back(0x21); wasm::append_uleb(code, value_local); }
				code.push_back(0x20); wasm::append_uleb(code, count_local); code.push_back(wide ? 0x42 : 0x41); if (wide) wasm::append_sleb64(code, width - 1); else code.push_back(width - 1); code.push_back(wide ? 0x83 : 0x71); code.push_back(0x21); wasm::append_uleb(code, count_local);
				auto shift = [&](bool left) { code.push_back(0x20); wasm::append_uleb(code, value_local); code.push_back(0x20); wasm::append_uleb(code, count_local); code.push_back(left ? (wide ? 0x86 : 0x74) : (wide ? 0x88 : 0x76)); };
				if (callee == "bit_rotate_left") { shift(true); code.push_back(0x20); wasm::append_uleb(code, value_local); code.push_back(wide ? 0x42 : 0x41); if (wide) wasm::append_sleb64(code, width); else code.push_back(width); code.push_back(0x20); wasm::append_uleb(code, count_local); code.push_back(wide ? 0x7d : 0x6b); code.push_back(wide ? 0x88 : 0x76); }
				else { shift(false); code.push_back(0x20); wasm::append_uleb(code, value_local); code.push_back(wide ? 0x42 : 0x41); if (wide) wasm::append_sleb64(code, width); else code.push_back(width); code.push_back(0x20); wasm::append_uleb(code, count_local); code.push_back(wide ? 0x7d : 0x6b); code.push_back(wide ? 0x86 : 0x74); }
				code.push_back(wide ? 0x85 : 0x73);
			}
			else if (callee == "min" || callee == "max")
			{
				const unsigned right = add_local("", type, call->location), left = add_local("", type, call->location);
				code.push_back(0x21); wasm::append_uleb(code, right); code.push_back(0x21); wasm::append_uleb(code, left);
				code.push_back(0x20); wasm::append_uleb(code, left); code.push_back(0x20); wasm::append_uleb(code, right);
				code.push_back(0x20); wasm::append_uleb(code, left); code.push_back(0x20); wasm::append_uleb(code, right);
				code.push_back(type[0] == 's' ? (wide ? (callee == "min" ? 0x53 : 0x55) : (callee == "min" ? 0x48 : 0x4a)) : (wide ? (callee == "min" ? 0x54 : 0x56) : (callee == "min" ? 0x49 : 0x4b)));
				code.push_back(0x1b);
			}
			else throw Error(call->location, "unsupported integer builtin " + callee);
			normalize(code, type);
			return {code, type};
		}
		if (!member && callee == "__bearer_byte")
		{
			if (call->arguments.size() != 1) throw Error(call->location, "__bearer_byte expects one integer value");
			auto argument = expression(call->arguments[0]);
			if (!integer_type(argument.second)) throw Error(call->arguments[0]->location, "__bearer_byte expects an integer value");
			argument.first.insert(argument.first.end(), {0xa7, 0x41, 0xff, 0x01, 0x71});
			return {std::move(argument.first), "u8"};
		}
		if (!member && primitive_constructor_name(callee) && call->arguments.size() == 1)
		{
			std::string source;
			if (auto integer = dynamic_cast<Integer*>(call->arguments[0]); integer && integer_type(callee))
				source = infer_integer(integer, integer_fits(*integer, "s64") ? "" : callee);
			else
				source = infer(call->arguments[0]);
			if (source == callee)
			{
				if (auto integer = dynamic_cast<Integer*>(call->arguments[0]))
					return integer_expression(integer, callee);
				return expression(call->arguments[0]);
			}
			if (((callee == "string" && source != "string" && call->arguments.size() == 1) ||
				 (!module_.exact_definition(callee, {source}) && !module_.default_definition(callee, {source}, call->location))) && can_convert(source, callee))
			{
				auto argument = expression(call->arguments[0]);
				return conversion(std::move(argument.first), source, callee, call->location, expression_is_owned(call->arguments[0]));
			}
		}
		const bool generated_struct = !member && module_.has_struct(callee) && [&]
		{
			const auto& fields = module_.struct_type(callee, named->location).fields;
			if (arguments->size() != fields.size())
				return false;
			for (std::size_t i = 0; i < fields.size(); ++i)
			{
				const bool contextual_integer = dynamic_cast<Integer*>((*arguments)[i]) && integer_type(fields[i].second);
				const std::string actual = contextual_integer ? infer_integer(static_cast<Integer*>((*arguments)[i]), fields[i].second) : infer((*arguments)[i]);
				if (actual != fields[i].second) return false;
			}
			return true;
		}();
		if (generated_struct)
		{
			const auto& aggregate = module_.struct_type(callee, named->location);
			std::vector<std::string> field_types;
			for (const auto& [name, field_type] : aggregate.fields)
				field_types.push_back(field_type);
			const AggregateLayout layout = aggregate_layout(field_types, 16);
			const std::string type = "struct:" + callee;
			const unsigned pointer = add_local("", type, value->location);
			Bytes code{0x41};
			wasm::append_sleb32(code, static_cast<std::int32_t>(layout.size));
			code.push_back(0x10);
			wasm::append_uleb(code, module_.import_index("bearer_alloc"));
			code.push_back(0x21);
			wasm::append_uleb(code, pointer);
			code.push_back(0x20);
			wasm::append_uleb(code, pointer);
			code.insert(code.end(), {0x45, 0x04, 0x40, 0x00, 0x0b});
			for (const auto [header, offset] : {std::pair<std::int32_t, unsigned>{1, 0},
												{1, 4},
												{static_cast<std::int32_t>(aggregate.type_id), 8},
												{static_cast<std::int32_t>(layout.size), 12}})
				store_i32_constant(code, pointer, header, offset);
			code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
			for (std::size_t i = 0; i < arguments->size(); ++i)
			{
				auto compiled = dynamic_cast<Integer*>((*arguments)[i]) ? integer_expression(static_cast<Integer*>((*arguments)[i]), aggregate.fields[i].second) : expression((*arguments)[i]);
				auto field = std::move(compiled.first);
				const std::string actual = std::move(compiled.second);
				if (actual != aggregate.fields[i].second)
					throw Error((*arguments)[i]->location, "expected " + aggregate.fields[i].second + ", found " + actual);
				const unsigned temporary = add_local("", actual, (*arguments)[i]->location);
				append(code, field);
				code.push_back(0x21);
				wasm::append_uleb(code, temporary);
				if (managed_type(actual) && !expression_is_owned((*arguments)[i]))
				{
					code.push_back(0x20);
					wasm::append_uleb(code, temporary);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.retain_index());
				}
				code.push_back(0x20);
				wasm::append_uleb(code, pointer);
				code.push_back(0x20);
				wasm::append_uleb(code, temporary);
				store_field(code, actual, layout.offsets[i]);
			}
			code.push_back(0x20);
			wasm::append_uleb(code, pointer);
			return {code, type};
		}
		if (!member && (callee == "string_from_bytes" || callee == "bytes_of"))
			return byte_conversion(call, callee == "string_from_bytes");
		if (!member && callee == "length")
		{
			if (call->arguments.size() != 1)
				throw Error(value->location, "length expects one string, array, or dval");
			auto [code, type] = expression(call->arguments[0]);
			if (type != "string" && type != "dval" && type.rfind("array<", 0) != 0)
				throw Error(call->arguments[0]->location, "length expects a string, array, or dval, found " + type);
			const unsigned source = add_local("", type, call->arguments[0]->location), result = add_local("", "s32", value->location);
			code.push_back(0x21); wasm::append_uleb(code, source);
			if (type == "dval")
			{
				managed_payload_span(code, source, "dval");
				code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_dv_count_brrb"));
				code.push_back(0x22); wasm::append_uleb(code, result);
				code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40}); append(code, module_.marker(call->arguments[0]->location)); code.insert(code.end(), {0x00, 0x0b});
			}
			else
			{
				code.push_back(0x20); wasm::append_uleb(code, source);
				code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21}); wasm::append_uleb(code, result);
			}
			if (expression_is_owned(call->arguments[0])) { code.push_back(0x20); wasm::append_uleb(code, source); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
			code.push_back(0x20); wasm::append_uleb(code, result); code.push_back(0xad);
			return {code, "s64"};
		}
		if (!member && callee == "dval")
		{
			if (call->arguments.size() != 1)
				throw Error(value->location, "dval expects one scalar, map, or list");
			return dval_value(call->arguments[0]);
		}
		if (!member && callee == "has")
		{
			if (call->arguments.size() != 2)
				throw Error(value->location, "has expects dval and string/s64 key");
			return dval_lookup(call->arguments[0], call->arguments[1], false);
		}
		if (!member && callee == "__bearer_dval_replace")
		{
			if (call->arguments.size() != 2 || infer(call->arguments[0]) != "dval" || infer(call->arguments[1]) != "dval")
				throw Error(value->location, "__bearer_dval_replace expects two dval values");
			auto target_code = expression(call->arguments[0]);
			auto replacement_code = expression(call->arguments[1]);
			const unsigned target = add_local("", "dval", call->arguments[0]->location);
			const unsigned replacement = add_local("", "dval", call->arguments[1]->location);
			Bytes code = std::move(target_code.first); code.push_back(0x21); wasm::append_uleb(code, target);
			append(code, replacement_code.first); code.push_back(0x21); wasm::append_uleb(code, replacement);
			if (!expression_is_owned(call->arguments[1]))
			{
				code.push_back(0x20); wasm::append_uleb(code, target); code.push_back(0x20); wasm::append_uleb(code, replacement);
				code.insert(code.end(), {0x47, 0x04, 0x40, 0x20}); wasm::append_uleb(code, replacement);
				code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); code.push_back(0x0b);
			}
			append(code, dval_replace(target, replacement, call->location, expression_is_owned(call->arguments[1])));
			if (expression_is_owned(call->arguments[0])) owned_expression_results_.insert(value);
			return {code, "dval"};
		}
		if (!member && callee == "clone")
		{
			if (call->arguments.size() != 1)
				throw Error(value->location, "clone expects one string");
			auto [source, type] = expression(call->arguments[0]);
			if (type != "string")
				throw Error(call->arguments[0]->location, "expected string, found " + type);
			Bytes code = std::move(source);
			const unsigned input = add_local("", "string", value->location);
			code.push_back(0x21); wasm::append_uleb(code, input);
			code.push_back(0x20); wasm::append_uleb(code, input);
			code.push_back(0x10); wasm::append_uleb(code, module_.clone_index());
			if (expression_is_owned(call->arguments[0]))
			{
				code.push_back(0x20); wasm::append_uleb(code, input);
				code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
			}
			return {code, "string"};
		}
		if (!member && callee == "arc_live")
		{
			if (!call->arguments.empty())
				throw Error(value->location, "arc_live expects no arguments");
			return {{0x23, 0x01, 0xad}, "s64"};
		}
		if (!member && callee == "trap")
		{
			if (!call->arguments.empty())
				throw Error(value->location, "trap expects no arguments");
			Bytes code = cleanup_scopes();
			code.push_back(0x00);
			return {code, "void"};
		}
		std::vector<std::string> types;
		for (Expr* argument : *arguments)
			types.push_back(dynamic_cast<Integer*>(argument) ? "s64" : infer(argument));
		const bool has_contextual_literal = std::any_of(arguments->begin(), arguments->end(), [](Expr* argument)
		{
			return dynamic_cast<Integer*>(argument) || dynamic_cast<Float*>(argument);
		});
		std::optional<std::vector<std::string>> contextual = has_contextual_literal && !member
			? module_.contextual_argument_types(callee, *arguments, types, call->location) : std::nullopt;
		if (contextual) types = *contextual;
		const Module::HostDeclaration* selected_host = member ? method_host : module_.host(callee, types);
		Definition* selected_definition = member ? method_definition : module_.compatible_definition(callee, types, call->location);
		if (!selected_host && !selected_definition && !contextual)
			if (auto fallback = module_.contextual_argument_types(callee, *arguments, types, call->location))
			{
				types = *fallback;
				selected_host = module_.host(callee, types);
				selected_definition = module_.compatible_definition(callee, types, call->location);
			}
		if (const Module::HostDeclaration* host = selected_host)
		{
			std::vector<Bytes> argument_code;
			for (std::size_t i = 0; i < arguments->size(); ++i)
				if (auto integer = dynamic_cast<Integer*>((*arguments)[i])) argument_code.push_back(integer_expression(integer, types[i]).first);
				else if (auto floating = dynamic_cast<Float*>((*arguments)[i])) argument_code.push_back(float_expression(floating, types[i]).first);
				else argument_code.push_back(expression((*arguments)[i]).first);
			Bytes code;
			std::vector<unsigned> locals, owned_arguments;
			for (std::size_t i = 0; i < argument_code.size(); ++i)
			{
				const unsigned local = add_local("", types[i], (*arguments)[i]->location);
				append(code, argument_code[i]);
				code.push_back(0x21); wasm::append_uleb(code, local);
				locals.push_back(local);
				if (managed_type(types[i]) && expression_is_owned((*arguments)[i]))
					owned_arguments.push_back(local);
			}
			auto append_release_inputs = [&](Bytes& target)
			{
				for (auto it = owned_arguments.rbegin(); it != owned_arguments.rend(); ++it)
				{
					target.push_back(0x20); wasm::append_uleb(target, *it);
					target.push_back(0x10); wasm::append_uleb(target, module_.release_index());
				}
			};
			auto release_inputs = [&] { append_release_inputs(code); };
			auto inputs = [&]
			{
				for (std::size_t i = 0; i < locals.size(); ++i)
				{
					if (types[i] == "string" || types[i] == "dval")
						managed_payload_span(code, locals[i], types[i]);
					else
					{
						code.push_back(0x20); wasm::append_uleb(code, locals[i]);
					}
				}
				if (host->trace)
				{
					code.push_back(0x23); wasm::append_uleb(code, 0);
					code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(module_.trace_stack_offset_));
					code.push_back(0x6a);
					code.push_back(0x23); wasm::append_uleb(code, module_.use_arc_global_ ? 2 : 1);
				}
			};
			const bool sized = host->result == "string" || host->result == "dval";
			if (!sized)
			{
				inputs(); code.push_back(0x10); wasm::append_uleb(code, module_.import_index(host->symbol));
				if (managed_type(host->result))
				{
					const unsigned result = add_local("", host->result, value->location);
					code.push_back(0x21); wasm::append_uleb(code, result);
					release_inputs();
					code.push_back(0x20); wasm::append_uleb(code, result);
				}
				else
					release_inputs();
				return {code, host->result};
			}
			inputs(); code.insert(code.end(), {0x41, 0x00, 0x41, 0x00, 0x10}); wasm::append_uleb(code, module_.import_index(host->symbol));
			const unsigned length = add_local("", "s32", value->location);
			code.push_back(0x21); wasm::append_uleb(code, length);
			// A failed sizing pass returns SIZE_MAX as i32 -1. Release owned inputs
			// before trapping at the user callsite rather than relying on reset cleanup.
			code.push_back(0x20); wasm::append_uleb(code, length);
			code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
			release_inputs(); append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b});
			Bytes allocation_cleanup; append_release_inputs(allocation_cleanup);
			auto [allocation, pointer] = allocate_blob(host->result, host->result == "string" ? 1 : 4, length, value->location, allocation_cleanup);
			append(code, allocation); inputs();
			managed_payload_pointer(code, pointer, host->result);
			code.push_back(0x20); wasm::append_uleb(code, length);
			code.push_back(0x10); wasm::append_uleb(code, module_.import_index(host->symbol));
			code.push_back(0x20); wasm::append_uleb(code, length);
			code.insert(code.end(), {0x47, 0x04, 0x40, 0x20}); wasm::append_uleb(code, pointer);
			code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
			release_inputs(); append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b});
			const unsigned result = add_local("", host->result, value->location);
			code.push_back(0x20); wasm::append_uleb(code, pointer);
			code.push_back(0x21); wasm::append_uleb(code, result);
			if (host->result == "dval") retain_dval_callables(code, result, value->location);
			release_inputs();
			code.push_back(0x20); wasm::append_uleb(code, result);
			return {code, host->result};
		}
		Definition& target = selected_definition ? *selected_definition : module_.resolve(callee, types, value->location);
		std::vector<Expr*> default_arguments;
		std::vector<std::unique_ptr<Expr>> default_synthetic;
		if (arguments->size() < target.parameters.size())
		{
			default_arguments = *arguments;
			for (std::size_t i = arguments->size(); i < target.parameters.size(); ++i)
			{
				if (i >= target.default_values.size() || !target.default_values[i])
					throw std::runtime_error("resolved Capy call omits a required parameter");
				auto default_value = callsite_default_literal(target.default_values[i], value->location, target.parameters[i]);
				types.push_back(literal_type(default_value.get(), target.parameters[i]));
				default_arguments.push_back(default_value.get());
				default_synthetic.push_back(std::move(default_value));
			}
			arguments = &default_arguments;
		}
		if (!member && target.inline_value && arguments->empty())
			return expression(target.inline_value);
		auto fused_variadic_sink = [&]() -> std::optional<std::pair<Bytes, std::string>>
		{
			const Module::HostDeclaration* sink = module_.fused_variadic_sink(target);
			if (!sink) return std::nullopt;
			Bytes code;
			if (target.function->location.file == "capy://stdlib.capy") append(code, module_.marker(value->location));
			auto emit_sink = [&](unsigned item)
			{
				code.push_back(0x20); wasm::append_uleb(code, item); code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20}); wasm::append_uleb(code, item);
				code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x10}); wasm::append_uleb(code, module_.import_index(sink->symbol));
			};
			auto emit_formatted_sink = [&](unsigned item, const std::string& source)
			{
				const std::string helper_type = source == "s32" ? "s64" : source;
				code.push_back(0x20); wasm::append_uleb(code, item);
				if (source == "s32") code.push_back(0xac);
				code.push_back(0x10); wasm::append_uleb(code, module_.helper_index(sink_format_helper(sink->symbol, helper_type)));
			};
			for (std::size_t i = 0; i < arguments->size(); ++i)
			{
				if (auto spread = dynamic_cast<Spread*>((*arguments)[i]))
				{
					const std::string spread_type = infer(spread->value);
					if (spread_type.rfind("array<", 0) != 0) return std::nullopt;
					const std::string source_element = spread_type.substr(6, spread_type.size() - 7);
					if (source_element != "string" && !target.variadic_convert) return std::nullopt;
					const unsigned source_size = array_element_size(source_element);
					auto source = expression(spread->value);
					const unsigned array = add_local("", spread_type, spread->location), length = add_local("", "s32", spread->location);
					const unsigned index = add_local("", "s32", spread->location), item = add_local("", source_element, spread->location);
					append(code, source.first); code.push_back(0x21); wasm::append_uleb(code, array); code.push_back(0x20); wasm::append_uleb(code, array);
					code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21}); wasm::append_uleb(code, length); code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, index);
					code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, index); code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4f, 0x0d, 0x01, 0x20}); wasm::append_uleb(code, array);
					code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET}); code.push_back(0x20); wasm::append_uleb(code, index); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(source_size)); code.insert(code.end(), {0x6c, 0x6a});
					code.push_back(array_load_opcode(source_element)); code.push_back(source_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x21}); wasm::append_uleb(code, item);
					if (source_element == "string")
						emit_sink(item);
					else if (source_element == "s32" || source_element == "s64" || source_element == "u64" || source_element == "f64")
						emit_formatted_sink(item, source_element);
					else
					{
						const std::string temporary_name = "\x1fvariadic_spread_" + std::to_string(item);
						scopes_.back()[temporary_name] = {item, source_element};
						Name input(spread->location, temporary_name), constructor(spread->location, "string");
						Call conversion_call(spread->location, &constructor); conversion_call.arguments.push_back(&input);
						auto converted = expression(&conversion_call);
						scopes_.back().erase(temporary_name);
						const unsigned output = add_local("", "string", spread->location);
						append(code, converted.first); code.push_back(0x21); wasm::append_uleb(code, output); emit_sink(output);
						code.push_back(0x20); wasm::append_uleb(code, output); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
					}
					code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, index); code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
					if (expression_is_owned(spread->value)) { code.push_back(0x20); wasm::append_uleb(code, array); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
					continue;
				}
				Expr* argument = (*arguments)[i];
				const std::string source_type = types[i];
				if (source_type == "s32" || source_type == "s64" || source_type == "u64" || source_type == "f64")
				{
					auto compiled = dynamic_cast<Integer*>(argument) ? integer_expression(static_cast<Integer*>(argument), source_type) : expression(argument);
					const unsigned item = add_local("", source_type, argument->location);
					append(code, compiled.first); code.push_back(0x21); wasm::append_uleb(code, item); emit_formatted_sink(item, source_type);
					continue;
				}
				std::unique_ptr<Name> constructor;
				std::unique_ptr<Call> converted;
				if (source_type != target.variadic_element)
				{
					constructor = std::make_unique<Name>(argument->location, target.variadic_element);
					converted = std::make_unique<Call>(argument->location, constructor.get());
					converted->arguments.push_back(argument);
					argument = converted.get();
				}
				auto compiled = expression(argument);
				if (compiled.second != "string") return std::nullopt;
				const unsigned item = add_local("", "string", argument->location);
				append(code, compiled.first); code.push_back(0x21); wasm::append_uleb(code, item); emit_sink(item);
				if (expression_is_owned(argument)) { code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
			}
			return std::pair{std::move(code), std::string("void")};
		};
		if (auto fused = fused_variadic_sink()) return std::move(*fused);
		std::vector<Expr*> packed_arguments;
		std::vector<std::unique_ptr<Expr>> synthetic_arguments;
		ArrayLiteral variadic_pack(value->location);
		Name variadic_type(value->location, target.variadic_element.rfind("struct:", 0) == 0 ? target.variadic_element.substr(7) : target.variadic_element);
		if (target.variadic)
		{
			const std::size_t fixed = target.parameters.size() - 1;
			for (std::size_t i = 0; i < fixed; ++i)
				packed_arguments.push_back((*arguments)[i]);
			variadic_pack.explicit_element_type = &variadic_type;
			for (std::size_t i = fixed; i < arguments->size(); ++i)
			{
				Expr* item = (*arguments)[i];
				const bool spread = types[i].rfind("spread<", 0) == 0;
				const std::string source_type = normalize_spread_type(types[i]);
				if (source_type != target.variadic_element)
				{
					if (spread)
					{
						auto converted_spread = std::make_unique<Spread>(item->location, static_cast<Spread*>(item)->value);
						converted_spread->target_element_type = &variadic_type;
						item = converted_spread.get();
						synthetic_arguments.push_back(std::move(converted_spread));
						variadic_pack.items.push_back(item);
						continue;
					}
					auto constructor = std::make_unique<Name>(item->location, variadic_type.value);
					auto converted = std::make_unique<Call>(item->location, constructor.get());
					converted->arguments.push_back(item);
					item = converted.get();
					synthetic_arguments.push_back(std::move(constructor));
					synthetic_arguments.push_back(std::move(converted));
				}
				variadic_pack.items.push_back(item);
			}
			packed_arguments.push_back(&variadic_pack);
			types.resize(fixed);
			types.push_back("array<" + target.variadic_element + ">");
			arguments = &packed_arguments;
		}
		std::vector<Bytes> argument_code;
		std::vector<bool> converted_owned(types.size(), false);
		for (std::size_t i = 0; i < types.size(); ++i)
		{
				if (types[i] == target.parameters[i])
			{
				if (auto integer = dynamic_cast<Integer*>((*arguments)[i])) argument_code.push_back(integer_expression(integer, types[i]).first);
				else if (auto floating = dynamic_cast<Float*>((*arguments)[i])) argument_code.push_back(float_expression(floating, types[i]).first);
				else if (auto array = dynamic_cast<ArrayLiteral*>((*arguments)[i]); array && types[i].rfind("array<", 0) == 0)
				{
					Name element_type(array->location, types[i].substr(6, types[i].size() - 7));
					ArrayLiteral typed_array(array->location);
					typed_array.items = array->items;
					typed_array.explicit_element_type = &element_type;
					argument_code.push_back(expression(&typed_array).first);
				}
				else argument_code.push_back(expression((*arguments)[i]).first);
				continue;
			}
			if (i >= target.convert.size() || !target.convert[i])
				throw std::runtime_error("resolved Capy call requires an undeclared parameter conversion");
			const std::string constructor_name = target.parameters[i].rfind("struct:", 0) == 0 ? target.parameters[i].substr(7) : target.parameters[i];
			std::pair<Bytes, std::string> converted;
			if (target.function->name == "string" && target.parameters[i] == "string")
			{
				auto source = dynamic_cast<Integer*>((*arguments)[i]) ? integer_expression(static_cast<Integer*>((*arguments)[i]), types[i]) : expression((*arguments)[i]);
				converted = conversion(std::move(source.first), source.second, "string", (*arguments)[i]->location, expression_is_owned((*arguments)[i]));
			}
			else
			{
				Name constructor((*arguments)[i]->location, constructor_name);
				Call converted_call((*arguments)[i]->location, &constructor);
				converted_call.arguments.push_back((*arguments)[i]);
				converted = expression(&converted_call);
			}
			if (converted.second != target.parameters[i])
				throw Error((*arguments)[i]->location, "constructor " + constructor_name + " returned " + converted.second + ", expected " + target.parameters[i]);
			converted_owned[i] = managed_type(converted.second);
			argument_code.push_back(std::move(converted.first));
			types[i] = std::move(converted.second);
		}
		Bytes code;
		std::vector<unsigned> owned_arguments;
		for (std::size_t i = 0; i < argument_code.size(); ++i)
		{
			if (managed_type(types[i]) && (converted_owned[i] || expression_is_owned((*arguments)[i])))
			{
				const unsigned temporary = add_local("", types[i], (*arguments)[i]->location);
				append(code, argument_code[i]);
				code.push_back(0x21);
				wasm::append_uleb(code, temporary);
				code.push_back(0x20);
				wasm::append_uleb(code, temporary);
				owned_arguments.push_back(temporary);
			}
			else
				append(code, argument_code[i]);
		}
		if (target.function && target.function->location.file == "capy://stdlib.capy")
			append(code, module_.marker(value->location));
		code.push_back(0x10);
		wasm::append_uleb(code, target.index);
		if (managed_type(target.result))
		{
			const unsigned result = add_local("", target.result, value->location);
			code.push_back(0x21);
			wasm::append_uleb(code, result);
			for (auto it = owned_arguments.rbegin(); it != owned_arguments.rend(); ++it)
			{
				code.push_back(0x20);
				wasm::append_uleb(code, *it);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.release_index());
			}
			code.push_back(0x20);
			wasm::append_uleb(code, result);
		}
		else
			for (auto it = owned_arguments.rbegin(); it != owned_arguments.rend(); ++it)
			{
				code.push_back(0x20);
				wasm::append_uleb(code, *it);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.release_index());
			}
		return {code, target.result};
	}
	if (dynamic_cast<Yield*>(value))
		throw Error(value->location, "block yield is only valid as a block item");
	if (auto returned = dynamic_cast<Return*>(value))
	{
		Bytes code;
		std::string type = "void";
		if (returned->value)
		{
			ArrayLiteral typed_array(returned->value->location);
			Name element_type(returned->value->location, "");
			Expr* returned_value = returned->value;
			if (auto array = dynamic_cast<ArrayLiteral*>(returned_value); array && definition_.result.rfind("array<", 0) == 0)
			{
				element_type.value = definition_.result.substr(6, definition_.result.size() - 7);
				typed_array.items = array->items;
				typed_array.explicit_element_type = &element_type;
				returned_value = &typed_array;
			}
			auto result = dynamic_cast<Integer*>(returned_value)
				? integer_expression(static_cast<Integer*>(returned_value), definition_.result) : expression(returned_value);
			code = std::move(result.first);
			type = result.second;
		}
		if (type != definition_.result)
			throw Error(value->location, "expected " + definition_.result + ", found " + type);
		if (managed_type(type))
		{
			const unsigned result = add_local("", type, value->location);
			code.push_back(0x21);
			wasm::append_uleb(code, result);
			if (!expression_is_owned(returned->value))
			{
				code.push_back(0x20);
				wasm::append_uleb(code, result);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.retain_index());
			}
			append(code, cleanup_scopes());
			code.push_back(0x20);
			wasm::append_uleb(code, result);
		}
		else
			append(code, cleanup_scopes());
		if (module_.use_trace_global_ && definition_.function && definition_.function->location.file != "capy://stdlib.capy")
		{
			code.push_back(0x23);
			wasm::append_uleb(code, module_.use_arc_global_ ? 2 : 1);
			code.insert(code.end(), {0x41, 0x01, 0x6b, 0x24});
			wasm::append_uleb(code, module_.use_arc_global_ ? 2 : 1);
		}
		code.push_back(0x0f);
		return {code, "void"};
	}
	if (auto conditional = dynamic_cast<If*>(value))
	{
		std::vector<std::unique_ptr<Expr>> condition_synthetic;
		Expr* condition_source = coerce_condition(conditional->condition, "if", condition_synthetic);
		auto [condition, type] = expression(condition_source);
		if (!conditional->else_body || !value_required)
		{
			condition.insert(condition.end(), {0x04, 0x40});
			++control_depth_;
			append(condition, block(conditional->then_body));
			--control_depth_;
			if (conditional->else_body)
			{
				condition.push_back(0x05);
				++control_depth_;
				append(condition, block(conditional->else_body));
				--control_depth_;
			}
			condition.push_back(0x0b);
			return {condition, "void"};
		}
		++control_depth_;
		BlockValue then_value = value_block(conditional->then_body);
		--control_depth_;
		++control_depth_;
		BlockValue else_value = value_block(conditional->else_body);
		--control_depth_;
		std::string result = "void";
		if (then_value.falls_through && else_value.falls_through)
		{
			if (then_value.type != else_value.type)
				throw Error(conditional->location, "if branches produce " + then_value.type + " and " + else_value.type);
			result = then_value.type;
		}
		else if (then_value.falls_through)
			result = then_value.type;
		else if (else_value.falls_through)
			result = else_value.type;
		condition.push_back(0x04);
		condition.push_back(result == "void" ? 0x40 : wasm_value_type(result));
		append(condition, then_value.code);
		condition.push_back(0x05);
		append(condition, else_value.code);
		condition.push_back(0x0b);
		if (managed_type(result))
			owned_expression_results_.insert(value);
		return {condition, result};
	}
	if (auto loop = dynamic_cast<While*>(value))
	{
		const auto previous_condition_scope = repeated_condition_scope_;
		repeated_condition_scope_ = owned_scopes_.size();
		std::vector<std::unique_ptr<Expr>> condition_synthetic;
		Expr* condition_source = coerce_condition(loop->condition, "while", condition_synthetic);
		auto [condition, type] = expression(condition_source);
		repeated_condition_scope_ = previous_condition_scope;
		const unsigned base = control_depth_, boundary = static_cast<unsigned>(owned_scopes_.size());
		control_depth_ += 2;
		loops_.push_back({base + 1, base + 2, boundary, {}, {}});
		Bytes body = block(loop->body);
		loops_.pop_back();
		control_depth_ -= 2;
		Bytes code{0x02, 0x40, 0x03, 0x40};
		append(code, condition);
		code.insert(code.end(), {0x45, 0x0d, 0x01});
		append(code, body);
		code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
		return {code, "void"};
	}
	if (auto loop = dynamic_cast<For*>(value))
	{
		if (infer(loop->iterable) == "dval")
		{
			if (loop->names.empty() || loop->names.size() > 2)
				throw Error(loop->location, "DValue iteration accepts one value binding and an optional key binding");
			auto [iterable_code, iterable_type] = expression(loop->iterable);
			const unsigned iterable = add_local("", "dval", loop->iterable->location), count = add_local("", "s32", loop->location),
						   index = add_local("", "s32", loop->location), item = add_local("", "dval", loop->location),
						   key = loop->names.size() == 2 ? add_local("", "string", loop->location) : 0xffffffffu;
			const bool owned_iterable = expression_is_owned(loop->iterable);
			Bytes code = std::move(iterable_code);
			code.push_back(0x21);
			wasm::append_uleb(code, iterable);
			if (!owned_iterable) { code.push_back(0x20); wasm::append_uleb(code, iterable); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
			code.insert(code.end(), {0x41, 0x00, 0x21});
			wasm::append_uleb(code, index);
			code.insert(code.end(), {0x02, 0x40, 0x03, 0x40});
			managed_payload_span(code, iterable, "dval");
			code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_dv_count_brrb"));
			code.push_back(0x22); wasm::append_uleb(code, count);
			code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40, 0x20}); wasm::append_uleb(code, iterable);
			code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
			append(code, module_.marker(loop->iterable->location));
			code.insert(code.end(), {0x00, 0x0b, 0x20}); wasm::append_uleb(code, index);
			code.push_back(0x20); wasm::append_uleb(code, count);
			code.insert(code.end(), {0x4f, 0x0d, 0x01});
			std::vector<unsigned> constructed_entries;
			auto entry_cleanup = [&]
			{
				Bytes cleanup;
				for (auto it = constructed_entries.rbegin(); it != constructed_entries.rend(); ++it)
				{
					cleanup.push_back(0x20); wasm::append_uleb(cleanup, *it);
					cleanup.push_back(0x10); wasm::append_uleb(cleanup, module_.release_index());
				}
				cleanup.push_back(0x20); wasm::append_uleb(cleanup, iterable);
				cleanup.push_back(0x10); wasm::append_uleb(cleanup, module_.release_index());
				return cleanup;
			};
			auto entry = [&](const char* import, const std::string& type, unsigned target)
			{
				const unsigned length = add_local("", "s32", loop->location);
				managed_payload_span(code, iterable, "dval");
				code.push_back(0x20); wasm::append_uleb(code, index);
				code.insert(code.end(), {0x41, 0x00, 0x41, 0x00, 0x10});
				wasm::append_uleb(code, module_.import_index(import));
				code.push_back(0x22); wasm::append_uleb(code, length);
				code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
				append(code, entry_cleanup()); append(code, module_.marker(loop->location));
				code.insert(code.end(), {0x00, 0x0b});
				const Bytes allocation_cleanup = entry_cleanup();
				auto [allocation, pointer] = allocate_blob(type, type == "string" ? 1 : 4, length, loop->location, allocation_cleanup);
				append(code, allocation);
				managed_payload_span(code, iterable, "dval");
				code.push_back(0x20); wasm::append_uleb(code, index);
				managed_payload_pointer(code, pointer, type);
				code.push_back(0x20); wasm::append_uleb(code, length);
				code.push_back(0x10); wasm::append_uleb(code, module_.import_index(import));
				code.push_back(0x20); wasm::append_uleb(code, length);
				code.insert(code.end(), {0x47, 0x04, 0x40, 0x20}); wasm::append_uleb(code, pointer);
				code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
				append(code, entry_cleanup()); append(code, module_.marker(loop->location)); code.insert(code.end(), {0x00, 0x0b, 0x20});
				wasm::append_uleb(code, pointer);
				code.push_back(0x21); wasm::append_uleb(code, target);
				if (type == "dval") retain_dval_callables(code, target, loop->location);
				constructed_entries.push_back(target);
			};
			if (key != 0xffffffffu)
				entry("bearer_dv_entry_key_brrb", "string", key);
			entry("bearer_dv_entry_value_brrb", "dval", item);
			std::unordered_map<std::string, std::pair<unsigned, std::string>> scope{{loop->names.front(), {item, "dval"}}};
			if (key != 0xffffffffu)
				scope[loop->names.back()] = {key, "string"};
			scopes_.push_back(std::move(scope));
			owned_scopes_.push_back({{iterable, "dval"}});
			const unsigned boundary = static_cast<unsigned>(owned_scopes_.size());
			std::vector<std::pair<unsigned, std::string>> iteration_values{{item, "dval"}};
			if (key != 0xffffffffu)
				iteration_values.push_back({key, "string"});
			owned_scopes_.push_back(std::move(iteration_values));
			Bytes release = cleanup_scopes(boundary);
			Bytes increment{0x20};
			wasm::append_uleb(increment, index);
			increment.insert(increment.end(), {0x41, 0x01, 0x6a, 0x21});
			wasm::append_uleb(increment, index);
			const unsigned base = control_depth_;
			control_depth_ += 2;
			loops_.push_back({base + 1, base + 2, boundary, {}, increment});
			Bytes body = block(loop->body);
			loops_.pop_back();
			control_depth_ -= 2;
			owned_scopes_.pop_back();
			scopes_.pop_back();
			append(code, body);
			append(code, release);
			append(code, increment);
			code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
			append(code, cleanup_scopes(owned_scopes_.size() - 1));
			owned_scopes_.pop_back();
			return {code, "void"};
		}
		const unsigned base = control_depth_, boundary = static_cast<unsigned>(owned_scopes_.size());
		if (auto range = dynamic_cast<Binary*>(loop->iterable); range && range->operator_ == "..")
		{
			if (loop->names.size() != 1)
				throw Error(loop->location, "range iteration accepts exactly one value binding");
			auto [start, st] = expression(range->left);
			auto [end, et] = expression(range->right);
			if (st != "s64" || et != "s64")
				throw Error(loop->location, "range bounds must be s64");
			const unsigned index = add_local("", "s64", loop->location), limit = add_local("", "s64", loop->location);
			const unsigned item = add_local("", "s64", loop->location);
			append(start, Bytes{0x21});
			wasm::append_uleb(start, index);
			append(end, Bytes{0x21});
			wasm::append_uleb(end, limit);
			scopes_.push_back({{loop->names[0], {item, "s64"}}});
			owned_scopes_.push_back({});
			control_depth_ += 3;
			loops_.push_back({base + 1, base + 3, boundary, {}, {}});
			Bytes body = block(loop->body);
			loops_.pop_back();
			control_depth_ -= 3;
			owned_scopes_.pop_back();
			scopes_.pop_back();
			Bytes code = std::move(start);
			append(code, end);
			code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20});
			wasm::append_uleb(code, index);
			code.push_back(0x20);
			wasm::append_uleb(code, limit);
			code.insert(code.end(), {0x59, 0x0d, 0x01, 0x20});
			wasm::append_uleb(code, index);
			code.push_back(0x21);
			wasm::append_uleb(code, item);
			code.insert(code.end(), {0x02, 0x40});
			append(code, body);
			code.push_back(0x0b);
			code.push_back(0x20);
			wasm::append_uleb(code, index);
			code.insert(code.end(), {0x42, 0x01, 0x7c, 0x21});
			wasm::append_uleb(code, index);
			code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
			return {code, "void"};
		}
		auto [iterable, type] = expression(loop->iterable);
		if (loop->names.empty() || loop->names.size() > 2)
			throw Error(loop->location, "array iteration accepts one value binding and an optional index binding");
		if (type.rfind("array<", 0) != 0)
			throw Error(loop->iterable->location, "for loop requires an exclusive range or array");
		const std::string element = type.substr(6, type.size() - 7);
		const unsigned array = add_local("", type, loop->iterable->location), cursor = add_local("", "s32", loop->location);
		const unsigned item = add_local("", element, loop->location);
		const unsigned metadata = loop->names.size() == 2 ? add_local("", "s64", loop->location) : 0xffffffffu;
		std::unordered_map<std::string, std::pair<unsigned, std::string>> scope{{loop->names[0], {item, element}}};
		if (metadata != 0xffffffffu) scope[loop->names[1]] = {metadata, "s64"};
		scopes_.push_back(std::move(scope));
		owned_scopes_.push_back({{array, type}});
		const unsigned array_loop_boundary = static_cast<unsigned>(owned_scopes_.size());
		owned_scopes_.push_back(managed_type(element) ? std::vector<std::pair<unsigned, std::string>>{{item, element}}
													 : std::vector<std::pair<unsigned, std::string>>{});
		Bytes release = cleanup_scopes(array_loop_boundary);
		Bytes increment{0x20}; wasm::append_uleb(increment, cursor);
		increment.insert(increment.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(increment, cursor);
		control_depth_ += 2;
		loops_.push_back({base + 1, base + 2, array_loop_boundary, {}, increment});
		Bytes body = block(loop->body);
		loops_.pop_back();
		control_depth_ -= 2;
		owned_scopes_.pop_back();
		owned_scopes_.pop_back();
		scopes_.pop_back();
		const bool owned_iterable = expression_is_owned(loop->iterable);
		Bytes code = std::move(iterable);
		code.push_back(0x21); wasm::append_uleb(code, array);
		if (!owned_iterable) { code.push_back(0x20); wasm::append_uleb(code, array); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
		code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, cursor);
		code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, cursor);
		code.push_back(0x20); wasm::append_uleb(code, array); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x4f, 0x0d, 0x01});
		code.push_back(0x20); wasm::append_uleb(code, array); code.insert(code.end(), {0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET});
		code.push_back(0x20); wasm::append_uleb(code, cursor);
		const unsigned element_size = array_element_size(element);
		code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a});
		code.push_back(array_load_opcode(element)); code.push_back(static_cast<std::uint8_t>(element_size == 8 ? 3 : 2));
		code.insert(code.end(), {0x00, 0x21}); wasm::append_uleb(code, item);
		if (managed_type(element)) { code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
		if (metadata != 0xffffffffu) { code.push_back(0x20); wasm::append_uleb(code, cursor); code.insert(code.end(), {0xad, 0x21}); wasm::append_uleb(code, metadata); }
		append(code, body);
		append(code, release);
		append(code, increment);
		code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
		code.push_back(0x20); wasm::append_uleb(code, array); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
		return {code, "void"};
	}
	if (dynamic_cast<Break*>(value) || dynamic_cast<Continue*>(value))
	{
		if (loops_.empty())
			throw Error(value->location, dynamic_cast<Break*>(value) ? "break is only valid inside a loop" : "continue is only valid inside a loop");
		const auto& loop = loops_.back();
		const bool is_break = dynamic_cast<Break*>(value);
		const unsigned target = is_break ? loop.break_depth : loop.continue_depth;
		Bytes code = cleanup_scopes(loop.ownership_boundary);
		append(code, is_break ? loop.break_edge : loop.continue_edge);
		code.push_back(0x0c);
		wasm::append_uleb(code, control_depth_ - target);
		return {code, "void"};
	}
	if (auto nested = dynamic_cast<Block*>(value))
	{
		if (!value_required)
			return {block(nested), "void"};
		BlockValue result = value_block(nested);
		if (managed_type(result.type))
			owned_expression_results_.insert(value);
		return {std::move(result.code), result.type};
	}
	throw Error(value->location, "native Capy backend does not yet lower expression");
}

FunctionLowerer::BlockValue FunctionLowerer::value_block(Block* block_value)
{
	scopes_.push_back({});
	owned_scopes_.push_back({});
	Bytes code;
	std::string result_type = "void";
	bool falls_through = true;
	for (std::size_t i = 0; i < block_value->items.size(); ++i)
	{
		Expr* item = block_value->items[i];
		const bool final = i + 1 == block_value->items.size();
		auto yielded = dynamic_cast<Yield*>(item);
		if (yielded && !final)
			throw Error(yielded->location, "block yield must be the final item in its block");
		Expr* compiled_item = yielded ? yielded->value : item;
		auto [part, type] = expression(compiled_item, yielded != nullptr);
		append(code, part);
		falls_through = expression_falls_through(item);
		if (!final)
		{
			if (type != "void" && type != "never")
			{
				if (managed_type(type) && expression_is_owned(item))
				{
					code.push_back(0x10);
					wasm::append_uleb(code, module_.release_index());
				}
				else
					code.push_back(0x1a);
			}
			if (!falls_through)
				break;
			continue;
		}
		if (!yielded)
		{
			if (!falls_through)
				result_type = "never";
			else
				throw Error(item->location, "value-producing block must end with '-> expression'");
			continue;
		}
		result_type = type;
		if (type == "void")
		{
			append(code, cleanup_scopes(owned_scopes_.size() - 1));
			continue;
		}
		const unsigned result = add_local("", type, item->location);
		code.push_back(0x21);
		wasm::append_uleb(code, result);
		if (managed_type(type) && !expression_is_owned(compiled_item))
		{
			code.push_back(0x20);
			wasm::append_uleb(code, result);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.retain_index());
		}
		append(code, cleanup_scopes(owned_scopes_.size() - 1));
		code.push_back(0x20);
		wasm::append_uleb(code, result);
	}
	if (block_value->items.empty())
		throw Error(block_value->location, "value-producing block must end with '-> expression'");
	owned_scopes_.pop_back();
	scopes_.pop_back();
	return {std::move(code), result_type, falls_through};
}

Bytes FunctionLowerer::block(Block* block_value, bool new_scope)
{
	if (new_scope)
	{
		scopes_.push_back({});
		owned_scopes_.push_back({});
	}
	Bytes code;
	for (std::size_t i = 0; i < block_value->items.size(); ++i)
	{
		inferred_types_.clear();
		Expr* item = block_value->items[i];
		auto yielded = dynamic_cast<Yield*>(item);
		const bool final = i + 1 == block_value->items.size();
		if (yielded && !final)
			throw Error(yielded->location, "block yield must be the final item in its block");
		Expr* compiled_item = yielded ? yielded->value : item;
		ArrayLiteral typed_array(compiled_item->location);
		Name element_type(compiled_item->location, "");
		if (yielded && !new_scope)
			if (auto array = dynamic_cast<ArrayLiteral*>(compiled_item); array && definition_.result.rfind("array<", 0) == 0)
			{
				element_type.value = definition_.result.substr(6, definition_.result.size() - 7);
				typed_array.items = array->items;
				typed_array.explicit_element_type = &element_type;
				compiled_item = &typed_array;
			}
		auto compiled = yielded && dynamic_cast<Integer*>(compiled_item) && !new_scope
			? integer_expression(static_cast<Integer*>(compiled_item), definition_.result) : expression(compiled_item, yielded != nullptr);
		auto part = std::move(compiled.first);
		const std::string type = std::move(compiled.second);
		append(code, std::move(part));
		if (yielded)
		{
			if (new_scope)
			{
				if (type != "void")
				{
					if (managed_type(type) && expression_is_owned(compiled_item))
					{
						code.push_back(0x10);
						wasm::append_uleb(code, module_.release_index());
					}
					else
						code.push_back(0x1a);
				}
				continue;
			}
			if (type != definition_.result)
				throw Error(yielded->location, "expected " + definition_.result + ", found " + type);
			yielded_result_ = true;
			if (managed_type(type))
			{
				const unsigned result = add_local("", type, yielded->location);
				code.push_back(0x21);
				wasm::append_uleb(code, result);
				if (!expression_is_owned(compiled_item))
				{
					code.push_back(0x20);
					wasm::append_uleb(code, result);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.retain_index());
				}
				append(code, cleanup_scopes());
				code.push_back(0x20);
				wasm::append_uleb(code, result);
			}
			else
				append(code, cleanup_scopes());
			continue;
		}
		if (type != "void")
		{
			if (managed_type(type) && expression_is_owned(item))
			{
				code.push_back(0x10);
				wasm::append_uleb(code, module_.release_index());
			}
			else
				code.push_back(0x1a);
		}
	}
	if (new_scope)
	{
		append(code, cleanup_scopes(owned_scopes_.size() - 1));
		owned_scopes_.pop_back();
		scopes_.pop_back();
	}
	return code;
}

Bytes FunctionLowerer::lower()
{
	if (definition_.handler_adapter)
	{
		const Definition& target = module_.definitions_[definition_.thunk_target];
		if (!target.first_parameter_used)
		{
			Bytes code{0x41, 0x00, 0x10};
			wasm::append_uleb(code, target.index);
			code.push_back(0x0b);
			Bytes locals{0}, body;
			wasm::append_uleb(body, static_cast<unsigned>(locals.size() + code.size()));
			body.insert(body.end(), locals.begin(), locals.end());
			body.insert(body.end(), code.begin(), code.end());
			return body;
		}
		const unsigned length = 1, capacity = 2, payload = 3, input = 4;
		Bytes code{0x20, 0x00, 0x41, 0x00, 0x41, 0x00, 0x10};
		wasm::append_uleb(code, module_.import_index("bearer_handler_input_brrb"));
		code.push_back(0x22); wasm::append_uleb(code, length);
		code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40}); append(code, module_.marker(target.function->location)); code.insert(code.end(), {0x00, 0x0b, 0x20}); wasm::append_uleb(code, length);
		code.insert(code.end(), {0x45, 0x04, 0x7f, 0x41, 0x01, 0x05, 0x20}); wasm::append_uleb(code, length); code.insert(code.end(), {0x0b, 0x22}); wasm::append_uleb(code, capacity);
		code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_alloc")); code.push_back(0x22); wasm::append_uleb(code, payload);
		code.insert(code.end(), {0x45, 0x04, 0x40}); append(code, module_.marker(target.function->location)); code.insert(code.end(), {0x00, 0x0b, 0x41}); wasm::append_sleb32(code, BEARER_WASM_OBJECT_HANDLE_SIZE); code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_alloc"));
		code.push_back(0x22); wasm::append_uleb(code, input); code.insert(code.end(), {0x45, 0x04, 0x40, 0x20}); wasm::append_uleb(code, payload); code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_free")); append(code, module_.marker(target.function->location)); code.insert(code.end(), {0x00, 0x0b});
		auto store = [&](unsigned offset, std::int32_t value) { code.push_back(0x20); wasm::append_uleb(code, input); code.push_back(0x41); wasm::append_sleb32(code, value); code.insert(code.end(), {0x36, 0x02}); wasm::append_uleb(code, offset); };
		store(BEARER_WASM_OBJECT_REFS_OFFSET, 1);
		store(BEARER_WASM_OBJECT_OWNER_OFFSET, 1);
		store(BEARER_WASM_OBJECT_TYPE_OFFSET, 4);
		store(BEARER_WASM_OBJECT_HEADER_SIZE_OFFSET, BEARER_WASM_OBJECT_HANDLE_SIZE);
		for (const auto [offset, local] : {std::pair<unsigned, unsigned>{BEARER_WASM_OBJECT_LENGTH_OFFSET, length}, {BEARER_WASM_OBJECT_CAPACITY_OFFSET, capacity}, {BEARER_WASM_OBJECT_PAYLOAD_OFFSET, payload}}) { code.push_back(0x20); wasm::append_uleb(code, input); code.push_back(0x20); wasm::append_uleb(code, local); code.insert(code.end(), {0x36, 0x02}); wasm::append_uleb(code, offset); }
		code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01, 0x20, 0x00, 0x20}); wasm::append_uleb(code, payload); code.push_back(0x20); wasm::append_uleb(code, length); code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_handler_input_brrb")); code.push_back(0x20); wasm::append_uleb(code, length);
		code.insert(code.end(), {0x47, 0x04, 0x40, 0x20}); wasm::append_uleb(code, input); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); append(code, module_.marker(target.function->location)); code.insert(code.end(), {0x00, 0x0b, 0x20}); wasm::append_uleb(code, input); code.push_back(0x10); wasm::append_uleb(code, target.index); code.push_back(0x20); wasm::append_uleb(code, input); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); code.push_back(0x0b);
		Bytes locals{0x01, 0x04, 0x7f}; Bytes body; wasm::append_uleb(body, static_cast<unsigned>(locals.size() + code.size())); body.insert(body.end(), locals.begin(), locals.end()); body.insert(body.end(), code.begin(), code.end()); return body;
	}
	if (definition_.thunk_target != 0xffffffffu)
	{
		const Definition& target = module_.definitions_[definition_.thunk_target];
		Bytes code;
		for (unsigned parameter = 1; parameter < definition_.parameters.size(); ++parameter)
		{
			code.push_back(0x20);
			wasm::append_uleb(code, parameter);
		}
		code.push_back(0x10);
		wasm::append_uleb(code, target.index);
		code.push_back(0x0b);
		Bytes locals{0};
		Bytes body;
		body.reserve(locals.size() + code.size() + 5);
		wasm::append_uleb(body, static_cast<unsigned>(locals.size() + code.size()));
		body.insert(body.end(), locals.begin(), locals.end());
		body.insert(body.end(), code.begin(), code.end());
		return body;
	}
	Bytes code;
	const bool trace = module_.use_trace_global_ && definition_.function && definition_.function->location.file != "capy://stdlib.capy";
	if (trace)
	{
		const std::string& function = definition_.function->name;
		const std::string& path = module_.artifact_path(definition_.function->location.file);
		if (function.size() > std::numeric_limits<std::uint32_t>::max() - 16 || path.size() > std::numeric_limits<std::uint32_t>::max() - 16 - function.size())
			throw Error(definition_.function->location, "backtrace metadata exceeds 4 GiB");
		std::string record;
		record.reserve(16 + function.size() + path.size());
		append_u32_le(record, static_cast<std::uint32_t>(function.size()));
		append_u32_le(record, static_cast<std::uint32_t>(path.size()));
		append_u32_le(record, static_cast<std::uint32_t>(definition_.function->location.line));
		append_u32_le(record, static_cast<std::uint32_t>(definition_.function->location.column));
		record += function;
		record += path;
		const unsigned record_offset = module_.add_data(record);
		auto address = [&]() {
			code.push_back(0x23);
			wasm::append_uleb(code, 0);
			code.push_back(0x41);
			wasm::append_sleb32(code, static_cast<std::int32_t>(module_.trace_stack_offset_));
			code.push_back(0x6a);
			code.push_back(0x23);
			wasm::append_uleb(code, module_.use_arc_global_ ? 2 : 1);
			code.insert(code.end(), {0x41, 0x80, 0x02, 0x70, 0x41, 0x08, 0x6c, 0x6a});
		};
		address();
		code.push_back(0x23);
		wasm::append_uleb(code, 0);
		code.push_back(0x41);
		wasm::append_sleb32(code, static_cast<std::int32_t>(record_offset));
		code.insert(code.end(), {0x6a, 0x36, 0x02, 0x00});
		address();
		code.push_back(0x41);
		wasm::append_sleb32(code, static_cast<std::int32_t>(record.size()));
		code.insert(code.end(), {0x36, 0x02, 0x04, 0x23});
		wasm::append_uleb(code, module_.use_arc_global_ ? 2 : 1);
		code.insert(code.end(), {0x41, 0x01, 0x6a, 0x24});
		wasm::append_uleb(code, module_.use_arc_global_ ? 2 : 1);
	}
	if (definition_.closure_body)
	{
		std::vector<std::string> capture_types;
		for (const auto& [name, type] : definition_.captures)
			capture_types.push_back(type);
		const AggregateLayout layout = aggregate_layout(capture_types, BEARER_WASM_OBJECT_CLOSURE_CAPTURES_OFFSET);
		for (std::size_t i = 0; i < definition_.captures.size(); ++i)
		{
			Name name(definition_.function->location, definition_.captures[i].first);
			auto [slot, type] = lookup(&name);
			code.push_back(0x20);
			wasm::append_uleb(code, 0);
			load_field(code, type, layout.offsets[i]);
			code.push_back(0x21);
			wasm::append_uleb(code, slot);
		}
	}
	append(code, block(definition_.function->body, false));
	if (definition_.result != "void" && !yielded_result_ && !expression_always_returns(definition_.function->body))
		throw Error(definition_.function->location, "not all paths produce " + definition_.result);
	if (definition_.result == "void")
		append(code, cleanup_scopes());
	if (trace)
	{
		if (definition_.result != "void")
		{
			const unsigned result = add_local("", definition_.result, definition_.function->location);
			code.push_back(0x21);
			wasm::append_uleb(code, result);
			code.push_back(0x23);
			wasm::append_uleb(code, module_.use_arc_global_ ? 2 : 1);
			code.insert(code.end(), {0x41, 0x01, 0x6b, 0x24});
			wasm::append_uleb(code, module_.use_arc_global_ ? 2 : 1);
			code.push_back(0x20);
			wasm::append_uleb(code, result);
		}
		else
		{
			code.push_back(0x23);
			wasm::append_uleb(code, module_.use_arc_global_ ? 2 : 1);
			code.insert(code.end(), {0x41, 0x01, 0x6b, 0x24});
			wasm::append_uleb(code, module_.use_arc_global_ ? 2 : 1);
		}
	}
	code.push_back(0x0b);
	Bytes locals;
	if (!local_types_.empty())
	{
		std::vector<std::pair<unsigned, std::uint8_t>> groups;
		for (const std::string& type : local_types_)
		{
			const std::uint8_t wasm_type = wasm_value_type(type);
			if (groups.empty() || groups.back().second != wasm_type)
				groups.push_back({1, wasm_type});
			else
				++groups.back().first;
		}
		wasm::append_uleb(locals, static_cast<unsigned>(groups.size()));
		for (const auto& [count, type] : groups)
		{
			wasm::append_uleb(locals, count);
			locals.push_back(type);
		}
	}
	else
		locals.push_back(0);
	Bytes body;
	body.reserve(locals.size() + code.size() + 5);
	wasm::append_uleb(body, static_cast<unsigned>(locals.size() + code.size()));
	body.insert(body.end(), locals.begin(), locals.end());
	body.insert(body.end(), code.begin(), code.end());
	return body;
}

std::vector<Bytes> Module::runtime_bodies() const
{
	auto body = [](Bytes locals, Bytes code)
	{
		code.push_back(0x0b);
		locals.insert(locals.end(), code.begin(), code.end());
		Bytes result;
		wasm::append_uleb(result, static_cast<unsigned>(locals.size()));
		result.insert(result.end(), locals.begin(), locals.end());
		return result;
	};
	std::vector<Bytes> result;
	if (use_retain_)
	{
		Bytes code{0x20, 0x00, 0x45, 0x04, 0x40, 0x0f, 0x0b, 0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_REFS_OFFSET, 0x41, 0x7f, 0x46, 0x04, 0x40, 0x0f, 0x0b, 0x20, 0x00, 0x28,
				   0x02, 0x00, 0x41, 0x7e, 0x46, 0x04, 0x40, 0x00, 0x0b, 0x20, 0x00, 0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_REFS_OFFSET, 0x41, 0x01, 0x6a, 0x36, 0x02, BEARER_WASM_OBJECT_REFS_OFFSET};
		result.push_back(body({0}, std::move(code)));
	}
	if (use_release_)
	{
		Bytes code{0x20, 0x00, 0x45, 0x04, 0x40, 0x0f, 0x0b, 0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_REFS_OFFSET, 0x41, 0x7f, 0x46, 0x04, 0x40,
				   0x0f, 0x0b, 0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_REFS_OFFSET, 0x45, 0x04, 0x40, 0x00, 0x0b, 0x20, 0x00, 0x20, 0x00, 0x28,
				   0x02, 0x00, 0x41, 0x01, 0x6b, 0x22, 0x01, 0x36, 0x02, BEARER_WASM_OBJECT_REFS_OFFSET, 0x20, 0x01, 0x45, 0x04, 0x40};
		// Type 3 arrays own every element, including nested managed aggregates.
		code.insert(code.end(),
					{0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_TYPE_OFFSET, 0x41, 0x03, 0x46, 0x04, 0x40, 0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21, 0x02, 0x41, 0x00, 0x21, 0x01, 0x02,
					 0x40, 0x03, 0x40, 0x20, 0x01, 0x20, 0x02, 0x4f, 0x0d, 0x01, 0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x20, 0x01, 0x41, 0x04, 0x6c, 0x6a, 0x28, 0x02, BEARER_WASM_OBJECT_REFS_OFFSET, 0x10});
		wasm::append_uleb(code, release_index());
		code.insert(code.end(), {0x20, 0x01, 0x41, 0x01, 0x6a, 0x21, 0x01, 0x0c, 0x00, 0x0b, 0x0b, 0x0b});
		// Both array type IDs own their separate raw backing allocation.
		code.insert(code.end(), {0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_TYPE_OFFSET, 0x41, 0x02, 0x46, 0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_TYPE_OFFSET, 0x41, 0x03, 0x46, 0x72, 0x04, 0x40,
								 0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x22, 0x01, 0x45, 0x04, 0x40, 0x05, 0x20, 0x01, 0x10});
		wasm::append_uleb(code, import_index("bearer_free"));
		code.insert(code.end(), {0x0b, 0x0b});
		if (imports_.contains("bearer_dv_callable_at_brrb"))
		{
			code.insert(code.end(), {0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_TYPE_OFFSET, 0x41, 0x04, 0x46, 0x04, 0x40, 0x41, 0x00, 0x21, 0x02, 0x02, 0x40, 0x03, 0x40,
				0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x20, 0x02, 0x10});
			wasm::append_uleb(code, import_index("bearer_dv_callable_at_brrb"));
			code.insert(code.end(), {0x45, 0x0d, 0x01, 0x20, 0x02, 0x41, 0x01, 0x6a, 0x21, 0x02, 0x0c, 0x00, 0x0b, 0x0b,
				0x02, 0x40, 0x03, 0x40, 0x20, 0x02, 0x45, 0x0d, 0x01, 0x20, 0x02, 0x41, 0x01, 0x6b, 0x21, 0x02,
				0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x20, 0x02, 0x10});
			wasm::append_uleb(code, import_index("bearer_dv_callable_at_brrb"));
			code.insert(code.end(), {0x10}); wasm::append_uleb(code, release_index());
			code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b, 0x0b});
		}
		// DValue handles own a separate raw BRRB payload allocation.
		code.insert(code.end(), {0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_TYPE_OFFSET, 0x41, 0x04, 0x46, 0x04, 0x40,
								 0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_PAYLOAD_OFFSET, 0x22, 0x01, 0x45, 0x04, 0x40, 0x05, 0x20, 0x01, 0x10});
		wasm::append_uleb(code, import_index("bearer_free"));
		code.insert(code.end(), {0x0b, 0x0b});
		auto release_aggregate_fields = [&](unsigned type_id, const std::vector<std::string>& fields, unsigned first_offset)
		{
			const AggregateLayout layout = aggregate_layout(fields, first_offset);
			bool guarded = false;
			for (unsigned i = 0; i < fields.size(); ++i)
			{
				if (!managed_type(fields[i]))
					continue;
				if (!guarded)
				{
					code.insert(code.end(), {0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_TYPE_OFFSET, 0x41});
					wasm::append_sleb32(code, static_cast<std::int32_t>(type_id));
					code.insert(code.end(), {0x46, 0x04, 0x40});
					guarded = true;
				}
				code.push_back(0x20);
				wasm::append_uleb(code, 0);
				load_field(code, fields[i], layout.offsets[i]);
				code.push_back(0x10);
				wasm::append_uleb(code, release_index());
			}
			if (guarded)
				code.push_back(0x0b);
		};
		for (const auto& [type_id, captures] : closure_types_)
			release_aggregate_fields(type_id, captures, BEARER_WASM_OBJECT_CLOSURE_CAPTURES_OFFSET);
		for (const auto& [name, aggregate] : structs_)
		{
			std::vector<std::string> fields;
			for (const auto& [field_name, field_type] : aggregate.fields)
				fields.push_back(field_type);
			release_aggregate_fields(aggregate.type_id, fields, BEARER_WASM_OBJECT_STRUCT_FIELDS_OFFSET);
		}
		code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6b, 0x24, 0x01, 0x20, 0x00, 0x10});
		wasm::append_uleb(code, import_index("bearer_free"));
		code.push_back(0x0b);
		result.push_back(body({0x01, 0x02, 0x7f}, std::move(code)));
	}
	if (use_clone_)
	{
		Bytes code{0x20, 0x00, 0x28, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x21, 0x02, 0x20, 0x02, 0x41, BEARER_WASM_OBJECT_BLOB_HEADER_SIZE, 0x6a, 0x10};
		wasm::append_uleb(code, import_index("bearer_alloc"));
		code.insert(code.end(), {0x21, 0x01, 0x20, 0x01, 0x45, 0x04, 0x40, 0x00, 0x0b});
		for (const auto [literal, offset] : {std::pair<std::uint8_t, unsigned>{1, 0}, {1, 4}, {1, 8}})
		{
			code.insert(code.end(), {0x20, 0x01, 0x41, literal, 0x36, 0x02});
			wasm::append_uleb(code, offset);
		}
		code.insert(code.end(), {0x20, 0x01, 0x20, 0x02, 0x41, 0x14, 0x6a, 0x36, 0x02, BEARER_WASM_OBJECT_HEADER_SIZE_OFFSET, 0x20, 0x01, 0x20, 0x02, 0x36, 0x02, BEARER_WASM_OBJECT_LENGTH_OFFSET, 0x20, 0x01, 0x41, 0x14,
								 0x6a, 0x20, 0x00, 0x41, 0x14, 0x6a, 0x20, 0x02, 0xfc, 0x0a, 0x00, 0x00, 0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01, 0x20, 0x01});
		result.push_back(body({0x01, 0x02, 0x7f}, std::move(code)));
	}
	for (const auto& [symbol, type] : fused_sink_formats_)
	{
		Bytes code{0x20, 0x00, 0x10}; wasm::append_uleb(code, format_scalar_index(type));
		code.push_back(0x21); wasm::append_uleb(code, 1);
		managed_payload_span(code, 1, "string");
		code.push_back(0x10); wasm::append_uleb(code, import_index(symbol));
		code.push_back(0x20); wasm::append_uleb(code, 1);
		code.push_back(0x10); wasm::append_uleb(code, release_index());
		result.push_back(body({0x01, 0x01, 0x7f}, std::move(code)));
	}
	return result;
}

Bytes Module::custom_export_body(const Definition& target)
{
	auto append_bytes = [](Bytes& destination, const Bytes& source) { destination.insert(destination.end(), source.begin(), source.end()); };
	const unsigned length = 1, input = 2, result = 3, output = 4, payload = 5, capacity = 6;
	Bytes code{0x20, 0x00, 0x41, 0x00, 0x41, 0x00, 0x10};
	wasm::append_uleb(code, import_index("bearer_dv_ptr_to_brrb"));
	code.push_back(0x22); wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40}); append_bytes(code, marker(target.function->location));
	code.insert(code.end(), {0x00, 0x0b, 0x20}); wasm::append_uleb(code, length);
	code.insert(code.end(), {0x45, 0x04, 0x7f, 0x41, 0x01, 0x05, 0x20}); wasm::append_uleb(code, length);
	code.insert(code.end(), {0x0b, 0x22}); wasm::append_uleb(code, capacity);
	code.push_back(0x10); wasm::append_uleb(code, import_index("bearer_alloc"));
	code.push_back(0x22); wasm::append_uleb(code, payload);
	code.insert(code.end(), {0x45, 0x04, 0x40}); append_bytes(code, marker(target.function->location));
	code.insert(code.end(), {0x00, 0x0b, 0x41, 0x1c, 0x10});
	wasm::append_uleb(code, import_index("bearer_alloc"));
	code.push_back(0x22); wasm::append_uleb(code, input);
	code.insert(code.end(), {0x45, 0x04, 0x40, 0x20}); wasm::append_uleb(code, payload);
	code.push_back(0x10); wasm::append_uleb(code, import_index("bearer_free"));
	append_bytes(code, marker(target.function->location)); code.insert(code.end(), {0x00, 0x0b});
	auto store_constant = [&](unsigned offset, std::int32_t value)
	{
		code.push_back(0x20); wasm::append_uleb(code, input); code.push_back(0x41); wasm::append_sleb32(code, value);
		code.insert(code.end(), {0x36, 0x02}); wasm::append_uleb(code, offset);
	};
	auto store_local = [&](unsigned offset, unsigned local)
	{
		code.push_back(0x20); wasm::append_uleb(code, input); code.push_back(0x20); wasm::append_uleb(code, local);
		code.insert(code.end(), {0x36, 0x02}); wasm::append_uleb(code, offset);
	};
	store_constant(0, 1); store_constant(4, 1); store_constant(8, 4); store_constant(12, 28);
	store_local(16, length); store_local(20, capacity); store_local(24, payload);
	code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01, 0x20, 0x00});
	managed_payload_pointer(code, input, "dval");
	code.push_back(0x20); wasm::append_uleb(code, length); code.push_back(0x10);
	wasm::append_uleb(code, import_index("bearer_dv_ptr_to_brrb"));
	code.push_back(0x20); wasm::append_uleb(code, length);
	code.insert(code.end(), {0x47, 0x04, 0x40, 0x20}); wasm::append_uleb(code, input);
	code.push_back(0x10); wasm::append_uleb(code, release_index()); append_bytes(code, marker(target.function->location));
	code.insert(code.end(), {0x00, 0x0b, 0x20}); wasm::append_uleb(code, input);
	code.push_back(0x10); wasm::append_uleb(code, target.index);
	code.push_back(0x21); wasm::append_uleb(code, result);
	managed_payload_span(code, result, "dval");
	code.push_back(0x10); wasm::append_uleb(code, import_index("bearer_dv_brrb_to_ptr"));
	code.push_back(0x21); wasm::append_uleb(code, output);
	for (unsigned value : {input, result})
	{
		code.push_back(0x20); wasm::append_uleb(code, value); code.push_back(0x10); wasm::append_uleb(code, release_index());
	}
	code.push_back(0x20); wasm::append_uleb(code, output); code.push_back(0x0b);
	Bytes body{0x01, 0x06, 0x7f};
	body.insert(body.end(), code.begin(), code.end());
	Bytes result_body;
	wasm::append_uleb(result_body, static_cast<unsigned>(body.size()));
	result_body.insert(result_body.end(), body.begin(), body.end());
	return result_body;
}

void Module::collect()
{
	auto split_top_level = [](const std::string& text)
	{
		std::vector<std::string> result;
		std::size_t item = 0, depth = 0;
		for (std::size_t cursor = 0; cursor <= text.size(); ++cursor)
		{
			if (cursor == text.size() || (text[cursor] == ',' && depth == 0))
			{
				result.push_back(text.substr(item, cursor - item));
				item = cursor + 1;
			}
			else if (text[cursor] == '<')
				++depth;
			else if (text[cursor] == '>' && depth)
				--depth;
		}
		return result;
	};
	std::function<std::string(std::string, const std::string&)> qualify_import_type = [&](std::string type, const std::string& alias) -> std::string
	{
		if (type.rfind("struct:", 0) == 0 && type.find('.') == std::string::npos)
			return "struct:" + alias + "." + type.substr(7);
		if (type.rfind("array<", 0) == 0)
			return "array<" + qualify_import_type(type.substr(6, type.size() - 7), alias) + ">";
		return type;
	};
	std::set<std::string> import_aliases;
	for (Expr* item : items_)
		if (auto import = dynamic_cast<Import*>(item))
		{
			if (!import_type_metadata_)
				throw Error(import->location, "#import is not available for this compiler entry point");
			if (!import_aliases.insert(import->alias).second || aliases_.contains(import->alias) || structs_.contains(import->alias))
				throw Error(import->location, "import alias '" + import->alias + "' is already declared");
			auto& imported = imported_types_[import->alias];
			std::vector<std::string> metadata;
			try
			{
				metadata = import_type_metadata_(import->path);
			}
			catch (const Error& error)
			{
				throw Error(import->location, error.message);
			}
			for (const std::string& raw : metadata)
			{
				std::string line = raw.rfind("capy type ", 0) == 0 ? raw.substr(10) : raw;
				if (line.rfind("struct ", 0) == 0)
				{
					std::size_t open = line.find('{'), close = line.rfind('}');
					if (open == std::string::npos || close == std::string::npos || close < open)
						throw Error(import->location, "malformed imported struct metadata");
					std::string name = line.substr(7, open - 7);
					std::string qualified = import->alias + "." + name;
					Aggregate aggregate{next_aggregate_type_++, {}};
					std::string fields = line.substr(open + 1, close - open - 1);
					if (!fields.empty())
						for (const std::string& field : split_top_level(fields))
						{
							std::size_t colon = field.find(':');
							if (colon == std::string::npos)
								throw Error(import->location, "malformed imported struct field metadata");
							aggregate.fields.push_back({field.substr(0, colon), qualify_import_type(field.substr(colon + 1), import->alias)});
						}
					if (!structs_.emplace(qualified, std::move(aggregate)).second || !imported.emplace(name, "struct:" + qualified).second)
						throw Error(import->location, "imported type '" + name + "' is already declared");
				}
				else if (line.rfind("alias ", 0) == 0)
				{
					std::size_t equals = line.find('=');
					if (equals == std::string::npos)
						throw Error(import->location, "malformed imported alias metadata");
					std::string name = line.substr(6, equals - 6);
					if (!imported.emplace(name, qualify_import_type(line.substr(equals + 1), import->alias)).second)
						throw Error(import->location, "imported type '" + name + "' is already declared");
				}
			}
		}
	for (Expr* item : items_)
		if (auto alias = dynamic_cast<TypeAlias*>(item))
		{
			if (alias->name == "request")
				throw Error(alias->location, "type request was removed. Use dval for Bearer handler input.");
			if (alias->name == "true" || alias->name == "false" || alias->name == "any" ||
				alias->name == "s8" || alias->name == "s16" || alias->name == "s32" || alias->name == "s64" || alias->name == "u8" || alias->name == "u16" || alias->name == "u32" || alias->name == "u64" || alias->name == "f32" || alias->name == "f64" || alias->name == "bool" ||
				alias->name == "string" || alias->name == "dval" || alias->name == "module" || alias->name == "void")
				throw Error(alias->location, "type alias name '" + alias->name + "' is reserved");
			if (!aliases_.emplace(alias->name, alias).second)
				throw Error(alias->location, "type alias '" + alias->name + "' is already declared");
		}
	// Reserve nominal IDs first so aliases and member types can refer to declarations in either order.
	for (Expr* item : items_)
	{
		check_cancelled();
		if (auto structure = dynamic_cast<Struct*>(item))
		{
			if (aliases_.contains(structure->name))
				throw Error(structure->location, "struct name conflicts with type alias");
			if (structs_.contains(structure->name))
				throw Error(structure->location, "struct '" + structure->name + "' is already declared");
			structs_[structure->name] = {next_aggregate_type_++, {}};
		}
	}
	for (const auto& [name, alias] : aliases_)
		alias_type(name, alias->location);
	for (Expr* item : items_)
	{
		check_cancelled();
		if (auto structure = dynamic_cast<Struct*>(item))
		{
			auto& aggregate = structs_.at(structure->name);
			std::set<std::string> names;
			for (Expr* member : structure->members)
			{
				auto annotation = dynamic_cast<Annotation*>(member);
				auto name = annotation ? dynamic_cast<Name*>(annotation->value) : nullptr;
				if (!name)
					throw Error(member->location, "struct members must be name:type annotations");
				if (annotation->convert)
					throw Error(annotation->location, "'as' conversion markers are valid only on function parameters");
				if (!names.insert(name->value).second)
					throw Error(member->location, "struct member '" + name->value + "' is already declared");
				const std::string type = value_type(annotation->type_expr);
				if (type == "module")
					throw Error(annotation->location, "module is opaque and cannot be stored in struct layouts");
				if (type.rfind("struct:", 0) == 0 && !structs_.contains(type.substr(7)))
					throw Error(annotation->location, "unknown struct type '" + type.substr(7) + "'");
				aggregate.fields.push_back({name->value, type});
			}
		}
	}
	std::set<std::string> handlers;
	auto add_custom_export = [&](const std::string& name, Definition& definition, const Location& location)
	{
		if (handlers.contains(name))
			throw Error(location, "custom DValue export collides with Bearer handler export '" + name + "'");
		for (const auto& existing : custom_exports_)
			if (existing.first == name)
				throw Error(location, "custom DValue export '" + name + "' is already declared");
		custom_exports_.push_back({name, &definition});
	};
	for (Expr* item : items_)
	{
		check_cancelled();
		auto function = dynamic_cast<Function*>(item);
		if (!function)
		{
			if (dynamic_cast<Struct*>(item) || dynamic_cast<Exports*>(item) || dynamic_cast<Import*>(item) || dynamic_cast<TypeAlias*>(item))
				continue;
			throw Error(item->location, "top-level executable expressions are not implemented by the native backend");
		}
		if (aliases_.contains(function->name))
			throw Error(function->location, "function name conflicts with type alias");
		if (function->host)
		{
			if (std::any_of(function->parameters.begin(), function->parameters.end(), [](const Parameter& parameter) { return parameter.default_value; }))
				throw Error(function->location, "host declarations cannot use default parameters");
			std::vector<std::string> parameters;
			for (const auto& parameter : function->parameters)
			{
				if (parameter.variadic)
					throw Error(parameter.type_expr->location, "host declarations cannot use variadic parameters");
				if (parameter.convert)
					throw Error(parameter.type_expr->location, "host declarations cannot request parameter conversion");
				const std::string type = value_type(parameter.type_expr);
				if (type == "void" || (!is_scalar(type) && type != "string" && type != "dval" && type != "module"))
					throw Error(parameter.type_expr->location, "host declarations support scalar, string, dval, and module parameters only");
				parameters.push_back(type);
			}
			if (function->name.rfind("__bearer_", 0) != 0)
				throw Error(function->location, "host declaration names must use the private __bearer_ prefix");
			const std::string result = value_type(function->return_type, true);
			if (result != "void" && !is_scalar(result) && result != "string" && result != "dval" && result != "module")
				throw Error(function->location, "host declarations support scalar, string, dval, module, and void results only");
			const std::string declaration_key = key(function->name, parameters);
			if (!hosts_.emplace(declaration_key, HostDeclaration{parameters, result, function->name.substr(2), function, function->trace_host}).second)
				throw Error(function->location, "host declaration is already declared");
			continue;
		}
		for (std::string_view prefix : {"RENDER_", "COMPONENT_", "SERVE_HTTP_", "TASK_"})
			if (function->name.rfind(prefix, 0) == 0)
				throw Error(function->location, "named Bearer handlers use ':' before the name, for example function COMPONENT:NAME");
		std::string exported;
		bool invalid_task_name = false;
		bool handler = is_handler(function->name, exported, invalid_task_name);
		if (invalid_task_name)
			throw Error(function->location, "TASK handler suffix must match [A-Za-z_][A-Za-z0-9_]*");
		std::vector<std::string> parameters;
		std::vector<bool> conversions;
		std::vector<Expr*> default_values;
		std::vector<int> dependent_parameters;
		bool generic = false;
		bool variadic = false, variadic_convert = false;
		std::string variadic_element;
		for (std::size_t parameter_index = 0; parameter_index < function->parameters.size(); ++parameter_index)
		{
			const auto& parameter = function->parameters[parameter_index];
			const bool any = dynamic_cast<Name*>(parameter.type_expr) && static_cast<Name*>(parameter.type_expr)->value == "any";
			const auto dependency_name = dependent_type_parameter(parameter.type_expr);
			int dependency = -1;
			if (dependency_name)
			{
				for (std::size_t i = 0; i < parameter_index; ++i)
					if (function->parameters[i].name == *dependency_name) dependency = static_cast<int>(i);
				if (dependency < 0)
					throw Error(parameter.type_expr->location, "dependent type names an unknown or forward parameter");
				const Expr* source_type = function->parameters[static_cast<std::size_t>(dependency)].type_expr;
				if (!(dynamic_cast<const Name*>(source_type) && type_name(*source_type) == "any"))
					throw Error(parameter.type_expr->location, "dependent type requires an earlier any parameter");
			}
			generic = generic || any || dependency >= 0;
			dependent_parameters.push_back(dependency);
			std::string type;
			if (any || dependency >= 0)
				type = "any";
			else
				type = value_type(parameter.type_expr);
			if (parameter.variadic)
			{
				variadic = true;
				variadic_convert = parameter.convert;
				variadic_element = type;
				type = "array<" + type + ">";
			}
			parameters.push_back(std::move(type));
			default_values.push_back(parameter.default_value);
			conversions.push_back(parameter.variadic ? false : parameter.convert);
			if (parameter.convert && (any || !dynamic_cast<Name*>(parameter.type_expr)))
				throw Error(parameter.type_expr->location, "'as' parameter conversion requires a concrete named type constructor");
			if (parameters.back() == "void")
				throw Error(parameter.type_expr->location, "function parameters cannot have type void");
		}
		if (std::any_of(default_values.begin(), default_values.end(), [](Expr* value) { return value; }))
		{
			if (variadic)
				throw Error(function->location, "variadic functions cannot use default parameters");
			if (generic)
				throw Error(function->location, "generic functions cannot use default parameters");
			for (std::size_t i = 0; i < default_values.size(); ++i)
				if (default_values[i] && literal_type(default_values[i], parameters[i]) != parameters[i])
					throw Error(default_values[i]->location, "default parameter literal must have type " + parameters[i]);
		}
		if (variadic && function->host)
			throw Error(function->location, "host declarations cannot use variadic parameters");
		if (variadic && generic)
			throw Error(function->location, "variadic parameters cannot use any yet");
		if (handler && variadic)
			throw Error(function->location, "Bearer handlers cannot use variadic parameters");
		if (handler && std::any_of(default_values.begin(), default_values.end(), [](Expr* value) { return value; }))
			throw Error(function->location, "Bearer handlers cannot use default parameters");
		if (handler && generic)
			throw Error(function->location, "Bearer handlers cannot use any parameters");
		if (handler && std::any_of(conversions.begin(), conversions.end(), [](bool value) { return value; }))
			throw Error(function->location, "Bearer handlers cannot request parameter conversion");
		if (handler && parameters != std::vector<std::string>{"dval"})
			throw Error(function->location, "Bearer handlers require exactly one dval parameter. Use the handler input snapshot instead of request readers.");
		if (handler && function->return_type && value_type(function->return_type, true) != "void")
			throw Error(function->location, "Bearer handlers must return void");
		if (handler && !handlers.insert(exported).second)
			throw Error(function->location, "Bearer handler is already declared; handlers cannot be overloaded");
		if (handler && std::any_of(custom_exports_.begin(), custom_exports_.end(), [&](const auto& existing) { return existing.first == exported; }))
			throw Error(function->location, "Bearer handler export collides with custom DValue export '" + exported + "'");
		if (has_struct(function->name))
		{
			std::vector<std::string> fields;
			for (const auto& field : struct_type(function->name, function->location).fields)
				fields.push_back(field.second);
			if (!generic && parameters == fields)
				throw Error(function->location, "constructor duplicates the generated " + function->name + " field constructor");
		}
		if (primitive_constructor_name(function->name) && !generic && parameters.size() == 1 && can_convert(parameters[0], function->name))
			throw Error(function->location, "constructor duplicates the built-in " + function->name + "(" + parameters[0] + ") constructor");
		if (generic)
		{
			int dependent = -1;
			std::string fixed_result = "void";
			if (function->return_type)
			{
				const auto dependency_name = dependent_type_parameter(function->return_type);
				if (dependency_name)
				{
					for (std::size_t i = 0; i < function->parameters.size(); ++i)
						if (function->parameters[i].name == *dependency_name) dependent = static_cast<int>(i);
					if (dependent < 0)
						throw Error(function->return_type->location, "dependent result names an unknown parameter");
					if (dependent_parameters[static_cast<std::size_t>(dependent)] < 0 &&
						!(dynamic_cast<Name*>(function->parameters[static_cast<std::size_t>(dependent)].type_expr) &&
						  type_name(*function->parameters[static_cast<std::size_t>(dependent)].type_expr) == "any"))
						throw Error(function->return_type->location, "dependent result requires an any parameter");
				}
				else
					fixed_result = value_type(function->return_type, true);
			}
			const std::string constructor_result = has_struct(function->name) ? "struct:" + function->name : primitive_constructor_name(function->name) ? function->name : "";
			if (!constructor_result.empty() && (dependent >= 0 || fixed_result != constructor_result))
				throw Error(function->location, "constructor '" + function->name + "' must return " + constructor_result);
			generics_[function->name].push_back({function, parameters, conversions, dependent_parameters, fixed_result, dependent});
		}
		else
		{
			const auto dependency_name = dependent_type_parameter(function->return_type);
			std::string result = handler || dependency_name ? "void" : value_type(function->return_type, true);
			if (dependency_name)
			{
				auto found = std::find_if(function->parameters.begin(), function->parameters.end(),
										  [&](const Parameter& value) { return value.name == *dependency_name; });
				if (found == function->parameters.end())
					throw Error(function->return_type->location, "dependent result names an unknown parameter");
				result = parameters[static_cast<std::size_t>(found - function->parameters.begin())];
			}
			const std::string constructor_result = has_struct(function->name) ? "struct:" + function->name : primitive_constructor_name(function->name) ? function->name : "";
			if (!constructor_result.empty() && result != constructor_result)
				throw Error(function->location, "constructor '" + function->name + "' must return " + constructor_result);
			std::string k = key(function->name + (variadic ? "\x1dvariadic" : ""), parameters);
			if (definitions_by_key_.contains(k))
				throw Error(function->location, "return type does not distinguish overloads");
			Definition definition;
			definition.function = function;
			definition.parameters = parameters;
			definition.convert = conversions;
			definition.default_values = default_values;
			definition.variadic = variadic;
			definition.variadic_element = variadic_element;
			definition.variadic_convert = variadic_convert;
			definition.result = result;
			if (handler)
			{
				Definition adapter;
				adapter.function = function;
				adapter.parameters = {"request"};
				adapter.result = "void";
				adapter.exported = exported;
				adapter.handler_adapter = true;
				adapter.thunk_target = static_cast<unsigned>(definitions_.size());
				definitions_.push_back(std::move(definition));
				definitions_.push_back(std::move(adapter));
				continue;
			}
			if (function->location.file == "capy://stdlib.capy" && parameters.empty() && function->body && function->body->items.size() == 1)
			{
				Expr* body_value = function->body->items[0];
				if (auto yielded = dynamic_cast<Yield*>(body_value))
					body_value = yielded->value;
				if (literal_type(body_value) == result)
					definition.inline_value = body_value;
			}
			definitions_by_key_[k] = definitions_.size();
			definitions_.push_back(std::move(definition));
			if (function->name.rfind("EXPORT_", 0) == 0)
			{
				const std::string name = function->name.substr(7);
				if (name.empty())
					throw Error(function->location, "custom DValue export requires a name after EXPORT_");
				if (parameters != std::vector<std::string>{"dval"} || result != "dval")
					throw Error(function->location, "custom DValue export must have signature (dval) dval");
				add_custom_export(name, definitions_.back(), function->location);
			}
		}
	}
	for (std::size_t index = 0; index < definitions_.size(); ++index)
		if (definitions_[index].function)
			definitions_by_name_[definitions_[index].function->name].push_back(index);
	auto add_function_export = [&](const std::string& line, const Location& location)
	{
		if (std::find(function_exports_.begin(), function_exports_.end(), line) != function_exports_.end())
			throw Error(location, "function export is already declared");
		function_exports_.push_back(line);
	};
	auto add_type_export = [&](const std::string& line, const Location& location)
	{
		if (std::find(type_exports_.begin(), type_exports_.end(), line) != type_exports_.end())
			throw Error(location, "type export is already declared");
		type_exports_.push_back(line);
	};
	auto function_export_line = [](const std::string& name, const std::vector<std::string>& parameters, const std::string& result)
	{
		std::string line = "capy function " + name + "(";
		for (std::size_t i = 0; i < parameters.size(); ++i)
		{
			if (i)
				line += ",";
			line += parameters[i];
		}
		return line + "):" + result;
	};
	auto struct_export_line = [&](const std::string& name)
	{
		std::string line = "capy type struct " + name + "{";
		const auto& fields = structs_.at(name).fields;
		for (std::size_t i = 0; i < fields.size(); ++i)
		{
			if (i)
				line += ",";
			line += fields[i].first + ":" + fields[i].second;
		}
		return line + "}";
	};
	for (Expr* item : items_)
		if (auto exports = dynamic_cast<Exports*>(item))
			for (const std::string& name : exports->names)
			{
				bool exported_something = false;
				if (std::any_of(custom_exports_.begin(), custom_exports_.end(), [&](const auto& existing) { return existing.first == name; }))
					throw Error(exports->location, "custom DValue export '" + name + "' is already declared");
				Definition* target = nullptr;
				for (Definition& definition : definitions_)
					if (definition.function && definition.function->location.file == source_ && definition.function->name == name &&
						definition.parameters == std::vector<std::string>{"dval"} && definition.result == "dval")
						target = &definition;
				if (target)
					add_custom_export(name, *target, exports->location);
				for (Definition& definition : definitions_)
					if (definition.function && definition.function->location.file == source_ && definition.function->name == name)
					{
						add_function_export(function_export_line(name, definition.parameters, definition.result), exports->location);
						exported_something = true;
					}
				if (auto found = generics_.find(name); found != generics_.end())
					for (const GenericDefinition& definition : found->second)
						if (definition.function->location.file == source_)
						{
							std::string result = definition.dependent_result >= 0 ? ("$" + std::to_string(definition.dependent_result) + "::type") : definition.fixed_result;
							std::vector<std::string> patterns = definition.patterns;
							for (std::size_t i = 0; i < patterns.size(); ++i)
								if (definition.dependent_parameters[i] >= 0)
									patterns[i] = "$" + std::to_string(definition.dependent_parameters[i]) + "::type";
							add_function_export(function_export_line(name, patterns, result), exports->location);
							exported_something = true;
						}
				if (structs_.contains(name))
				{
					add_type_export(struct_export_line(name), exports->location);
					exported_something = true;
				}
				if (aliases_.contains(name))
				{
					add_type_export("capy type alias " + name + "=" + resolved_aliases_.at(name), exports->location);
					exported_something = true;
				}
				if (!exported_something)
					throw Error(exports->location, "#exports names unknown local function or type '" + name + "'");
			}
	bool any_export = std::any_of(definitions_.begin(), definitions_.end(), [](const Definition& d) { return !d.exported.empty(); });
	if (!any_export && custom_exports_.empty() && function_exports_.empty() && type_exports_.empty())
		throw Error({source_, 1, 1, 0}, "Capy Bearer unit exports no CLI, RENDER, WS, ONCE, INIT handler, or metadata");
}

// One-shot discovery for a fresh Module. Operation-specific capability sets remain
// Module-owned because lowering consumes them directly; the returned scalar facts
// are the inputs still needed by ABI assignment and emission.
Module::Capabilities Module::discover_capabilities()
{
	// Imports are deliberately discovered before assigning indices.  The direct ABI
	// always imports memory and __memory_base; functions remain demand driven.
	bool scan_format_s64 = false, scan_format_u64 = false, scan_format_f64 = false;
	bool scan_alloc = false;
	bool scan_retain = false, scan_release = false, scan_clone = false, scan_arc_live = false;
	runtime_imports_.clear();
	std::set<std::string> scan_string_names;
	std::map<std::string, std::string> scan_value_names;
	std::function<std::string(Expr*)> scan_value_type = [&](Expr* e) -> std::string
	{
		if (dynamic_cast<Integer*>(e))
			return "s64";
		if (dynamic_cast<SignedInteger*>(e))
			return "s64";
		if (dynamic_cast<UnsignedInteger*>(e))
			return "u64";
		if (dynamic_cast<Float*>(e))
			return "f64";
		if (dynamic_cast<String*>(e))
			return "string";
		if (dynamic_cast<Markup*>(e))
			return "string";
		if (auto block = dynamic_cast<Block*>(e))
		{
			if (block->items.empty()) return "void";
			if (auto yielded = dynamic_cast<Yield*>(block->items.back())) return scan_value_type(yielded->value);
			return dynamic_cast<Return*>(block->items.back()) ? "never" : "void";
		}
		if (auto conditional = dynamic_cast<If*>(e))
		{
			if (!conditional->else_body) return "void";
			const std::string then_type = scan_value_type(conditional->then_body);
			const std::string else_type = scan_value_type(conditional->else_body);
			return then_type == else_type ? then_type : "";
		}
		if (auto array = dynamic_cast<ArrayLiteral*>(e))
		{
			if (array->items.empty()) return array->explicit_element_type ? "array<" + value_type(array->explicit_element_type) + ">" : "";
			std::string element;
			for (Expr* value : array->items)
			{
				std::string item = normalize_spread_type(scan_value_type(value));
				if (item.empty() || (!element.empty() && item != element)) return "";
				element = item;
			}
			return "array<" + element + ">";
		}
		if (auto scope = dynamic_cast<ScopeLookup*>(e))
		{
			if (scope->member == "type_name") return "string";
			if (scope->member == "size") return "s64";
			if (scope->member == "items") return "dval";
			return "";
		}
		if (auto name = dynamic_cast<Name*>(e))
		{
			if (name->value == "true" || name->value == "false")
				return "bool";
			if (auto found = scan_value_names.find(name->value); found != scan_value_names.end())
				return found->second;
			if (name->value == "none")
				return "dval";
		}
		if (auto spread = dynamic_cast<Spread*>(e))
		{
			const std::string source = scan_value_type(spread->value);
			if (source.rfind("array<", 0) == 0) return "spread<" + source.substr(6);
			return "";
		}
		if (auto variable = dynamic_cast<Variable*>(e))
			return variable->annotation ? value_type(variable->annotation) : scan_value_type(variable->value);
		if (dynamic_cast<MapLiteral*>(e))
			return "dval";
		if (auto index = dynamic_cast<Index*>(e))
		{
			const std::string source = scan_value_type(index->value);
			if (source.rfind("array<", 0) == 0) return source.substr(6, source.size() - 7);
			if (source == "dval") return "dval";
		}
		if (auto binary = dynamic_cast<Binary*>(e))
		{
			if (binary->operator_ == "postfix?") return "bool";
			if (binary->operator_ == "=")
			{
				if (auto name = dynamic_cast<Name*>(binary->left))
					return scan_value_type(name);
				if (auto member = dynamic_cast<Member*>(binary->left))
				{
					const std::string receiver = scan_value_type(member->value);
					if (receiver.rfind("struct:", 0) == 0)
						for (const auto& field : struct_type(receiver.substr(7), member->location).fields)
							if (field.first == member->member) return field.second;
					return "";
				}
				if (auto index = dynamic_cast<Index*>(binary->left))
				{
					const std::string receiver = scan_value_type(index->value);
					if (receiver.rfind("array<", 0) == 0) return receiver.substr(6, receiver.size() - 7);
					if (receiver == "dval") return "dval";
					return "";
				}
			}
			const bool comparison = binary->operator_ == "==" || binary->operator_ == "!=" || binary->operator_ == "<" || binary->operator_ == ">" ||
									binary->operator_ == "<=" || binary->operator_ == ">=" || binary->operator_ == "&&" || binary->operator_ == "||" ||
									binary->operator_ == "unary!";
			return comparison ? "bool" : scan_value_type(binary->right);
		}
		if (auto member = dynamic_cast<Member*>(e))
		{
			const std::string receiver = scan_value_type(member->value);
			if (receiver == "dval") return "dval";
			if (receiver.rfind("struct:", 0) == 0)
				for (const auto& field : struct_type(receiver.substr(7), member->location).fields)
					if (field.first == member->member) return field.second;
		}
		if (auto call = dynamic_cast<Call*>(e))
		{
			if (const Member* member = member_call(call))
			{
				const std::string receiver = scan_value_type(member->value);
				if (receiver == "module")
					return "dval";
				if (receiver.rfind("array<", 0) == 0)
				{
					if (member->member == "capacity") return "s64";
					if (member->member == "pop" || member->member == "remove") return receiver.substr(6, receiver.size() - 7);
					if (member->member == "push" || member->member == "insert" || member->member == "clear" || member->member == "reserve" || member->member == "resize") return "void";
				}
				if (receiver.rfind("struct:", 0) == 0)
					for (const auto& field : struct_type(receiver.substr(7), member->location).fields)
						if (field.first == member->member && field.second.rfind("function#", 0) == 0)
							return types_.at(static_cast<unsigned>(std::stoul(field.second.substr(9)))).second;
				std::vector<std::string> arguments{receiver};
				for (Expr* argument : call->arguments)
					arguments.push_back(scan_value_type(argument));
				if (const HostDeclaration* declaration = host(member->member, arguments))
					return declaration->result;
				if (auto result = compatible_result(member->member, arguments, member->location))
					return *result;
			}
			if (auto name = dynamic_cast<Name*>(call->function))
			{
				if (auto found = scan_value_names.find(name->value); found != scan_value_names.end() && found->second.rfind("function(", 0) == 0)
				{
					const std::size_t result = found->second.rfind(") ");
					return result == std::string::npos ? "" : found->second.substr(result + 2);
				}
				const std::string callee = has_alias(name->value) ? constructor_name(name->value, name->location) : name->value;
				if (callee == "dval") return "dval";
				if (has_struct(callee)) return "struct:" + callee;
				if (primitive_constructor_name(callee) && call->arguments.size() == 1 && can_convert(scan_value_type(call->arguments[0]), callee)) return callee;
				if (name->value == "clone") return call->arguments.empty() ? "" : scan_value_type(call->arguments.front());
				if (name->value == "length" || name->value == "arc_live") return "s64";
				if (name->value == "__bearer_byte") return "u8";
				if (name->value == "string_from_bytes") return "string";
				if (name->value == "bytes_of") return "array<u8>";
				if (name->value == "has") return "bool";
				std::vector<std::string> arguments;
				for (Expr* argument : call->arguments)
					arguments.push_back(scan_value_type(argument));
				if (const HostDeclaration* declaration = host(callee, arguments))
					return declaration->result;
				if (auto result = compatible_result(callee, arguments, name->location))
					return *result;
			}
		}
		return "";
	};
	std::function<void(Expr*)> scan_dval;
	scan_dval = [&](Expr* e)
	{
		if (auto map = dynamic_cast<MapLiteral*>(e))
		{
			runtime_imports_.insert("bearer_dv_build_brrb");
			for (const auto& [key, value] : map->entries) scan_dval(value);
			return;
		}
		if (auto array = dynamic_cast<ArrayLiteral*>(e))
		{
			runtime_imports_.insert("bearer_dv_build_brrb");
			for (Expr* value : array->items) scan_dval(value);
			return;
		}
		const std::string type = scan_value_type(e);
		if (auto integer = dynamic_cast<Integer*>(e); integer && type == "s64" && integer_fits(*integer, "s32"))
		{
			runtime_imports_.insert("bearer_dv_s32_to_brrb");
			return;
		}
		if (dynamic_cast<Lambda*>(e) || dynamic_cast<Name*>(e)) { runtime_imports_.insert("bearer_dv_callable_brrb"); runtime_imports_.insert("bearer_dv_callable_at_brrb"); }
		if (type == "string") runtime_imports_.insert("bearer_dv_string_to_brrb");
		else if (type == "s8" || type == "s16" || type == "s32" || type == "u8" || type == "u16") runtime_imports_.insert("bearer_dv_s32_to_brrb");
		else if (type == "s64") runtime_imports_.insert("bearer_dv_s64_to_brrb");
		else if (type == "u32" || type == "u64") runtime_imports_.insert("bearer_dv_u64_to_brrb");
		else if (type == "f32" || type == "f64") runtime_imports_.insert("bearer_dv_f64_to_brrb");
		else if (type == "bool") runtime_imports_.insert("bearer_dv_bool_to_brrb");
		else if (type.rfind("function#", 0) == 0) { runtime_imports_.insert("bearer_dv_callable_brrb"); runtime_imports_.insert("bearer_dv_callable_at_brrb"); }
	};
	std::function<bool(Expr*)> scan_is_string = [&](Expr* e)
	{
		if (dynamic_cast<String*>(e) || dynamic_cast<Markup*>(e))
			return true;
		if (auto name = dynamic_cast<Name*>(e))
			return scan_string_names.contains(name->value);
		if (auto binary = dynamic_cast<Binary*>(e))
			return binary->operator_ == "+" && (scan_is_string(binary->left) || scan_is_string(binary->right));
		if (auto call = dynamic_cast<Call*>(e))
			if (auto name = dynamic_cast<Name*>(call->function))
				if (auto found = definitions_by_name_.find(name->value); found != definitions_by_name_.end())
					for (std::size_t index : found->second)
						if (definitions_[index].result == "string")
							return true;
		return false;
	};
	auto scan_string_construction = [&](const std::string& source)
	{
		if (source == "string") return;
		if (source == "bool") { scan_release = true; return; }
		scan_alloc = scan_retain = scan_release = true;
		scan_format_s64 = scan_format_s64 || source == "s8" || source == "s16" || source == "s32" || source == "s64";
		scan_format_u64 = scan_format_u64 || source == "u8" || source == "u16" || source == "u32" || source == "u64";
		scan_format_f64 = scan_format_f64 || source == "f32" || source == "f64";
		if (source == "dval") { dval_ = true; used_hosts_.insert("bearer_dv_extract_string"); }
		else if (source == "s8" || source == "s16" || source == "s32" || source == "s64") string_format_types_.insert("s64");
		else if (source == "u8" || source == "u16" || source == "u32" || source == "u64") string_format_types_.insert("u64");
		else if (source == "f32" || source == "f64") string_format_types_.insert("f64");
	};
	std::function<void(const std::string&, const std::string&)> scan_construction;
	scan_construction = [&](const std::string& source, const std::string& target)
	{
		if (target == "string")
		{
			scan_string_construction(source);
			if (!exact_definition("string", {source}))
				if (const Definition* constructor = default_definition("string", {source}, {}))
					for (std::size_t i = 1; i < constructor->default_values.size(); ++i)
						if (constructor->parameters[i] == "dval" && constructor->default_values[i])
							scan_dval(constructor->default_values[i]);
		}
		else if (source == "dval" && is_scalar(target))
		{
			dval_ = true;
			scan_alloc = scan_retain = scan_release = true;
			used_hosts_.insert("bearer_dv_extract_" + target);
			if (!exact_definition(target, {source}))
				if (const Definition* constructor = default_definition(target, {source}, {}))
					for (std::size_t i = 1; i < constructor->default_values.size(); ++i)
						if (constructor->parameters[i] == "dval" && constructor->default_values[i])
							scan_dval(constructor->default_values[i]);
		}
		else
		{
			const Definition* constructor = converted_definition(target, {source}, {});
			if (!constructor) constructor = default_definition(target, {source}, {});
			if (!constructor) return;
			if (!exact_definition(target, {source}) && !constructor->parameters.empty() && constructor->convert[0])
				scan_construction(source, constructor->parameters[0]);
			for (std::size_t i = 1; i < constructor->default_values.size(); ++i)
				if (constructor->parameters[i] == "dval" && constructor->default_values[i])
					scan_dval(constructor->default_values[i]);
		}
	};
	Definition* scanning_definition = nullptr;
	std::function<void(Expr*)> scan = [&](Expr* e)
	{
		check_cancelled();
		if (auto scope = dynamic_cast<ScopeLookup*>(e))
		{
			const std::string receiver = scan_value_type(scope->value);
			if (scope->member == "items")
			{
				dval_ = true;
				scan_alloc = scan_retain = scan_release = true;
				runtime_imports_.insert("bearer_capy_reflect_struct_brrb");
				runtime_imports_.insert("bearer_dv_callable_at_brrb");
			}
			else if ((scope->member == "type_name" || scope->member == "size") && managed_type(receiver))
				scan_retain = scan_release = true;
			scan(scope->value);
			return;
		}
		if (auto c = dynamic_cast<Call*>(e))
		{
			const Member* member = member_call(c);
			const Name* named_call = dynamic_cast<Name*>(c->function);
			const std::string named_callee = named_call && has_alias(named_call->value) && !scan_value_names.contains(named_call->value)
				? constructor_name(named_call->value, named_call->location) : named_call ? named_call->value : "";
			if (named_callee == "length" && c->arguments.size() == 1 && scan_value_type(c->arguments[0]) == "dval")
			{
				dval_ = true;
				runtime_imports_.insert("bearer_dv_count_brrb");
			}
			if (named_callee == "__bearer_dval_replace")
			{
				dval_ = true;
				scan_retain = scan_release = true;
				for (Expr* argument : c->arguments) scan(argument);
				return;
			}
			if (member && scan_value_type(member->value).rfind("array<", 0) == 0 &&
				(member->member == "push" || member->member == "pop" || member->member == "insert" || member->member == "remove" ||
				 member->member == "clear" || member->member == "reserve" || member->member == "resize"))
				scan_alloc = scan_retain = scan_release = true;
			if (named_call && has_struct(named_callee))
				scan_alloc = scan_retain = scan_release = true;
			if (auto n = dynamic_cast<Name*>(c->function); n || member)
			{
				std::vector<std::string> host_arguments;
				if (member)
					host_arguments.push_back(scan_value_type(member->value));
				for (Expr* argument : c->arguments)
				{
					const std::string type = scan_value_type(argument);
					host_arguments.push_back(type);
				}
				const std::string callee = member ? member->member : has_alias(n->value) ? constructor_name(n->value, n->location) : n->value;
				if (member)
				{
					const std::string receiver = scan_value_type(member->value);
					if (receiver.rfind("struct:", 0) == 0)
						for (const auto& field : struct_type(receiver.substr(7), member->location).fields)
							if (field.first == member->member && field.second.rfind("function#", 0) == 0)
							{
								const unsigned type = static_cast<unsigned>(std::stoul(field.second.substr(9)));
								if (auto contract = variadic_function_types_.find(type); contract != variadic_function_types_.end() && contract->second.convert)
									for (std::size_t i = contract->second.fixed + 1; i < host_arguments.size(); ++i)
										scan_construction(normalize_spread_type(host_arguments[i]), contract->second.element);
								else if (const auto& signature = types_.at(type).first; !signature.empty() && signature.back() == "array<string>")
									for (std::size_t i = signature.size() - 1; i < host_arguments.size(); ++i)
										scan_string_construction(normalize_spread_type(host_arguments[i]));
								break;
							}
				}
				else if (auto callable = scan_value_names.find(n->value); callable != scan_value_names.end() && callable->second.rfind("function#", 0) == 0)
				{
					const unsigned type = static_cast<unsigned>(std::stoul(callable->second.substr(9)));
					if (auto contract = variadic_function_types_.find(type); contract != variadic_function_types_.end() && contract->second.convert)
						for (std::size_t i = contract->second.fixed; i < host_arguments.size(); ++i)
						{
							std::string source = normalize_spread_type(host_arguments[i]);
							scan_construction(source, contract->second.element);
						}
				}
				if (callee == "__bearer_dv_parse_s8" || callee == "__bearer_dv_parse_s16" || callee == "__bearer_dv_parse_s32" || callee == "__bearer_dv_parse_s64" || callee == "__bearer_dv_parse_u8" || callee == "__bearer_dv_parse_u16" || callee == "__bearer_dv_parse_u32" || callee == "__bearer_dv_parse_u64" || callee == "__bearer_dv_validate_radix")
				used_hosts_.insert(callee.substr(2));
			const HostDeclaration* selected_host = this->host(callee, host_arguments);
				if (!selected_host && callee.rfind("__bearer_", 0) == 0)
				{
					std::vector<Expr*> contextual_arguments;
					if (member) contextual_arguments.push_back(member->value);
					contextual_arguments.insert(contextual_arguments.end(), c->arguments.begin(), c->arguments.end());
					if (auto contextual = contextual_argument_types(callee, contextual_arguments, host_arguments, c->location))
						if (const HostDeclaration* candidate = this->host(callee, *contextual))
						{
							host_arguments = *contextual;
							selected_host = candidate;
						}
				}
				if (selected_host)
				{
					used_hosts_.insert(selected_host->symbol);
					trace_host_ = trace_host_ || selected_host->trace;
					for (const std::string& type : selected_host->parameters)
						if (managed_type(type)) { dval_ = dval_ || type == "dval"; scan_retain = true; scan_release = true; }
					if (managed_type(selected_host->result)) { dval_ = dval_ || selected_host->result == "dval"; scan_alloc = true; scan_retain = true; scan_release = true; }
				}
				const Definition* converted = converted_definition(callee, host_arguments, c->location);
				if (!converted)
					converted = default_definition(callee, host_arguments, c->location);
				if (converted)
				{
					for (std::size_t i = 0; i < host_arguments.size(); ++i)
					{
						if (converted->parameters[i] != host_arguments[i])
						{
							scan_construction(host_arguments[i], converted->parameters[i]);
							if (converted->function->name != "string" && converted->parameters[i] == "string" && host_arguments[i] == "s32")
								scan_string_construction("u64");
						}
						if (converted->parameters[i] == "dval" && host_arguments[i] != "dval")
						{
							Expr* argument = member ? (i == 0 ? member->value : c->arguments.at(i - 1)) : c->arguments.at(i);
							dval_ = true;
							scan_alloc = scan_retain = scan_release = true;
							scan_dval(argument);
						}
					}
					for (std::size_t i = host_arguments.size(); i < converted->parameters.size(); ++i)
						if (converted->default_values[i])
						{
							Expr* default_value = converted->default_values[i];
							scan(default_value);
							scan_construction(scan_value_type(default_value), converted->parameters[i]);
							if (converted->parameters[i] == "dval")
							{
								dval_ = true;
								scan_alloc = scan_retain = scan_release = true;
								scan_dval(default_value);
							}
						}
				}
				if (const Definition* variadic = variadic_definition(callee, host_arguments, c->location))
				{
					const HostDeclaration* sink = fused_variadic_sink(*variadic);
					if (sink) used_hosts_.insert(sink->symbol);
					else scan_alloc = scan_retain = scan_release = true;
					const std::size_t fixed = variadic->parameters.size() - 1;
					for (std::size_t i = fixed; i < host_arguments.size(); ++i)
					{
						std::string source = normalize_spread_type(host_arguments[i]);
						if (variadic->variadic_convert && variadic->variadic_element == "string")
						{
							const std::string type = source == "s8" || source == "s16" || source == "s32" || source == "s64" ? "s64"
								: source == "u8" || source == "u16" || source == "u32" || source == "u64" ? "u64"
								: source == "f32" || source == "f64" ? "f64" : "";
							if (sink && !type.empty())
							{
								fused_sink_formats_.insert({sink->symbol, type});
								string_format_types_.insert(type);
								scan_format_s64 = scan_format_s64 || type == "s64";
								scan_format_u64 = scan_format_u64 || type == "u64";
								scan_format_f64 = scan_format_f64 || type == "f64";
							}
							else
								scan_string_construction(source);
						}
					}
				}
				else if (std::find(host_arguments.begin(), host_arguments.end(), "") != host_arguments.end())
					if (auto found = definitions_by_name_.find(callee); found != definitions_by_name_.end())
					for (std::size_t index : found->second)
						{
							const Definition& candidate = definitions_[index];
							if (candidate.variadic && candidate.parameters.size() - 1 <= host_arguments.size() && candidate.variadic_convert && candidate.variadic_element == "string")
							{
								if (const HostDeclaration* sink = fused_variadic_sink(candidate))
								{
									used_hosts_.insert(sink->symbol);
									for (const std::string type : {"s64", "u64", "f64"}) fused_sink_formats_.insert({sink->symbol, type});
								}
								scan_alloc = scan_retain = scan_release = true;
								scan_format_s64 = scan_format_u64 = scan_format_f64 = true;
								string_format_types_.insert("s64"); string_format_types_.insert("u64"); string_format_types_.insert("f64");
							}
							if (candidate.parameters.size() == host_arguments.size())
								for (std::size_t i = 0; i < host_arguments.size(); ++i)
									if (candidate.convert.size() == host_arguments.size() && candidate.convert[i] && candidate.parameters[i] == "string")
									{
										scan_alloc = scan_retain = scan_release = true;
										scan_format_s64 = scan_format_u64 = scan_format_f64 = true;
										string_format_types_.insert("s64"); string_format_types_.insert("u64"); string_format_types_.insert("f64");
									}
						}
			}
			if (named_call && (named_callee == "dval" || named_callee == "has"))
			{
				dval_ = true;
				scan_alloc = true;
				scan_retain = true;
				scan_release = true;
				if (named_callee == "dval")
				{
					if (!c->arguments.empty())
						scan_dval(c->arguments.front());
				}
				else runtime_imports_.insert("bearer_dv_get_brrb");
			}
			if (named_call && primitive_constructor_name(named_callee) && c->arguments.size() == 1)
				scan_construction(scan_value_type(c->arguments[0]), named_callee);
			if (auto n = dynamic_cast<Name*>(c->function); n && n->value == "clone")
			{
				scan_alloc = true;
				scan_release = true;
				scan_clone = true;
			}
			if (auto n = dynamic_cast<Name*>(c->function); n && (n->value == "string_from_bytes" || n->value == "bytes_of"))
			{
				scan_alloc = true;
				scan_retain = true;
				scan_release = true;
			}
			if (auto n = dynamic_cast<Name*>(c->function); n && n->value == "arc_live")
				scan_arc_live = true;
			if (member)
				scan(member->value);
			for (auto a : c->arguments)
				scan(a);
		}
		else if (auto yielded = dynamic_cast<Yield*>(e))
			scan(yielded->value);
		else if (auto b = dynamic_cast<Block*>(e))
		{
			for (auto x : b->items)
				scan(x);
			if (!b->items.empty() && managed_type(scan_value_type(b)))
				scan_retain = scan_release = true;
		}
		else if (auto f = dynamic_cast<Function*>(e))
		{
			auto outer_strings = scan_string_names;
			auto outer_values = scan_value_names;
			scan_string_names.clear();
			scan_value_names.clear();
			for (const Parameter& parameter : f->parameters)
			{
				std::string type = value_type(parameter.type_expr);
				if (parameter.variadic)
					type = "array<" + type + ">";
				if (type == "string")
					scan_string_names.insert(parameter.name);
				if (!type.empty())
					scan_value_names[parameter.name] = type;
			}
			scan(f->body);
			scan_string_names = std::move(outer_strings);
			scan_value_names = std::move(outer_values);
		}
		else if (auto lambda = dynamic_cast<Lambda*>(e))
		{
			scan_alloc = true;
			scan_retain = true;
			scan_release = true;
			scan(lambda->body);
		}
		else if (auto v = dynamic_cast<Variable*>(e))
		{
			scan(v->value);
			const std::string annotation = v->annotation ? value_type(v->annotation) : "";
			if (annotation.rfind("function#", 0) == 0 && scan_value_type(v->value) == "dval") runtime_imports_.insert("bearer_dv_callable_extract_brrb");
			if (annotation.rfind("function#", 0) == 0)
			{
				const unsigned type = static_cast<unsigned>(std::stoul(annotation.substr(9)));
				if (auto contract = variadic_function_types_.find(type); contract != variadic_function_types_.end() &&
					contract->second.convert && contract->second.element == "string")
				{
					scan_alloc = scan_retain = scan_release = scan_clone = true;
					scan_format_s64 = scan_format_u64 = scan_format_f64 = true;
					string_format_types_.insert("s64"); string_format_types_.insert("u64"); string_format_types_.insert("f64");
				}
			}
			if ((v->annotation && annotation == "string") || (!v->annotation && scan_is_string(v->value)))
				scan_string_names.insert(v->name);
			const std::string inferred = annotation.empty() ? scan_value_type(v->value) : annotation;
			if (!inferred.empty())
				scan_value_names[v->name] = inferred;
			if (managed_type(inferred))
			{
				scan_retain = true;
				scan_release = true;
			}
		}
		else if (auto b = dynamic_cast<Binary*>(e))
		{
			if (b->operator_ == "postfix?")
			{
				dval_ = true;
				scan_alloc = scan_retain = scan_release = true;
				runtime_imports_.insert("bearer_dv_is_none_brrb");
			}
			if (b->operator_ == "=" && (dynamic_cast<Index*>(b->left) || dynamic_cast<Member*>(b->left)))
			{
				const Expr* root = b->left;
				while (true)
					if (auto member = dynamic_cast<const Member*>(root)) root = member->value;
					else if (auto index = dynamic_cast<const Index*>(root)) root = index->value;
					else break;
				if (auto name = dynamic_cast<const Name*>(root); name && scan_value_type(const_cast<Name*>(name)) == "dval")
				{
					dval_ = true;
					scan_alloc = scan_retain = scan_release = true;
					runtime_imports_.insert("bearer_dv_set_path_brrb");
					runtime_imports_.insert("bearer_dv_build_brrb");
					scan_dval(b->right);
					for (const Expr* selector = b->left; selector != root;)
						if (auto member = dynamic_cast<const Member*>(selector))
						{
							runtime_imports_.insert("bearer_dv_string_to_brrb");
							selector = member->value;
						}
						else if (auto index = dynamic_cast<const Index*>(selector))
						{
							const std::string type = scan_value_type(index->index);
							if (type == "string") runtime_imports_.insert("bearer_dv_string_to_brrb");
							else if (type == "s32" || type == "s64") runtime_imports_.insert("bearer_dv_s32_to_brrb");
							selector = index->value;
						}
						else break;
				}
				else
					scan_alloc = scan_retain = scan_release = true;
			}
			if (b->operator_ == "+" && (scan_is_string(b->left) || scan_is_string(b->right)))
			{
				scan_alloc = true;
				scan_retain = true;
				scan_release = true;
			}
			scan(b->left);
			scan(b->right);
			if (b->operator_ == ":=")
				if (auto name = dynamic_cast<Name*>(b->left))
				{
					const std::string inferred = scan_value_type(b->right);
					if (!inferred.empty())
						scan_value_names[name->value] = inferred;
					if (managed_type(inferred))
						scan_retain = scan_release = true;
				}
		}
		else if (auto r = dynamic_cast<Return*>(e))
		{
			if (r->value)
				scan(r->value);
		}
		else if (auto i = dynamic_cast<If*>(e))
		{
			scan(i->condition);
			scan(i->then_body);
			if (i->else_body)
				scan(i->else_body);
		}
		else if (auto w = dynamic_cast<While*>(e))
		{
			scan(w->condition);
			scan(w->body);
		}
		else if (auto a = dynamic_cast<ArrayLiteral*>(e))
		{
			scan_alloc = true;
			scan_retain = true;
			scan_release = true;
			for (auto item : a->items)
				scan(item);
		}
		else if (auto m = dynamic_cast<MapLiteral*>(e))
		{
			dval_ = true;
			scan_alloc = true;
			scan_retain = true;
			scan_release = true;
			scan_dval(m);
			for (const auto& [key, item] : m->entries)
				scan(item);
		}
		else if (auto name = dynamic_cast<Name*>(e))
		{
			if (scanning_definition && !scanning_definition->function->parameters.empty() &&
				name->value == scanning_definition->function->parameters.front().name)
				scanning_definition->first_parameter_used = true;
			if (name->value == "none" && !scan_value_names.contains(name->value))
			{
				dval_ = true;
				scan_alloc = scan_retain = scan_release = true;
				runtime_imports_.insert("bearer_dv_none_brrb");
			}
		}
		else if (auto spread = dynamic_cast<Spread*>(e))
			scan(spread->value);
		else if (auto i = dynamic_cast<Index*>(e))
		{
			const std::string receiver = scan_value_type(i->value);
			if (receiver.empty() || receiver == "dval")
				runtime_imports_.insert("bearer_dv_read_brrb");
			scan(i->value);
			scan(i->index);
		}
		else if (auto m = dynamic_cast<Member*>(e))
		{
			const std::string receiver = scan_value_type(m->value);
			if (receiver.empty() || receiver == "dval")
			{
				dval_ = true;
				scan_alloc = true;
				scan_retain = true;
				scan_release = true;
				runtime_imports_.insert("bearer_dv_read_brrb");
			}
			scan(m->value);
		}
		else if (auto m = dynamic_cast<Markup*>(e))
		{
			scan_alloc = true;
			scan_retain = true;
			scan_release = true;
			for (auto part : m->parts)
				if (auto field = dynamic_cast<MarkupField*>(part))
				{
					const std::string type = scan_value_type(field->value);
					scan_string_construction(type);
					scan(field->value);
				}
		}
		else if (auto f = dynamic_cast<For*>(e))
		{
			const std::string iterable = scan_value_type(f->iterable);
			if (iterable == "dval")
			{
				runtime_imports_.insert("bearer_dv_count_brrb");
				runtime_imports_.insert("bearer_dv_entry_value_brrb");
				if (f->names.size() == 2)
					runtime_imports_.insert("bearer_dv_entry_key_brrb");
			}
			scan(f->iterable);
			auto outer = scan_value_names;
			if (iterable.rfind("array<", 0) == 0 && !f->names.empty())
			{
				scan_value_names[f->names[0]] = iterable.substr(6, iterable.size() - 7);
				if (f->names.size() == 2) scan_value_names[f->names[1]] = "s64";
			}
			else if (iterable == "dval" && !f->names.empty())
			{
				scan_value_names[f->names.front()] = "dval";
				if (f->names.size() == 2) scan_value_names[f->names.back()] = "string";
			}
			else if (!f->names.empty())
				scan_value_names[f->names[0]] = "s64";
			scan(f->body);
			scan_value_names = std::move(outer);
		}
	};
	for (auto& d : definitions_)
	{
		if (d.body_omitted || d.inline_only || d.handler_adapter || (d.function &&
			(d.function->name == "string_from_bytes" || d.function->name == "bytes_of" || d.function->name == "__bearer_reverse_bytes" ||
			 d.function->name == "__bearer_format_s64_capy" || d.function->name == "__bearer_format_u64_capy" || d.function->name == "__bearer_format_f64_capy"))) continue;
		scanning_definition = &d;
		scan(d.function);
		scanning_definition = nullptr;
		for (std::size_t parameter = 0; parameter < d.parameters.size(); ++parameter)
			if (managed_type(d.parameters[parameter]) && (parameter != 0 || d.first_parameter_used))
			{
				scan_retain = true;
				scan_release = true;
			}
		if (managed_type(d.result))
		{
			scan_retain = true;
			scan_release = true;
		}
	}
	for (const auto& [name, generics] : generics_)
		for (const auto& generic : generics)
		{
			auto outer_strings = scan_string_names;
			auto outer_values = scan_value_names;
			scan_string_names.clear();
			scan_value_names.clear();
			for (std::size_t i = 0; i < generic.function->parameters.size(); ++i)
				if (generic.patterns[i] != "any")
				{
					const std::string& type = generic.patterns[i];
					if (type == "string")
						scan_string_names.insert(generic.function->parameters[i].name);
					scan_value_names[generic.function->parameters[i].name] = type;
				}
			scan(generic.function->body);
			scan_string_names = std::move(outer_strings);
			scan_value_names = std::move(outer_values);
		}
	if (std::any_of(definitions_.begin(), definitions_.end(), [&](const Definition& definition) {
		return definition.handler_adapter && definitions_[definition.thunk_target].first_parameter_used;
	}))
	{
		dval_ = true;
		scan_alloc = true;
		scan_release = true;
	}
	if (!custom_exports_.empty())
	{
		dval_ = true;
		scan_alloc = true;
		scan_retain = true;
		scan_release = true;
	}

	return {.format_s64 = scan_format_s64,
			.format_u64 = scan_format_u64,
			.format_f64 = scan_format_f64,
			.alloc = scan_alloc,
			.retain = scan_retain,
			.release = scan_release,
			.clone = scan_clone,
			.arc_live = scan_arc_live};
}

CompileResult Module::compile()
{
	collect();
	prepare_reflection_descriptors();
	check_cancelled();
	for (Definition& definition : definitions_)
		if (definition.function && (definition.function->name == "string_from_bytes" || definition.function->name == "bytes_of" ||
			definition.function->name == "__bearer_reverse_bytes" || definition.function->name == "__bearer_format_s64_capy" ||
			definition.function->name == "__bearer_format_u64_capy" || definition.function->name == "__bearer_format_f64_capy"))
			definition.body_omitted = true;
	for (Definition& definition : definitions_)
		if (definition.function && definition.function->location.file == "capy://stdlib.capy" &&
			(fused_variadic_sink(definition) || definition.inline_value))
		{
			const bool referenced = std::any_of(items_.begin(), items_.end(), [&](Expr* item)
			{
				return references_function_value(item, definition.function->name);
			});
			definition.inline_only = definition.inline_value && !referenced;
			definition.body_omitted = !definition.inline_value && !referenced;
		}
	for (Definition& definition : definitions_)
		if (definition.function && (definition.function->name == "string_from_bytes" || definition.function->name == "bytes_of" ||
			definition.function->name == "__bearer_reverse_bytes" || definition.function->name == "__bearer_format_s64_capy" ||
			definition.function->name == "__bearer_format_u64_capy" || definition.function->name == "__bearer_format_f64_capy"))
			definition.body_omitted = true;
	const Capabilities capabilities = discover_capabilities();
	for (Definition& definition : definitions_)
		if (definition.function)
		{
			const std::string& name = definition.function->name;
			if (name == "__bearer_format_s64_capy" || name == "__bearer_format_u64_capy") definition.body_omitted = false;
			else if (name == "__bearer_format_f64_capy") definition.body_omitted = !capabilities.format_f64;
			else if (name == "__bearer_reverse_bytes") definition.body_omitted = true;
		}
	const bool integer_formatting = true;
	const bool scan_alloc = capabilities.alloc || integer_formatting, scan_retain = capabilities.retain || integer_formatting,
			   scan_release = capabilities.release || integer_formatting, scan_clone = capabilities.clone,
			   scan_arc_live = capabilities.arc_live;
	const bool scan_free = scan_release;
	use_retain_ = scan_retain;
	use_release_ = scan_release;
	use_clone_ = scan_clone;
	use_arc_global_ = scan_arc_live || scan_alloc || scan_release || scan_clone;
	use_trace_global_ = trace_host_;
	if (use_trace_global_)
	{
		while (data_.size() % 8)
			data_.push_back(0);
		trace_stack_offset_ = static_cast<unsigned>(data_.size());
		data_.insert(data_.end(), 256 * 8, 0);
	}
	unsigned next = 0;

	if (scan_alloc || scan_clone)
		imports_["bearer_alloc"] = next++;
	if (scan_free)
		imports_["bearer_free"] = next++;
	for (const std::string& name : runtime_imports_)
		imports_[name] = next++;
	const bool has_handler_input = std::any_of(definitions_.begin(), definitions_.end(), [&](const Definition& definition) {
		return definition.handler_adapter && definitions_[definition.thunk_target].first_parameter_used;
	});
	if (has_handler_input)
		imports_["bearer_handler_input_brrb"] = next++;
	if (!custom_exports_.empty())
	{
		imports_["bearer_dv_ptr_to_brrb"] = next++;
		imports_["bearer_dv_brrb_to_ptr"] = next++;
	}
	std::map<std::string, std::pair<std::vector<std::string>, std::string>> host_signatures;
	for (const auto& [key, host] : hosts_)
	{
		if (!used_hosts_.contains(host.symbol))
			continue;
		std::vector<std::string> abi_parameters;
		for (const std::string& type : host.parameters)
		{
			if (type == "string" || type == "dval") { abi_parameters.push_back("s32"); abi_parameters.push_back("s32"); }
			else if (type == "request") abi_parameters.push_back("s32");
			else abi_parameters.push_back(type);
		}
		if (host.trace) { abi_parameters.push_back("s32"); abi_parameters.push_back("s32"); }
		const bool sized = host.result == "string" || host.result == "dval";
		if (sized) { abi_parameters.push_back("s32"); abi_parameters.push_back("s32"); }
		const std::string abi_result = sized ? "s32" : host.result;
		auto [found, inserted] = host_signatures.emplace(host.symbol, std::pair{abi_parameters, abi_result});
		if (!inserted && found->second != std::pair{abi_parameters, abi_result})
			throw Error(host.function->location, "overloaded host declarations must use one ABI signature");
	}
	for (const auto& [symbol, signature] : host_signatures)
	{
		if (!imports_.contains(symbol))
			imports_[symbol] = next++;
		host_types_[symbol] = wasm_type(signature.first, signature.second);
	}
	if (use_retain_)
		helpers_["retain"] = next++;
	if (use_release_)
		helpers_["release"] = next++;
	if (use_clone_)
		helpers_["clone"] = next++;

	for (const auto& [symbol, type] : fused_sink_formats_)
		helpers_[sink_format_helper(symbol, type)] = next++;
	first_user_index_ = next;
	for (Definition& d : definitions_)
	{
		if (d.inline_only) continue;
		d.index = next++;
		if (d.handler_adapter)
		{
			d.type = wasm_type(d.parameters, d.result);
			continue;
		}
		if (d.thunk_target != 0xffffffffu || d.closure_body)
			continue;
		const std::string contract = d.variadic ? "variadic:" + d.variadic_element + (d.variadic_convert ? ":convert" : "") : "";
		d.type = wasm_type(d.parameters, d.result, contract);
		if (d.variadic) variadic_function_types_[d.type] = {d.parameters.size() - 1, d.variadic_element, d.variadic_convert};
	}
	// Ensure import signatures precede user types and lower after indexes are stable.
	unsigned alloc_type = (scan_alloc || scan_clone) ? wasm_type({"s32"}, "s32") : 0;
	unsigned release_type = scan_free ? wasm_type({"s32"}, "void") : 0;
	unsigned clone_type = scan_clone ? wasm_type({"s32"}, "s32") : 0;
	std::map<std::pair<std::string, std::string>, unsigned> sink_format_types;
	for (const auto& sink : fused_sink_formats_) sink_format_types[sink] = wasm_type({sink.second}, "void");
	unsigned blob_type = wasm_type({"s32", "s32", "s32", "s32"}, "s32");
	unsigned scalar_adapter_type = wasm_type({"s32", "s32", "s32"}, "s32");
	unsigned f64_adapter_type = wasm_type({"f64", "s32", "s32"}, "s32");
	unsigned u64_adapter_type = wasm_type({"u64", "s32", "s32"}, "s32");
	unsigned build_type = wasm_type({"s32", "s32", "s32", "s32", "s32"}, "s32");
	unsigned callable_type = wasm_type({"s32", "s32", "s32", "s32"}, "s32");
	unsigned get_type = wasm_type({"s32", "s32", "s32", "s32", "s32", "s32", "s32", "s32"}, "s32");
	unsigned set_path_type = wasm_type({"s32", "s32", "s32", "s32", "s32", "s32", "s32", "s32"}, "s32");
	unsigned entry_type = wasm_type({"s32", "s32", "s32", "s32", "s32"}, "s32");
	unsigned count_type = wasm_type({"s32", "s32"}, "s32");
	unsigned reflect_struct_type = wasm_type({"s32", "s32", "s32", "s32", "s32"}, "s32");
	auto omitted_body = [](const std::string& result)
	{
		Bytes content{0x00};
		if (result == "s64" || result == "u64") content.insert(content.end(), {0x42, 0x00});
		else if (result == "f32") content.insert(content.end(), {0x43, 0x00, 0x00, 0x00, 0x00});
		else if (result == "f64") { content.push_back(0x44); wasm::append_f64(content, 0.0); }
		else if (result != "void") content.insert(content.end(), {0x41, 0x00});
		content.push_back(0x0b);
		Bytes body;
		wasm::append_uleb(body, static_cast<unsigned>(content.size()));
		body.insert(body.end(), content.begin(), content.end());
		return body;
	};
	std::vector<Bytes> user_bodies;
	user_bodies.reserve(definitions_.size());
	for (std::size_t index = 0; index < definitions_.size(); ++index)
	{
		if (definitions_[index].inline_only) continue;
		user_bodies.push_back(definitions_[index].body_omitted ? omitted_body(definitions_[index].result) : FunctionLowerer(*this, definitions_[index]).lower());
	}
	std::vector<const Definition*> emitted_definitions;
	emitted_definitions.reserve(definitions_.size());
	for (const Definition& definition : definitions_)
		if (!definition.inline_only)
			emitted_definitions.push_back(&definition);
	std::vector<Bytes> bodies = runtime_bodies();
	const std::size_t runtime_count = bodies.size();
	bodies.reserve(runtime_count + user_bodies.size() + custom_exports_.size());
	bodies.insert(bodies.end(), std::make_move_iterator(user_bodies.begin()), std::make_move_iterator(user_bodies.end()));
	for (const auto& [name, target] : custom_exports_)
		bodies.push_back(custom_export_body(*target));

	const unsigned custom_export_type = !custom_exports_.empty() ? wasm_type({"s32"}, "s32") : 0;
	Bytes type_payload;
	wasm::append_uleb(type_payload, static_cast<unsigned>(types_.size()));
	for (const auto& [params, result] : types_)
	{
		type_payload.push_back(0x60);
		wasm::append_uleb(type_payload, static_cast<unsigned>(params.size()));
		for (const std::string& parameter : params)
			type_payload.push_back(wasm_value_type(parameter));
		wasm::append_uleb(type_payload, result == "void" ? 0 : 1);
		if (result != "void")
			type_payload.push_back(wasm_value_type(result));
	}
	Bytes imports;
	wasm::append_uleb(imports, 2 + imports_.size());
	wasm::append_string(imports, "env");
	wasm::append_string(imports, "memory");
	imports.insert(imports.end(), {0x02, 0x00, 0x01});
	wasm::append_string(imports, "env");
	wasm::append_string(imports, "__memory_base");
	imports.insert(imports.end(), {0x03, 0x7f, 0x00});
	std::vector<std::string> import_names(imports_.size());
	for (const auto& [name, index] : imports_)
		import_names[index] = name;
	for (const auto& name : import_names)
	{
		wasm::append_string(imports, "env");
		wasm::append_string(imports, name);
		imports.push_back(0);
		const unsigned type = host_types_.contains(name) ? host_types_.at(name)
			: name == "bearer_alloc" ? alloc_type : name == "bearer_free" ? release_type
			: name == "bearer_dv_string_to_brrb" ? blob_type
			: name == "bearer_dv_f64_to_brrb" ? f64_adapter_type : name == "bearer_dv_s64_to_brrb" || name == "bearer_dv_u64_to_brrb" ? u64_adapter_type
			: name == "bearer_dv_s32_to_brrb" || name == "bearer_dv_bool_to_brrb" ? scalar_adapter_type
			: name == "bearer_dv_none_brrb" ? count_type
			: name == "bearer_dv_build_brrb" ? build_type : name == "bearer_dv_callable_brrb" ? callable_type : name == "bearer_dv_get_brrb" || name == "bearer_dv_read_brrb" ? get_type
			: name == "bearer_dv_callable_extract_brrb" || name == "bearer_dv_callable_at_brrb" ? scalar_adapter_type
			: name == "bearer_dv_is_none_brrb" ? count_type : name == "bearer_dv_set_path_brrb" ? set_path_type
			: name == "bearer_dv_count_brrb" ? count_type
			: name == "bearer_capy_reflect_struct_brrb" ? reflect_struct_type
			: name == "bearer_dv_entry_key_brrb" || name == "bearer_dv_entry_value_brrb" ? entry_type
			: name == "bearer_dv_ptr_to_brrb" || name == "bearer_handler_input_brrb" ? scalar_adapter_type : name == "bearer_dv_brrb_to_ptr" ? count_type : 0;
		wasm::append_uleb(imports, type);
	}
	Bytes functions;
	wasm::append_uleb(functions, static_cast<unsigned>(bodies.size()));
	if (use_retain_)
		wasm::append_uleb(functions, release_type);
	if (use_release_)
		wasm::append_uleb(functions, release_type);
	if (use_clone_)
		wasm::append_uleb(functions, clone_type);
	for (const auto& sink : fused_sink_formats_)
		wasm::append_uleb(functions, sink_format_types.at(sink));
	for (const Definition* definition : emitted_definitions)
		wasm::append_uleb(functions, definition->type);
	for (std::size_t i = 0; i < custom_exports_.size(); ++i)
		wasm::append_uleb(functions, custom_export_type);
	Bytes exports;
	unsigned export_count = static_cast<unsigned>(custom_exports_.size());
	for (const Definition* definition : emitted_definitions)
		if (!definition->exported.empty())
			++export_count;
	wasm::append_uleb(exports, export_count);
	for (const Definition* definition : emitted_definitions)
		if (!definition->exported.empty())
		{
			wasm::append_string(exports, definition->exported);
			exports.push_back(0);
			wasm::append_uleb(exports, definition->index);
		}
	for (std::size_t i = 0; i < custom_exports_.size(); ++i)
	{
		wasm::append_string(exports, custom_exports_[i].first);
		exports.push_back(0);
		wasm::append_uleb(exports, first_user_index_ + static_cast<unsigned>(emitted_definitions.size() + i));
	}
	Bytes data_segment{0, 0x23, 0, 0x0b};
	wasm::append_uleb(data_segment, static_cast<unsigned>(data_.size()));
	data_segment.insert(data_segment.end(), data_.begin(), data_.end());
	Bytes data;
	wasm::append_uleb(data, 1);
	data.insert(data.end(), data_segment.begin(), data_segment.end());
	Bytes code_count;
	wasm::append_uleb(code_count, static_cast<unsigned>(bodies.size()));
	std::size_t code_payload_size = code_count.size();
	for (const Bytes& body : bodies)
	{
		if (body.size() > std::numeric_limits<std::uint32_t>::max() - code_payload_size)
			throw Error({source_, 1, 1, 0}, "generated Wasm code section exceeds the u32 size limit");
		code_payload_size += body.size();
	}
	Bytes mem;
	wasm::append_uleb(mem, static_cast<unsigned>(data_.size()));
	wasm::append_uleb(mem, 3);
	wasm::append_uleb(mem, 0);
	wasm::append_uleb(mem, 0);
	std::string abi = "format=bearer-wasm-unit-abi-v1\nunit_abi_version=" + std::to_string(abi_) + "\ntoolchain=capyc-native-cpp20\nsource=" + artifact_source_ + "\n";
	Bytes result{0, 'a', 's', 'm', 1, 0, 0, 0};
	Bytes dylink{1};
	wasm::append_uleb(dylink, static_cast<unsigned>(mem.size()));
	dylink.insert(dylink.end(), mem.begin(), mem.end());
	wasm::append_custom_section(result, "dylink.0", dylink);
	wasm::append_section(result, 1, type_payload);
	wasm::append_section(result, 2, imports);
	wasm::append_section(result, 3, functions);
	if (!table_functions_.empty())
	{
		Bytes table;
		wasm::append_uleb(table, 1);
		table.insert(table.end(), {0x70, 0x00});
		wasm::append_uleb(table, static_cast<unsigned>(table_functions_.size()));
		wasm::append_section(result, 4, table);
	}
	if (use_arc_global_ || use_trace_global_)
	{
		Bytes globals;
		wasm::append_uleb(globals, (use_arc_global_ ? 1 : 0) + (use_trace_global_ ? 1 : 0));
		if (use_arc_global_)
			globals.insert(globals.end(), {0x7f, 0x01, 0x41, 0x00, 0x0b});
		if (use_trace_global_)
			globals.insert(globals.end(), {0x7f, 0x01, 0x41, 0x00, 0x0b});
		wasm::append_section(result, 6, globals);
	}
	wasm::append_section(result, 7, exports);
	if (!table_functions_.empty())
	{
		Bytes elements;
		wasm::append_uleb(elements, 1);
		elements.insert(elements.end(), {0x00, 0x41, 0x00, 0x0b});
		wasm::append_uleb(elements, static_cast<unsigned>(table_functions_.size()));
		for (unsigned function : table_functions_)
			wasm::append_uleb(elements, function);
		wasm::append_section(result, 9, elements);
	}
	const std::size_t code_section_offset = result.size();
	std::size_t result_capacity = result.size();
	for (std::size_t size : {code_payload_size, data.size(), abi.size(), module_.size(), module_.size(), std::size_t{128}})
	{
		if (size > result.max_size() - result_capacity)
			throw Error({source_, 1, 1, 0}, "generated Wasm module exceeds the platform size limit");
		result_capacity += size;
	}
	result.reserve(result_capacity);
	result.push_back(10);
	wasm::append_uleb(result, static_cast<unsigned>(code_payload_size));
	result.insert(result.end(), code_count.begin(), code_count.end());
	for (const Bytes& body : bodies)
		result.insert(result.end(), body.begin(), body.end());
	const std::size_t code_section_end = result.size();
	wasm::append_section(result, 11, data);
	Bytes named{0};
	Bytes module_string;
	wasm::append_string(module_string, module_);
	wasm::append_uleb(named, static_cast<unsigned>(module_string.size()));
	named.insert(named.end(), module_string.begin(), module_string.end());
	wasm::append_custom_section(result, "name", named);
	wasm::append_custom_section(result, "bearer.abi", Bytes(abi.begin(), abi.end()));
	wasm::append_custom_section(result, "bearer.module", Bytes(module_.begin(), module_.end()));
	auto uleb_size = [](std::size_t value)
	{
		std::size_t size = 1;
		while (value >= 128)
		{
			value >>= 7;
			++size;
		}
		return size;
	};
	auto read_uleb = [](const Bytes& value, std::size_t& offset)
	{
		std::uint32_t result = 0;
		unsigned shift = 0;
		for (;;)
		{
			std::uint8_t byte = value.at(offset++);
			result |= std::uint32_t(byte & 0x7f) << shift;
			if (!(byte & 0x80))
				return result;
			shift += 7;
		}
	};
	std::size_t cursor = code_section_offset + 1 + uleb_size(code_payload_size) + code_count.size();
	for (std::size_t index = 0; index < runtime_count; ++index)
		cursor += bodies[index].size();
	std::vector<std::pair<std::size_t, Location>> source_rows;
	source_rows.reserve(emitted_definitions.size() + custom_exports_.size() + markers_.size());
	for (std::size_t index = runtime_count; index < runtime_count + emitted_definitions.size(); ++index)
	{
		std::size_t instruction = 0;
		read_uleb(bodies[index], instruction); // body byte length
		const std::uint32_t groups = read_uleb(bodies[index], instruction);
		for (std::uint32_t group = 0; group < groups; ++group)
		{
			read_uleb(bodies[index], instruction);
			++instruction;
		}
		const Definition& definition = *emitted_definitions[index - runtime_count];
		const Location& location = definition.function ? definition.function->location : definitions_[definition.thunk_target].function->location;
		source_rows.push_back({cursor + instruction, location});
		cursor += bodies[index].size();
	}
	for (std::size_t index = 0; index < custom_exports_.size(); ++index)
	{
		const Bytes& body = bodies[runtime_count + emitted_definitions.size() + index];
		std::size_t instruction = 0;
		read_uleb(body, instruction);
		const std::uint32_t groups = read_uleb(body, instruction);
		for (std::uint32_t group = 0; group < groups; ++group)
		{
			read_uleb(body, instruction);
			++instruction;
		}
		source_rows.push_back({cursor + instruction, custom_exports_[index].second->function->location});
		cursor += body.size();
	}
	std::vector<std::size_t> marker_offsets(markers_.size(), std::numeric_limits<std::size_t>::max());
	std::vector<bool> marker_ambiguous(markers_.size());
	const Bytes marker_prefix{0x01, 0x01, 0x01, 0x41};
	Bytes canonical_marker = marker_prefix;
	canonical_marker.reserve(10);
	auto code_begin = result.begin() + static_cast<std::ptrdiff_t>(code_section_offset);
	auto code_end = result.begin() + static_cast<std::ptrdiff_t>(code_section_end);
	for (auto cursor = code_begin; cursor != code_end;)
	{
		auto found = std::search(cursor, code_end, marker_prefix.begin(), marker_prefix.end());
		if (found == code_end)
			break;
		auto value_cursor = found + static_cast<std::ptrdiff_t>(marker_prefix.size());
		std::uint32_t encoded = 0;
		unsigned shift = 0;
		std::uint8_t byte = 0;
		for (; value_cursor != code_end && shift < 35; shift += 7)
		{
			byte = *value_cursor++;
			encoded |= std::uint32_t(byte & 0x7f) << shift;
			if (!(byte & 0x80))
				break;
		}
		if (!(byte & 0x80) && value_cursor != code_end && *value_cursor == 0x1a)
		{
			canonical_marker.resize(marker_prefix.size());
			wasm::append_sleb32(canonical_marker, static_cast<std::int32_t>(encoded));
			canonical_marker.push_back(0x1a);
			const std::uint32_t marker_base = 0x5a000000u;
			if (code_end - found >= static_cast<std::ptrdiff_t>(canonical_marker.size()) && std::equal(canonical_marker.begin(), canonical_marker.end(), found) &&
				encoded >= marker_base && encoded - marker_base < markers_.size())
			{
				const std::size_t index = encoded - marker_base;
				const std::size_t offset = static_cast<std::size_t>(found - result.begin());
				if (marker_offsets[index] == std::numeric_limits<std::size_t>::max()) marker_offsets[index] = offset;
				else marker_ambiguous[index] = true;
			}
		}
		cursor = found + 1;
	}
	for (std::size_t index = 0; index < markers_.size(); ++index)
	{
		check_cancelled();
		if (marker_offsets[index] == std::numeric_limits<std::size_t>::max() || marker_ambiguous[index])
			throw Error(markers_[index], "native Capy source marker is missing or ambiguous in final Wasm");
		source_rows.push_back({marker_offsets[index], markers_[index]});
	}
	check_cancelled();
	std::sort(source_rows.begin(), source_rows.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
	std::ostringstream map;
	map << "BEARER_SOURCE_MAP_V1\t" << module_ << "\n";
	for (std::size_t index = 0; index < sources_.size(); ++index)
		map << "F\t" << index + 1 << "\t" << artifact_path(sources_[index]) << "\n";
	for (const auto& [address, location] : source_rows)
	{
		auto source = std::find(sources_.begin(), sources_.end(), location.file);
		if (source == sources_.end())
			throw Error(location, "source location is not registered in this Capy module");
		map << "L\t" << std::hex << address << std::dec << "\t" << (source - sources_.begin()) + 1 << "\t" << location.line << "\t" << location.column << "\n";
	}
	CompileResult compiled{std::move(result), map.str(), {}, {}, {}};
	for (const auto& [name, target] : custom_exports_)
		compiled.custom_exports.push_back(name);
	compiled.function_exports = function_exports_;
	compiled.type_exports = type_exports_;
	return compiled;
}

} // namespace

namespace
{

bool local(const std::vector<std::set<std::string>>& scopes, const std::string& name)
{
	return std::any_of(scopes.rbegin(), scopes.rend(), [&](const auto& scope) { return scope.contains(name); });
}

bool obvious_bool_condition(const Expr* expression)
{
	if (auto name = dynamic_cast<const Name*>(expression))
		return name->value == "true" || name->value == "false";
	if (auto binary = dynamic_cast<const Binary*>(expression))
		return binary->operator_ == "postfix?" || binary->operator_ == "unary!" || binary->operator_ == "&&" || binary->operator_ == "||" ||
			binary->operator_ == "==" || binary->operator_ == "!=" || binary->operator_ == "<" || binary->operator_ == ">" ||
			binary->operator_ == "<=" || binary->operator_ == ">=";
	return false;
}

void collect_stdlib_demand(Expr* expression, std::set<std::pair<std::string, std::size_t>>& calls,
	std::set<FunctionKey, bool (*)(const FunctionKey&, const FunctionKey&)>& values, std::vector<std::set<std::string>>& scopes)
{
	if (!expression)
		return;
	if (auto call = dynamic_cast<Call*>(expression))
	{
		if (const Member* member = member_call(call))
		{
			if (!local(scopes, member->member))
			{
				calls.insert({member->member, call->arguments.size() + 1});
				calls.insert({"call", call->arguments.size() + 2});
			}
			collect_stdlib_demand(member->value, calls, values, scopes);
		}
		else if (auto name = dynamic_cast<Name*>(call->function); name && !local(scopes, name->value))
			calls.insert({name->value, call->arguments.size()});
		else
			collect_stdlib_demand(call->function, calls, values, scopes);
		for (Expr* argument : call->arguments)
			collect_stdlib_demand(argument, calls, values, scopes);
	}
	else if (auto block = dynamic_cast<Block*>(expression))
	{
		scopes.push_back({});
		for (Expr* item : block->items)
			collect_stdlib_demand(item, calls, values, scopes);
		scopes.pop_back();
	}
	else if (auto function = dynamic_cast<Function*>(expression))
	{
		scopes.push_back({});
		for (const auto& parameter : function->parameters)
			scopes.back().insert(parameter.name);
		collect_stdlib_demand(function->body, calls, values, scopes);
		scopes.pop_back();
	}
	else if (auto variable = dynamic_cast<Variable*>(expression))
	{
		const auto name = dynamic_cast<Name*>(variable->value);
		const auto type = variable->annotation ? dynamic_cast<FunctionType*>(variable->annotation) : nullptr;
		if (name && type && !local(scopes, name->value))
		{
			FunctionKey key{name->value, {}};
			for (const auto& parameter : type->parameters)
				key.parameter_types.push_back(type_name(*parameter.type_expr));
			values.insert(std::move(key));
		}
		else
			collect_stdlib_demand(variable->value, calls, values, scopes);
		scopes.back().insert(variable->name);
	}
	else if (auto binary = dynamic_cast<Binary*>(expression))
	{
		collect_stdlib_demand(binary->right, calls, values, scopes);
		if (binary->operator_ != ":=")
			collect_stdlib_demand(binary->left, calls, values, scopes);
		else if (auto name = dynamic_cast<Name*>(binary->left))
			scopes.back().insert(name->value);
	}
	else if (auto returned = dynamic_cast<Return*>(expression))
		collect_stdlib_demand(returned->value, calls, values, scopes);
	else if (auto yielded = dynamic_cast<Yield*>(expression))
		collect_stdlib_demand(yielded->value, calls, values, scopes);
	else if (auto conditional = dynamic_cast<If*>(expression))
	{
		if (!obvious_bool_condition(conditional->condition))
		{
			calls.insert({"bool", 1});
			calls.insert({"bool", 2});
		}
		collect_stdlib_demand(conditional->condition, calls, values, scopes);
		collect_stdlib_demand(conditional->then_body, calls, values, scopes);
		collect_stdlib_demand(conditional->else_body, calls, values, scopes);
	}
	else if (auto loop = dynamic_cast<While*>(expression))
	{
		if (!obvious_bool_condition(loop->condition))
		{
			calls.insert({"bool", 1});
			calls.insert({"bool", 2});
		}
		collect_stdlib_demand(loop->condition, calls, values, scopes);
		collect_stdlib_demand(loop->body, calls, values, scopes);
	}
	else if (auto loop = dynamic_cast<For*>(expression))
	{
		collect_stdlib_demand(loop->iterable, calls, values, scopes);
		scopes.push_back({});
		for (const auto& name : loop->names)
			scopes.back().insert(name);
		collect_stdlib_demand(loop->body, calls, values, scopes);
		scopes.pop_back();
	}
	else if (auto index = dynamic_cast<Index*>(expression))
	{
		collect_stdlib_demand(index->value, calls, values, scopes);
		collect_stdlib_demand(index->index, calls, values, scopes);
	}
	else if (auto member = dynamic_cast<Member*>(expression))
		collect_stdlib_demand(member->value, calls, values, scopes);
	else if (auto scope = dynamic_cast<ScopeLookup*>(expression))
		collect_stdlib_demand(scope->value, calls, values, scopes);
	else if (auto array = dynamic_cast<ArrayLiteral*>(expression))
		for (Expr* item : array->items)
			collect_stdlib_demand(item, calls, values, scopes);
	else if (auto spread = dynamic_cast<Spread*>(expression))
		collect_stdlib_demand(spread->value, calls, values, scopes);
	else if (auto map = dynamic_cast<MapLiteral*>(expression))
		for (const auto& [key, item] : map->entries)
			collect_stdlib_demand(item, calls, values, scopes);
	else if (auto markup = dynamic_cast<Markup*>(expression))
		for (Expr* item : markup->parts)
			collect_stdlib_demand(item, calls, values, scopes);
	else if (auto field = dynamic_cast<MarkupField*>(expression))
		collect_stdlib_demand(field->value, calls, values, scopes);
	else if (auto lambda = dynamic_cast<Lambda*>(expression))
	{
		scopes.push_back({});
		for (const auto& parameter : lambda->parameters)
			scopes.back().insert(parameter.name);
		collect_stdlib_demand(lambda->body, calls, values, scopes);
		scopes.pop_back();
	}
	else if (auto name = dynamic_cast<Name*>(expression); name && !local(scopes, name->value))
		values.insert({name->value, {}});
}

void validate_user_source(const Program& program, const std::set<std::string>& public_names)
{
	auto reject_none_name = [](const std::string& name, const Location& location)
	{
		if (name == "none")
			throw Error(location, "'none' is a reserved literal");
	};
	std::function<void(Expr*)> reject_reserved_calls = [&](Expr* expression)
	{
		if (!expression)
			return;
		if (auto call = dynamic_cast<Call*>(expression))
		{
			if (auto name = dynamic_cast<Name*>(call->function); name && name->value.rfind("__bearer_", 0) == 0)
				throw Error(name->location, "__bearer_* names are reserved for the Capy standard library");
			reject_reserved_calls(call->function);
			for (Expr* argument : call->arguments)
				reject_reserved_calls(argument);
		}
		else if (auto block = dynamic_cast<Block*>(expression)) for (Expr* item : block->items) reject_reserved_calls(item);
		else if (auto function = dynamic_cast<Function*>(expression))
		{
			for (const auto& parameter : function->parameters)
			{
				reject_none_name(parameter.name, function->location);
				if (parameter.name.rfind("__bearer_", 0) == 0)
					throw Error(function->location, "__bearer_* names are reserved for the Capy standard library");
			}
			reject_reserved_calls(function->body);
		}
		else if (auto variable = dynamic_cast<Variable*>(expression))
		{
			reject_none_name(variable->name, variable->location);
			if (variable->name.rfind("__bearer_", 0) == 0)
				throw Error(variable->location, "__bearer_* names are reserved for the Capy standard library");
			reject_reserved_calls(variable->value);
		}
		else if (auto binary = dynamic_cast<Binary*>(expression))
		{
			if (binary->operator_ == ":=")
				if (auto name = dynamic_cast<Name*>(binary->left))
				{
					reject_none_name(name->value, name->location);
					if (name->value.rfind("__bearer_", 0) == 0)
						throw Error(name->location, "__bearer_* names are reserved for the Capy standard library");
				}
			reject_reserved_calls(binary->left); reject_reserved_calls(binary->right);
		}
		else if (auto returned = dynamic_cast<Return*>(expression)) reject_reserved_calls(returned->value);
		else if (auto yielded = dynamic_cast<Yield*>(expression)) reject_reserved_calls(yielded->value);
		else if (auto conditional = dynamic_cast<If*>(expression)) { reject_reserved_calls(conditional->condition); reject_reserved_calls(conditional->then_body); reject_reserved_calls(conditional->else_body); }
		else if (auto loop = dynamic_cast<While*>(expression)) { reject_reserved_calls(loop->condition); reject_reserved_calls(loop->body); }
		else if (auto loop = dynamic_cast<For*>(expression))
		{
			for (const std::string& name : loop->names)
			{
				reject_none_name(name, loop->location);
				if (name.rfind("__bearer_", 0) == 0)
					throw Error(loop->location, "__bearer_* names are reserved for the Capy standard library");
			}
			reject_reserved_calls(loop->iterable); reject_reserved_calls(loop->body);
		}
		else if (auto index = dynamic_cast<Index*>(expression)) { reject_reserved_calls(index->value); reject_reserved_calls(index->index); }
		else if (auto member = dynamic_cast<Member*>(expression)) reject_reserved_calls(member->value);
		else if (auto array = dynamic_cast<ArrayLiteral*>(expression)) for (Expr* item : array->items) reject_reserved_calls(item);
		else if (auto spread = dynamic_cast<Spread*>(expression)) reject_reserved_calls(spread->value);
		else if (auto map = dynamic_cast<MapLiteral*>(expression)) for (const auto& [key, item] : map->entries) reject_reserved_calls(item);
		else if (auto markup = dynamic_cast<Markup*>(expression)) for (Expr* item : markup->parts) reject_reserved_calls(item);
		else if (auto field = dynamic_cast<MarkupField*>(expression)) reject_reserved_calls(field->value);
		else if (auto lambda = dynamic_cast<Lambda*>(expression))
		{
			for (const auto& parameter : lambda->parameters)
			{
				reject_none_name(parameter.name, lambda->location);
				if (parameter.name.rfind("__bearer_", 0) == 0)
					throw Error(lambda->location, "__bearer_* names are reserved for the Capy standard library");
			}
			reject_reserved_calls(lambda->body);
		}
	};
	for (Expr* item : program.items)
	{
		reject_reserved_calls(item);
		if (auto function = dynamic_cast<Function*>(item))
		{
			reject_none_name(function->name, function->location);
			if (function->host)
				throw Error(function->location, "host declarations are available only in the embedded Capy standard library");
			if (function->name.rfind("__bearer_", 0) == 0)
				throw Error(function->location, "__bearer_* names are reserved for the Capy standard library");
			if (public_names.contains(function->name) && !primitive_constructor_name(function->name))
				throw Error(function->location, "'" + function->name + "' is reserved by the Capy standard library");
		}
		else if (auto structure = dynamic_cast<Struct*>(item))
		{
			reject_none_name(structure->name, structure->location);
			if (structure->name.rfind("__bearer_", 0) == 0)
				throw Error(structure->location, "__bearer_* names are reserved for the Capy standard library");
		}
		else if (auto alias = dynamic_cast<TypeAlias*>(item))
		{
			reject_none_name(alias->name, alias->location);
			if (alias->name.rfind("__bearer_", 0) == 0)
				throw Error(alias->location, "__bearer_* names are reserved for the Capy standard library");
			if (public_names.contains(alias->name))
				throw Error(alias->location, "'" + alias->name + "' is reserved by the Capy standard library");
		}
	}
}

std::vector<Expr*> selected_stdlib(const Program& unit, const Program& library)
{
	std::set<std::pair<std::string, std::size_t>> calls;
	std::set<FunctionKey, bool (*)(const FunctionKey&, const FunctionKey&)> values(
		[](const FunctionKey& left, const FunctionKey& right) { return left.name != right.name ? left.name < right.name : left.parameter_types < right.parameter_types; });
	std::vector<std::set<std::string>> scopes{{}};
	calls.insert({"__bearer_format_s64_capy", 1});
	calls.insert({"__bearer_format_u64_capy", 1});
	calls.insert({"__bearer_format_f64_capy", 1});
	for (Expr* item : unit.items)
		collect_stdlib_demand(item, calls, values, scopes);
	std::map<std::string, Expr*> aliases;
	for (Expr* item : unit.items)
		if (auto alias = dynamic_cast<TypeAlias*>(item))
			aliases.emplace(alias->name, alias->value);
	for (Expr* item : unit.items)
		if (auto function = dynamic_cast<Function*>(item))
			for (const auto& parameter : function->parameters)
				if (parameter.convert)
				{
					std::string target = type_name(*parameter.type_expr);
					for (std::size_t checked = 0; checked < aliases.size(); ++checked)
					{
						auto alias = aliases.find(target);
						if (alias == aliases.end())
							break;
						const std::string next = type_name(*alias->second);
						if (next == target)
							break;
						target = next;
					}
					calls.insert({target, 1});
					calls.insert({target, 2});
				}
	std::vector<Function*> functions;
	for (Expr* item : library.items)
		if (auto function = dynamic_cast<Function*>(item))
			functions.push_back(function);
	std::set<Function*> selected;
	for (bool changed = true; changed;)
	{
		changed = false;
		for (Function* function : functions)
		{
			FunctionKey key{function->name, {}};
			for (const auto& parameter : function->parameters)
				key.parameter_types.push_back(type_name(*parameter.type_expr));
			bool called = calls.contains({function->name, function->parameters.size()});
			if (std::any_of(function->parameters.begin(), function->parameters.end(), [](const Parameter& parameter) { return parameter.default_value; }))
			{
				std::size_t required = function->parameters.size();
				while (required && function->parameters[required - 1].default_value)
					--required;
				called = std::any_of(calls.begin(), calls.end(), [&](const auto& call)
				{
					return call.first == function->name && call.second >= required && call.second <= function->parameters.size();
				});
			}
			if (!function->parameters.empty() && function->parameters.back().variadic)
				called = std::any_of(calls.begin(), calls.end(), [&](const auto& call)
				{
					return call.first == function->name && call.second + 1 >= function->parameters.size();
				});
			const bool referenced_value = std::any_of(values.begin(), values.end(), [&](const FunctionKey& value)
			{
				return value.name == key.name && (value.parameter_types.empty() || value.parameter_types == key.parameter_types);
			});
			if (!called && !referenced_value)
				continue;
			if (selected.insert(function).second)
			{
				changed = true;
				if (function->name != "__bearer_format_s64_capy" && function->name != "__bearer_format_u64_capy" && function->name != "__bearer_format_f64_capy")
					collect_stdlib_demand(function, calls, values, scopes);
			}
		}
	}
	std::vector<Expr*> result;
	for (Expr* item : library.items)
		if (auto function = dynamic_cast<Function*>(item); function && (function->host || selected.contains(function)))
			result.push_back(item);
		else if (dynamic_cast<TypeAlias*>(item))
			result.push_back(item);
	return result;
}

std::shared_ptr<const Program> parsed_source(std::string_view source, const std::string& canonical_identity,
	const std::string& diagnostic_identity, unsigned abi_version, CancellationCallback cancelled, ParsedSourceCache* cache, bool pinned = false)
{
	if (!cache || canonical_identity.empty())
		return std::make_shared<const Program>(parse(source, diagnostic_identity, std::move(cancelled)));
	return detail::ParsedSourceCacheAccess::acquire(*cache, source, canonical_identity, diagnostic_identity,
		"capy-parsed-source-v2:" CAPY_COMPILER_BUILD_ID, abi_version, std::move(cancelled), pinned);
}

std::string artifact_source_path(const std::string& source_path, const std::string& source_root)
{
	if (source_path.find("://") != std::string::npos)
		return source_path;
	if (!source_root.empty())
	{
		const auto source = std::filesystem::absolute(source_path).lexically_normal();
		const auto root = std::filesystem::absolute(source_root).lexically_normal();
		const auto relative = source.lexically_relative(root);
		if (!relative.empty() && relative != "." && *relative.begin() != "..")
			return relative.generic_string();
	}
	const std::string basename = std::filesystem::path(source_path).filename().generic_string();
	return basename.empty() ? "<input>" : basename;
}

CompileResult compile_program(const Program& program, const std::string& source_path, const std::string& artifact_source,
	const std::string& module_name, unsigned abi_version, CancellationCallback cancelled, ParsedSourceCache* cache,
	std::function<std::vector<std::string>(const std::string&)> import_type_metadata = {})
{
	auto library = parsed_source(stdlib::text, "capy://stdlib.capy", "capy://stdlib.capy", abi_version, cancelled, cache, true);
	std::set<std::string> public_names;
	for (Expr* item : library->items)
		if (auto function = dynamic_cast<Function*>(item))
			public_names.insert(function->name);
	validate_user_source(program, public_names);
	std::vector<Expr*> items = program.items;
	std::vector<Expr*> selected = selected_stdlib(program, *library);
	items.insert(items.end(), selected.begin(), selected.end());
	Program combined;
	combined.items = items;
	DeclarationIndex declarations;
	declarations.add_program(combined);
	std::vector<std::string> sources{source_path};
	if (!selected.empty())
		sources.push_back("capy://stdlib.capy");
	return Module(std::move(items), std::move(sources), source_path, artifact_source, module_name, abi_version, std::move(cancelled),
		std::move(import_type_metadata)).compile();
}

} // namespace

CompileResult compile_bearer_unit(std::string_view source, const CompileOptions& options)
{
	auto program = parsed_source(source, options.canonical_source_identity, options.source_path, options.abi_version,
		options.cancelled, options.parsed_source_cache);
	return compile_program(*program, options.source_path, artifact_source_path(options.source_path, options.source_root), options.module_name,
		options.abi_version, options.cancelled, options.parsed_source_cache, options.import_type_metadata);
}

CompileResult compile_bearer_unit(const Program& program, const std::string& source_path, const std::string& module_name, unsigned abi_version,
								  CancellationCallback cancelled)
{
	return compile_program(program, source_path, artifact_source_path(source_path, {}), module_name, abi_version, std::move(cancelled), nullptr);
}

CompileResult compile_bearer_file(const std::string& path, CompileOptions options)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw Error({path, 1, 1, 0}, "cannot read Capy source file");
	std::ostringstream source;
	source << input.rdbuf();
	if (options.source_path == "<input>")
		options.source_path = path;
	if (options.canonical_source_identity.empty())
		options.canonical_source_identity = std::filesystem::absolute(path).lexically_normal().string();
	return compile_bearer_unit(source.str(), options);
}

} // namespace capy
