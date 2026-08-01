#include "compiler.h"

#include "frontend.h"
#include "stdlib.embedded.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
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
	return type == "s32" || type == "s64" || type == "u64" || type == "f64" || type == "bool";
}

bool can_convert(const std::string& source, const std::string& target)
{
	return source == target || (is_scalar(source) && is_scalar(target)) || (is_scalar(source) && target == "string") ||
		(source == "markup" && target == "string");
}

bool primitive_constructor_name(const std::string& name)
{
	return name == "s32" || name == "s64" || name == "u64" || name == "f64" || name == "bool" || name == "string";
}

constexpr unsigned scalar_format_scratch_size = 32;

std::string sink_format_helper(const std::string& symbol, const std::string& type)
{
	return "sink_format:" + symbol + ":" + type;
}

std::uint8_t wasm_value_type(const std::string& type)
{
	return type == "s64" || type == "u64" ? 0x7e : type == "f64" ? 0x7c : 0x7f;
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
	return type == "s64" || type == "u64" ? 0x29 : type == "f64" ? 0x2b : 0x28;
}

std::uint8_t array_store_opcode(const std::string& type)
{
	return type == "s64" || type == "u64" ? 0x37 : type == "f64" ? 0x39 : 0x36;
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

void append_u32_le(std::string& out, std::uint32_t value)
{
	out.push_back(static_cast<char>(value));
	out.push_back(static_cast<char>(value >> 8));
	out.push_back(static_cast<char>(value >> 16));
	out.push_back(static_cast<char>(value >> 24));
}

bool managed_type(const std::string& type)
{
	return type == "string" || type == "markup" || type == "dval" || type.rfind("array<", 0) == 0 || type.rfind("struct:", 0) == 0 ||
		   type.rfind("tuple<", 0) == 0 || type.rfind("function#", 0) == 0;
}

std::vector<std::string> aggregate_elements(const std::string& type)
{
	const auto begin = type.find('<');
	if (begin == std::string::npos || type.back() != '>')
		return {};
	std::vector<std::string> result;
	std::size_t item = begin + 1, depth = 0;
	for (std::size_t cursor = item; cursor < type.size(); ++cursor)
	{
		if (type[cursor] == '<')
			++depth;
		else if (type[cursor] == '>')
		{
			if (depth == 0)
			{
				result.push_back(type.substr(item, cursor - item));
				return result;
			}
			--depth;
		}
		else if (type[cursor] == ',' && depth == 0)
		{
			result.push_back(type.substr(item, cursor - item));
			item = cursor + 1;
		}
	}
	return {};
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
	if (auto tuple = dynamic_cast<const TupleExpr*>(expression))
	{
		if (tuple->items.size() < 2)
			throw Error(expression->location, "tuple type requires at least two element types");
		std::string type = "tuple<";
		for (std::size_t i = 0; i < tuple->items.size(); ++i)
		{
			if (i)
				type += ',';
			type += type_of_expression(tuple->items[i]);
		}
		return type + '>';
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
		throw Error(expression->location, "compile-time any and dependent result types are only valid in a generic function declaration");
	if (name == "s32" || name == "s64" || name == "u64" || name == "f64" || name == "bool" || name == "string" || name == "markup" || name == "dval" ||
		name == "request" || name == "module" || name == "void")
		return name;
	return "struct:" + name;
}

std::string literal_type(const Expr* expression)
{
	if (dynamic_cast<const Integer*>(expression)) return "s32";
	if (dynamic_cast<const SignedInteger*>(expression)) return "s64";
	if (dynamic_cast<const UnsignedInteger*>(expression)) return "u64";
	if (dynamic_cast<const Float*>(expression)) return "f64";
	if (dynamic_cast<const String*>(expression)) return "string";
	if (auto name = dynamic_cast<const Name*>(expression); name && (name->value == "true" || name->value == "false")) return "bool";
	return "";
}

struct Definition
{
	Function* function = nullptr;
	std::vector<std::string> parameters;
	std::vector<bool> convert;
	bool variadic = false;
	std::string variadic_element;
	bool variadic_convert = false;
	std::string result;
	std::string exported;
	unsigned index = 0;
	unsigned type = 0;
	unsigned thunk_target = 0xffffffffu;
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
	std::string fixed_result;
	int dependent_result = -1;
};

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
	if (auto conditional = dynamic_cast<If*>(expression))
		return references_function_value(conditional->condition, target) || references_function_value(conditional->then_body, target) || references_function_value(conditional->else_body, target);
	if (auto loop = dynamic_cast<While*>(expression)) return references_function_value(loop->condition, target) || references_function_value(loop->body, target);
	if (auto loop = dynamic_cast<For*>(expression)) return references_function_value(loop->iterable, target) || references_function_value(loop->body, target);
	if (auto index = dynamic_cast<Index*>(expression)) return references_function_value(index->value, target) || references_function_value(index->index, target);
	if (auto member = dynamic_cast<Member*>(expression)) return references_function_value(member->value, target);
	if (auto tuple = dynamic_cast<TupleExpr*>(expression))
	{
		for (Expr* item : tuple->items) if (references_function_value(item, target)) return true;
		return false;
	}
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
	std::vector<unsigned> owned_array_parameters_;
	unsigned local_count_ = 0;
	std::vector<std::string> local_types_;
	bool implicit_result_ = false;
	std::set<const Expr*> owned_expression_results_;
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
	std::optional<std::pair<unsigned, std::string>> compatible_local_callable(const std::string& name, const std::vector<std::string>& arguments) const;
	std::vector<std::pair<std::string, std::string>> lambda_captures(Lambda* value) const;
	std::tuple<std::string, unsigned, unsigned, Definition*, std::vector<std::pair<std::string, std::string>>> register_lambda(Lambda* value);
	Bytes markup_escape_length(unsigned source, unsigned total, const Location& location);
	Bytes markup_escape_write(unsigned source, unsigned cursor, const Location& location);
	Bytes markup_s32_length(unsigned source, unsigned total, const Location& location);
	Bytes markup_s32_write(unsigned source, unsigned cursor, const Location& location);
	Bytes markup_write_bytes(unsigned cursor, std::string_view text);
	std::pair<Bytes, std::string> dval_value(Expr* value);
	std::pair<Bytes, std::string> dval_lookup(Expr* value, Expr* key, bool require_present);
	std::pair<Bytes, std::string> dval_scalar(Call* call, const std::string& result);
	std::pair<Bytes, std::string> array_method(Call* call, const Member* member);
	Bytes array_ensure_unique(unsigned slot, const std::string& array_type, unsigned required, const Location& location);
	std::pair<Bytes, unsigned> allocate_blob(const std::string& type, unsigned type_id, unsigned length, const Location& location);
	Bytes format_wide_scalar(Bytes code, const std::string& type, const Location& location);
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
	bool expression_is_owned(const Expr* value);
	std::pair<unsigned, std::string> lookup(const Name* name) const;
	unsigned add_local(const std::string& name, const std::string& type, const Location& location);
	static void append(Bytes& target, const Bytes& source)
	{
		target.insert(target.end(), source.begin(), source.end());
	}
};

struct Module
{
	Module(std::vector<Expr*> items, std::vector<std::string> sources, std::string source, std::string module, unsigned abi, CancellationCallback cancelled)
		: items_(std::move(items)), sources_(std::move(sources)), source_(std::move(source)), module_(std::move(module)), abi_(abi), cancelled_(std::move(cancelled))
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
	unsigned add_static_closure(unsigned slot)
	{
		while (data_.size() % 8)
			data_.push_back(0);
		const unsigned offset = static_cast<unsigned>(data_.size());
		const std::uint32_t header[] = {0xffffffffu, 0xffffffffu, 0x3fffffffu, 20u, slot};
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
		const std::uint32_t header[] = {0xffffffffu, 0xffffffffu, 1u, static_cast<std::uint32_t>(20 + text.size()), static_cast<std::uint32_t>(text.size())};
		for (std::uint32_t value : header)
			for (unsigned byte = 0; byte != 4; ++byte)
				data_.push_back(static_cast<std::uint8_t>(value >> (8 * byte)));
		data_.insert(data_.end(), text.begin(), text.end());
		return offset;
	}
	unsigned import_index(const std::string& name) const
	{
		auto found = imports_.find(name);
		if (found == imports_.end())
			throw std::runtime_error("missing native Capy import " + name);
		return found->second;
	}
	unsigned helper_index(const std::string& name) const
	{
		auto found = helpers_.find(name);
		if (found == helpers_.end())
			throw std::runtime_error("missing native Capy helper " + name);
		return found->second;
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
	unsigned tuple_type(const std::string& type)
	{
		auto found = tuples_.find(type);
		if (found != tuples_.end())
			return found->second;
		const unsigned id = next_aggregate_type_++;
		tuples_[type] = id;
		return id;
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
	std::string named_type(const std::string& name, const Location& location)
	{
		if (has_alias(name))
			return alias_type(name, location);
		if (name == "s32" || name == "s64" || name == "u64" || name == "f64" || name == "bool" || name == "string" || name == "markup" ||
			name == "dval" || name == "request" || name == "module" || name == "void")
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
		for (std::size_t i = 0; i < definitions_.size(); ++i)
			if (definitions_[i].function && definitions_[i].function->name == name && definitions_[i].exported.empty())
				candidates.push_back(i);
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
		if (target == "dval" && (source == "string" || source == "s32" || source == "u64" || source == "f64" || source == "bool" || source == "dval"))
			return true;
		if (target.rfind("struct:", 0) == 0)
		{
			auto structure = structs_.find(name);
			if (structure != structs_.end() && structure->second.fields.size() == 1 && structure->second.fields[0].second == source)
				return true;
		}
		if (const Definition* exact = exact_definition(name, {source}))
			return exact->result == target;
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
				if (generic.patterns.size() != types.size())
					continue;
				bool matches = true;
				for (std::size_t i = 0; i < types.size(); ++i)
					if (generic.patterns[i] != "any" && generic.patterns[i] != types[i]) { matches = false; break; }
				if (matches)
					return nullptr;
			}
		const Definition* selected = nullptr;
		unsigned best = std::numeric_limits<unsigned>::max();
		bool ambiguous = false;
		for (const Definition& definition : definitions_)
		{
			if (!definition.function || definition.function->name != name || definition.parameters.size() != types.size() || definition.convert.size() != types.size())
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
	const Definition* variadic_definition(const std::string& name, const std::vector<std::string>& types, const Location& location) const
	{
		const Definition* selected = nullptr;
		unsigned best_conversions = std::numeric_limits<unsigned>::max(), best_fixed = 0;
		bool ambiguous = false;
		for (const Definition& definition : definitions_)
		{
			if (!definition.function || definition.function->name != name || !definition.variadic || definition.parameters.empty())
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
				const bool spread = types[i].rfind("spread<", 0) == 0;
				const std::string source = spread ? types[i].substr(7, types[i].size() - 8) : types[i];
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
		std::vector<const GenericDefinition*> candidates;
		unsigned best = 0;
		if (auto found = generics_.find(name); found != generics_.end())
			for (const auto& generic : found->second)
			{
				if (generic.patterns.size() != types.size())
					continue;
				unsigned exact = 0;
				bool matches = true;
				for (std::size_t i = 0; i < types.size(); ++i)
					if (generic.patterns[i] != "any" && generic.patterns[i] != types[i]) { matches = false; break; }
					else if (generic.patterns[i] != "any") ++exact;
				if (!matches) continue;
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
		return std::nullopt;
	}
	Definition* compatible_definition(const std::string& name, const std::vector<std::string>& types, const Location& location)
	{
		if (auto it = definitions_by_key_.find(key(name, types)); it != definitions_by_key_.end())
			return &definitions_[it->second];
		std::vector<const GenericDefinition*> candidates;
		unsigned best = 0;
		for (const auto& generic : generics_[name])
		{
			if (generic.patterns.size() != types.size())
				continue;
			unsigned exact = 0;
			bool matches = true;
			for (std::size_t i = 0; i < types.size(); ++i)
			{
				if (generic.patterns[i] == "any")
					continue;
				if (generic.patterns[i] != types[i])
				{
					matches = false;
					break;
				}
				++exact;
			}
			if (!matches)
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
			return const_cast<Definition*>(variadic_definition(name, types, location));
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
		definitions_.push_back(std::move(definition));
		return &definitions_.back();
	}
	Definition& resolve(const std::string& name, const std::vector<std::string>& types, const Location& location)
	{
		if (Definition* definition = compatible_definition(name, types, location))
			return *definition;
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
	const std::vector<Expr*> items_;
	const std::vector<std::string> sources_;
	std::string source_, module_;
	unsigned abi_;
	CancellationCallback cancelled_;
	std::deque<Definition> definitions_;
	std::deque<Function> lambda_functions_;
	std::unordered_map<std::string, std::size_t> definitions_by_key_;
	std::map<std::string, std::vector<GenericDefinition>> generics_;
	unsigned first_user_index_ = 0;
	std::map<std::string, unsigned> function_values_;
	std::unordered_map<const Lambda*, std::tuple<std::string, unsigned, unsigned, Definition*, std::vector<std::pair<std::string, std::string>>>> lambdas_;
	std::map<unsigned, std::vector<std::string>> closure_types_;
	std::vector<unsigned> table_functions_;
	std::map<std::string, unsigned> imports_;
	std::set<std::string> runtime_imports_;
	std::unordered_map<std::string, HostDeclaration> hosts_;
	std::map<std::string, TypeAlias*> aliases_;
	std::map<std::string, std::string> resolved_aliases_;
	std::set<std::string> resolving_aliases_;
	std::set<std::string> used_hosts_;
	std::map<std::string, unsigned> host_types_;
	std::map<std::string, unsigned> helpers_;
	std::set<std::pair<std::string, std::string>> fused_sink_formats_;
	std::set<std::string> string_format_types_;
	unsigned fused_sink_scratch_offset_ = 0;
	Bytes data_;
	bool dval_ = false, trace_host_ = false, use_trace_global_ = false;
	unsigned trace_stack_offset_ = 0;
	std::vector<std::pair<std::string, Definition*>> custom_exports_;
	bool use_retain_ = false, use_release_ = false, use_clone_ = false, use_arc_global_ = false;
	std::vector<Location> markers_;
	std::vector<std::pair<std::vector<std::string>, std::string>> types_;
	struct VariadicFunctionType { std::size_t fixed; std::string element; bool convert; };
	std::map<unsigned, VariadicFunctionType> variadic_function_types_;
	std::map<std::string, unsigned> type_indices_;
	std::map<std::string, Aggregate> structs_;
	std::map<std::string, unsigned> tuples_;
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
	Bytes custom_export_body(const Definition& target) const;
};

std::string Module::value_type(const Expr* expression, bool allow_void)
{
	if (auto tuple = dynamic_cast<const TupleExpr*>(expression))
	{
		if (tuple->items.size() < 2)
			throw Error(expression->location, "tuple type requires at least two element types");
		std::string type = "tuple<";
		for (std::size_t i = 0; i < tuple->items.size(); ++i)
		{
			const std::string field = value_type(tuple->items[i]);
			if (field == "module")
				throw Error(tuple->items[i]->location, "module is opaque and cannot be stored in tuple layouts");
			type += (i ? "," : "") + field;
		}
		return type + ">";
	}
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
			if (type.rfind("array<", 0) == 0)
			{
				owned_array_parameters_.push_back(parameter);
				owned_scopes_.front().push_back({parameter, type});
			}
			else if (managed_type(type))
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
			const std::string type = definition.exported.empty() ? definition.parameters[parameter] : "request";
			scopes_.back()[value.name] = {parameter, type};
			if (type.rfind("array<", 0) == 0)
			{
				owned_array_parameters_.push_back(parameter);
				owned_scopes_.front().push_back({parameter, type});
			}
			else if (managed_type(type))
				borrowed_managed_slots_.insert(parameter);
			++parameter;
		}
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
	if (dynamic_cast<const ArrayLiteral*>(value) || dynamic_cast<const MapLiteral*>(value) || dynamic_cast<const TupleExpr*>(value) ||
		dynamic_cast<const Markup*>(value))
		return true;
	if (auto index = dynamic_cast<const Index*>(value))
	{
		if (infer(index->value) == "dval")
			return true;
		return managed_type(infer(const_cast<Index*>(index))) && expression_is_owned(index->value);
	}
	if (auto member = dynamic_cast<const Member*>(value))
		return managed_type(infer(member->value)) && expression_is_owned(member->value);
	if (auto binary = dynamic_cast<const Binary*>(value))
		return binary->operator_ == "+" && infer(binary->left) == "string" && infer(binary->right) == "string";
	if (auto call = dynamic_cast<const Call*>(value))
		return managed_type(infer(const_cast<Call*>(call)));
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
				visit(binary->right);
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
		else if (auto tuple = dynamic_cast<TupleExpr*>(value))
			for (Expr* item : tuple->items)
				visit(item);
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
		Expr* item = block_value->items[i];
		const bool final = i + 1 == block_value->items.size();
		if (dynamic_cast<Return*>(item) || dynamic_cast<Break*>(item) || dynamic_cast<Continue*>(item))
			result = "never";
		else if (auto conditional = dynamic_cast<If*>(item); conditional && !final)
		{
			if (infer(conditional->condition) != "bool")
				throw Error(conditional->condition->location, "if condition must be bool");
			result = "void";
		}
		else
			result = infer(item);
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

std::string FunctionLowerer::infer(Expr* value)
{
	if (dynamic_cast<Integer*>(value))
		return "s32";
	if (dynamic_cast<UnsignedInteger*>(value))
		return "u64";
	if (dynamic_cast<SignedInteger*>(value))
		return "s64";
	if (dynamic_cast<Float*>(value))
		return "f64";
	if (dynamic_cast<String*>(value))
		return "string";
	if (dynamic_cast<Markup*>(value))
		return "markup";
	if (auto lambda = dynamic_cast<Lambda*>(value))
		return std::get<0>(register_lambda(lambda));
	if (auto block = dynamic_cast<Block*>(value))
		return infer_block(block);
	if (auto conditional = dynamic_cast<If*>(value))
	{
		const auto saved_scopes = scopes_;
		const std::string condition = infer(conditional->condition);
		if (condition != "bool")
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
		return module_.reference_function(name->value, name->location).first;
	}
	if (auto tuple = dynamic_cast<TupleExpr*>(value))
	{
		if (tuple->items.size() < 2)
			throw Error(value->location, "tuple value requires at least two elements");
		std::string type = "tuple<";
		for (std::size_t i = 0; i < tuple->items.size(); ++i)
		{
			const std::string field = infer(tuple->items[i]);
			if (field == "module")
				throw Error(tuple->items[i]->location, "module is opaque and cannot be stored in tuple layouts");
			type += (i ? "," : "") + field;
		}
		return type + ">";
	}
	if (auto variable = dynamic_cast<Variable*>(value))
	{
		const std::string declared = variable->annotation ? module_.value_type(variable->annotation) : "";
		const bool typed_empty_array = variable->annotation && dynamic_cast<ArrayLiteral*>(variable->value) &&
			static_cast<ArrayLiteral*>(variable->value)->items.empty() && declared.rfind("array<", 0) == 0;
		const std::string actual = typed_empty_array ? declared : infer(variable->value);
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
		if (source.rfind("tuple<", 0) == 0)
			return "spread-" + source;
		throw Error(spread->location, "spread requires an array or tuple");
	}
	if (dynamic_cast<MapLiteral*>(value))
		throw Error(value->location, "map literals must be wrapped in dval(...)");
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
			else
				item_type = infer(item);
			if (element.empty()) element = item_type;
			if (item_type != element) throw Error(item->location, "array literal elements must have one type");
		}
		if (element == "module")
			throw Error(value->location, "module is opaque and cannot be stored in array layouts");
		return "array<" + element + ">";
	}
	if (auto member = dynamic_cast<Member*>(value))
	{
		const std::string object = infer(member->value);
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
			if (key != "string" && key != "s32")
				throw Error(index->index->location, "dval index must be string or s32");
			return "dval";
		}
		if (object.rfind("array<", 0) == 0)
			return object.substr(6, object.size() - 7);
		if (object.rfind("tuple<", 0) == 0)
		{
			auto integer = dynamic_cast<Integer*>(index->index);
			auto elements = aggregate_elements(object);
			if (!integer || integer->value < 0 || static_cast<std::size_t>(integer->value) >= elements.size())
				throw Error(index->index->location, "tuple index is out of bounds");
			return elements[integer->value];
		}
		throw Error(index->location, "indexing requires an array or tuple");
	}
	if (auto call = dynamic_cast<Call*>(value))
	{
		if (const Member* member = member_call(call))
		{
			const std::string receiver = infer(member->value);
			if (receiver.rfind("array<", 0) == 0)
			{
				const std::string element = receiver.substr(6, receiver.size() - 7);
				const std::size_t count = call->arguments.size();
				if (member->member == "capacity")
				{
					if (count != 0) throw Error(call->location, "array capacity expects no arguments");
					return "s32";
				}
				if (member->member == "push")
				{
					if (count != 1 || infer(call->arguments[0]) != element) throw Error(call->location, "array push expects one " + element + " value");
					return "void";
				}
				if (member->member == "pop")
				{
					if (count != 0) throw Error(call->location, "array pop expects no arguments");
					return element;
				}
				if (member->member == "insert")
				{
					if (count != 2 || infer(call->arguments[0]) != "s32" || infer(call->arguments[1]) != element)
						throw Error(call->location, "array insert expects an s32 index and " + element + " value");
					return "void";
				}
				if (member->member == "remove")
				{
					if (count != 1 || infer(call->arguments[0]) != "s32") throw Error(call->location, "array remove expects one s32 index");
					return element;
				}
				if (member->member == "clear" || member->member == "reserve")
				{
					if (count != (member->member == "reserve" ? 1u : 0u) || (count && infer(call->arguments[0]) != "s32"))
						throw Error(call->location, "array " + member->member + (count ? " expects an s32 capacity" : " expects no arguments"));
					return "void";
				}
				if (member->member == "resize")
				{
					if (count != 2 || infer(call->arguments[0]) != "s32" || infer(call->arguments[1]) != element)
						throw Error(call->location, "array resize expects an s32 length and " + element + " fill value");
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
				if (auto spread = dynamic_cast<Spread*>(argument); spread && infer(spread->value).rfind("tuple<", 0) == 0)
				{
					if (auto literal = dynamic_cast<TupleExpr*>(spread->value))
						for (Expr* item : literal->items) argument_types.push_back({infer(item), item->location});
					else
						for (const std::string& field : aggregate_elements(infer(spread->value))) argument_types.push_back({field, spread->location});
				}
				else
					argument_types.push_back({infer(argument), argument->location});
			auto variadic = module_.variadic_function_types_.find(type);
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
				std::string source = argument_types[i].first;
				if (source.rfind("spread<", 0) == 0) source = source.substr(7, source.size() - 8);
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
			if (argument != "string" && argument != "s32" && argument != "u64" && argument != "f64" && argument != "bool" && argument != "dval")
				throw Error(call->arguments[0]->location, "cannot construct dval from " + argument);
			return "dval";
		}
		std::vector<std::string> arguments;
		for (Expr* argument : call->arguments)
			if (auto spread = dynamic_cast<Spread*>(argument); spread && infer(spread->value).rfind("tuple<", 0) == 0)
			{
				if (auto literal = dynamic_cast<TupleExpr*>(spread->value))
					for (Expr* item : literal->items) arguments.push_back(infer(item));
				else
					for (const std::string& field : aggregate_elements(infer(spread->value))) arguments.push_back(field);
			}
			else
				arguments.push_back(infer(argument));
		if (primitive_constructor_name(type_callee) && arguments.size() == 1)
		{
			if (Definition* exact = module_.exact_definition(type_callee, arguments))
				return exact->result;
			if (can_convert(arguments[0], type_callee))
				return type_callee;
			return module_.resolve(type_callee, arguments, call->location).result;
		}
		if (module_.has_struct(type_callee))
		{
			std::vector<std::string> fields;
			for (const auto& field : module_.struct_type(type_callee, call->location).fields)
				fields.push_back(field.second);
			if (arguments == fields)
				return "struct:" + type_callee;
			return module_.resolve(type_callee, arguments, call->location).result;
		}
		if (name->value == "clone")
			return infer(call->arguments.at(0));
		if (name->value == "length" || name->value == "arc_live") return "s32";
		if (name->value == "dval_has") return "bool";
		if (name->value == "dval_string") return "string";
		if (name->value == "dval_s32") return "s32";
		if (name->value == "dval_f64") return "f64";
		if (name->value == "dval_bool") return "bool";
		if (name->value == "trusted_markup") return "markup";
		if (name->value == "trap")
			return "void";
		if (const Module::HostDeclaration* declaration = module_.host(name->value, arguments))
			return declaration->result;
		return module_.resolve(name->value, arguments, call->location).result;
	}
	if (auto binary = dynamic_cast<Binary*>(value))
	{
		if (binary->operator_ == "..")
			return "range";
		if (binary->operator_ == "=" || binary->operator_ == ":=")
			return infer(binary->right);
		if (binary->operator_ == "&&" || binary->operator_ == "||" || binary->operator_ == "unary!")
		{
			if (infer(binary->right) != "bool" || (binary->operator_ != "unary!" && infer(binary->left) != "bool"))
				throw Error(binary->location, "logical operators require bool operands");
			return "bool";
		}
		if (infer(binary->left) == "string" || infer(binary->right) == "string")
		{
			if (infer(binary->left) != "string" || infer(binary->right) != "string")
				throw Error(binary->location, "string operators require string operands");
			if (binary->operator_ == "+") return "string";
			if (binary->operator_ == "==" || binary->operator_ == "!=") return "bool";
			throw Error(binary->location, "strings support only +, ==, and != operators");
		}
		if (binary->operator_ == "unary-")
		{
			const std::string operand = infer(binary->right);
			if (operand != "s32" && operand != "s64" && operand != "f64")
				throw Error(binary->location, "unary - requires an s32, s64, or f64 operand");
			return operand;
		}
		const std::string left = infer(binary->left), right = infer(binary->right);
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

Bytes FunctionLowerer::markup_escape_length(unsigned source, unsigned total, const Location& location)
{
	const unsigned index = add_local("", "s32", location), length = add_local("", "s32", location), byte = add_local("", "s32", location);
	Bytes code{0x20};
	wasm::append_uleb(code, source);
	code.insert(code.end(), {0x28, 0x02, 0x10, 0x21});
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
	code.push_back(0x20);
	wasm::append_uleb(code, total);
	code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
	wasm::append_uleb(code, total);
	for (const auto [character, extra] : {std::pair<std::int32_t, std::int32_t>{'&', 4}, {'<', 3}, {'>', 3}, {'"', 5}, {'\'', 4}})
	{
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
	code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
	return code;
}

Bytes FunctionLowerer::markup_escape_write(unsigned source, unsigned cursor, const Location& location)
{
	const unsigned index = add_local("", "s32", location), length = add_local("", "s32", location), byte = add_local("", "s32", location);
	Bytes code{0x20};
	wasm::append_uleb(code, source);
	code.insert(code.end(), {0x28, 0x02, 0x10, 0x21});
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
	code.insert(code.end(), {0x02, 0x40});
	for (const auto& [character, escaped] :
		 std::initializer_list<std::pair<std::int32_t, std::string_view>>{{'&', "&amp;"}, {'<', "&lt;"}, {'>', "&gt;"}, {'"', "&quot;"}, {'\'', "&#39;"}})
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
	code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
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

std::pair<Bytes, unsigned> FunctionLowerer::allocate_blob(const std::string& type, unsigned type_id, unsigned length, const Location& location)
{
	const unsigned pointer = add_local("", type, location);
	Bytes code{0x20};
	wasm::append_uleb(code, length);
	code.push_back(0x41);
	wasm::append_sleb32(code, std::numeric_limits<std::int32_t>::max() - 20);
	code.insert(code.end(), {0x4b, 0x04, 0x40});
	append(code, module_.marker(location));
	code.insert(code.end(), {0x00, 0x0b, 0x20});
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x14, 0x6a, 0x10});
	wasm::append_uleb(code, module_.import_index("bearer_alloc"));
	code.push_back(0x21);
	wasm::append_uleb(code, pointer);
	code.push_back(0x20);
	wasm::append_uleb(code, pointer);
	code.insert(code.end(), {0x45, 0x04, 0x40});
	append(code, module_.marker(location));
	code.insert(code.end(), {0x00, 0x0b});
	for (const auto [header, offset] : {std::pair<std::int32_t, unsigned>{1, 0}, {1, 4}, {static_cast<std::int32_t>(type_id), 8}})
	{
		code.push_back(0x20);
		wasm::append_uleb(code, pointer);
		code.push_back(0x41);
		wasm::append_sleb32(code, header);
		code.insert(code.end(), {0x36, 0x02});
		wasm::append_uleb(code, offset);
	}
	code.push_back(0x20);
	wasm::append_uleb(code, pointer);
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x14, 0x6a, 0x36, 0x02, 0x0c, 0x20});
	wasm::append_uleb(code, pointer);
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x36, 0x02, 0x10, 0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
	return {std::move(code), pointer};
}

Bytes FunctionLowerer::format_wide_scalar(Bytes code, const std::string& type, const Location& location)
{
	code.push_back(0x10);
	wasm::append_uleb(code, module_.helper_index("format_" + type));
	const unsigned result = add_local("", "string", location);
	code.push_back(0x22);
	wasm::append_uleb(code, result);
	code.insert(code.end(), {0x45, 0x04, 0x40});
	append(code, module_.marker(location));
	code.insert(code.end(), {0x00, 0x0b, 0x20});
	wasm::append_uleb(code, result);
	return code;
}

std::pair<Bytes, std::string> FunctionLowerer::dval_lookup(Expr* value, Expr* key, bool require_present)
{
	auto [object_code, object_type] = expression(value);
	if (object_type != "dval")
		throw Error(value->location, "expected dval, found " + object_type);
	auto [key_code, key_type] = expression(key);
	if (key_type != "string" && key_type != "s32")
		throw Error(key->location, "dval index must be string or s32");
	const unsigned object = add_local("", "dval", value->location), key_local = add_local("", key_type, key->location),
				   length = add_local("", "s32", key->location);
	Bytes code = std::move(object_code);
	code.push_back(0x21);
	wasm::append_uleb(code, object);
	append(code, key_code);
	code.push_back(0x21);
	wasm::append_uleb(code, key_local);
	auto append_call = [&](bool output, unsigned pointer)
	{
		code.push_back(0x20);
		wasm::append_uleb(code, object);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
		wasm::append_uleb(code, object);
		code.insert(code.end(), {0x28, 0x02, 0x10});
		if (key_type == "string")
		{
			code.insert(code.end(), {0x41, 0x00, 0x20});
			wasm::append_uleb(code, key_local);
			code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
			wasm::append_uleb(code, key_local);
			code.insert(code.end(), {0x28, 0x02, 0x10, 0x41, 0x00});
		}
		else
		{
			code.insert(code.end(), {0x41, 0x01, 0x41, 0x00, 0x41, 0x00, 0x20});
			wasm::append_uleb(code, key_local);
		}
		if (output)
		{
			code.push_back(0x20);
			wasm::append_uleb(code, pointer);
			code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
			wasm::append_uleb(code, length);
		}
		else
		{
			code.insert(code.end(), {0x41, 0x00, 0x41, 0x00});
		}
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index("bearer_dv_get_brrb"));
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
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
	append(code, module_.marker(key->location));
	code.insert(code.end(), {0x00, 0x0b});
	auto [allocation, pointer] = allocate_blob("dval", 4, length, key->location);
	append(code, allocation);
	append_call(true, pointer);
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x47, 0x04, 0x40, 0x00, 0x0b});
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

std::pair<Bytes, std::string> FunctionLowerer::dval_scalar(Call* call, const std::string& result)
{
	if (call->arguments.size() != 1)
		throw Error(call->location, "dval extraction expects one dval");
	auto [source_code, source_type] = expression(call->arguments[0]);
	if (source_type != "dval")
		throw Error(call->arguments[0]->location, "expected dval, found " + source_type);
	const unsigned source = add_local("", "dval", call->location);
	Bytes code = std::move(source_code);
	code.push_back(0x21);
	wasm::append_uleb(code, source);
	if (result == "string")
	{
		code.push_back(0x20);
		wasm::append_uleb(code, source);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
		wasm::append_uleb(code, source);
		code.insert(code.end(), {0x28, 0x02, 0x10, 0x10});
		wasm::append_uleb(code, module_.import_index("bearer_dv_scalar_type_brrb"));
		code.push_back(0x41);
		wasm::append_sleb32(code, 'S');
		code.insert(code.end(), {0x47, 0x04, 0x40});
		append(code, module_.marker(call->location));
		code.insert(code.end(), {0x00, 0x0b});
		const unsigned length = add_local("", "s32", call->location);
		code.push_back(0x20);
		wasm::append_uleb(code, source);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
		wasm::append_uleb(code, source);
		code.insert(code.end(), {0x28, 0x02, 0x10, 0x41, 0x00, 0x41, 0x00, 0x10});
		wasm::append_uleb(code, module_.import_index("bearer_dv_brrb_to_string"));
		code.push_back(0x21);
		wasm::append_uleb(code, length);
		auto [allocation, pointer] = allocate_blob("string", 1, length, call->location);
		append(code, allocation);
		code.push_back(0x20);
		wasm::append_uleb(code, source);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
		wasm::append_uleb(code, source);
		code.insert(code.end(), {0x28, 0x02, 0x10, 0x20});
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
		wasm::append_uleb(code, length);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index("bearer_dv_brrb_to_string"));
		code.push_back(0x20);
		wasm::append_uleb(code, length);
		code.insert(code.end(), {0x47, 0x04, 0x40, 0x00, 0x0b, 0x20});
		wasm::append_uleb(code, pointer);
	}
	else
	{
		const unsigned output = add_local("", "s32", call->location), result_local = add_local("", result, call->location);
		code.push_back(0x41);
		wasm::append_sleb32(code, result == "f64" ? 8 : 4);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index("bearer_alloc"));
		code.push_back(0x21);
		wasm::append_uleb(code, output);
		code.push_back(0x20);
		wasm::append_uleb(code, output);
		code.insert(code.end(), {0x45, 0x04, 0x40});
		append(code, module_.marker(call->location));
		code.insert(code.end(), {0x00, 0x0b, 0x20});
		wasm::append_uleb(code, source);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
		wasm::append_uleb(code, source);
		code.insert(code.end(), {0x28, 0x02, 0x10, 0x20});
		wasm::append_uleb(code, output);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index(result == "s32" ? "bearer_dv_s32_brrb" : result == "f64" ? "bearer_dv_f64_brrb" : "bearer_dv_bool_brrb"));
		code.insert(code.end(), {0x45, 0x04, 0x40});
		append(code, module_.marker(call->location));
		code.insert(code.end(), {0x00, 0x0b, 0x20});
		wasm::append_uleb(code, output);
		if (result == "f64")
			code.insert(code.end(), {0x2b, 0x03, 0x00});
		else
			code.insert(code.end(), {0x28, 0x02, 0x00});
		code.push_back(0x21);
		wasm::append_uleb(code, result_local);
		code.push_back(0x20);
		wasm::append_uleb(code, output);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index("bearer_free"));
		code.push_back(0x20);
		wasm::append_uleb(code, result_local);
	}
	if (expression_is_owned(call->arguments[0]))
	{
		code.push_back(0x20);
		wasm::append_uleb(code, source);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.release_index());
	}
	return {code, result};
}

std::pair<Bytes, std::string> FunctionLowerer::dval_value(Expr* value)
{
	if (!dynamic_cast<MapLiteral*>(value) && !dynamic_cast<ArrayLiteral*>(value))
	{
		const std::string type = infer(value);
		if (type == "dval")
			return expression(value);
		auto [source, actual] = expression(value);
		if (actual != "string" && actual != "s32" && actual != "u64" && actual != "f64" && actual != "bool")
			throw Error(value->location, "cannot construct dval from " + actual);
		const unsigned input = add_local("", actual, value->location), length = add_local("", "s32", value->location);
		const char* import = actual == "string" ? "bearer_dv_string_to_brrb"
							 : actual == "s32"	? "bearer_dv_s32_to_brrb"
							 : actual == "u64"	? "bearer_dv_u64_to_brrb"
							 : actual == "f64"	? "bearer_dv_f64_to_brrb"
												: "bearer_dv_bool_to_brrb";
		Bytes code = std::move(source);
		code.push_back(0x21);
		wasm::append_uleb(code, input);
		code.push_back(0x20);
		wasm::append_uleb(code, input);
		if (actual == "string")
		{
			code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
			wasm::append_uleb(code, input);
			code.insert(code.end(), {0x28, 0x02, 0x10});
		}
		code.insert(code.end(), {0x41, 0x00, 0x41, 0x00, 0x10});
		wasm::append_uleb(code, module_.import_index(import));
		code.push_back(0x21);
		wasm::append_uleb(code, length);
		auto [allocation, pointer] = allocate_blob("dval", 4, length, value->location);
		append(code, allocation);
		code.push_back(0x20);
		wasm::append_uleb(code, input);
		if (actual == "string")
		{
			code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
			wasm::append_uleb(code, input);
			code.insert(code.end(), {0x28, 0x02, 0x10});
		}
		code.push_back(0x20);
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
		wasm::append_uleb(code, length);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index(import));
		code.push_back(0x20);
		wasm::append_uleb(code, length);
		code.insert(code.end(), {0x47, 0x04, 0x40, 0x00, 0x0b});
		if (actual == "string" && expression_is_owned(value))
		{
			code.push_back(0x20);
			wasm::append_uleb(code, input);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.release_index());
		}
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
	if (size)
	{
		code.push_back(0x41);
		wasm::append_sleb32(code, size);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index("bearer_alloc"));
		code.push_back(0x21);
		wasm::append_uleb(code, descriptor);
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
		code.push_back(0x20);
		wasm::append_uleb(code, values[i].first);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x36, 0x02});
		wasm::append_uleb(code, base + 8);
		code.push_back(0x20);
		wasm::append_uleb(code, descriptor);
		code.push_back(0x20);
		wasm::append_uleb(code, values[i].first);
		code.insert(code.end(), {0x28, 0x02, 0x10, 0x36, 0x02});
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
	auto [allocation, pointer] = allocate_blob("dval", 4, length, value->location);
	append(code, allocation);
	code.push_back(0x41);
	wasm::append_sleb32(code, list);
	code.push_back(0x20);
	wasm::append_uleb(code, descriptor);
	code.push_back(0x41);
	wasm::append_sleb32(code, entries.size());
	code.push_back(0x20);
	wasm::append_uleb(code, pointer);
	code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
	wasm::append_uleb(code, length);
	code.push_back(0x10);
	wasm::append_uleb(code, module_.import_index("bearer_dv_build_brrb"));
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x47, 0x04, 0x40, 0x00, 0x0b});
	if (size)
	{
		code.push_back(0x20);
		wasm::append_uleb(code, descriptor);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index("bearer_free"));
	}
	for (auto it = values.rbegin(); it != values.rend(); ++it)
		if (it->second)
		{
			code.push_back(0x20);
			wasm::append_uleb(code, it->first);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.release_index());
		}
	code.push_back(0x20);
	wasm::append_uleb(code, pointer);
	return {code, "dval"};
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
		code.push_back(0x20); wasm::append_uleb(code, array); code.insert(code.end(), {0x28, 0x02, 0x14, 0x21}); wasm::append_uleb(code, result);
		if (expression_is_owned(member->value)) { code.push_back(0x20); wasm::append_uleb(code, array); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
		code.push_back(0x20); wasm::append_uleb(code, result);
		return {code, "s32"};
	}
	infer(call);
	auto receiver = dynamic_cast<Name*>(member->value);
	if (!receiver)
		throw Error(member->value->location, "array mutation requires a local array name");
	auto [slot, type] = lookup(receiver);
	if (type != array_type)
		throw Error(member->value->location, "array receiver type changed during mutation");
	if (borrowed_managed_slots_.contains(slot))
		throw Error(member->value->location, "cannot mutate a borrowed array; copy it into a local first");
	auto address = [&](Bytes& code, unsigned array, unsigned index)
	{
		code.push_back(0x20); wasm::append_uleb(code, array);
		code.push_back(0x20); wasm::append_uleb(code, index);
		code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
		code.insert(code.end(), {0x6c, 0x6a, 0x41, 0x18, 0x6a});
	};
	auto load_length = [&](Bytes& code, unsigned target)
	{
		code.push_back(0x20); wasm::append_uleb(code, slot);
		code.insert(code.end(), {0x28, 0x02, 0x10, 0x21}); wasm::append_uleb(code, target);
	};
	auto store_length = [&](Bytes& code, unsigned source)
	{
		code.push_back(0x20); wasm::append_uleb(code, slot);
		code.push_back(0x20); wasm::append_uleb(code, source);
		code.insert(code.end(), {0x36, 0x02, 0x10});
	};
	auto require_nonnegative = [&](Bytes& code, unsigned value, const Location& location)
	{
		code.push_back(0x20); wasm::append_uleb(code, value);
		code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40}); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b});
	};
	if (member->member == "push")
	{
		auto [value_code, value_type] = expression(call->arguments[0]);
		const unsigned item = add_local("", element, call->arguments[0]->location), length = add_local("", "s32", call->location), required = add_local("", "s32", call->location);
		Bytes code = std::move(value_code); code.push_back(0x21); wasm::append_uleb(code, item); load_length(code, length);
		code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x41, 0x01, 0x6a, 0x22}); wasm::append_uleb(code, required);
		code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4d, 0x04, 0x40}); append(code, module_.marker(call->location)); code.insert(code.end(), {0x00, 0x0b});
		append(code, array_ensure_unique(slot, array_type, required, call->location));
		if (managed_type(element) && !expression_is_owned(call->arguments[0])) { code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
		address(code, slot, length); code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(array_store_opcode(element)); code.push_back(element_size == 8 ? 3 : 2); code.push_back(0x00);
		store_length(code, required);
		return {code, "void"};
	}
	if (member->member == "pop")
	{
		const unsigned length = add_local("", "s32", call->location), index = add_local("", "s32", call->location), result = add_local("", element, call->location);
		Bytes code; load_length(code, length); code.push_back(0x20); wasm::append_uleb(code, length);
		code.insert(code.end(), {0x45, 0x04, 0x40}); append(code, module_.marker(call->location)); code.insert(code.end(), {0x00, 0x0b});
		append(code, array_ensure_unique(slot, array_type, length, call->location));
		code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x41, 0x01, 0x6b, 0x21}); wasm::append_uleb(code, index);
		address(code, slot, index); code.push_back(array_load_opcode(element)); code.push_back(element_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x21}); wasm::append_uleb(code, result);
		if (managed_type(element)) { code.push_back(0x20); wasm::append_uleb(code, result); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); code.push_back(0x20); wasm::append_uleb(code, result); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
		store_length(code, index); code.push_back(0x20); wasm::append_uleb(code, result);
		return {code, element};
	}
	if (member->member == "reserve")
	{
		auto [required_code, required_type] = expression(call->arguments[0]);
		const unsigned required = add_local("", "s32", call->arguments[0]->location), capacity = add_local("", "s32", call->location);
		Bytes code = std::move(required_code); code.push_back(0x21); wasm::append_uleb(code, required); require_nonnegative(code, required, call->arguments[0]->location);
		code.push_back(0x20); wasm::append_uleb(code, slot); code.insert(code.end(), {0x28, 0x02, 0x14, 0x21}); wasm::append_uleb(code, capacity);
		code.push_back(0x20); wasm::append_uleb(code, required); code.push_back(0x20); wasm::append_uleb(code, capacity); code.insert(code.end(), {0x4b, 0x04, 0x40});
		append(code, array_ensure_unique(slot, array_type, required, call->location)); code.push_back(0x0b);
		return {code, "void"};
	}
	if (member->member == "clear")
	{
		const unsigned length = add_local("", "s32", call->location), index = add_local("", "s32", call->location);
		Bytes code; load_length(code, length); append(code, array_ensure_unique(slot, array_type, length, call->location));
		if (managed_type(element))
		{
			code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, index);
			code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, index); code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4f, 0x0d, 0x01});
			address(code, slot, index); code.insert(code.end(), {0x28, 0x02, 0x00, 0x10}); wasm::append_uleb(code, module_.release_index());
			code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, index); code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
		}
		code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, length); store_length(code, length);
		return {code, "void"};
	}
	if (member->member == "insert" || member->member == "remove")
	{
		auto [index_code, index_type] = expression(call->arguments[0]);
		const unsigned index = add_local("", "s32", call->arguments[0]->location), length = add_local("", "s32", call->location), required = add_local("", "s32", call->location);
		const unsigned item = add_local("", element, call->location);
		Bytes code = std::move(index_code); code.push_back(0x21); wasm::append_uleb(code, index); require_nonnegative(code, index, call->arguments[0]->location);
		bool insert = member->member == "insert";
		bool item_owned = false;
		if (insert)
		{
			auto value = expression(call->arguments[1]); append(code, value.first); code.push_back(0x21); wasm::append_uleb(code, item); item_owned = expression_is_owned(call->arguments[1]);
		}
		load_length(code, length); code.push_back(0x20); wasm::append_uleb(code, index); code.push_back(0x20); wasm::append_uleb(code, length); code.push_back(insert ? 0x4b : 0x4f);
		code.insert(code.end(), {0x04, 0x40}); append(code, module_.marker(call->arguments[0]->location)); code.insert(code.end(), {0x00, 0x0b});
		if (insert) { code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, required); }
		else { code.push_back(0x20); wasm::append_uleb(code, length); code.push_back(0x21); wasm::append_uleb(code, required); }
		append(code, array_ensure_unique(slot, array_type, required, call->location));
		if (!insert)
		{
			address(code, slot, index); code.push_back(array_load_opcode(element)); code.push_back(element_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x21}); wasm::append_uleb(code, item);
			if (managed_type(element)) { code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
		}
		code.push_back(0x20); wasm::append_uleb(code, slot); code.push_back(0x41); wasm::append_sleb32(code, 24); code.push_back(0x6a); code.push_back(0x20); wasm::append_uleb(code, index);
		if (insert) code.insert(code.end(), {0x41, 0x01, 0x6a});
		code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a});
		code.push_back(0x20); wasm::append_uleb(code, slot); code.push_back(0x41); wasm::append_sleb32(code, 24); code.push_back(0x6a); code.push_back(0x20); wasm::append_uleb(code, index);
		if (!insert) code.insert(code.end(), {0x41, 0x01, 0x6a});
		code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a, 0x20}); wasm::append_uleb(code, length); code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x6b});
		if (!insert) code.insert(code.end(), {0x41, 0x01, 0x6b});
		code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0xfc, 0x0a, 0x00, 0x00});
		if (insert)
		{
			if (managed_type(element) && !item_owned) { code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
			address(code, slot, index); code.push_back(0x20); wasm::append_uleb(code, item); code.push_back(array_store_opcode(element)); code.push_back(element_size == 8 ? 3 : 2); code.push_back(0x00); store_length(code, required);
			return {code, "void"};
		}
		code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x41, 0x01, 0x6b, 0x21}); wasm::append_uleb(code, required); store_length(code, required); code.push_back(0x20); wasm::append_uleb(code, item);
		return {code, element};
	}
	if (member->member == "resize")
	{
		auto [size_code, size_type] = expression(call->arguments[0]); auto [fill_code, fill_type] = expression(call->arguments[1]);
		const unsigned desired = add_local("", "s32", call->arguments[0]->location), fill = add_local("", element, call->arguments[1]->location);
		const unsigned length = add_local("", "s32", call->location), index = add_local("", "s32", call->location);
		Bytes code = std::move(size_code); code.push_back(0x21); wasm::append_uleb(code, desired); append(code, fill_code); code.push_back(0x21); wasm::append_uleb(code, fill);
		require_nonnegative(code, desired, call->arguments[0]->location); load_length(code, length);
		code.push_back(0x20); wasm::append_uleb(code, desired); code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4b, 0x04, 0x40}); append(code, array_ensure_unique(slot, array_type, desired, call->location));
		code.push_back(0x05); append(code, array_ensure_unique(slot, array_type, length, call->location)); code.push_back(0x0b);
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
		return {code, "void"};
	}
	throw Error(call->location, "unknown array method '" + member->member + "'");
}

