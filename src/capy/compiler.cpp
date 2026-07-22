#include "compiler.h"

#include "frontend.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace capy
{
namespace
{

using wasm::Bytes;

bool is_scalar(const std::string& type)
{
	return type == "s32" || type == "bool";
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

bool is_handler(const std::string& name, std::string& exported)
{
	static const std::map<std::string, std::string> names = {
		{"RENDER", "__bearer_render"}, {"COMPONENT", "__bearer_component"},	  {"CLI", "__bearer_cli"}, {"WS", "__bearer_websocket"}, {"ONCE", "__bearer_once"},
		{"INIT", "__bearer_init"},	   {"SERVE_HTTP", "__bearer_serve_http"},
	};
	if (auto it = names.find(name); it != names.end())
	{
		exported = it->second;
		return true;
	}
	for (const auto& prefix : {std::pair<std::string_view, std::string_view>{"RENDER_", "__bearer_render_"},
							   {"COMPONENT_", "__bearer_component_"},
							   {"SERVE_HTTP_", "__bearer_serve_http_"}})
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
	if (name == "s32" || name == "bool" || name == "string" || name == "markup" || name == "dval" || name == "request" || name == "void")
		return name;
	return "struct:" + name;
}

struct Definition
{
	Function* function = nullptr;
	std::vector<std::string> parameters;
	std::string result;
	std::string exported;
	unsigned index = 0;
	unsigned type = 0;
	unsigned thunk_target = 0xffffffffu;
	bool closure_body = false;
	std::vector<std::pair<std::string, std::string>> captures;
};

struct GenericDefinition
{
	Function* function = nullptr;
	std::vector<std::string> patterns;
	int dependent_result = -1;
};

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
	unsigned local_count_ = 0;
	bool implicit_result_ = false;
	struct Loop
	{
		unsigned break_depth, continue_depth, ownership_boundary;
		Bytes break_edge, continue_edge;
	};
	std::vector<Loop> loops_;
	unsigned control_depth_ = 0;

	std::pair<Bytes, std::string> expression(Expr* value);
	std::string infer(Expr* value);
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
	std::pair<Bytes, unsigned> allocate_blob(const std::string& type, unsigned type_id, unsigned length, const Location& location);
	Bytes block(Block* block, bool new_scope = true);
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
	Module(const Program& program, std::string source, std::string module, unsigned abi, CancellationCallback cancelled)
		: program_(program), source_(std::move(source)), module_(std::move(module)), abi_(abi), cancelled_(std::move(cancelled))
	{
	}

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
	void need_print_bytes()
	{
		print_bytes_ = true;
	}
	void need_print_s32()
	{
		print_s32_ = true;
	}
	void need_alloc()
	{
		alloc_ = true;
	}
	void need_unit_render()
	{
		unit_render_ = true;
	}
	void need_component_render()
	{
		component_render_ = true;
	}
	void need_dval()
	{
		dval_ = true;
		alloc_ = true;
	}
	void need_unit_call()
	{
		unit_call_ = true;
		dval_ = true;
		alloc_ = true;
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
		return helpers_.at(name);
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
	std::pair<std::string, unsigned> reference_function(const std::string& name, const Location& location)
	{
		std::vector<std::size_t> candidates;
		for (std::size_t i = 0; i < definitions_.size(); ++i)
			if (definitions_[i].function && definitions_[i].function->name == name && definitions_[i].exported.empty())
				candidates.push_back(i);
		if (generics_.contains(name))
			throw Error(location, "generic function value '" + name + "' requires an explicit concrete function type");
		if (candidates.empty())
			throw Error(location, "unknown local '" + name + "'");
		if (candidates.size() != 1)
			throw Error(location, "function value '" + name + "' requires exactly one concrete overload; found more than one overload");
		Definition& target = definitions_[candidates.front()];
		const std::string value_type = "function#" + std::to_string(wasm_type(
														 [&]
														 {
															 auto values = target.parameters;
															 values.insert(values.begin(), "s32");
															 return values;
														 }(),
														 target.result));
		const std::string cache = key(name, target.parameters);
		if (auto found = function_values_.find(cache); found != function_values_.end())
			return {value_type, found->second};
		Definition thunk;
		thunk.parameters = target.parameters;
		thunk.parameters.insert(thunk.parameters.begin(), "s32");
		thunk.result = target.result;
		thunk.index = first_user_index_ + static_cast<unsigned>(definitions_.size());
		thunk.type = wasm_type(thunk.parameters, thunk.result);
		thunk.thunk_target = static_cast<unsigned>(candidates.front());
		definitions_.push_back(std::move(thunk));
		const unsigned slot = static_cast<unsigned>(table_functions_.size());
		table_functions_.push_back(definitions_.back().index);
		function_values_[cache] = slot;
		return {value_type, slot};
	}
	Definition& resolve(const std::string& name, const std::vector<std::string>& types, const Location& location)
	{
		if (auto it = definitions_by_key_.find(key(name, types)); it != definitions_by_key_.end())
			return definitions_[it->second];
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
			throw Error(location, "no overload " + name + "(" + rendered + ")");
		if (candidates.size() != 1)
			throw Error(location, "ambiguous generic overload " + name + "(" + rendered + ")");
		const GenericDefinition& generic = *candidates.front();
		Definition definition;
		definition.function = generic.function;
		definition.parameters = types;
		definition.result = generic.dependent_result < 0 ? "void" : types[static_cast<unsigned>(generic.dependent_result)];
		definition.index = first_user_index_ + static_cast<unsigned>(definitions_.size());
		definition.type = wasm_type(definition.parameters, definition.result);
		definitions_by_key_[key(name, types)] = definitions_.size();
		definitions_.push_back(std::move(definition));
		return definitions_.back();
	}
	Bytes marker(const Location& location)
	{
		const std::int32_t value = static_cast<std::int32_t>(0x5a000000u + markers_.size());
		markers_.push_back(location);
		Bytes code{0x41};
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
	const Program& program_;
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
	std::map<std::string, unsigned> helpers_;
	Bytes data_;
	bool print_bytes_ = false, print_s32_ = false, alloc_ = false;
	bool unit_render_ = false, component_render_ = false, dval_ = false, unit_call_ = false;
	std::vector<std::pair<std::string, Definition*>> custom_exports_;
	bool use_retain_ = false, use_release_ = false, use_clone_ = false, use_arc_global_ = false;
	std::vector<Location> markers_;
	std::vector<std::pair<std::vector<std::string>, std::string>> types_;
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
	unsigned wasm_type(const std::vector<std::string>& parameters, const std::string& result)
	{
		std::string signature = result + "|";
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
			type += (i ? "," : "") + value_type(tuple->items[i]);
		return type + ">";
	}
	if (auto array = dynamic_cast<const ArrayLiteral*>(expression))
	{
		if (array->items.size() != 1)
			throw Error(expression->location, "array type requires exactly one element type");
		return "array<" + value_type(array->items[0]) + ">";
	}
	if (auto function = dynamic_cast<const FunctionType*>(expression))
	{
		std::vector<std::string> parameters{"s32"};
		for (const auto& parameter : function->parameters)
			parameters.push_back(value_type(parameter.type_expr));
		return "function#" + std::to_string(wasm_type(parameters, value_type(function->return_type, true)));
	}
	return type_of_expression(expression, allow_void);
}

FunctionLowerer::FunctionLowerer(Module& module, Definition& definition) : module_(module), definition_(definition)
{
	if (!definition.function)
		return;
	scopes_.push_back({});
	owned_scopes_.push_back({});
	unsigned parameter = definition.exported.empty() ? 0 : (definition.function->parameters.empty() ? 0 : 1);
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
			const std::string type = definition.exported.empty() ? definition.parameters[parameter] : "request";
			scopes_.back()[value.name] = {parameter, type};
			if (managed_type(type))
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

bool FunctionLowerer::expression_is_owned(const Expr* value)
{
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
	if (auto call = dynamic_cast<const Call*>(value))
	{
		if (auto name = dynamic_cast<const Name*>(call->function))
		{
			if (name->value == "clone" || module_.has_struct(name->value))
				return true;
			if (name->value != "print" && name->value != "trap" && name->value != "arc_live")
				return true;
		}
	}
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
	unsigned parameter_count =
		definition_.exported.empty() ? static_cast<unsigned>(definition_.parameters.size()) : (definition_.function->parameters.empty() ? 0 : 1);
	unsigned slot = parameter_count + local_count_++;
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
			visit(binary->left);
			visit(binary->right);
		}
		else if (auto cast = dynamic_cast<Cast*>(value))
			visit(cast->value);
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
	for (const auto& parameter : lambda->parameters)
		parameters.push_back(module_.value_type(parameter.type_expr));
	const std::string result = module_.value_type(lambda->return_type, true);
	const auto captures = lambda_captures(lambda);
	auto indirect_parameters = parameters;
	indirect_parameters.insert(indirect_parameters.begin(), "s32");
	const unsigned type = module_.wasm_type(indirect_parameters, result);
	const std::string value_type = "function#" + std::to_string(type);
	module_.lambda_functions_.emplace_back(lambda->location, "__lambda_" + std::to_string(module_.lambdas_.size()));
	Function& function = module_.lambda_functions_.back();
	function.parameters = lambda->parameters;
	function.return_type = lambda->return_type;
	function.body = lambda->body;
	Definition definition;
	definition.function = &function;
	definition.parameters = std::move(indirect_parameters);
	definition.result = result;
	definition.index = module_.first_user_index_ + static_cast<unsigned>(module_.definitions_.size());
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

std::string FunctionLowerer::infer(Expr* value)
{
	if (dynamic_cast<Integer*>(value))
		return "s32";
	if (dynamic_cast<String*>(value))
		return "string";
	if (auto lambda = dynamic_cast<Lambda*>(value))
		return std::get<0>(register_lambda(lambda));
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
			type += (i ? "," : "") + infer(tuple->items[i]);
		return type + ">";
	}
	if (dynamic_cast<MapLiteral*>(value))
		throw Error(value->location, "map literals must be wrapped in dval(...)");
	if (auto array = dynamic_cast<ArrayLiteral*>(value))
	{
		if (array->items.empty())
			throw Error(value->location, "empty array literal needs an explicit element type");
		const std::string element = infer(array->items.front());
		for (Expr* item : array->items)
			if (infer(item) != element)
				throw Error(item->location, "array literal elements must have one type");
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
		auto name = dynamic_cast<Name*>(call->function);
		if (!name)
		{
			const std::string function = infer(call->function);
			if (function.rfind("function#", 0) != 0)
				throw Error(call->function->location, "call target is not a function value");
			const auto& signature = module_.types_.at(static_cast<unsigned>(std::stoul(function.substr(9))));
			if (signature.first.size() != call->arguments.size() + 1)
				throw Error(call->location, "function value argument count does not match signature");
			for (std::size_t i = 0; i < call->arguments.size(); ++i)
				if (infer(call->arguments[i]) != signature.first[i + 1])
					throw Error(call->arguments[i]->location, "function value argument type does not match signature");
			return signature.second;
		}
		for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
			if (auto found = scope->find(name->value); found != scope->end() && found->second.second.rfind("function#", 0) == 0)
			{
				const unsigned type = static_cast<unsigned>(std::stoul(found->second.second.substr(9)));
				if (type >= module_.types_.size())
					throw Error(call->location, "invalid function value type");
				const auto& signature = module_.types_[type];
				if (signature.first.size() != call->arguments.size() + 1)
					throw Error(call->location, "function value argument count does not match signature");
				for (std::size_t i = 0; i < call->arguments.size(); ++i)
					if (infer(call->arguments[i]) != signature.first[i + 1])
						throw Error(call->arguments[i]->location, "function value argument type does not match signature");
				return signature.second;
			}
		if (module_.has_struct(name->value))
			return "struct:" + name->value;
		if (name->value == "clone")
			return "string";
		if (name->value == "trusted_markup")
			return "markup";
		if (name->value == "dval")
		{
			if (call->arguments.size() != 1)
				throw Error(call->location, "dval expects one scalar, map, or list");
			if (dynamic_cast<MapLiteral*>(call->arguments[0]) || dynamic_cast<ArrayLiteral*>(call->arguments[0]))
				return "dval";
			const std::string argument = infer(call->arguments[0]);
			if (argument != "string" && argument != "s32" && argument != "bool" && argument != "dval")
				throw Error(call->arguments[0]->location, "cannot construct dval from " + argument);
			return "dval";
		}
		if (name->value == "dval_has")
			return "bool";
		if (name->value == "dval_string")
			return "string";
		if (name->value == "dval_s32")
			return "s32";
		if (name->value == "dval_bool")
			return "bool";
		if (name->value == "unit_call")
			return "dval";
		if (name->value == "unit_render" || name->value == "component_render")
			return "void";
		if (name->value == "arc_live")
			return "s32";
		if (name->value == "print" || name->value == "trap")
			return "void";
		std::vector<std::string> arguments;
		for (Expr* argument : call->arguments)
			arguments.push_back(infer(argument));
		return module_.resolve(name->value, arguments, call->location).result;
	}
	if (auto binary = dynamic_cast<Binary*>(value))
	{
		if (binary->operator_ == "..")
			return "range";
		if (binary->operator_ == "=")
			return infer(binary->right);
		return binary->operator_ == "==" || binary->operator_ == "!=" || binary->operator_ == "<" || binary->operator_ == ">" || binary->operator_ == "<=" ||
					   binary->operator_ == ">="
				   ? "bool"
				   : "s32";
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
		code.insert(code.end(), {0x41, 0x04, 0x10});
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
		wasm::append_uleb(code, module_.import_index(result == "s32" ? "bearer_dv_s32_brrb" : "bearer_dv_bool_brrb"));
		code.insert(code.end(), {0x45, 0x04, 0x40});
		append(code, module_.marker(call->location));
		code.insert(code.end(), {0x00, 0x0b, 0x20});
		wasm::append_uleb(code, output);
		code.insert(code.end(), {0x28, 0x02, 0x00, 0x21});
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
		if (actual != "string" && actual != "s32" && actual != "bool")
			throw Error(value->location, "cannot construct dval from " + actual);
		const unsigned input = add_local("", actual, value->location), length = add_local("", "s32", value->location);
		const char* import = actual == "string" ? "bearer_dv_string_to_brrb" : actual == "s32" ? "bearer_dv_s32_to_brrb" : "bearer_dv_bool_to_brrb";
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
		code.insert(code.end(), {0x47, 0x04, 0x40, 0x00, 0x0b, 0x20});
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

std::pair<Bytes, std::string> FunctionLowerer::expression(Expr* value)
{
	module_.check_cancelled();
	if (auto integer = dynamic_cast<Integer*>(value))
	{
		Bytes code{0x41};
		wasm::append_sleb32(code, static_cast<std::int32_t>(integer->value));
		return {code, "s32"};
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
		module_.need_alloc();
		const unsigned pointer = add_local("", type, value->location), size = 20 + 4 * static_cast<unsigned>(captures.size());
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
			code.insert(code.end(), {0x36, 0x02});
			wasm::append_uleb(code, static_cast<unsigned>(20 + 4 * i));
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
			fields.push_back(infer(item));
		std::string type = "tuple<";
		for (std::size_t i = 0; i < fields.size(); ++i)
			type += (i ? "," : "") + fields[i];
		type += ">";
		const unsigned pointer = add_local("", type, value->location);
		module_.need_alloc();
		Bytes code{0x41};
		wasm::append_sleb32(code, static_cast<std::int32_t>(16 + 4 * fields.size()));
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
											{static_cast<std::int32_t>(16 + 4 * fields.size()), 12}})
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
			code.insert(code.end(), {0x36, 0x02});
			wasm::append_uleb(code, static_cast<unsigned>(16 + 4 * i));
		}
		code.push_back(0x20);
		wasm::append_uleb(code, pointer);
		return {code, type};
	}
	if (auto array = dynamic_cast<ArrayLiteral*>(value))
	{
		if (array->items.empty())
			throw Error(value->location, "empty array literal needs an explicit element type");
		std::vector<std::pair<Bytes, bool>> items;
		std::string element_type;
		for (Expr* item : array->items)
		{
			auto compiled = expression(item);
			if (element_type.empty())
				element_type = compiled.second;
			if (compiled.second != element_type)
				throw Error(item->location, "array literal elements must have one type");
			if (!is_scalar(element_type) && !managed_type(element_type))
				throw Error(item->location, "array element type is unsupported");
			items.push_back({std::move(compiled.first), expression_is_owned(item)});
		}
		module_.need_alloc();
		const unsigned pointer = add_local("", "array<" + element_type + ">", value->location);
		Bytes code{0x41};
		wasm::append_sleb32(code, static_cast<std::int32_t>(20 + 4 * items.size()));
		code.push_back(0x10);
		wasm::append_uleb(code, module_.import_index("bearer_alloc"));
		code.push_back(0x21);
		wasm::append_uleb(code, pointer);
		code.push_back(0x20);
		wasm::append_uleb(code, pointer);
		code.insert(code.end(), {0x45, 0x04, 0x40, 0x00, 0x0b});
		// ARC header: strong, weak, type-id, allocation bytes, element count.
		const std::int32_t type_id = managed_type(element_type) ? 3 : 2;
		for (const auto [header_value, offset] : {std::pair<std::int32_t, unsigned>{1, 0},
												  {1, 4},
												  {type_id, 8},
												  {static_cast<std::int32_t>(20 + 4 * items.size()), 12},
												  {static_cast<std::int32_t>(items.size()), 16}})
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
			code.insert(code.end(), {0x36, 0x02});
			wasm::append_uleb(code, static_cast<unsigned>(20 + 4 * i));
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
			const unsigned object = add_local("", array_type, index->value->location);
			Bytes code = std::move(array_code);
			code.push_back(0x21);
			wasm::append_uleb(code, object);
			code.push_back(0x20);
			wasm::append_uleb(code, object);
			code.insert(code.end(), {0x28, 0x02});
			wasm::append_uleb(code, static_cast<unsigned>(16 + 4 * item->value));
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
		code.push_back(0x20);
		wasm::append_uleb(code, array_local);
		code.push_back(0x20);
		wasm::append_uleb(code, index_local);
		code.insert(code.end(), {0x41, 0x04, 0x6c, 0x6a, 0x28, 0x02, 0x14});
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
		Bytes code = std::move(object_code);
		code.push_back(0x21);
		wasm::append_uleb(code, object);
		code.push_back(0x20);
		wasm::append_uleb(code, object);
		code.insert(code.end(), {0x28, 0x02});
		wasm::append_uleb(code, 16 + 4 * field_index);
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
			if (type != "string" && type != "markup" && type != "s32" && type != "bool")
				throw Error(field->location, "markup interpolation does not support " + type);
			if (!field->escaped && type != "markup")
				throw Error(field->location, "raw markup interpolation requires a markup value");
			const unsigned local = add_local("", type, field->location);
			append(code, field_code);
			code.push_back(0x21);
			wasm::append_uleb(code, local);
			compiled.push_back({field, local, std::move(type), expression_is_owned(field->value)});
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
		auto [code, type] = expression(variable->value);
		std::string declared = variable->annotation ? module_.value_type(variable->annotation) : type;
		if (declared != type)
			throw Error(value->location, "expected " + declared + ", found " + type);
		unsigned slot = add_local(variable->name, declared, value->location);
		code.push_back(0x21);
		wasm::append_uleb(code, slot);
		if (managed_type(declared))
		{
			if (!expression_is_owned(variable->value))
			{
				code.push_back(0x20);
				wasm::append_uleb(code, slot);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.retain_index());
			}
			owned_scopes_.back().push_back({slot, declared});
		}
		return {code, "void"};
	}
	if (auto binary = dynamic_cast<Binary*>(value))
	{
		if (binary->operator_ == "=")
		{
			auto target = dynamic_cast<Name*>(binary->left);
			if (!target)
				throw Error(binary->left->location, "assignment target must be a local name");
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
		auto [left, left_type] = expression(binary->left);
		auto [right, right_type] = expression(binary->right);
		if (left_type != right_type)
			throw Error(value->location, "expected " + left_type + ", found " + right_type);
		static const std::map<std::string, std::uint8_t> ops = {{"+", 0x6a},  {"-", 0x6b}, {"*", 0x6c}, {"/", 0x6d},  {"%", 0x6f}, {"==", 0x46},
																{"!=", 0x47}, {"<", 0x48}, {">", 0x4a}, {"<=", 0x4c}, {">=", 0x4e}};
		auto found = ops.find(binary->operator_);
		if (found == ops.end() || !is_scalar(left_type))
			throw Error(value->location, "unsupported operator " + binary->operator_);
		left.insert(left.end(), right.begin(), right.end());
		left.push_back(found->second);
		return {left, binary->operator_ == "+" || binary->operator_ == "-" || binary->operator_ == "*" || binary->operator_ == "/" || binary->operator_ == "%"
						  ? "s32"
						  : "bool"};
	}
	if (auto cast = dynamic_cast<Cast*>(value))
	{
		auto [code, source] = expression(cast->value);
		std::string target = type_of_expression(cast->target_type);
		if (!is_scalar(source) || !is_scalar(target))
			throw Error(value->location, "no explicit conversion from " + source + " to " + target);
		if (target == "bool" && source != "bool")
			code.insert(code.end(), {0x45, 0x45});
		return {code, target};
	}
	if (auto call = dynamic_cast<Call*>(value))
	{
		auto named = dynamic_cast<Name*>(call->function);
		if (!named)
		{
			auto [function_code, function_type] = expression(call->function);
			if (function_type.rfind("function#", 0) != 0)
				throw Error(call->function->location, "call target is not a function value");
			const unsigned type = static_cast<unsigned>(std::stoul(function_type.substr(9)));
			const auto& signature = module_.types_.at(type);
			if (signature.first.size() != call->arguments.size() + 1)
				throw Error(call->location, "function value argument count does not match signature");
			const unsigned closure = add_local("", function_type, call->function->location);
			Bytes code = std::move(function_code);
			code.push_back(0x21);
			wasm::append_uleb(code, closure);
			code.push_back(0x20);
			wasm::append_uleb(code, closure);
			std::vector<unsigned> owned_arguments;
			for (std::size_t i = 0; i < call->arguments.size(); ++i)
			{
				auto [argument, actual] = expression(call->arguments[i]);
				if (actual != signature.first[i + 1])
					throw Error(call->arguments[i]->location, "function value argument type does not match signature");
				if (managed_type(actual) && expression_is_owned(call->arguments[i]))
				{
					const unsigned temporary = add_local("", actual, call->arguments[i]->location);
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
		for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
			if (auto found = scope->find(named->value); found != scope->end() && found->second.second.rfind("function#", 0) == 0)
			{
				const unsigned type = static_cast<unsigned>(std::stoul(found->second.second.substr(9)));
				if (type >= module_.types_.size())
					throw Error(value->location, "invalid function value type");
				const auto& signature = module_.types_[type];
				if (signature.first.size() != call->arguments.size() + 1)
					throw Error(value->location, "function value argument count does not match signature");
				Bytes code{0x20};
				wasm::append_uleb(code, found->second.first);
				std::vector<unsigned> owned_arguments;
				for (std::size_t i = 0; i < call->arguments.size(); ++i)
				{
					auto [argument, actual] = expression(call->arguments[i]);
					if (actual != signature.first[i + 1])
						throw Error(call->arguments[i]->location, "function value argument type does not match signature");
					if (managed_type(actual) && expression_is_owned(call->arguments[i]))
					{
						const unsigned temporary = add_local("", actual, call->arguments[i]->location);
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
		if (module_.has_struct(named->value))
		{
			const auto& aggregate = module_.struct_type(named->value, named->location);
			if (call->arguments.size() != aggregate.fields.size())
				throw Error(value->location, "struct " + named->value + " constructor field count does not match declaration");
			const std::string type = "struct:" + named->value;
			const unsigned pointer = add_local("", type, value->location);
			module_.need_alloc();
			Bytes code{0x41};
			wasm::append_sleb32(code, static_cast<std::int32_t>(16 + 4 * aggregate.fields.size()));
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
												{static_cast<std::int32_t>(16 + 4 * aggregate.fields.size()), 12}})
			{
				code.push_back(0x20);
				wasm::append_uleb(code, pointer);
				code.push_back(0x41);
				wasm::append_sleb32(code, header);
				code.insert(code.end(), {0x36, 0x02});
				wasm::append_uleb(code, offset);
			}
			code.insert(code.end(), {0x23, 0x01, 0x41, 0x01, 0x6a, 0x24, 0x01});
			for (std::size_t i = 0; i < call->arguments.size(); ++i)
			{
				auto [field, actual] = expression(call->arguments[i]);
				if (actual != aggregate.fields[i].second)
					throw Error(call->arguments[i]->location, "expected " + aggregate.fields[i].second + ", found " + actual);
				const unsigned temporary = add_local("", actual, call->arguments[i]->location);
				append(code, field);
				code.push_back(0x21);
				wasm::append_uleb(code, temporary);
				if (managed_type(actual) && !expression_is_owned(call->arguments[i]))
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
				code.insert(code.end(), {0x36, 0x02});
				wasm::append_uleb(code, static_cast<unsigned>(16 + 4 * i));
			}
			code.push_back(0x20);
			wasm::append_uleb(code, pointer);
			return {code, type};
		}
		if (named->value == "dval")
		{
			if (call->arguments.size() != 1)
				throw Error(value->location, "dval expects one scalar, map, or list");
			return dval_value(call->arguments[0]);
		}
		if (named->value == "dval_has")
		{
			if (call->arguments.size() != 2)
				throw Error(value->location, "dval_has expects dval and string/s32 key");
			return dval_lookup(call->arguments[0], call->arguments[1], false);
		}
		if (named->value == "dval_string")
			return dval_scalar(call, "string");
		if (named->value == "dval_s32")
			return dval_scalar(call, "s32");
		if (named->value == "dval_bool")
			return dval_scalar(call, "bool");
		if (named->value == "trusted_markup")
		{
			if (call->arguments.size() != 1)
				throw Error(value->location, "trusted_markup expects one string");
			auto [source, type] = expression(call->arguments[0]);
			if (type != "string")
				throw Error(call->arguments[0]->location, "expected string, found " + type);
			return {std::move(source), "markup"};
		}
		if (named->value == "unit_call")
		{
			if (call->arguments.size() != 3)
				throw Error(value->location, "unit_call expects target, function, and dval");
			std::vector<unsigned> locals;
			Bytes code;
			for (unsigned index = 0; index != 3; ++index)
			{
				auto [part, type] = expression(call->arguments[index]);
				const std::string expected = index < 2 ? "string" : "dval";
				if (type != expected)
					throw Error(call->arguments[index]->location, "expected " + expected + ", found " + type);
				const unsigned local = add_local("", expected, call->arguments[index]->location);
				append(code, part);
				code.push_back(0x21);
				wasm::append_uleb(code, local);
				locals.push_back(local);
			}
			auto inputs = [&]
			{
				for (unsigned local : locals)
				{
					code.push_back(0x20);
					wasm::append_uleb(code, local);
					code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
					wasm::append_uleb(code, local);
					code.insert(code.end(), {0x28, 0x02, 0x10});
				}
			};
			inputs();
			code.insert(code.end(), {0x41, 0x00, 0x41, 0x00, 0x10});
			wasm::append_uleb(code, module_.import_index("bearer_unit_call_brrb"));
			const unsigned length = add_local("", "s32", value->location);
			code.push_back(0x21);
			wasm::append_uleb(code, length);
			auto [allocation, pointer] = allocate_blob("dval", 4, length, value->location);
			append(code, allocation);
			inputs();
			code.push_back(0x20);
			wasm::append_uleb(code, pointer);
			code.insert(code.end(), {0x41, 0x14, 0x6a, 0x20});
			wasm::append_uleb(code, length);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.import_index("bearer_unit_call_brrb"));
			code.push_back(0x20);
			wasm::append_uleb(code, length);
			code.insert(code.end(), {0x47, 0x04, 0x40, 0x00, 0x0b});
			for (int i = 2; i >= 0; --i)
				if (expression_is_owned(call->arguments[i]))
				{
					code.push_back(0x20);
					wasm::append_uleb(code, locals[i]);
					code.push_back(0x10);
					wasm::append_uleb(code, module_.release_index());
				}
			code.push_back(0x20);
			wasm::append_uleb(code, pointer);
			return {code, "dval"};
		}
		if (named->value == "unit_render" || named->value == "component_render")
		{
			if (call->arguments.size() != 1)
				throw Error(value->location, named->value + " expects one string target");
			auto [target, type] = expression(call->arguments[0]);
			if (type != "string")
				throw Error(call->arguments[0]->location, "expected string, found " + type);
			const unsigned temporary = add_local("", "string", call->arguments[0]->location);
			Bytes code = std::move(target);
			code.push_back(0x21);
			wasm::append_uleb(code, temporary);
			code.push_back(0x20);
			wasm::append_uleb(code, temporary);
			code.insert(code.end(), {0x41, 0x14, 0x6a});
			code.push_back(0x20);
			wasm::append_uleb(code, temporary);
			code.insert(code.end(), {0x28, 0x02, 0x10});
			const char* import = named->value == "unit_render" ? "bearer_unit_render_bytes" : "bearer_component_render_bytes";
			code.push_back(0x10);
			wasm::append_uleb(code, module_.import_index(import));
			if (expression_is_owned(call->arguments[0]))
			{
				code.push_back(0x20);
				wasm::append_uleb(code, temporary);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.release_index());
			}
			return {code, "void"};
		}
		if (named->value == "clone")
		{
			if (call->arguments.size() != 1)
				throw Error(value->location, "clone expects one string");
			auto [source, type] = expression(call->arguments[0]);
			if (type != "string")
				throw Error(call->arguments[0]->location, "expected string, found " + type);
			Bytes code = std::move(source);
			const unsigned input = add_local("", "string", value->location);
			code.push_back(0x21);
			wasm::append_uleb(code, input);
			code.push_back(0x20);
			wasm::append_uleb(code, input);
			code.push_back(0x10);
			wasm::append_uleb(code, module_.clone_index());
			if (expression_is_owned(call->arguments[0]))
			{
				code.push_back(0x20);
				wasm::append_uleb(code, input);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.release_index());
			}
			return {code, "string"};
		}
		if (named->value == "arc_live")
		{
			if (!call->arguments.empty())
				throw Error(value->location, "arc_live expects no arguments");
			return {{0x23, 0x01}, "s32"};
		}
		if (named->value == "trap")
		{
			if (!call->arguments.empty())
				throw Error(value->location, "trap expects no arguments");
			Bytes code = cleanup_scopes();
			code.push_back(0x00);
			return {code, "void"};
		}
		if (named->value == "print")
		{
			Bytes code;
			for (Expr* argument : call->arguments)
			{
				if (auto text = dynamic_cast<String*>(argument))
				{
					module_.need_print_bytes();
					unsigned offset = module_.add_data(text->value);
					code.insert(code.end(), {0x23, 0x00, 0x41});
					wasm::append_sleb32(code, static_cast<std::int32_t>(offset));
					code.push_back(0x6a);
					code.push_back(0x41);
					wasm::append_sleb32(code, static_cast<std::int32_t>(text->value.size()));
					code.push_back(0x10);
					wasm::append_uleb(code, module_.import_index("bearer_print_bytes"));
				}
				else
				{
					auto [part, type] = expression(argument);
					if (type == "string" || type == "markup")
					{
						module_.need_print_bytes();
						const unsigned temporary = add_local("", type, argument->location);
						append(code, part);
						code.push_back(0x21);
						wasm::append_uleb(code, temporary);
						code.push_back(0x20);
						wasm::append_uleb(code, temporary);
						code.insert(code.end(), {0x41, 0x14, 0x6a});
						code.push_back(0x20);
						wasm::append_uleb(code, temporary);
						code.insert(code.end(), {0x28, 0x02, 0x10});
						code.push_back(0x10);
						wasm::append_uleb(code, module_.import_index("bearer_print_bytes"));
						if (expression_is_owned(argument))
						{
							code.push_back(0x20);
							wasm::append_uleb(code, temporary);
							code.push_back(0x10);
							wasm::append_uleb(code, module_.release_index());
						}
					}
					else
					{
						if (!is_scalar(type))
							throw Error(argument->location, "print supports scalar values and strings");
						module_.need_print_s32();
						append(code, part);
						code.push_back(0x10);
						wasm::append_uleb(code, module_.import_index("bearer_print_s32"));
					}
				}
			}
			return {code, "void"};
		}
		std::vector<Bytes> arguments;
		std::vector<std::string> types;
		for (Expr* argument : call->arguments)
		{
			auto item = expression(argument);
			types.push_back(item.second);
			arguments.push_back(std::move(item.first));
		}
		Definition& target = module_.resolve(named->value, types, value->location);
		Bytes code;
		std::vector<unsigned> owned_arguments;
		for (std::size_t i = 0; i < arguments.size(); ++i)
		{
			if (managed_type(types[i]) && expression_is_owned(call->arguments[i]))
			{
				const unsigned temporary = add_local("", types[i], call->arguments[i]->location);
				append(code, arguments[i]);
				code.push_back(0x21);
				wasm::append_uleb(code, temporary);
				code.push_back(0x20);
				wasm::append_uleb(code, temporary);
				owned_arguments.push_back(temporary);
			}
			else
				append(code, arguments[i]);
		}
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
		code.push_back(0x0f);
		return {code, "void"};
	}
	if (auto conditional = dynamic_cast<If*>(value))
	{
		auto [condition, type] = expression(conditional->condition);
		if (!is_scalar(type))
			throw Error(conditional->condition->location, "if condition must be scalar");
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
	if (auto loop = dynamic_cast<While*>(value))
	{
		auto [condition, type] = expression(loop->condition);
		if (!is_scalar(type))
			throw Error(loop->condition->location, "while condition must be scalar");
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
			Bytes release;
			release.push_back(0x20);
			wasm::append_uleb(release, item);
			release.push_back(0x10);
			wasm::append_uleb(release, module_.release_index());
			if (key != 0xffffffffu)
			{
				release.push_back(0x20);
				wasm::append_uleb(release, key);
				release.push_back(0x10);
				wasm::append_uleb(release, module_.release_index());
			}
			Bytes increment{0x20};
			wasm::append_uleb(increment, index);
			increment.insert(increment.end(), {0x41, 0x01, 0x6a, 0x21});
			wasm::append_uleb(increment, index);
			const unsigned base = control_depth_, boundary = static_cast<unsigned>(owned_scopes_.size());
			control_depth_ += 2;
			loops_.push_back({base + 1, base + 2, boundary, release, Bytes(release)});
			append(loops_.back().continue_edge, increment);
			Bytes body = block(loop->body);
			loops_.pop_back();
			control_depth_ -= 2;
			scopes_.pop_back();
			append(code, body);
			append(code, release);
			append(code, increment);
			code.insert(code.end(), {0x0c, 0x00, 0x0b, 0x0b});
			if (expression_is_owned(loop->iterable))
			{
				code.push_back(0x20);
				wasm::append_uleb(code, iterable);
				code.push_back(0x10);
				wasm::append_uleb(code, module_.release_index());
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
		control_depth_ += 3;
		loops_.push_back({base + 1, base + 3, boundary, {}, {}});
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
		code.insert(code.end(), {0x41, 0x04, 0x6c, 0x6a, 0x28, 0x02, 0x14, 0x21});
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
		return {block(nested), "void"};
	throw Error(value->location, "native Capy backend does not yet lower expression");
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
		auto [part, type] = expression(item);
		append(code, part);
		const bool implicit_result = !new_scope && i + 1 == block_value->items.size() && type == definition_.result && type != "void";
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
	if (definition_.closure_body)
		for (std::size_t i = 0; i < definition_.captures.size(); ++i)
		{
			Name name(definition_.function->location, definition_.captures[i].first);
			auto [slot, type] = lookup(&name);
			code.push_back(0x20);
			wasm::append_uleb(code, 0);
			code.insert(code.end(), {0x28, 0x02});
			wasm::append_uleb(code, static_cast<unsigned>(20 + 4 * i));
			code.push_back(0x21);
			wasm::append_uleb(code, slot);
		}
	append(code, block(definition_.function->body, false));
	if (definition_.result != "void" && !implicit_result_ && !expression_always_returns(definition_.function->body))
		throw Error(definition_.function->location, "not all paths produce " + definition_.result);
	if (definition_.result == "void")
		append(code, cleanup_scopes());
	code.push_back(0x0b);
	Bytes locals;
	if (local_count_)
	{
		wasm::append_uleb(locals, 1);
		wasm::append_uleb(locals, local_count_);
		locals.push_back(0x7f);
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
					 0x40, 0x03, 0x40, 0x20, 0x01, 0x20, 0x02, 0x4f, 0x0d, 0x01, 0x20, 0x00, 0x20, 0x01, 0x41, 0x04, 0x6c, 0x6a, 0x28, 0x02, 0x14, 0x10});
		wasm::append_uleb(code, release_index());
		code.insert(code.end(), {0x20, 0x01, 0x41, 0x01, 0x6a, 0x21, 0x01, 0x0c, 0x00, 0x0b, 0x0b, 0x0b});
		for (const auto& [type_id, captures] : closure_types_)
		{
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
				code.insert(code.end(), {0x20, 0x00, 0x28, 0x02});
				wasm::append_uleb(code, 20 + 4 * i);
				code.push_back(0x10);
				wasm::append_uleb(code, release_index());
			}
			code.push_back(0x0b);
		}
		for (const auto& [name, aggregate] : structs_)
		{
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
				code.insert(code.end(), {0x20, 0x00, 0x28, 0x02});
				wasm::append_uleb(code, 16 + 4 * i);
				code.push_back(0x10);
				wasm::append_uleb(code, release_index());
			}
			code.push_back(0x0b);
		}
		for (const auto& [type, id] : tuples_)
		{
			const auto fields = aggregate_elements(type);
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
				code.insert(code.end(), {0x20, 0x00, 0x28, 0x02});
				wasm::append_uleb(code, 16 + 4 * i);
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
	// Reserve nominal IDs first so member types can refer to declarations in either order.
	for (Expr* item : program_.items)
	{
		check_cancelled();
		if (auto structure = dynamic_cast<Struct*>(item))
		{
			if (structs_.contains(structure->name))
				throw Error(structure->location, "struct '" + structure->name + "' is already declared");
			structs_[structure->name] = {next_aggregate_type_++, {}};
		}
	}
	for (Expr* item : program_.items)
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
				if (!names.insert(name->value).second)
					throw Error(member->location, "struct member '" + name->value + "' is already declared");
				const std::string type = value_type(annotation->type_expr);
				if (type.rfind("struct:", 0) == 0 && !structs_.contains(type.substr(7)))
					throw Error(annotation->location, "unknown struct type '" + type.substr(7) + "'");
				aggregate.fields.push_back({name->value, type});
			}
		}
	}
	std::set<std::string> handlers;
	for (Expr* item : program_.items)
	{
		check_cancelled();
		auto function = dynamic_cast<Function*>(item);
		if (!function)
		{
			if (dynamic_cast<Struct*>(item))
				continue;
			throw Error(item->location, "top-level executable expressions are not implemented by the native backend");
		}
		if (has_struct(function->name))
			throw Error(function->location, "function name conflicts with struct constructor");
		std::string exported;
		bool handler = is_handler(function->name, exported);
		std::vector<std::string> parameters;
		bool generic = false;
		for (const auto& parameter : function->parameters)
		{
			const bool any = dynamic_cast<Name*>(parameter.type_expr) && type_name(*parameter.type_expr) == "any";
			generic = generic || any;
			parameters.push_back(any ? "any" : value_type(parameter.type_expr));
			if (parameters.back() == "void")
				throw Error(parameter.type_expr->location, "function parameters cannot have type void");
		}
		if (handler && generic)
			throw Error(function->location, "Bearer handlers cannot use any parameters");
		if (handler && (function->parameters.size() > 1 || (!parameters.empty() && parameters != std::vector<std::string>{"request"})))
			throw Error(function->location, "Bearer handler accepts zero parameters or one request parameter");
		if (handler && !handlers.insert(exported).second)
			throw Error(function->location, "Bearer handler is already declared; handlers cannot be overloaded");
		if (generic)
		{
			int dependent = -1;
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
				else if (type_of_expression(function->return_type, true) != "void")
				{
					throw Error(function->location, "generic results must be void or use x::type");
				}
			}
			generics_[function->name].push_back({function, parameters, dependent});
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
			std::string k = key(function->name, parameters);
			if (definitions_by_key_.contains(k))
				throw Error(function->location, "return type does not distinguish overloads");
			Definition definition;
			definition.function = function;
			definition.parameters = parameters;
			definition.result = result;
			definition.exported = exported;
			definitions_by_key_[k] = definitions_.size();
			definitions_.push_back(std::move(definition));
			if (function->name.rfind("EXPORT_", 0) == 0)
			{
				const std::string name = function->name.substr(7);
				if (name.empty())
					throw Error(function->location, "custom DValue export requires a name after EXPORT_");
				if (parameters != std::vector<std::string>{"dval"} || result != "dval")
					throw Error(function->location, "custom DValue export must have signature (dval) dval");
				for (const auto& existing : custom_exports_)
					if (existing.first == name)
						throw Error(function->location, "custom DValue export '" + name + "' is already declared");
				custom_exports_.push_back({name, &definitions_.back()});
			}
		}
	}
	bool any_export = std::any_of(definitions_.begin(), definitions_.end(), [](const Definition& d) { return !d.exported.empty(); });
	if (!any_export)
		throw Error({source_, 1, 1, 0}, "Capy Bearer unit exports no CLI, RENDER, WS, ONCE, or INIT handler");
}

CompileResult Module::compile()
{
	collect();
	check_cancelled();
	// Imports are deliberately discovered before assigning indices.  The direct ABI
	// always imports memory and __memory_base; functions remain demand driven.
	bool scan_print_bytes = false, scan_print_s32 = false, scan_alloc = false;
	bool scan_retain = false, scan_release = false, scan_clone = false, scan_arc_live = false;
	std::function<void(Expr*)> scan = [&](Expr* e)
	{
		check_cancelled();
		if (auto c = dynamic_cast<Call*>(e))
		{
			if (auto n = dynamic_cast<Name*>(c->function); n && (n->value == "clone" || has_struct(n->value)))
			{
				scan_alloc = true;
				scan_release = true;
				if (n->value == "clone")
					scan_clone = true;
			}
			if (auto n = dynamic_cast<Name*>(c->function);
				n && (n->value == "dval" || n->value == "dval_has" || n->value == "dval_string" || n->value == "dval_s32" || n->value == "dval_bool"))
			{
				dval_ = true;
				scan_alloc = true;
				scan_retain = true;
				scan_release = true;
			}
			if (auto n = dynamic_cast<Name*>(c->function); n && n->value == "unit_call")
			{
				unit_call_ = true;
				dval_ = true;
				scan_alloc = true;
				scan_retain = true;
				scan_release = true;
			}
			if (auto n = dynamic_cast<Name*>(c->function); n && n->value == "unit_render")
				unit_render_ = true;
			if (auto n = dynamic_cast<Name*>(c->function); n && n->value == "component_render")
				component_render_ = true;
			if (auto n = dynamic_cast<Name*>(c->function); n && n->value == "arc_live")
				scan_arc_live = true;
			if (auto n = dynamic_cast<Name*>(c->function); n && n->value == "print")
				for (auto a : c->arguments)
				{
					if (dynamic_cast<String*>(a) || dynamic_cast<Markup*>(a))
						scan_print_bytes = true;
					else if (auto called = dynamic_cast<Call*>(a))
					{
						auto function = dynamic_cast<Name*>(called->function);
						bool string_result = function && function->value == "dval_string";
						if (function)
							for (const auto& d : definitions_)
								if (d.function->name == function->value && d.result == "string")
									string_result = true;
						if (string_result)
							scan_print_bytes = true;
						else
							scan_print_s32 = true;
					}
					else
					{
						scan_print_bytes = true;
						scan_print_s32 = true;
					}
				}
			for (auto a : c->arguments)
				scan(a);
		}
		else if (auto b = dynamic_cast<Block*>(e))
			for (auto x : b->items)
				scan(x);
		else if (auto f = dynamic_cast<Function*>(e))
			scan(f->body);
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
			if (v->annotation && managed_type(type_of_expression(v->annotation)))
			{
				scan_retain = true;
				scan_release = true;
			}
		}
		else if (auto b = dynamic_cast<Binary*>(e))
		{
			scan(b->left);
			scan(b->right);
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
		else if (auto i = dynamic_cast<Index*>(e))
		{
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
					scan(field->value);
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
			scan(f->iterable);
			scan(f->body);
		}
	};
	for (auto& d : definitions_)
	{
		scan(d.function->body);
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
			scan(generic.function->body);
	if (!custom_exports_.empty())
	{
		dval_ = true;
		scan_alloc = true;
		scan_retain = true;
		scan_release = true;
	}
	use_retain_ = scan_retain;
	use_release_ = scan_release;
	use_clone_ = scan_clone;
	use_arc_global_ = scan_arc_live || scan_alloc || scan_release || scan_clone;
	unsigned next = 0;
	if (scan_print_bytes)
		imports_["bearer_print_bytes"] = next++;
	if (scan_print_s32)
		imports_["bearer_print_s32"] = next++;
	if (scan_alloc || scan_clone)
		imports_["bearer_alloc"] = next++;
	if (scan_release)
		imports_["bearer_free"] = next++;
	if (unit_render_)
		imports_["bearer_unit_render_bytes"] = next++;
	if (component_render_)
		imports_["bearer_component_render_bytes"] = next++;
	if (dval_)
		for (const char* name : {"bearer_dv_string_to_brrb", "bearer_dv_s32_to_brrb", "bearer_dv_bool_to_brrb", "bearer_dv_build_brrb", "bearer_dv_get_brrb",
								 "bearer_dv_count_brrb", "bearer_dv_entry_key_brrb", "bearer_dv_entry_value_brrb", "bearer_dv_scalar_type_brrb",
								 "bearer_dv_s32_brrb", "bearer_dv_bool_brrb", "bearer_dv_brrb_to_string"})
			imports_[name] = next++;
	if (!custom_exports_.empty())
	{
		imports_["bearer_dv_ptr_to_brrb"] = next++;
		imports_["bearer_dv_brrb_to_ptr"] = next++;
	}
	if (unit_call_)
		imports_["bearer_unit_call_brrb"] = next++;
	if (use_retain_)
		helpers_["retain"] = next++;
	if (use_release_)
		helpers_["release"] = next++;
	if (use_clone_)
		helpers_["clone"] = next++;
	first_user_index_ = next;
	for (auto& d : definitions_)
	{
		d.index = next++;
		d.type = wasm_type(d.exported.empty() ? d.parameters : std::vector<std::string>{"request"}, d.result);
	}
	// Ensure import signatures precede user types and lower after indexes are stable.
	unsigned bytes_type = scan_print_bytes ? wasm_type({"s32", "s32"}, "void") : 0;
	unsigned scalar_type = scan_print_s32 ? wasm_type({"s32"}, "void") : 0;
	unsigned alloc_type = (scan_alloc || scan_clone) ? wasm_type({"s32"}, "s32") : 0;
	unsigned release_type = scan_release ? wasm_type({"s32"}, "void") : 0;
	unsigned clone_type = scan_clone ? wasm_type({"s32"}, "s32") : 0;
	unsigned blob_type = dval_ ? wasm_type({"s32", "s32", "s32", "s32"}, "s32") : 0;
	unsigned scalar_adapter_type = dval_ ? wasm_type({"s32", "s32", "s32"}, "s32") : 0;
	unsigned build_type = dval_ ? wasm_type({"s32", "s32", "s32", "s32", "s32"}, "s32") : 0;
	unsigned get_type = dval_ ? wasm_type({"s32", "s32", "s32", "s32", "s32", "s32", "s32", "s32"}, "s32") : 0;
	unsigned entry_type = dval_ ? wasm_type({"s32", "s32", "s32", "s32", "s32"}, "s32") : 0;
	unsigned count_type = dval_ ? wasm_type({"s32", "s32"}, "s32") : 0;
	unsigned unit_call_type = unit_call_ ? wasm_type({"s32", "s32", "s32", "s32", "s32", "s32", "s32", "s32"}, "s32") : 0;
	std::vector<Bytes> user_bodies;
	for (std::size_t i = 0; i < definitions_.size(); ++i)
		user_bodies.push_back(FunctionLowerer(*this, definitions_[i]).lower());
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
		for (std::size_t i = 0; i < params.size(); ++i)
			type_payload.push_back(0x7f);
		wasm::append_uleb(type_payload, result == "void" ? 0 : 1);
		if (result != "void")
			type_payload.push_back(0x7f);
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
		const unsigned type =
			name == "bearer_print_bytes" || name == "bearer_unit_render_bytes" || name == "bearer_component_render_bytes" ? bytes_type
			: name == "bearer_alloc"																					  ? alloc_type
			: name == "bearer_free"																						  ? release_type
			: name == "bearer_dv_string_to_brrb" || name == "bearer_dv_brrb_to_string"									  ? blob_type
			: name == "bearer_dv_s32_to_brrb" || name == "bearer_dv_bool_to_brrb" || name == "bearer_dv_s32_brrb" || name == "bearer_dv_bool_brrb"
				? scalar_adapter_type
			: name == "bearer_dv_build_brrb"											 ? build_type
			: name == "bearer_dv_get_brrb"												 ? get_type
			: name == "bearer_dv_count_brrb" || name == "bearer_dv_scalar_type_brrb"	 ? count_type
			: name == "bearer_dv_entry_key_brrb" || name == "bearer_dv_entry_value_brrb" ? entry_type
			: name == "bearer_dv_ptr_to_brrb"											 ? scalar_adapter_type
			: name == "bearer_dv_brrb_to_ptr"											 ? count_type
			: name == "bearer_unit_call_brrb"											 ? unit_call_type
																						 : scalar_type;
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
	for (const auto& d : definitions_)
		wasm::append_uleb(functions, d.type);
	for (std::size_t i = 0; i < custom_exports_.size(); ++i)
		wasm::append_uleb(functions, custom_export_type);
	Bytes exports;
	unsigned export_count = static_cast<unsigned>(custom_exports_.size());
	for (const auto& d : definitions_)
		if (!d.exported.empty())
			++export_count;
	wasm::append_uleb(exports, export_count);
	for (const auto& d : definitions_)
		if (!d.exported.empty())
		{
			wasm::append_string(exports, d.exported);
			exports.push_back(0);
			wasm::append_uleb(exports, d.index);
		}
	for (std::size_t i = 0; i < custom_exports_.size(); ++i)
	{
		wasm::append_string(exports, custom_exports_[i].first);
		exports.push_back(0);
		wasm::append_uleb(exports, first_user_index_ + static_cast<unsigned>(definitions_.size() + i));
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
	wasm::append_custom_section(result, "dylink.0", Bytes{1, static_cast<std::uint8_t>(mem.size())}); // replace the short payload below for multi-byte ULEB
	result.resize(8);
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
	if (use_arc_global_)
	{
		Bytes globals;
		wasm::append_uleb(globals, 1);
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
	for (std::size_t index = runtime_count; index < runtime_count + definitions_.size(); ++index)
	{
		std::size_t instruction = 0;
		read_uleb(bodies[index], instruction); // body byte length
		const std::uint32_t groups = read_uleb(bodies[index], instruction);
		for (std::uint32_t group = 0; group < groups; ++group)
		{
			read_uleb(bodies[index], instruction);
			++instruction;
		}
		const Definition& definition = definitions_[index - runtime_count];
		const Location& location = definition.function ? definition.function->location : definitions_[definition.thunk_target].function->location;
		source_rows.push_back({cursor + instruction, location});
		cursor += bodies[index].size();
	}
	for (std::size_t index = 0; index < custom_exports_.size(); ++index)
	{
		const Bytes& body = bodies[runtime_count + definitions_.size() + index];
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
		Bytes marker{0x41};
		wasm::append_sleb32(marker, static_cast<std::int32_t>(0x5a000000u + index));
		marker.push_back(0x1a);
		auto found = std::search(result.begin(), result.end(), marker.begin(), marker.end());
		if (found == result.end() || std::search(found + 1, result.end(), marker.begin(), marker.end()) != result.end())
			throw Error(markers_[index], "native Capy source marker is missing or ambiguous in final Wasm");
		source_rows.push_back({static_cast<std::size_t>(found - result.begin()), markers_[index]});
	}
	check_cancelled();
	std::sort(source_rows.begin(), source_rows.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
	std::ostringstream map;
	map << "BEARER_SOURCE_MAP_V1\t" << module_ << "\nF\t1\t" << source_ << "\n";
	for (const auto& [address, location] : source_rows)
		map << "L\t" << std::hex << address << std::dec << "\t1\t" << location.line << "\t" << location.column << "\n";
	return {std::move(result), map.str()};
}

} // namespace

CompileResult compile_bearer_unit(std::string_view source, const CompileOptions& options)
{
	Program program = parse(source, options.source_path, options.cancelled);
	return compile_bearer_unit(program, options.source_path, options.module_name, options.abi_version, options.cancelled);
}

CompileResult compile_bearer_unit(const Program& program, const std::string& source_path, const std::string& module_name, unsigned abi_version,
								  CancellationCallback cancelled)
{
	return Module(program, source_path, module_name, abi_version, std::move(cancelled)).compile();
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
	return compile_bearer_unit(source.str(), options);
}

} // namespace capy