Bytes FunctionLowerer::array_ensure_unique(unsigned slot, const std::string& array_type, unsigned required, const Location& location)
{
	const std::string element = array_type.substr(6, array_type.size() - 7);
	const unsigned element_size = array_element_size(element);
	const unsigned old = add_local("", array_type, location), length = add_local("", "s32", location), capacity = add_local("", "s32", location);
	const unsigned new_capacity = add_local("", "s32", location), bytes = add_local("", "s32", location), replacement = add_local("", array_type, location);
	Bytes code{0x20};
	wasm::append_uleb(code, slot);
	code.push_back(0x22); wasm::append_uleb(code, old);
	code.insert(code.end(), {0x28, 0x02, 0x10, 0x21}); wasm::append_uleb(code, length);
	code.push_back(0x20); wasm::append_uleb(code, old);
	code.insert(code.end(), {0x28, 0x02, 0x14, 0x21}); wasm::append_uleb(code, capacity);
	code.push_back(0x20); wasm::append_uleb(code, old);
	code.insert(code.end(), {0x28, 0x02, 0x00, 0x41, 0x01, 0x46, 0x20}); wasm::append_uleb(code, capacity);
	code.push_back(0x20); wasm::append_uleb(code, required);
	code.insert(code.end(), {0x4f, 0x71, 0x04, 0x40, 0x05});
	code.push_back(0x20); wasm::append_uleb(code, capacity);
	code.push_back(0x20); wasm::append_uleb(code, required);
	code.insert(code.end(), {0x49, 0x04, 0x40, 0x20}); wasm::append_uleb(code, capacity);
	code.insert(code.end(), {0x41, 0x02, 0x6c, 0x22}); wasm::append_uleb(code, new_capacity);
	code.push_back(0x20); wasm::append_uleb(code, required);
	code.insert(code.end(), {0x49, 0x04, 0x40, 0x20}); wasm::append_uleb(code, required);
	code.push_back(0x21); wasm::append_uleb(code, new_capacity);
	code.push_back(0x0b);
	code.push_back(0x05);
	code.push_back(0x20); wasm::append_uleb(code, capacity);
	code.push_back(0x21); wasm::append_uleb(code, new_capacity);
	code.push_back(0x0b);
	code.push_back(0x20); wasm::append_uleb(code, new_capacity);
	code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>((std::numeric_limits<std::int32_t>::max() - 24) / element_size));
	code.insert(code.end(), {0x4b, 0x04, 0x40}); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b});
	code.push_back(0x20); wasm::append_uleb(code, new_capacity);
	code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
	code.insert(code.end(), {0x6c, 0x41, 0x18, 0x6a, 0x22}); wasm::append_uleb(code, bytes);
	code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_alloc"));
	code.push_back(0x22); wasm::append_uleb(code, replacement);
	code.insert(code.end(), {0x45, 0x04, 0x40}); append(code, module_.marker(location)); code.insert(code.end(), {0x00, 0x0b});
	auto store_header = [&](unsigned offset, unsigned local, std::optional<std::int32_t> constant = std::nullopt)
	{
		code.push_back(0x20); wasm::append_uleb(code, replacement);
		if (constant) { code.push_back(0x41); wasm::append_sleb32(code, *constant); }
		else { code.push_back(0x20); wasm::append_uleb(code, local); }
		code.insert(code.end(), {0x36, 0x02}); wasm::append_uleb(code, offset);
	};
	store_header(0, 0, 1); store_header(4, 0, 1); store_header(8, 0, managed_type(element) ? 3 : 2);
	store_header(12, bytes); store_header(16, length); store_header(20, new_capacity);
	code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
	if (managed_type(element))
	{
		const unsigned index = add_local("", "s32", location), item = add_local("", element, location);
		code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, index);
		code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, index);
		code.push_back(0x20); wasm::append_uleb(code, length);
		code.insert(code.end(), {0x4f, 0x0d, 0x01, 0x20}); wasm::append_uleb(code, old);
		code.push_back(0x20); wasm::append_uleb(code, index);
		code.insert(code.end(), {0x41, 0x04, 0x6c, 0x6a, 0x28, 0x02, 0x18, 0x22}); wasm::append_uleb(code, item);
		code.push_back(0x10); wasm::append_uleb(code, module_.retain_index());
		code.push_back(0x20); wasm::append_uleb(code, replacement);
		code.push_back(0x20); wasm::append_uleb(code, index);
		code.insert(code.end(), {0x41, 0x04, 0x6c, 0x6a, 0x20}); wasm::append_uleb(code, item);
		code.insert(code.end(), {0x36, 0x02, 0x18, 0x20}); wasm::append_uleb(code, index);
		code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, index);
		code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
	}
	else
	{
		code.push_back(0x20); wasm::append_uleb(code, replacement); code.insert(code.end(), {0x41, 0x18, 0x6a, 0x20}); wasm::append_uleb(code, old);
		code.insert(code.end(), {0x41, 0x18, 0x6a, 0x20}); wasm::append_uleb(code, length);
		code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
		code.insert(code.end(), {0x6c, 0xfc, 0x0a, 0x00, 0x00});
	}
	code.push_back(0x20); wasm::append_uleb(code, old); code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
	code.push_back(0x20); wasm::append_uleb(code, replacement); code.push_back(0x21); wasm::append_uleb(code, slot);
	code.push_back(0x0b);
	return code;
}

std::pair<Bytes, std::string> FunctionLowerer::conversion(Bytes code, const std::string& source, const std::string& target,
														 const Location& location, bool source_owned)
{
	if (!can_convert(source, target))
		throw Error(location, "no explicit conversion from " + source + " to " + target);
	if (source == target || (source == "bool" && target == "s32"))
		return {std::move(code), target};
	if (target == "string")
	{
		if (source == "markup")
		{
			const unsigned input = add_local("", "markup", location), result = add_local("", "string", location);
			code.push_back(0x21); wasm::append_uleb(code, input); code.push_back(0x20); wasm::append_uleb(code, input);
			code.push_back(0x10); wasm::append_uleb(code, module_.clone_index()); code.push_back(0x21); wasm::append_uleb(code, result);
			if (source_owned) { code.push_back(0x20); wasm::append_uleb(code, input); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
			code.push_back(0x20); wasm::append_uleb(code, result);
			return {std::move(code), "string"};
		}
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
		if (source == "s32")
		{
			code.push_back(0xac);
			return {format_wide_scalar(std::move(code), "s64", location), "string"};
		}
		return {format_wide_scalar(std::move(code), source, location), "string"};
	}
	if (target == "bool")
	{
		if (source == "s32")
			code.insert(code.end(), {0x45, 0x45});
		else if (source == "s64" || source == "u64")
			code.insert(code.end(), {0x50, 0x45});
		else
		{
			code.push_back(0x44);
			wasm::append_f64(code, 0.0);
			code.push_back(0x62);
		}
		return {std::move(code), "bool"};
	}
	if (source == "bool" || source == "s32")
	{
		if (target == "s64")
			code.push_back(0xac);
		else if (target == "u64")
			code.push_back(source == "bool" ? 0xad : 0xac);
		else
			code.push_back(0xb7);
	}
	else if (source == "s64")
	{
		if (target == "s32")
			code.push_back(0xa7);
		else if (target == "f64")
			code.push_back(0xb9);
	}
	else if (source == "u64")
	{
		if (target == "s32")
			code.push_back(0xa7);
		else if (target == "f64")
			code.push_back(0xba);
	}
	else if (source == "f64")
	{
		append(code, module_.marker(location));
		code.push_back(target == "s32" ? 0xaa : target == "s64" ? 0xb0 : 0xb1);
	}
	return {std::move(code), target};
}

std::pair<Bytes, std::string> FunctionLowerer::expression(Expr* value, bool value_required)
{
	module_.check_cancelled();
	if (auto integer = dynamic_cast<Integer*>(value))
	{
		if (integer->value < std::numeric_limits<std::int32_t>::min() || integer->value > std::numeric_limits<std::int32_t>::max())
			throw Error(integer->location, "integer literal is outside the s32 range");
		Bytes code{0x41};
		wasm::append_sleb32(code, static_cast<std::int32_t>(integer->value));
		return {code, "s32"};
	}
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
	{
		Bytes code{0x44};
		wasm::append_f64(code, floating->value);
		return {code, "f64"};
	}
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
			const unsigned offset = module_.add_static_closure(slot);
			Bytes code{0x23, 0x00, 0x41};
			wasm::append_sleb32(code, static_cast<std::int32_t>(offset));
			code.push_back(0x6a);
			return {code, type};
		}
		std::vector<std::string> capture_types;
		for (const auto& [name, capture_type] : captures)
			capture_types.push_back(capture_type);
		const AggregateLayout layout = aggregate_layout(capture_types, 24);
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
											{static_cast<std::int32_t>(slot), 16}})
		{
			code.push_back(0x20);
			wasm::append_uleb(code, pointer);
			code.push_back(0x41);
			wasm::append_sleb32(code, header);
			code.insert(code.end(), {0x36, 0x02});
			wasm::append_uleb(code, offset);
		}
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
	if (auto tuple = dynamic_cast<TupleExpr*>(value))
	{
		if (tuple->items.size() < 2)
			throw Error(value->location, "tuple value requires at least two elements");
		std::vector<std::string> fields;
		for (Expr* item : tuple->items)
		{
			const std::string field = infer(item);
			if (field == "module")
				throw Error(item->location, "module is opaque and cannot be stored in tuple layouts");
			fields.push_back(field);
		}
		std::string type = "tuple<";
		for (std::size_t i = 0; i < fields.size(); ++i)
			type += (i ? "," : "") + fields[i];
		type += ">";
		const AggregateLayout layout = aggregate_layout(fields, 16);
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
											{static_cast<std::int32_t>(module_.tuple_type(type)), 8},
											{static_cast<std::int32_t>(layout.size), 12}})
		{
			code.push_back(0x20);
			wasm::append_uleb(code, pointer);
			code.push_back(0x41);
			wasm::append_sleb32(code, header);
			code.insert(code.end(), {0x36, 0x02});
			wasm::append_uleb(code, offset);
		}
		code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
		for (std::size_t i = 0; i < tuple->items.size(); ++i)
		{
			auto [item, actual] = expression(tuple->items[i]);
			if (actual != fields[i])
				throw Error(tuple->items[i]->location, "tuple field type changed while lowering");
			const unsigned temporary = add_local("", actual, tuple->items[i]->location);
			append(code, item);
			code.push_back(0x21);
			wasm::append_uleb(code, temporary);
			if (managed_type(actual) && !expression_is_owned(tuple->items[i]))
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
					const unsigned local = add_local("", compiled.second, spread->location), length = add_local("", "s32", spread->location);
					append(code, compiled.first); code.push_back(0x21); wasm::append_uleb(code, local);
					code.push_back(0x20); wasm::append_uleb(code, local); code.insert(code.end(), {0x28, 0x02, 0x10, 0x21}); wasm::append_uleb(code, length);
					parts.push_back({true, local, length, expression_is_owned(spread->value), source_element});
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
				code.push_back(0x20); wasm::append_uleb(code, previous); code.insert(code.end(), {0x49, 0x04, 0x40}); append(code, module_.marker(item->location)); code.insert(code.end(), {0x00, 0x0b});
			}
			if (element_type.empty()) throw Error(value->location, "empty array literal needs an explicit element type");
			const unsigned element_size = array_element_size(element_type), bytes = add_local("", "s32", value->location);
			const unsigned pointer = add_local("", "array<" + element_type + ">", value->location), cursor = add_local("", "s32", value->location);
			code.push_back(0x20); wasm::append_uleb(code, total); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>((std::numeric_limits<std::int32_t>::max() - 24) / element_size));
			code.insert(code.end(), {0x4b, 0x04, 0x40}); append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b, 0x20}); wasm::append_uleb(code, total);
			code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x41, 0x18, 0x6a, 0x22}); wasm::append_uleb(code, bytes);
			code.push_back(0x10); wasm::append_uleb(code, module_.import_index("bearer_alloc")); code.push_back(0x22); wasm::append_uleb(code, pointer);
			code.insert(code.end(), {0x45, 0x04, 0x40}); append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b});
			auto header = [&](unsigned offset, std::optional<std::int32_t> constant, unsigned local)
			{
				code.push_back(0x20); wasm::append_uleb(code, pointer);
				if (constant) { code.push_back(0x41); wasm::append_sleb32(code, *constant); } else { code.push_back(0x20); wasm::append_uleb(code, local); }
				code.insert(code.end(), {0x36, 0x02}); wasm::append_uleb(code, offset);
			};
			header(0, 1, 0); header(4, 1, 0); header(8, managed_type(element_type) ? 3 : 2, 0); header(12, std::nullopt, bytes); header(16, std::nullopt, total); header(20, std::nullopt, total);
			code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01, 0x41, 0x00, 0x21}); wasm::append_uleb(code, cursor);
			for (const Part& part : parts)
			{
				if (part.spread && part.source_element == element_type && !managed_type(element_type))
				{
					code.push_back(0x20); wasm::append_uleb(code, pointer); code.insert(code.end(), {0x41, 0x18, 0x6a, 0x20}); wasm::append_uleb(code, cursor);
					code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a, 0x20}); wasm::append_uleb(code, part.local);
					code.insert(code.end(), {0x41, 0x18, 0x6a, 0x20}); wasm::append_uleb(code, part.length); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
					code.insert(code.end(), {0x6c, 0xfc, 0x0a, 0x00, 0x00});
				}
				else if (part.spread && part.source_element == element_type)
				{
					const unsigned index = add_local("", "s32", value->location), copied = add_local("", element_type, value->location);
					code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, index); code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, index);
					code.push_back(0x20); wasm::append_uleb(code, part.length); code.insert(code.end(), {0x4f, 0x0d, 0x01, 0x20}); wasm::append_uleb(code, part.local); code.push_back(0x20); wasm::append_uleb(code, index);
					code.insert(code.end(), {0x41, 0x04, 0x6c, 0x6a, 0x28, 0x02, 0x18, 0x22}); wasm::append_uleb(code, copied); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index());
					code.push_back(0x20); wasm::append_uleb(code, pointer); code.push_back(0x20); wasm::append_uleb(code, cursor); code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x6a, 0x41, 0x04, 0x6c, 0x6a, 0x20}); wasm::append_uleb(code, copied); code.insert(code.end(), {0x36, 0x02, 0x18, 0x20}); wasm::append_uleb(code, index);
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
					code.push_back(0x20); wasm::append_uleb(code, part.length); code.insert(code.end(), {0x4f, 0x0d, 0x01, 0x20}); wasm::append_uleb(code, part.local); code.push_back(0x20); wasm::append_uleb(code, index);
					code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(source_size)); code.insert(code.end(), {0x6c, 0x6a, 0x41, 0x18, 0x6a}); code.push_back(array_load_opcode(part.source_element));
					code.push_back(source_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x21}); wasm::append_uleb(code, source_item); append(code, converted.first); code.push_back(0x21); wasm::append_uleb(code, converted_item);
					code.push_back(0x20); wasm::append_uleb(code, pointer); code.push_back(0x20); wasm::append_uleb(code, cursor); code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x6a});
					code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a, 0x41, 0x18, 0x6a, 0x20}); wasm::append_uleb(code, converted_item);
					code.push_back(array_store_opcode(element_type)); code.push_back(element_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x20}); wasm::append_uleb(code, index);
					code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21}); wasm::append_uleb(code, index); code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
				}
				else
				{
					if (managed_type(element_type) && !part.owned) { code.push_back(0x20); wasm::append_uleb(code, part.local); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
					code.push_back(0x20); wasm::append_uleb(code, pointer); code.push_back(0x20); wasm::append_uleb(code, cursor); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size)); code.insert(code.end(), {0x6c, 0x6a, 0x41, 0x18, 0x6a, 0x20}); wasm::append_uleb(code, part.local);
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
			auto compiled = expression(item);
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
		const unsigned allocation_size = 24 + element_size * static_cast<unsigned>(items.size());
		const unsigned pointer = add_local("", "array<" + element_type + ">", value->location);
		Bytes code{0x41};
		wasm::append_sleb32(code, static_cast<std::int32_t>(allocation_size));
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index("bearer_alloc"));
		code.push_back(0x21);
		wasm::append_uleb(code, pointer);
		code.push_back(0x20);
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x45, 0x04, 0x40, 0x00, 0x0b});
		// ARC header: strong, weak, type-id, allocation bytes, element count, capacity.
		const std::int32_t type_id = managed_type(element_type) ? 3 : 2;
		for (const auto [header_value, offset] : {std::pair<std::int32_t, unsigned>{1, 0},
												  {1, 4},
												  {type_id, 8},
												  {static_cast<std::int32_t>(allocation_size), 12},
												  {static_cast<std::int32_t>(items.size()), 16},
												  {static_cast<std::int32_t>(items.size()), 20}})
		{
			code.push_back(0x20);
			wasm::append_uleb(code, pointer);
			code.push_back(0x41);
			wasm::append_sleb32(code, header_value);
			code.insert(code.end(), {0x36, 0x02});
			wasm::append_uleb(code, offset);
		}
		code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
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
				code.push_back(0x20);
				wasm::append_uleb(code, item);
			}
			else
			{
				code.push_back(0x20);
				wasm::append_uleb(code, pointer);
				append(code, items[i].first);
			}
			code.push_back(array_store_opcode(element_type));
			code.push_back(static_cast<std::uint8_t>(element_size == 8 ? 3 : 2));
			wasm::append_uleb(code, static_cast<unsigned>(24 + element_size * i));
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
		if (array_type.rfind("tuple<", 0) == 0)
		{
			auto item = dynamic_cast<Integer*>(index->index);
			auto fields = aggregate_elements(array_type);
			if (!item || item->value < 0 || static_cast<std::size_t>(item->value) >= fields.size())
				throw Error(index->index->location, "tuple index is out of bounds");
			const std::string result_type = fields[item->value];
			const AggregateLayout layout = aggregate_layout(fields, 16);
			const unsigned object = add_local("", array_type, index->value->location);
			Bytes code = std::move(array_code);
			code.push_back(0x21);
			wasm::append_uleb(code, object);
			code.push_back(0x20);
			wasm::append_uleb(code, object);
			load_field(code, result_type, layout.offsets[item->value]);
			if (expression_is_owned(index->value))
			{
				const unsigned result = add_local("", result_type, value->location);
				code.push_back(0x21);
				wasm::append_uleb(code, result);
				if (managed_type(result_type))
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
			return {code, result_type};
		}
		if (array_type.rfind("array<", 0) != 0)
			throw Error(value->location, "indexing requires an array or tuple");
		auto [index_code, index_type] = expression(index->index);
		if (index_type != "s32")
			throw Error(index->index->location, "expected s32, found " + index_type);
		const unsigned array_local = add_local("", array_type, index->value->location);
		const unsigned index_local = add_local("", "s32", index->index->location);
		Bytes code = std::move(array_code);
		code.push_back(0x21);
		wasm::append_uleb(code, array_local);
		append(code, index_code);
		code.push_back(0x21);
		wasm::append_uleb(code, index_local);
		code.push_back(0x20);
		wasm::append_uleb(code, index_local);
		code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
		append(code, module_.marker(value->location));
		code.insert(code.end(), {0x00, 0x0b});
		code.push_back(0x20);
		wasm::append_uleb(code, index_local);
		code.push_back(0x20);
		wasm::append_uleb(code, array_local);
		code.insert(code.end(), {0x28, 0x02, 0x10, 0x4f, 0x04, 0x40});
		append(code, module_.marker(value->location));
		code.insert(code.end(), {0x00, 0x0b});
		const std::string element_type = array_type.substr(6, array_type.size() - 7);
		const unsigned element_size = array_element_size(element_type);
		code.push_back(0x20);
		wasm::append_uleb(code, array_local);
		code.push_back(0x20);
		wasm::append_uleb(code, index_local);
		code.push_back(0x41);
		wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
		code.insert(code.end(), {0x6c, 0x6a, 0x41, 0x18, 0x6a});
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
	if (auto member = dynamic_cast<Member*>(value))
	{
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
			return {code, "markup"};
		}

		struct Field
		{
			MarkupField* node;
			unsigned local;
			std::string type;
			bool owned;
		};
		std::vector<Field> compiled;
		Bytes code;
		for (MarkupField* field : fields)
		{
			auto [field_code, type] = expression(field->value);
			bool owned = expression_is_owned(field->value);
			if (wide_scalar(type))
			{
				field_code = format_wide_scalar(std::move(field_code), type, field->location);
				type = "string";
				owned = true;
			}
			if (type != "string" && type != "markup" && type != "s32" && type != "bool")
				throw Error(field->location, "markup interpolation does not support " + type);
			if (!field->escaped && type != "markup")
				throw Error(field->location, "raw markup interpolation requires a markup value");
			const unsigned local = add_local("", type, field->location);
			append(code, field_code);
			code.push_back(0x21);
			wasm::append_uleb(code, local);
			compiled.push_back({field, local, std::move(type), owned});
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
			if (field.node->escaped && field.type == "string")
				append(code, markup_escape_length(field.local, total, field.node->location));
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
				code.insert(code.end(), {0x28, 0x02, 0x10, 0x6a, 0x21});
				wasm::append_uleb(code, total);
			}
		}

		const unsigned pointer = add_local("", "markup", markup->location);
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
		{
			code.insert(code.end(), {0x20});
			wasm::append_uleb(code, pointer);
			code.push_back(0x41);
			wasm::append_sleb32(code, header);
			code.insert(code.end(), {0x36, 0x02});
			wasm::append_uleb(code, offset);
		}
		code.insert(code.end(), {0x20});
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x20});
		wasm::append_uleb(code, total);
		code.insert(code.end(), {0x41, 0x14, 0x6a, 0x36, 0x02, 0x0c});
		code.insert(code.end(), {0x20});
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x20});
		wasm::append_uleb(code, total);
		code.insert(code.end(), {0x36, 0x02, 0x10, 0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
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
			if (field->node->escaped && field->type == "string")
				append(code, markup_escape_write(field->local, cursor, field->node->location));
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
				code.insert(code.end(), {0x28, 0x02, 0x10, 0xfc, 0x0a, 0x00, 0x00, 0x20});
				wasm::append_uleb(code, cursor);
				code.insert(code.end(), {0x20});
				wasm::append_uleb(code, field->local);
				code.insert(code.end(), {0x28, 0x02, 0x10, 0x6a, 0x21});
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
		return {code, "markup"};
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
		auto [type, slot] = module_.reference_function(name->value, name->location);
		const unsigned offset = module_.add_static_closure(slot);
		Bytes code{0x23, 0x00, 0x41};
		wasm::append_sleb32(code, static_cast<std::int32_t>(offset));
		code.push_back(0x6a);
		return {code, type};
	}
	if (auto variable = dynamic_cast<Variable*>(value))
	{
		ArrayLiteral typed_empty(variable->value->location);
		Expr* initializer = variable->value;
		if (variable->annotation)
			if (auto value_array = dynamic_cast<ArrayLiteral*>(variable->value); value_array && value_array->items.empty())
				if (auto type_array = dynamic_cast<ArrayLiteral*>(variable->annotation); type_array && type_array->items.size() == 1)
				{
					typed_empty.explicit_element_type = type_array->items[0];
					initializer = &typed_empty;
				}
		auto [code, type] = expression(initializer);
		std::string declared = variable->annotation ? module_.value_type(variable->annotation) : type;
		if (declared != type)
			throw Error(value->location, "expected " + declared + ", found " + type);
		unsigned slot = add_local(variable->name, declared, value->location);
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
		if (binary->operator_ == "=")
		{
			if (auto target_index = dynamic_cast<Index*>(binary->left))
			{
				auto receiver = dynamic_cast<Name*>(target_index->value);
				if (!receiver)
					throw Error(target_index->value->location, "array assignment requires a local array name");
				auto [slot, array_type] = lookup(receiver);
				if (array_type.rfind("array<", 0) != 0)
					throw Error(target_index->value->location, "indexed assignment requires an array");
				if (borrowed_managed_slots_.contains(slot))
					throw Error(target_index->value->location, "cannot mutate a borrowed array; copy it into a local first");
				const std::string element = array_type.substr(6, array_type.size() - 7);
				const unsigned element_size = array_element_size(element);
				auto [index_code, index_type] = expression(target_index->index);
				if (index_type != "s32") throw Error(target_index->index->location, "array index must be s32");
				auto [replacement_code, replacement_type] = expression(binary->right);
				if (replacement_type != element) throw Error(binary->right->location, "expected " + element + ", found " + replacement_type);
				const unsigned index = add_local("", "s32", target_index->index->location), replacement = add_local("", element, binary->right->location), length = add_local("", "s32", binary->location);
				Bytes code = std::move(index_code); code.push_back(0x21); wasm::append_uleb(code, index); append(code, replacement_code); code.push_back(0x21); wasm::append_uleb(code, replacement);
				code.push_back(0x20); wasm::append_uleb(code, index); code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40}); append(code, module_.marker(target_index->location)); code.insert(code.end(), {0x00, 0x0b});
				code.push_back(0x20); wasm::append_uleb(code, slot); code.insert(code.end(), {0x28, 0x02, 0x10, 0x21}); wasm::append_uleb(code, length);
				code.push_back(0x20); wasm::append_uleb(code, index); code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4f, 0x04, 0x40}); append(code, module_.marker(target_index->location)); code.insert(code.end(), {0x00, 0x0b});
				append(code, array_ensure_unique(slot, array_type, length, binary->location));
				if (managed_type(element))
				{
					if (!expression_is_owned(binary->right)) { code.push_back(0x20); wasm::append_uleb(code, replacement); code.push_back(0x10); wasm::append_uleb(code, module_.retain_index()); }
					code.push_back(0x20); wasm::append_uleb(code, slot); code.push_back(0x20); wasm::append_uleb(code, index); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
					code.insert(code.end(), {0x6c, 0x6a, 0x41, 0x18, 0x6a, 0x28, 0x02, 0x00, 0x10}); wasm::append_uleb(code, module_.release_index());
				}
				code.push_back(0x20); wasm::append_uleb(code, slot); code.push_back(0x20); wasm::append_uleb(code, index); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
				code.insert(code.end(), {0x6c, 0x6a, 0x41, 0x18, 0x6a, 0x20}); wasm::append_uleb(code, replacement); code.push_back(array_store_opcode(element)); code.push_back(element_size == 8 ? 3 : 2); code.insert(code.end(), {0x00, 0x20}); wasm::append_uleb(code, replacement);
				return {code, element};
			}
			auto target = dynamic_cast<Name*>(binary->left);
			if (!target)
				throw Error(binary->left->location, "assignment target must be a local name or array element");
			auto [slot, expected] = lookup(target);
			auto [code, actual] = expression(binary->right);
			if (actual != expected)
				throw Error(value->location, "expected " + expected + ", found " + actual);
			if (managed_type(expected))
			{
				if (borrowed_managed_slots_.contains(slot))
					throw Error(target->location, "cannot assign to a borrowed managed value; copy it into a local first");
				const unsigned temporary = add_local("", expected, value->location);
				code.push_back(0x21);
				wasm::append_uleb(code, temporary);
				if (!expression_is_owned(binary->right))
				{
					code.push_back(0x20);
					wasm::append_uleb(code, temporary);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.retain_index());
				}
				code.push_back(0x20);
				wasm::append_uleb(code, slot);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.release_index());
				code.push_back(0x20);
				wasm::append_uleb(code, temporary);
				code.push_back(0x22);
				wasm::append_uleb(code, slot);
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
				if (right_type == "s32")
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
		if (infer(binary->left) == "string" || infer(binary->right) == "string")
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
				code.insert(code.end(), {0x28, 0x02, 0x10, 0x21});
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
		auto [left, left_type] = expression(binary->left);
		auto [right, right_type] = expression(binary->right);
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
			{"f64", {{"+", 0xa0}, {"-", 0xa1}, {"*", 0xa2}, {"/", 0xa3}, {"==", 0x61}, {"!=", 0x62}, {"<", 0x63}, {">", 0x64}, {"<=", 0x65}, {">=", 0x66}}}};
		auto type_ops = ops.find(left_type);
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
		if (member && infer(member->value).rfind("array<", 0) == 0 &&
			(member->member == "capacity" || member->member == "push" || member->member == "pop" || member->member == "insert" ||
			 member->member == "remove" || member->member == "clear" || member->member == "reserve" || member->member == "resize"))
			return array_method(call, member);
		std::vector<Expr*> method_arguments;
		const std::vector<Expr*>* arguments = &call->arguments;
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
			if (method_local || method_host || method_definition)
				arguments = &method_arguments;
		}
		std::vector<Expr*> tuple_expanded_arguments;
		std::vector<std::unique_ptr<Expr>> tuple_synthetic;
		bool expanded_tuple = false;
		for (Expr* argument : *arguments)
			if (auto spread = dynamic_cast<Spread*>(argument); spread && infer(spread->value).rfind("tuple<", 0) == 0)
			{
				expanded_tuple = true;
				if (auto literal = dynamic_cast<TupleExpr*>(spread->value))
					for (Expr* item : literal->items) tuple_expanded_arguments.push_back(item);
				else if (dynamic_cast<Name*>(spread->value))
				{
					const auto fields = aggregate_elements(infer(spread->value));
					for (std::size_t i = 0; i < fields.size(); ++i)
					{
						auto index = std::make_unique<Integer>(spread->location, static_cast<long long>(i));
						auto item = std::make_unique<Index>(spread->location, spread->value, index.get());
						tuple_expanded_arguments.push_back(item.get());
						tuple_synthetic.push_back(std::move(index)); tuple_synthetic.push_back(std::move(item));
					}
				}
				else
					throw Error(spread->location, "tuple spread requires a tuple literal or local name");
			}
			else
				tuple_expanded_arguments.push_back(argument);
		if (expanded_tuple)
			arguments = &tuple_expanded_arguments;
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
				if (spread) source = source.substr(7, source.size() - 8);
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
		if (!named && (!member || (!method_local && !method_host && !method_definition)))
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
				auto [argument, actual] = expression((*indirect)[i]);
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
			code.insert(code.end(), {0x28, 0x02, 0x10, 0x11});
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
		std::string callee = member ? member->member : named->value;
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
					auto [argument, actual] = expression((*arguments)[i]);
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
				code.insert(code.end(), {0x28, 0x02, 0x10, 0x11});
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
		if (!member && primitive_constructor_name(callee) && call->arguments.size() == 1)
		{
			const std::string source = infer(call->arguments[0]);
			if (!module_.exact_definition(callee, {source}) && can_convert(source, callee))
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
				if (infer((*arguments)[i]) != fields[i].second)
					return false;
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
			{
				code.push_back(0x20);
				wasm::append_uleb(code, pointer);
				code.push_back(0x41);
				wasm::append_sleb32(code, header);
				code.insert(code.end(), {0x36, 0x02});
				wasm::append_uleb(code, offset);
			}
			code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
			for (std::size_t i = 0; i < arguments->size(); ++i)
			{
				auto [field, actual] = expression((*arguments)[i]);
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
		if (!member && callee == "length")
		{
			if (call->arguments.size() != 1)
				throw Error(value->location, "length expects one string, markup, or array");
			auto [code, type] = expression(call->arguments[0]);
			if (type != "string" && type != "markup" && type.rfind("array<", 0) != 0)
				throw Error(call->arguments[0]->location, "length expects a string, markup, or array, found " + type);
			const unsigned source = add_local("", type, call->arguments[0]->location), result = add_local("", "s32", value->location);
			code.push_back(0x21); wasm::append_uleb(code, source);
			code.push_back(0x20); wasm::append_uleb(code, source);
			code.insert(code.end(), {0x28, 0x02, 0x10, 0x21}); wasm::append_uleb(code, result);
			if (expression_is_owned(call->arguments[0])) { code.push_back(0x20); wasm::append_uleb(code, source); code.push_back(0x10); wasm::append_uleb(code, module_.release_index()); }
			code.push_back(0x20); wasm::append_uleb(code, result);
			return {code, "s32"};
		}
		if (!member && callee == "dval")
		{
			if (call->arguments.size() != 1)
				throw Error(value->location, "dval expects one scalar, map, or list");
			return dval_value(call->arguments[0]);
		}
		if (!member && callee == "dval_has")
		{
			if (call->arguments.size() != 2)
				throw Error(value->location, "dval_has expects dval and string/s32 key");
			return dval_lookup(call->arguments[0], call->arguments[1], false);
		}
		if (!member && callee == "dval_string") return dval_scalar(call, "string");
		if (!member && callee == "dval_s32") return dval_scalar(call, "s32");
		if (!member && callee == "dval_f64") return dval_scalar(call, "f64");
		if (!member && callee == "dval_bool") return dval_scalar(call, "bool");
		if (!member && callee == "trusted_markup")
		{
			if (call->arguments.size() != 1) throw Error(value->location, "trusted_markup expects one string");
			auto [source, type] = expression(call->arguments[0]);
			if (type != "string") throw Error(call->arguments[0]->location, "expected string, found " + type);
			return {std::move(source), "markup"};
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
			return {{0x23, 0x01}, "s32"};
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
			types.push_back(infer(argument));
		if (const Module::HostDeclaration* host = member ? method_host : module_.host(callee, types))
		{
			std::vector<Bytes> argument_code;
			for (Expr* argument : *arguments)
				argument_code.push_back(expression(argument).first);
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
			auto release_inputs = [&]
			{
				for (auto it = owned_arguments.rbegin(); it != owned_arguments.rend(); ++it)
				{
					code.push_back(0x20); wasm::append_uleb(code, *it);
					code.push_back(0x10); wasm::append_uleb(code, module_.release_index());
				}
			};
			auto inputs = [&]
			{
				for (std::size_t i = 0; i < locals.size(); ++i)
				{
					code.push_back(0x20); wasm::append_uleb(code, locals[i]);
					if (types[i] == "string" || types[i] == "dval")
					{
						code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20}); wasm::append_uleb(code, locals[i]);
						code.insert(code.end(), {0x28, 0x02, 0x10});
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
			auto [allocation, pointer] = allocate_blob(host->result, host->result == "string" ? 1 : 4, length, value->location);
			append(code, allocation); inputs();
			code.push_back(0x20); wasm::append_uleb(code, pointer);
			code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20}); wasm::append_uleb(code, length);
			code.push_back(0x10); wasm::append_uleb(code, module_.import_index(host->symbol));
			code.push_back(0x20); wasm::append_uleb(code, length);
			code.insert(code.end(), {0x47, 0x04, 0x40}); release_inputs(); append(code, module_.marker(value->location)); code.insert(code.end(), {0x00, 0x0b});
			const unsigned result = add_local("", host->result, value->location);
			code.push_back(0x20); wasm::append_uleb(code, pointer);
			code.push_back(0x21); wasm::append_uleb(code, result);
			release_inputs();
			code.push_back(0x20); wasm::append_uleb(code, result);
			return {code, host->result};
		}
		Definition& target = member ? *method_definition : module_.resolve(callee, types, value->location);
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
				code.insert(code.end(), {0x28, 0x02, 0x10, 0x10}); wasm::append_uleb(code, module_.import_index(sink->symbol));
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
					code.insert(code.end(), {0x28, 0x02, 0x10, 0x21}); wasm::append_uleb(code, length); code.insert(code.end(), {0x41, 0x00, 0x21}); wasm::append_uleb(code, index);
					code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20}); wasm::append_uleb(code, index); code.push_back(0x20); wasm::append_uleb(code, length); code.insert(code.end(), {0x4f, 0x0d, 0x01, 0x20}); wasm::append_uleb(code, array);
					code.push_back(0x20); wasm::append_uleb(code, index); code.push_back(0x41); wasm::append_sleb32(code, static_cast<std::int32_t>(source_size)); code.insert(code.end(), {0x6c, 0x6a, 0x41, 0x18, 0x6a});
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
					auto compiled = expression(argument);
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
				const std::string source_type = spread ? types[i].substr(7, types[i].size() - 8) : types[i];
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
				argument_code.push_back(expression((*arguments)[i]).first);
				continue;
			}
			if (i >= target.convert.size() || !target.convert[i])
				throw std::runtime_error("resolved Capy call requires an undeclared parameter conversion");
			const std::string constructor_name = target.parameters[i].rfind("struct:", 0) == 0 ? target.parameters[i].substr(7) : target.parameters[i];
			Name constructor((*arguments)[i]->location, constructor_name);
			Call converted_call((*arguments)[i]->location, &constructor);
			converted_call.arguments.push_back((*arguments)[i]);
			auto converted = expression(&converted_call);
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
	if (auto returned = dynamic_cast<Return*>(value))
	{
		Bytes code;
		std::string type = "void";
		if (returned->value)
		{
			auto result = expression(returned->value);
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
		auto [condition, type] = expression(conditional->condition);
		if (type != "bool")
			throw Error(conditional->condition->location, type == "module" ? "module is opaque and cannot be used as a condition" : "if condition must be bool");
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
		auto [condition, type] = expression(loop->condition);
		repeated_condition_scope_ = previous_condition_scope;
		if (type != "bool")
			throw Error(loop->condition->location, type == "module" ? "module is opaque and cannot be used as a condition" : "while condition must be bool");
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
				throw Error(loop->location, "dval iteration accepts value or key,value bindings");
			auto [iterable_code, iterable_type] = expression(loop->iterable);
			const unsigned iterable = add_local("", "dval", loop->iterable->location), count = add_local("", "s32", loop->location),
						   index = add_local("", "s32", loop->location), item = add_local("", "dval", loop->location),
						   key = loop->names.size() == 2 ? add_local("", "string", loop->location) : 0xffffffffu;
			borrowed_managed_slots_.insert(item);
			if (key != 0xffffffffu)
				borrowed_managed_slots_.insert(key);
			const bool owned_iterable = expression_is_owned(loop->iterable);
			Bytes code = std::move(iterable_code);
			code.push_back(0x21);
			wasm::append_uleb(code, iterable);
			code.push_back(0x20);
			wasm::append_uleb(code, iterable);
			code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
			wasm::append_uleb(code, iterable);
			code.insert(code.end(), {0x28, 0x02, 0x10, 0x10});
			wasm::append_uleb(code, module_.import_index("bearer_dv_count_brrb"));
			code.push_back(0x21);
			wasm::append_uleb(code, count);
			code.push_back(0x20);
			wasm::append_uleb(code, count);
			code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
			append(code, module_.marker(loop->iterable->location));
			code.insert(code.end(), {0x00, 0x0b, 0x41, 0x00, 0x21});
			wasm::append_uleb(code, index);
			code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20});
			wasm::append_uleb(code, index);
			code.push_back(0x20);
			wasm::append_uleb(code, count);
			code.insert(code.end(), {0x4f, 0x0d, 0x01});
			auto entry = [&](const char* import, const std::string& type, unsigned target)
			{
				const unsigned length = add_local("", "s32", loop->location);
				code.push_back(0x20);
				wasm::append_uleb(code, iterable);
				code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
				wasm::append_uleb(code, iterable);
				code.insert(code.end(), {0x28, 0x02, 0x10, 0x20});
				wasm::append_uleb(code, index);
				code.insert(code.end(), {0x41, 0x00, 0x41, 0x00, 0x10});
				wasm::append_uleb(code, module_.import_index(import));
				code.push_back(0x21);
				wasm::append_uleb(code, length);
				code.push_back(0x20);
				wasm::append_uleb(code, length);
				code.insert(code.end(), {0x41, 0x00, 0x48, 0x04, 0x40});
				append(code, module_.marker(loop->location));
				code.insert(code.end(), {0x00, 0x0b});
				auto [allocation, pointer] = allocate_blob(type, type == "string" ? 1 : 4, length, loop->location);
				append(code, allocation);
				code.push_back(0x20);
				wasm::append_uleb(code, iterable);
				code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
				wasm::append_uleb(code, iterable);
				code.insert(code.end(), {0x28, 0x02, 0x10, 0x20});
				wasm::append_uleb(code, index);
				code.push_back(0x20);
				wasm::append_uleb(code, pointer);
				code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
				wasm::append_uleb(code, length);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.import_index(import));
				code.push_back(0x20);
				wasm::append_uleb(code, length);
				code.insert(code.end(), {0x47, 0x04, 0x40, 0x00, 0x0b, 0x20});
				wasm::append_uleb(code, pointer);
				code.push_back(0x21);
				wasm::append_uleb(code, target);
			};
			if (key != 0xffffffffu)
				entry("bearer_dv_entry_key_brrb", "string", key);
			entry("bearer_dv_entry_value_brrb", "dval", item);
			std::unordered_map<std::string, std::pair<unsigned, std::string>> scope{{loop->names.back(), {item, "dval"}}};
			if (key != 0xffffffffu)
				scope[loop->names.front()] = {key, "string"};
			scopes_.push_back(std::move(scope));
			if (owned_iterable)
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
			if (owned_iterable)
			{
				append(code, cleanup_scopes(owned_scopes_.size() - 1));
				owned_scopes_.pop_back();
			}
			return {code, "void"};
		}
		if (loop->names.size() != 1)
			throw Error(loop->location, "array and range loops require one binding");
		const unsigned base = control_depth_, boundary = static_cast<unsigned>(owned_scopes_.size());
		if (auto range = dynamic_cast<Binary*>(loop->iterable); range && range->operator_ == "..")
		{
			auto [start, st] = expression(range->left);
			auto [end, et] = expression(range->right);
			if (st != "s32" || et != "s32")
				throw Error(loop->location, "range bounds must be s32");
			const unsigned index = add_local(loop->names[0], "s32", loop->location), limit = add_local("", "s32", loop->location);
			append(start, Bytes{0x21});
			wasm::append_uleb(start, index);
			append(end, Bytes{0x21});
			wasm::append_uleb(end, limit);
			scopes_.push_back({{loop->names[0], {index, "s32"}}});
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
			code.insert(code.end(), {0x4e, 0x0d, 0x01, 0x02, 0x40});
			append(code, body);
			code.push_back(0x0b);
			code.push_back(0x20);
			wasm::append_uleb(code, index);
			code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
			wasm::append_uleb(code, index);
			code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
			return {code, "void"};
		}
		auto [iterable, type] = expression(loop->iterable);
		if (type.rfind("array<", 0) != 0)
			throw Error(loop->iterable->location, "for loop requires an exclusive range or array");
		const std::string element = type.substr(6, type.size() - 7);
		const unsigned array = add_local("", type, loop->iterable->location), index = add_local("", "s32", loop->location),
					   length = add_local("", "s32", loop->location), item = add_local(loop->names[0], element, loop->location);
		scopes_.push_back({{loop->names[0], {item, element}}});
		owned_scopes_.push_back(expression_is_owned(loop->iterable) ? std::vector<std::pair<unsigned, std::string>>{{array, type}}
																	: std::vector<std::pair<unsigned, std::string>>{});
		const unsigned array_loop_boundary = static_cast<unsigned>(owned_scopes_.size());
		control_depth_ += 3;
		loops_.push_back({base + 1, base + 3, array_loop_boundary, {}, {}});
		Bytes body = block(loop->body);
		loops_.pop_back();
		control_depth_ -= 3;
		owned_scopes_.pop_back();
		scopes_.pop_back();
		Bytes code = std::move(iterable);
		code.push_back(0x21);
		wasm::append_uleb(code, array);
		code.insert(code.end(), {0x41, 0x00, 0x21});
		wasm::append_uleb(code, index);
		code.push_back(0x20);
		wasm::append_uleb(code, array);
		code.insert(code.end(), {0x28, 0x02, 0x10, 0x21});
		wasm::append_uleb(code, length);
		code.insert(code.end(), {0x02, 0x40, 0x03, 0x40, 0x20});
		wasm::append_uleb(code, index);
		code.push_back(0x20);
		wasm::append_uleb(code, length);
		code.insert(code.end(), {0x4f, 0x0d, 0x01, 0x20});
		wasm::append_uleb(code, array);
		code.push_back(0x20);
		wasm::append_uleb(code, index);
		const unsigned element_size = array_element_size(element);
		code.push_back(0x41);
		wasm::append_sleb32(code, static_cast<std::int32_t>(element_size));
		code.insert(code.end(), {0x6c, 0x6a, 0x41, 0x18, 0x6a});
		code.push_back(array_load_opcode(element));
		code.push_back(static_cast<std::uint8_t>(element_size == 8 ? 3 : 2));
		code.insert(code.end(), {0x00, 0x21});
		wasm::append_uleb(code, item);
		code.insert(code.end(), {0x02, 0x40});
		append(code, body);
		code.push_back(0x0b);
		code.push_back(0x20);
		wasm::append_uleb(code, index);
		code.insert(code.end(), {0x41, 0x01, 0x6a, 0x21});
		wasm::append_uleb(code, index);
		code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
		if (expression_is_owned(loop->iterable))
		{
			code.push_back(0x20);
			wasm::append_uleb(code, array);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.release_index());
		}
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
		auto [part, type] = expression(item, final);
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
		result_type = type;
		if (falls_through)
		{
			if (type != "void")
			{
				const unsigned result = add_local("", type, item->location);
				code.push_back(0x21);
				wasm::append_uleb(code, result);
				if (managed_type(type) && !expression_is_owned(item))
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
			else
				append(code, cleanup_scopes(owned_scopes_.size() - 1));
		}
	}
	if (block_value->items.empty())
		append(code, cleanup_scopes(owned_scopes_.size() - 1));
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
		Expr* item = block_value->items[i];
		const auto variable = dynamic_cast<Variable*>(item);
		const bool final_result = !new_scope && i + 1 == block_value->items.size() && definition_.result != "void";
		const bool declaration_result = variable && final_result && infer(variable) == definition_.result;
		auto [part, type] = expression(item, declaration_result || (!variable && final_result));
		append(code, part);
		const bool implicit_result = declaration_result || (!variable && !new_scope && i + 1 == block_value->items.size() && type == definition_.result && type != "void");
		if (implicit_result)
		{
			implicit_result_ = true;
			if (managed_type(type))
			{
				const unsigned result = add_local("", type, item->location);
				code.push_back(0x21);
				wasm::append_uleb(code, result);
				if (!expression_is_owned(item))
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
		}
		if (type != "void" && !implicit_result)
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
		const std::string& path = definition_.function->location.file;
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
	for (unsigned parameter : owned_array_parameters_)
	{
		code.push_back(0x20);
		wasm::append_uleb(code, parameter);
		code.push_back(0x10);
		wasm::append_uleb(code, module_.retain_index());
	}
	if (definition_.closure_body)
	{
		std::vector<std::string> capture_types;
		for (const auto& [name, type] : definition_.captures)
			capture_types.push_back(type);
		const AggregateLayout layout = aggregate_layout(capture_types, 24);
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
	if (definition_.result != "void" && !implicit_result_ && !expression_always_returns(definition_.function->body))
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
	locals.insert(locals.end(), code.begin(), code.end());
	Bytes body;
	wasm::append_uleb(body, static_cast<unsigned>(locals.size()));
	body.insert(body.end(), locals.begin(), locals.end());
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
		Bytes code{0x20, 0x00, 0x45, 0x04, 0x40, 0x0f, 0x0b, 0x20, 0x00, 0x28, 0x02, 0x00, 0x41, 0x7f, 0x46, 0x04, 0x40, 0x0f, 0x0b, 0x20, 0x00, 0x28,
				   0x02, 0x00, 0x41, 0x7e, 0x46, 0x04, 0x40, 0x00, 0x0b, 0x20, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x41, 0x01, 0x6a, 0x36, 0x02, 0x00};
		result.push_back(body({0}, std::move(code)));
	}
	if (use_release_)
	{
		Bytes code{0x20, 0x00, 0x45, 0x04, 0x40, 0x0f, 0x0b, 0x20, 0x00, 0x28, 0x02, 0x00, 0x41, 0x7f, 0x46, 0x04, 0x40,
				   0x0f, 0x0b, 0x20, 0x00, 0x28, 0x02, 0x00, 0x45, 0x04, 0x40, 0x00, 0x0b, 0x20, 0x00, 0x20, 0x00, 0x28,
				   0x02, 0x00, 0x41, 0x01, 0x6b, 0x22, 0x01, 0x36, 0x02, 0x00, 0x20, 0x01, 0x45, 0x04, 0x40};
		// Type 3 arrays own every element, including nested managed aggregates.
		code.insert(code.end(),
					{0x20, 0x00, 0x28, 0x02, 0x08, 0x41, 0x03, 0x46, 0x04, 0x40, 0x20, 0x00, 0x28, 0x02, 0x10, 0x21, 0x02, 0x41, 0x00, 0x21, 0x01, 0x02,
					 0x40, 0x03, 0x40, 0x20, 0x01, 0x20, 0x02, 0x4f, 0x0d, 0x01, 0x20, 0x00, 0x20, 0x01, 0x41, 0x04, 0x6c, 0x6a, 0x28, 0x02, 0x18, 0x10});
		wasm::append_uleb(code, release_index());
		code.insert(code.end(), {0x20, 0x01, 0x41, 0x01, 0x6a, 0x21, 0x01, 0x0c, 0x00, 0x0b, 0x0b, 0x0b});
		for (const auto& [type_id, captures] : closure_types_)
		{
			const AggregateLayout layout = aggregate_layout(captures, 24);
			std::vector<unsigned> managed;
			for (unsigned i = 0; i < captures.size(); ++i)
				if (managed_type(captures[i]))
					managed.push_back(i);
			if (managed.empty())
				continue;
			code.insert(code.end(), {0x20, 0x00, 0x28, 0x02, 0x08, 0x41});
			wasm::append_sleb32(code, static_cast<std::int32_t>(type_id));
			code.insert(code.end(), {0x46, 0x04, 0x40});
			for (unsigned i : managed)
			{
				code.push_back(0x20);
				wasm::append_uleb(code, 0);
				load_field(code, captures[i], layout.offsets[i]);
				code.push_back(0x10);
				wasm::append_uleb(code, release_index());
			}
			code.push_back(0x0b);
		}
		for (const auto& [name, aggregate] : structs_)
		{
			std::vector<std::string> field_types;
			for (const auto& [field_name, field_type] : aggregate.fields)
				field_types.push_back(field_type);
			const AggregateLayout layout = aggregate_layout(field_types, 16);
			std::vector<unsigned> managed;
			for (unsigned i = 0; i < aggregate.fields.size(); ++i)
				if (managed_type(aggregate.fields[i].second))
					managed.push_back(i);
			if (managed.empty())
				continue;
			code.insert(code.end(), {0x20, 0x00, 0x28, 0x02, 0x08, 0x41});
			wasm::append_sleb32(code, static_cast<std::int32_t>(aggregate.type_id));
			code.insert(code.end(), {0x46, 0x04, 0x40});
			for (unsigned i : managed)
			{
				code.push_back(0x20);
				wasm::append_uleb(code, 0);
				load_field(code, aggregate.fields[i].second, layout.offsets[i]);
				code.push_back(0x10);
				wasm::append_uleb(code, release_index());
			}
			code.push_back(0x0b);
		}
		for (const auto& [type, id] : tuples_)
		{
			const auto fields = aggregate_elements(type);
			const AggregateLayout layout = aggregate_layout(fields, 16);
			std::vector<unsigned> managed;
			for (unsigned i = 0; i < fields.size(); ++i)
				if (managed_type(fields[i]))
					managed.push_back(i);
			if (managed.empty())
				continue;
			code.insert(code.end(), {0x20, 0x00, 0x28, 0x02, 0x08, 0x41});
			wasm::append_sleb32(code, static_cast<std::int32_t>(id));
			code.insert(code.end(), {0x46, 0x04, 0x40});
			for (unsigned i : managed)
			{
				code.push_back(0x20);
				wasm::append_uleb(code, 0);
				load_field(code, fields[i], layout.offsets[i]);
				code.push_back(0x10);
				wasm::append_uleb(code, release_index());
			}
			code.push_back(0x0b);
		}
		code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6b, 0x24, 0x01, 0x20, 0x00, 0x10});
		wasm::append_uleb(code, import_index("bearer_free"));
		code.push_back(0x0b);
		result.push_back(body({0x01, 0x02, 0x7f}, std::move(code)));
	}
	if (use_clone_)
	{
		Bytes code{0x20, 0x00, 0x28, 0x02, 0x10, 0x21, 0x02, 0x20, 0x02, 0x41, 0x14, 0x6a, 0x10};
		wasm::append_uleb(code, import_index("bearer_alloc"));
		code.insert(code.end(), {0x21, 0x01, 0x20, 0x01, 0x45, 0x04, 0x40, 0x00, 0x0b});
		for (const auto [literal, offset] : {std::pair<std::uint8_t, unsigned>{1, 0}, {1, 4}, {1, 8}})
		{
			code.insert(code.end(), {0x20, 0x01, 0x41, literal, 0x36, 0x02});
			wasm::append_uleb(code, offset);
		}
		code.insert(code.end(), {0x20, 0x01, 0x20, 0x02, 0x41, 0x14, 0x6a, 0x36, 0x02, 0x0c, 0x20, 0x01, 0x20, 0x02, 0x36, 0x02, 0x10, 0x20, 0x01, 0x41, 0x14,
								 0x6a, 0x20, 0x00, 0x41, 0x14, 0x6a, 0x20, 0x02, 0xfc, 0x0a, 0x00, 0x00, 0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01, 0x20, 0x01});
		result.push_back(body({0x01, 0x02, 0x7f}, std::move(code)));
	}
	auto format_body = [&](const std::string& type)
	{
		Bytes code{0x20, 0x00, 0x41, 0x00, 0x41, 0x00, 0x10};
		wasm::append_uleb(code, import_index("bearer_format_" + type));
		code.insert(code.end(), {0x22, 0x01, 0x41});
		wasm::append_sleb32(code, std::numeric_limits<std::int32_t>::max() - 20);
		code.insert(code.end(), {0x4b, 0x04, 0x40, 0x41, 0x00, 0x0f, 0x0b, 0x20, 0x01, 0x41, 0x14, 0x6a, 0x10});
		wasm::append_uleb(code, import_index("bearer_alloc"));
		code.insert(code.end(), {0x22, 0x02, 0x45, 0x04, 0x40, 0x41, 0x00, 0x0f, 0x0b});
		for (const auto [literal, offset] : {std::pair<std::int32_t, unsigned>{1, 0}, {1, 4}, {1, 8}})
		{
			code.insert(code.end(), {0x20, 0x02, 0x41});
			wasm::append_sleb32(code, literal);
			code.insert(code.end(), {0x36, 0x02});
			wasm::append_uleb(code, offset);
		}
		code.insert(code.end(), {0x20, 0x02, 0x20, 0x01, 0x41, 0x14, 0x6a, 0x36, 0x02, 0x0c, 0x20, 0x02, 0x20, 0x01, 0x36, 0x02, 0x10,
								 0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01, 0x20, 0x00, 0x20, 0x02, 0x41, 0x14, 0x6a, 0x20, 0x01, 0x10});
		wasm::append_uleb(code, import_index("bearer_format_" + type));
		code.insert(code.end(), {0x20, 0x01, 0x47, 0x04, 0x40, 0x20, 0x02, 0x10});
		wasm::append_uleb(code, release_index());
		code.insert(code.end(), {0x41, 0x00, 0x0f, 0x0b, 0x20, 0x02});
		result.push_back(body({0x01, 0x02, 0x7f}, std::move(code)));
	};
	if (string_format_types_.contains("s64")) format_body("s64");
	if (string_format_types_.contains("u64")) format_body("u64");
	if (string_format_types_.contains("f64")) format_body("f64");
	for (const auto& [symbol, type] : fused_sink_formats_)
	{
		Bytes code{0x20, 0x00, 0x23, 0x00, 0x41}; wasm::append_sleb32(code, static_cast<std::int32_t>(fused_sink_scratch_offset_));
		code.insert(code.end(), {0x6a, 0x41}); wasm::append_sleb32(code, scalar_format_scratch_size);
		code.push_back(0x10); wasm::append_uleb(code, import_index("bearer_format_" + type));
		code.insert(code.end(), {0x22, 0x01, 0x41}); wasm::append_sleb32(code, scalar_format_scratch_size);
		code.insert(code.end(), {0x4b, 0x04, 0x40, 0x00, 0x0b, 0x23, 0x00, 0x41}); wasm::append_sleb32(code, static_cast<std::int32_t>(fused_sink_scratch_offset_));
		code.insert(code.end(), {0x6a, 0x20, 0x01, 0x10}); wasm::append_uleb(code, import_index(symbol));
		result.push_back(body({0x01, 0x01, 0x7f}, std::move(code)));
	}
	return result;
}

Bytes Module::custom_export_body(const Definition& target) const
{
	const unsigned length = 1, input = 2, result = 3, output = 4;
	Bytes code{0x20, 0x00, 0x41, 0x00, 0x41, 0x00, 0x10};
	wasm::append_uleb(code, import_index("bearer_dv_ptr_to_brrb"));
	code.push_back(0x21);
	wasm::append_uleb(code, length);
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x14, 0x6a, 0x10});
	wasm::append_uleb(code, import_index("bearer_alloc"));
	code.push_back(0x21);
	wasm::append_uleb(code, input);
	code.push_back(0x20);
	wasm::append_uleb(code, input);
	code.insert(code.end(), {0x45, 0x04, 0x40, 0x00, 0x0b});
	for (const auto [header, offset] : {std::pair<std::int32_t, unsigned>{1, 0}, {1, 4}, {4, 8}})
	{
		code.push_back(0x20);
		wasm::append_uleb(code, input);
		code.push_back(0x41);
		wasm::append_sleb32(code, header);
		code.insert(code.end(), {0x36, 0x02});
		wasm::append_uleb(code, offset);
	}
	code.push_back(0x20);
	wasm::append_uleb(code, input);
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x41, 0x14, 0x6a, 0x36, 0x02, 0x0c, 0x20});
	wasm::append_uleb(code, input);
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x36, 0x02, 0x10, 0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
	code.push_back(0x20);
	wasm::append_uleb(code, 0);
	code.push_back(0x20);
	wasm::append_uleb(code, input);
	code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
	wasm::append_uleb(code, length);
	code.push_back(0x10);
	wasm::append_uleb(code, import_index("bearer_dv_ptr_to_brrb"));
	code.push_back(0x20);
	wasm::append_uleb(code, length);
	code.insert(code.end(), {0x47, 0x04, 0x40, 0x00, 0x0b});
	code.push_back(0x20);
	wasm::append_uleb(code, input);
	code.push_back(0x10);
	wasm::append_uleb(code, target.index);
	code.push_back(0x21);
	wasm::append_uleb(code, result);
	code.push_back(0x20);
	wasm::append_uleb(code, result);
	code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
	wasm::append_uleb(code, result);
	code.insert(code.end(), {0x28, 0x02, 0x10, 0x10});
	wasm::append_uleb(code, import_index("bearer_dv_brrb_to_ptr"));
	code.push_back(0x21);
	wasm::append_uleb(code, output);
	code.push_back(0x20);
	wasm::append_uleb(code, input);
	code.push_back(0x10);
	wasm::append_uleb(code, release_index());
	code.push_back(0x20);
	wasm::append_uleb(code, result);
	code.push_back(0x10);
	wasm::append_uleb(code, release_index());
	code.push_back(0x20);
	wasm::append_uleb(code, output);
	code.push_back(0x0b);
	Bytes body{0x01, 0x04, 0x7f};
	body.insert(body.end(), code.begin(), code.end());
	Bytes result_body;
	wasm::append_uleb(result_body, static_cast<unsigned>(body.size()));
	result_body.insert(result_body.end(), body.begin(), body.end());
	return result_body;
}

void Module::collect()
{
	for (Expr* item : items_)
		if (auto alias = dynamic_cast<TypeAlias*>(item))
		{
			if (alias->name == "true" || alias->name == "false" || alias->name == "any" ||
				alias->name == "s32" || alias->name == "s64" || alias->name == "u64" || alias->name == "f64" || alias->name == "bool" ||
				alias->name == "string" || alias->name == "markup" || alias->name == "dval" || alias->name == "request" || alias->name == "module" || alias->name == "void")
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
			if (dynamic_cast<Struct*>(item) || dynamic_cast<Exports*>(item) || dynamic_cast<TypeAlias*>(item))
				continue;
			throw Error(item->location, "top-level executable expressions are not implemented by the native backend");
		}
		if (aliases_.contains(function->name))
			throw Error(function->location, "function name conflicts with type alias");
		if (function->host)
		{
			std::vector<std::string> parameters;
			for (const auto& parameter : function->parameters)
			{
				if (parameter.variadic)
					throw Error(parameter.type_expr->location, "host declarations cannot use variadic parameters");
				if (parameter.convert)
					throw Error(parameter.type_expr->location, "host declarations cannot request parameter conversion");
				const std::string type = value_type(parameter.type_expr);
				if (type == "void" || (!is_scalar(type) && type != "string" && type != "dval" && type != "request" && type != "module"))
					throw Error(parameter.type_expr->location, "host declarations support scalar, string, dval, request, and module parameters only");
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
		bool generic = false;
		bool variadic = false, variadic_convert = false;
		std::string variadic_element;
		for (const auto& parameter : function->parameters)
		{
			const bool any = dynamic_cast<Name*>(parameter.type_expr) && type_name(*parameter.type_expr) == "any";
			generic = generic || any;
			std::string type = any ? "any" : value_type(parameter.type_expr);
			if (parameter.variadic)
			{
				variadic = true;
				variadic_convert = parameter.convert;
				variadic_element = type;
				type = "array<" + type + ">";
			}
			parameters.push_back(std::move(type));
			conversions.push_back(parameter.variadic ? false : parameter.convert);
			if (parameter.convert && (any || !dynamic_cast<Name*>(parameter.type_expr)))
				throw Error(parameter.type_expr->location, "'as' parameter conversion requires a concrete named type constructor");
			if (parameters.back() == "void")
				throw Error(parameter.type_expr->location, "function parameters cannot have type void");
		}
		if (variadic && function->host)
			throw Error(function->location, "host declarations cannot use variadic parameters");
		if (variadic && generic)
			throw Error(function->location, "variadic parameters cannot use any yet");
		if (handler && variadic)
			throw Error(function->location, "Bearer handlers cannot use variadic parameters");
		if (handler && generic)
			throw Error(function->location, "Bearer handlers cannot use any parameters");
		if (handler && std::any_of(conversions.begin(), conversions.end(), [](bool value) { return value; }))
			throw Error(function->location, "Bearer handlers cannot request parameter conversion");
		const bool task_handler = function->name == "TASK" || function->name.rfind("TASK:", 0) == 0;
		if (task_handler && parameters != std::vector<std::string>{"request"})
			throw Error(function->location, "TASK handler requires exactly one request parameter");
		if (handler && !task_handler && (function->parameters.size() > 1 || (!parameters.empty() && parameters != std::vector<std::string>{"request"})))
			throw Error(function->location, "Bearer handler accepts zero parameters or one request parameter");
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
				auto result = dynamic_cast<ScopeLookup*>(function->return_type);
				auto parameter = result ? dynamic_cast<Name*>(result->value) : nullptr;
				if (result && parameter && result->member == "type")
				{
					for (std::size_t i = 0; i < function->parameters.size(); ++i)
						if (function->parameters[i].name == parameter->value)
							dependent = static_cast<int>(i);
					if (dependent < 0)
						throw Error(result->location, "dependent result names an unknown parameter");
				}
				else
					fixed_result = value_type(function->return_type, true);
			}
			const std::string constructor_result = has_struct(function->name) ? "struct:" + function->name : primitive_constructor_name(function->name) ? function->name : "";
			if (!constructor_result.empty() && (dependent >= 0 || fixed_result != constructor_result))
				throw Error(function->location, "constructor '" + function->name + "' must return " + constructor_result);
			generics_[function->name].push_back({function, parameters, conversions, fixed_result, dependent});
		}
		else
		{
			auto dependent = dynamic_cast<ScopeLookup*>(function->return_type);
			std::string result = handler || dependent ? "void" : value_type(function->return_type, true);
			if (dependent && dependent->member == "type")
			{
				auto parameter = dynamic_cast<Name*>(dependent->value);
				if (!parameter)
					throw Error(dependent->location, "dependent result names an unknown parameter");
				auto found = std::find_if(function->parameters.begin(), function->parameters.end(),
										  [&](const Parameter& value) { return value.name == parameter->value; });
				if (found == function->parameters.end())
					throw Error(dependent->location, "dependent result names an unknown parameter");
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
			definition.variadic = variadic;
			definition.variadic_element = variadic_element;
			definition.variadic_convert = variadic_convert;
			definition.result = result;
			definition.exported = exported;
			if (function->location.file == "capy://stdlib.capy" && parameters.empty() && function->body && function->body->items.size() == 1 && literal_type(function->body->items[0]) == result)
				definition.inline_value = function->body->items[0];
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
	for (Expr* item : items_)
		if (auto exports = dynamic_cast<Exports*>(item))
			for (const std::string& name : exports->names)
			{
				if (std::any_of(custom_exports_.begin(), custom_exports_.end(), [&](const auto& existing) { return existing.first == name; }))
					throw Error(exports->location, "custom DValue export '" + name + "' is already declared");
				Definition* target = nullptr;
				bool local = false, generic = false;
				for (Definition& definition : definitions_)
					if (definition.function && definition.function->location.file == source_ && definition.function->name == name)
					{
						local = true;
						if (definition.parameters == std::vector<std::string>{"dval"} && definition.result == "dval")
							target = &definition;
					}
				if (auto found = generics_.find(name); found != generics_.end())
					generic = std::any_of(found->second.begin(), found->second.end(), [&](const GenericDefinition& definition) { return definition.function->location.file == source_; });
				if (!target)
				{
					if (generic)
						throw Error(exports->location, "EXPORTS name '" + name + "' must name a non-generic local function with signature (dval) dval");
					if (local)
						throw Error(exports->location, "EXPORTS name '" + name + "' must have signature (dval) dval");
					throw Error(exports->location, "EXPORTS names unknown local function '" + name + "'");
				}
				add_custom_export(name, *target, exports->location);
			}
	bool any_export = std::any_of(definitions_.begin(), definitions_.end(), [](const Definition& d) { return !d.exported.empty(); });
	if (!any_export && custom_exports_.empty())
		throw Error({source_, 1, 1, 0}, "Capy Bearer unit exports no CLI, RENDER, WS, ONCE, or INIT handler");
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
			return "s32";
		if (dynamic_cast<SignedInteger*>(e))
			return "s64";
		if (dynamic_cast<UnsignedInteger*>(e))
			return "u64";
		if (dynamic_cast<Float*>(e))
			return "f64";
		if (dynamic_cast<String*>(e))
			return "string";
		if (dynamic_cast<Markup*>(e))
			return "markup";
		if (auto block = dynamic_cast<Block*>(e))
			return block->items.empty() ? "void" : scan_value_type(block->items.back());
		if (auto conditional = dynamic_cast<If*>(e))
		{
			if (!conditional->else_body) return "void";
			const std::string then_type = scan_value_type(conditional->then_body);
			const std::string else_type = scan_value_type(conditional->else_body);
			return then_type == else_type ? then_type : "";
		}
		if (auto tuple = dynamic_cast<TupleExpr*>(e))
		{
			std::string type = "tuple<";
			for (std::size_t i = 0; i < tuple->items.size(); ++i)
			{
				const std::string item = scan_value_type(tuple->items[i]);
				if (item.empty()) return "";
				type += (i ? "," : "") + item;
			}
			return type + ">";
		}
		if (auto array = dynamic_cast<ArrayLiteral*>(e))
		{
			if (array->items.empty()) return array->explicit_element_type ? "array<" + value_type(array->explicit_element_type) + ">" : "";
			std::string element;
			for (Expr* value : array->items)
			{
				std::string item = scan_value_type(value);
				if (item.rfind("spread<", 0) == 0) item = item.substr(7, item.size() - 8);
				if (item.empty() || (!element.empty() && item != element)) return "";
				element = item;
			}
			return "array<" + element + ">";
		}
		if (auto name = dynamic_cast<Name*>(e))
		{
			if (name->value == "true" || name->value == "false")
				return "bool";
			if (auto found = scan_value_names.find(name->value); found != scan_value_names.end())
				return found->second;
		}
		if (auto spread = dynamic_cast<Spread*>(e))
		{
			const std::string source = scan_value_type(spread->value);
			if (source.rfind("array<", 0) == 0) return "spread<" + source.substr(6);
			if (source.rfind("tuple<", 0) == 0) return "spread-" + source;
			return "";
		}
		if (auto variable = dynamic_cast<Variable*>(e))
			return variable->annotation ? value_type(variable->annotation) : scan_value_type(variable->value);
		if (auto index = dynamic_cast<Index*>(e))
		{
			const std::string source = scan_value_type(index->value);
			if (source.rfind("array<", 0) == 0) return source.substr(6, source.size() - 7);
			if (source == "dval") return "dval";
		}
		if (auto binary = dynamic_cast<Binary*>(e))
		{
			const bool comparison = binary->operator_ == "==" || binary->operator_ == "!=" || binary->operator_ == "<" || binary->operator_ == ">" ||
									binary->operator_ == "<=" || binary->operator_ == ">=" || binary->operator_ == "&&" || binary->operator_ == "||" ||
									binary->operator_ == "unary!";
			return comparison ? "bool" : scan_value_type(binary->right);
		}
		if (auto member = dynamic_cast<Member*>(e))
		{
			const std::string receiver = scan_value_type(member->value);
			if (receiver.rfind("struct:", 0) == 0)
				for (const auto& field : struct_type(receiver.substr(7), member->location).fields)
					if (field.first == member->member) return field.second;
		}
		if (auto call = dynamic_cast<Call*>(e))
		{
			if (const Member* member = member_call(call))
			{
				const std::string receiver = scan_value_type(member->value);
				if (receiver.rfind("array<", 0) == 0)
				{
					if (member->member == "capacity") return "s32";
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
				if (name->value == "length" || name->value == "arc_live") return "s32";
				if (name->value == "dval_has" || name->value == "dval_bool") return "bool";
				if (name->value == "dval_string") return "string";
				if (name->value == "dval_s32") return "s32";
				if (name->value == "dval_f64") return "f64";
				if (name->value == "trusted_markup") return "markup";
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
	std::function<void(Expr*)> scan_dval = [&](Expr* e)
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
		if (type == "string") runtime_imports_.insert("bearer_dv_string_to_brrb");
		else if (type == "s32") runtime_imports_.insert("bearer_dv_s32_to_brrb");
		else if (type == "u64") runtime_imports_.insert("bearer_dv_u64_to_brrb");
		else if (type == "f64") runtime_imports_.insert("bearer_dv_f64_to_brrb");
		else if (type == "bool") runtime_imports_.insert("bearer_dv_bool_to_brrb");
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
			{
				for (const Definition& definition : definitions_)
					if (definition.function->name == name->value && definition.result == "string")
						return true;
			}
		return false;
	};
	auto scan_string_construction = [&](const std::string& source)
	{
		if (source == "string" || source == "bool") return;
		scan_alloc = scan_retain = scan_release = true;
		scan_clone = scan_clone || source == "markup";
		scan_format_s64 = scan_format_s64 || source == "s32" || source == "s64";
		scan_format_u64 = scan_format_u64 || source == "u64";
		scan_format_f64 = scan_format_f64 || source == "f64";
		if (source == "s32" || source == "s64") string_format_types_.insert("s64");
		else if (source == "u64" || source == "f64") string_format_types_.insert(source);
	};
	std::function<void(Expr*)> scan = [&](Expr* e)
	{
		check_cancelled();
		if (auto c = dynamic_cast<Call*>(e))
		{
			const Member* member = member_call(c);
			const Name* named_call = dynamic_cast<Name*>(c->function);
			const std::string named_callee = named_call && has_alias(named_call->value) && !scan_value_names.contains(named_call->value)
				? constructor_name(named_call->value, named_call->location) : named_call ? named_call->value : "";
			if (member && scan_value_type(member->value).rfind("array<", 0) == 0 &&
				(member->member == "push" || member->member == "pop" || member->member == "insert" || member->member == "remove" ||
				 member->member == "clear" || member->member == "reserve" || member->member == "resize"))
				scan_alloc = scan_retain = scan_release = true;
			if (auto n = dynamic_cast<Name*>(c->function); n || member)
			{
				std::vector<std::string> host_arguments;
				if (member)
					host_arguments.push_back(scan_value_type(member->value));
				for (Expr* argument : c->arguments)
				{
					const std::string type = scan_value_type(argument);
					if (type.rfind("spread-tuple<", 0) == 0)
						for (const std::string& field : aggregate_elements(type.substr(7))) host_arguments.push_back(field);
					else
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
								if (auto contract = variadic_function_types_.find(type); contract != variadic_function_types_.end())
								{
									scan_alloc = scan_retain = scan_release = true;
									for (std::size_t i = contract->second.fixed + 1; i < host_arguments.size(); ++i)
										scan_string_construction(host_arguments[i]);
								}
								break;
							}
				}
				if (const HostDeclaration* host = this->host(callee, host_arguments))
				{
					used_hosts_.insert(host->symbol);
					trace_host_ = trace_host_ || host->trace;
					for (const std::string& type : host->parameters)
						if (managed_type(type)) { dval_ = dval_ || type == "dval"; scan_retain = true; scan_release = true; }
					if (managed_type(host->result)) { dval_ = dval_ || host->result == "dval"; scan_alloc = true; scan_retain = true; scan_release = true; }
				}
				if (const Definition* converted = converted_definition(callee, host_arguments, c->location))
				{
					for (std::size_t i = 0; i < host_arguments.size(); ++i)
						if (converted->parameters[i] == "string" && host_arguments[i] != "string" && host_arguments[i] != "bool")
							scan_string_construction(host_arguments[i]);
				}
				if (const Definition* variadic = variadic_definition(callee, host_arguments, c->location))
				{
					const HostDeclaration* sink = fused_variadic_sink(*variadic);
					if (sink) used_hosts_.insert(sink->symbol);
					else scan_alloc = scan_retain = scan_release = true;
					const std::size_t fixed = variadic->parameters.size() - 1;
					for (std::size_t i = fixed; i < host_arguments.size(); ++i)
					{
						const std::string source = host_arguments[i].rfind("spread<", 0) == 0 ? host_arguments[i].substr(7, host_arguments[i].size() - 8) : host_arguments[i];
						if (variadic->variadic_convert && variadic->variadic_element == "string")
						{
							if (sink && (source == "s32" || source == "s64" || source == "u64" || source == "f64"))
							{
								const std::string type = source == "s32" ? "s64" : source;
								fused_sink_formats_.insert({sink->symbol, type});
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
					for (const Definition& candidate : definitions_)
						if (candidate.function && candidate.function->name == callee)
						{
							if (candidate.variadic && candidate.parameters.size() - 1 <= host_arguments.size() && candidate.variadic_convert && candidate.variadic_element == "string")
							{
								if (const HostDeclaration* sink = fused_variadic_sink(candidate))
								{
									used_hosts_.insert(sink->symbol);
									for (const std::string& type : {"s64", "u64", "f64"}) fused_sink_formats_.insert({sink->symbol, type});
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
			if (named_call && (named_callee == "dval" || named_callee == "dval_has" || named_callee == "dval_string" || named_callee == "dval_s32" || named_callee == "dval_f64" || named_callee == "dval_bool"))
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
				else if (named_callee == "dval_has") runtime_imports_.insert("bearer_dv_get_brrb");
				else if (named_callee == "dval_string") { runtime_imports_.insert("bearer_dv_scalar_type_brrb"); runtime_imports_.insert("bearer_dv_brrb_to_string"); }
				else if (named_callee == "dval_s32") runtime_imports_.insert("bearer_dv_s32_brrb");
				else if (named_callee == "dval_f64") runtime_imports_.insert("bearer_dv_f64_brrb");
				else if (named_callee == "dval_bool") runtime_imports_.insert("bearer_dv_bool_brrb");
			}
			if (named_call && primitive_constructor_name(named_callee) && c->arguments.size() == 1)
			{
				const std::string source = scan_value_type(c->arguments[0]);
				if (named_callee == "string" && source != "string" && source != "bool") scan_string_construction(source);
			}
			if (auto n = dynamic_cast<Name*>(c->function); n && n->value == "clone")
			{
				scan_alloc = true;
				scan_release = true;
				scan_clone = true;
			}
			if (auto n = dynamic_cast<Name*>(c->function); n && n->value == "arc_live")
				scan_arc_live = true;
			if (member)
				scan(member->value);
			for (auto a : c->arguments)
				scan(a);
		}
		else if (auto b = dynamic_cast<Block*>(e))
		{
			for (auto x : b->items)
				scan(x);
			if (!b->items.empty() && managed_type(scan_value_type(b->items.back())))
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
			if (v->annotation && managed_type(annotation))
			{
				scan_retain = true;
				scan_release = true;
			}
		}
		else if (auto b = dynamic_cast<Binary*>(e))
		{
			if (b->operator_ == "=" && dynamic_cast<Index*>(b->left))
				scan_alloc = scan_retain = scan_release = true;
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
		else if (auto spread = dynamic_cast<Spread*>(e))
			scan(spread->value);
		else if (auto i = dynamic_cast<Index*>(e))
		{
			if (scan_value_type(i->value) == "dval")
				runtime_imports_.insert("bearer_dv_get_brrb");
			scan(i->value);
			scan(i->index);
		}
		else if (auto m = dynamic_cast<Member*>(e))
			scan(m->value);
		else if (auto m = dynamic_cast<Markup*>(e))
		{
			scan_alloc = true;
			scan_retain = true;
			scan_release = true;
			for (auto part : m->parts)
				if (auto field = dynamic_cast<MarkupField*>(part))
				{
					const std::string wide = scan_value_type(field->value);
					scan_format_s64 = scan_format_s64 || wide == "s64";
					scan_format_u64 = scan_format_u64 || wide == "u64";
					scan_format_f64 = scan_format_f64 || wide == "f64";
					if (wide == "s64" || wide == "u64" || wide == "f64") string_format_types_.insert(wide);
					scan(field->value);
				}
		}
		else if (auto t = dynamic_cast<TupleExpr*>(e))
		{
			scan_alloc = true;
			scan_retain = true;
			scan_release = true;
			for (auto item : t->items)
				scan(item);
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
			if (iterable.rfind("array<", 0) == 0 && f->names.size() == 1)
				scan_value_names[f->names[0]] = iterable.substr(6, iterable.size() - 7);
			else if (iterable == "dval" && !f->names.empty())
			{
				scan_value_names[f->names.back()] = "dval";
				if (f->names.size() == 2) scan_value_names[f->names.front()] = "string";
			}
			else if (!f->names.empty())
				scan_value_names[f->names[0]] = "s32";
			scan(f->body);
			scan_value_names = std::move(outer);
		}
	};
	for (auto& d : definitions_)
	{
		if (d.body_omitted || d.inline_only) continue;
		scan(d.function);
		for (const auto& type : d.parameters)
			if (managed_type(type))
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
	if (dval_)
		runtime_imports_.insert("bearer_dv_get_brrb");
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
	check_cancelled();
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
	const Capabilities capabilities = discover_capabilities();
	const bool scan_format_s64 = capabilities.format_s64, scan_format_u64 = capabilities.format_u64, scan_format_f64 = capabilities.format_f64;
	const bool scan_alloc = capabilities.alloc, scan_retain = capabilities.retain, scan_release = capabilities.release, scan_clone = capabilities.clone,
			   scan_arc_live = capabilities.arc_live;
	const bool scan_free = scan_release;
	use_retain_ = scan_retain;
	use_release_ = scan_release;
	use_clone_ = scan_clone;
	use_arc_global_ = scan_arc_live || scan_alloc || scan_release || scan_clone;
	use_trace_global_ = trace_host_;
	if (!fused_sink_formats_.empty())
	{
		while (data_.size() % 8) data_.push_back(0);
		fused_sink_scratch_offset_ = static_cast<unsigned>(data_.size());
		data_.insert(data_.end(), scalar_format_scratch_size, 0);
	}
	if (use_trace_global_)
	{
		while (data_.size() % 8)
			data_.push_back(0);
		trace_stack_offset_ = static_cast<unsigned>(data_.size());
		data_.insert(data_.end(), 256 * 8, 0);
	}
	unsigned next = 0;
	if (scan_format_s64)
		imports_["bearer_format_s64"] = next++;
	if (scan_format_u64)
		imports_["bearer_format_u64"] = next++;
	if (scan_format_f64)
		imports_["bearer_format_f64"] = next++;
	if (scan_alloc || scan_clone)
		imports_["bearer_alloc"] = next++;
	if (scan_free)
		imports_["bearer_free"] = next++;
	for (const std::string& name : runtime_imports_)
		imports_[name] = next++;
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
	if (string_format_types_.contains("s64"))
		helpers_["format_s64"] = next++;
	if (string_format_types_.contains("u64"))
		helpers_["format_u64"] = next++;
	if (string_format_types_.contains("f64"))
		helpers_["format_f64"] = next++;
	for (const auto& [symbol, type] : fused_sink_formats_)
		helpers_[sink_format_helper(symbol, type)] = next++;
	first_user_index_ = next;
	for (Definition& d : definitions_)
	{
		if (d.inline_only) continue;
		d.index = next++;
		if (d.thunk_target != 0xffffffffu || d.closure_body)
			continue;
		const std::string contract = d.variadic ? "variadic:" + d.variadic_element + (d.variadic_convert ? ":convert" : "") : "";
		d.type = wasm_type(d.exported.empty() ? d.parameters : std::vector<std::string>{"request"}, d.result, contract);
		if (d.variadic) variadic_function_types_[d.type] = {d.parameters.size() - 1, d.variadic_element, d.variadic_convert};
	}
	// Ensure import signatures precede user types and lower after indexes are stable.
	unsigned format_s64_type = wasm_type({"s64", "s32", "s32"}, "s32");
	unsigned format_u64_type = wasm_type({"u64", "s32", "s32"}, "s32");
	unsigned format_f64_type = wasm_type({"f64", "s32", "s32"}, "s32");
	unsigned alloc_type = (scan_alloc || scan_clone) ? wasm_type({"s32"}, "s32") : 0;
	unsigned release_type = scan_free ? wasm_type({"s32"}, "void") : 0;
	unsigned clone_type = scan_clone ? wasm_type({"s32"}, "s32") : 0;
	unsigned format_string_s64_type = string_format_types_.contains("s64") ? wasm_type({"s64"}, "string") : 0;
	unsigned format_string_u64_type = string_format_types_.contains("u64") ? wasm_type({"u64"}, "string") : 0;
	unsigned format_string_f64_type = string_format_types_.contains("f64") ? wasm_type({"f64"}, "string") : 0;
	std::map<std::pair<std::string, std::string>, unsigned> sink_format_types;
	for (const auto& sink : fused_sink_formats_) sink_format_types[sink] = wasm_type({sink.second}, "void");
	unsigned blob_type = wasm_type({"s32", "s32", "s32", "s32"}, "s32");
	unsigned scalar_adapter_type = wasm_type({"s32", "s32", "s32"}, "s32");
	unsigned f64_adapter_type = wasm_type({"f64", "s32", "s32"}, "s32");
	unsigned u64_adapter_type = wasm_type({"u64", "s32", "s32"}, "s32");
	unsigned build_type = wasm_type({"s32", "s32", "s32", "s32", "s32"}, "s32");
	unsigned get_type = wasm_type({"s32", "s32", "s32", "s32", "s32", "s32", "s32", "s32"}, "s32");
	unsigned entry_type = wasm_type({"s32", "s32", "s32", "s32", "s32"}, "s32");
	unsigned count_type = wasm_type({"s32", "s32"}, "s32");
	auto omitted_body = [](const std::string& result)
	{
		Bytes content{0x00};
		if (result == "s64" || result == "u64") content.insert(content.end(), {0x42, 0x00});
		else if (result == "f64") { content.push_back(0x44); wasm::append_f64(content, 0.0); }
		else if (result != "void") content.insert(content.end(), {0x41, 0x00});
		content.push_back(0x0b);
		Bytes body;
		wasm::append_uleb(body, static_cast<unsigned>(content.size()));
		body.insert(body.end(), content.begin(), content.end());
		return body;
	};
	std::vector<Bytes> user_bodies;
	for (std::size_t index = 0; index < definitions_.size(); ++index)
	{
		if (definitions_[index].inline_only) continue;
		user_bodies.push_back(definitions_[index].body_omitted ? omitted_body(definitions_[index].result) : FunctionLowerer(*this, definitions_[index]).lower());
	}
	std::vector<const Definition*> emitted_definitions;
	for (const Definition& definition : definitions_)
		if (!definition.inline_only)
			emitted_definitions.push_back(&definition);
	std::vector<Bytes> bodies = runtime_bodies();
	bodies.insert(bodies.end(), user_bodies.begin(), user_bodies.end());
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
			: name == "bearer_format_s64" ? format_s64_type : name == "bearer_format_u64" ? format_u64_type : name == "bearer_format_f64" ? format_f64_type
			: name == "bearer_alloc" ? alloc_type : name == "bearer_free" ? release_type
			: name == "bearer_dv_string_to_brrb" || name == "bearer_dv_brrb_to_string" ? blob_type
			: name == "bearer_dv_f64_to_brrb" ? f64_adapter_type : name == "bearer_dv_u64_to_brrb" ? u64_adapter_type
			: name == "bearer_dv_s32_to_brrb" || name == "bearer_dv_bool_to_brrb" || name == "bearer_dv_s32_brrb" || name == "bearer_dv_f64_brrb" || name == "bearer_dv_bool_brrb" ? scalar_adapter_type
			: name == "bearer_dv_build_brrb" ? build_type : name == "bearer_dv_get_brrb" ? get_type
			: name == "bearer_dv_count_brrb" || name == "bearer_dv_scalar_type_brrb" ? count_type
			: name == "bearer_dv_entry_key_brrb" || name == "bearer_dv_entry_value_brrb" ? entry_type
			: name == "bearer_dv_ptr_to_brrb" ? scalar_adapter_type : name == "bearer_dv_brrb_to_ptr" ? count_type : 0;
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
	if (string_format_types_.contains("s64"))
		wasm::append_uleb(functions, format_string_s64_type);
	if (string_format_types_.contains("u64"))
		wasm::append_uleb(functions, format_string_u64_type);
	if (string_format_types_.contains("f64"))
		wasm::append_uleb(functions, format_string_f64_type);
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
	Bytes code;
	wasm::append_vector(code, bodies);
	Bytes mem;
	wasm::append_uleb(mem, static_cast<unsigned>(data_.size()));
	wasm::append_uleb(mem, 3);
	wasm::append_uleb(mem, 0);
	wasm::append_uleb(mem, 0);
	std::string abi = "format=bearer-wasm-unit-abi-v1\nunit_abi_version=" + std::to_string(abi_) + "\ntoolchain=capyc-native-cpp20\nsource=" + source_ + "\n";
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
	wasm::append_section(result, 10, code);
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
	std::size_t cursor = code_section_offset + 1 + uleb_size(code.size()) + uleb_size(bodies.size());
	const std::size_t runtime_count = runtime_bodies().size();
	for (std::size_t index = 0; index < runtime_count; ++index)
		cursor += bodies[index].size();
	std::vector<std::pair<std::size_t, Location>> source_rows;
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
	for (std::size_t index = 0; index < markers_.size(); ++index)
	{
		check_cancelled();
		Bytes marker{0x01, 0x01, 0x01, 0x41};
		wasm::append_sleb32(marker, static_cast<std::int32_t>(0x5a000000u + index));
		marker.push_back(0x1a);
		auto code_begin = result.begin() + static_cast<std::ptrdiff_t>(code_section_offset);
		auto code_end = result.begin() + static_cast<std::ptrdiff_t>(code_section_end);
		auto found = std::search(code_begin, code_end, marker.begin(), marker.end());
		if (found == code_end || std::search(found + 1, code_end, marker.begin(), marker.end()) != code_end)
			throw Error(markers_[index], "native Capy source marker is missing or ambiguous in final Wasm");
		source_rows.push_back({static_cast<std::size_t>(found - result.begin()), markers_[index]});
	}
	check_cancelled();
	std::sort(source_rows.begin(), source_rows.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
	std::ostringstream map;
	map << "BEARER_SOURCE_MAP_V1\t" << module_ << "\n";
	for (std::size_t index = 0; index < sources_.size(); ++index)
		map << "F\t" << index + 1 << "\t" << sources_[index] << "\n";
	for (const auto& [address, location] : source_rows)
	{
		auto source = std::find(sources_.begin(), sources_.end(), location.file);
		if (source == sources_.end())
			throw Error(location, "source location is not registered in this Capy module");
		map << "L\t" << std::hex << address << std::dec << "\t" << (source - sources_.begin()) + 1 << "\t" << location.line << "\t" << location.column << "\n";
	}
	CompileResult compiled{std::move(result), map.str(), {}};
	for (const auto& [name, target] : custom_exports_)
		compiled.custom_exports.push_back(name);
	return compiled;
}

} // namespace

namespace
{

bool local(const std::vector<std::set<std::string>>& scopes, const std::string& name)
{
	return std::any_of(scopes.rbegin(), scopes.rend(), [&](const auto& scope) { return scope.contains(name); });
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
			// The receiver is an ordinary first argument only after compatibility
			// resolution; demand selection records that possible direct call without
			// rewriting the parsed member expression.
			if (!local(scopes, member->member))
				calls.insert({member->member, call->arguments.size() + 1});
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
	else if (auto conditional = dynamic_cast<If*>(expression))
	{
		collect_stdlib_demand(conditional->condition, calls, values, scopes);
		collect_stdlib_demand(conditional->then_body, calls, values, scopes);
		collect_stdlib_demand(conditional->else_body, calls, values, scopes);
	}
	else if (auto loop = dynamic_cast<While*>(expression))
	{
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
	else if (auto tuple = dynamic_cast<TupleExpr*>(expression))
		for (Expr* item : tuple->items)
			collect_stdlib_demand(item, calls, values, scopes);
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
				if (parameter.name.rfind("__bearer_", 0) == 0)
					throw Error(function->location, "__bearer_* names are reserved for the Capy standard library");
			reject_reserved_calls(function->body);
		}
		else if (auto variable = dynamic_cast<Variable*>(expression))
		{
			if (variable->name.rfind("__bearer_", 0) == 0)
				throw Error(variable->location, "__bearer_* names are reserved for the Capy standard library");
			reject_reserved_calls(variable->value);
		}
		else if (auto binary = dynamic_cast<Binary*>(expression))
		{
			if (binary->operator_ == ":=")
				if (auto name = dynamic_cast<Name*>(binary->left); name && name->value.rfind("__bearer_", 0) == 0)
					throw Error(name->location, "__bearer_* names are reserved for the Capy standard library");
			reject_reserved_calls(binary->left); reject_reserved_calls(binary->right);
		}
		else if (auto returned = dynamic_cast<Return*>(expression)) reject_reserved_calls(returned->value);
		else if (auto conditional = dynamic_cast<If*>(expression)) { reject_reserved_calls(conditional->condition); reject_reserved_calls(conditional->then_body); reject_reserved_calls(conditional->else_body); }
		else if (auto loop = dynamic_cast<While*>(expression)) { reject_reserved_calls(loop->condition); reject_reserved_calls(loop->body); }
		else if (auto loop = dynamic_cast<For*>(expression))
		{
			for (const std::string& name : loop->names)
				if (name.rfind("__bearer_", 0) == 0)
					throw Error(loop->location, "__bearer_* names are reserved for the Capy standard library");
			reject_reserved_calls(loop->iterable); reject_reserved_calls(loop->body);
		}
		else if (auto index = dynamic_cast<Index*>(expression)) { reject_reserved_calls(index->value); reject_reserved_calls(index->index); }
		else if (auto member = dynamic_cast<Member*>(expression)) reject_reserved_calls(member->value);
		else if (auto tuple = dynamic_cast<TupleExpr*>(expression)) for (Expr* item : tuple->items) reject_reserved_calls(item);
		else if (auto array = dynamic_cast<ArrayLiteral*>(expression)) for (Expr* item : array->items) reject_reserved_calls(item);
		else if (auto spread = dynamic_cast<Spread*>(expression)) reject_reserved_calls(spread->value);
		else if (auto map = dynamic_cast<MapLiteral*>(expression)) for (const auto& [key, item] : map->entries) reject_reserved_calls(item);
		else if (auto markup = dynamic_cast<Markup*>(expression)) for (Expr* item : markup->parts) reject_reserved_calls(item);
		else if (auto field = dynamic_cast<MarkupField*>(expression)) reject_reserved_calls(field->value);
		else if (auto lambda = dynamic_cast<Lambda*>(expression))
		{
			for (const auto& parameter : lambda->parameters)
				if (parameter.name.rfind("__bearer_", 0) == 0)
					throw Error(lambda->location, "__bearer_* names are reserved for the Capy standard library");
			reject_reserved_calls(lambda->body);
		}
	};
	for (Expr* item : program.items)
	{
		reject_reserved_calls(item);
		if (auto function = dynamic_cast<Function*>(item))
		{
			if (function->host)
				throw Error(function->location, "host declarations are available only in the embedded Capy standard library");
			if (function->name.rfind("__bearer_", 0) == 0)
				throw Error(function->location, "__bearer_* names are reserved for the Capy standard library");
			if (public_names.contains(function->name))
				throw Error(function->location, "'" + function->name + "' is reserved by the Capy standard library");
		}
		else if (auto structure = dynamic_cast<Struct*>(item); structure && structure->name.rfind("__bearer_", 0) == 0)
			throw Error(structure->location, "__bearer_* names are reserved for the Capy standard library");
		else if (auto alias = dynamic_cast<TypeAlias*>(item))
		{
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
	for (Expr* item : unit.items)
		collect_stdlib_demand(item, calls, values, scopes);
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

CompileResult compile_program(const Program& program, const std::string& source_path, const std::string& module_name, unsigned abi_version,
	CancellationCallback cancelled, ParsedSourceCache* cache)
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
	return Module(std::move(items), std::move(sources), source_path, module_name, abi_version, std::move(cancelled)).compile();
}

} // namespace

CompileResult compile_bearer_unit(std::string_view source, const CompileOptions& options)
{
	auto program = parsed_source(source, options.canonical_source_identity, options.source_path, options.abi_version,
		options.cancelled, options.parsed_source_cache);
	return compile_program(*program, options.source_path, options.module_name, options.abi_version, options.cancelled, options.parsed_source_cache);
}

CompileResult compile_bearer_unit(const Program& program, const std::string& source_path, const std::string& module_name, unsigned abi_version,
								  CancellationCallback cancelled)
{
	return compile_program(program, source_path, module_name, abi_version, std::move(cancelled), nullptr);
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
