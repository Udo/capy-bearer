#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../lib/markup-context.h"

namespace capy
{

using CancellationCallback = std::function<bool()>;

struct Location
{
	std::string file;
	std::size_t line = 1, column = 1, offset = 0;
};
std::string format_location(const Location& location);

struct Error : std::runtime_error
{
	Location location;
	std::string message;
	Error(Location location, std::string message);
};

enum struct TokenKind
{
	identifier,
	integer,
	floating,
	string,
	markup,
	directive,
	symbol,
	newline,
	eof
};
struct MarkupTokenPart
{
	std::string kind, source;
	Location location;
	bearer::MarkupContext context = bearer::MarkupContext::html_text;
};
struct Token
{
	TokenKind kind;
	std::string text;
	Location location;
	std::vector<MarkupTokenPart> markup;
};

struct Lexer
{
	Lexer(std::string_view source, std::string file = "<input>", std::size_t line = 1, std::size_t column = 1, std::size_t base_offset = 0,
		  CancellationCallback cancelled = {});
	std::vector<Token> tokens();
	std::string_view source_;
	std::string file_;
	std::size_t byte_ = 0, offset_ = 0, line_, column_, base_offset_, checked_byte_ = 0;
	CancellationCallback cancelled_;
	Location location() const;
	std::string advance();
	bool starts(std::string_view text) const;
	Token markup_token();
	Token string_token();
};

enum struct ExprKind
{
	Name,
	Integer,
	UnsignedInteger,
	SignedInteger,
	Float,
	String,
	MarkupText,
	MarkupField,
	Markup,
	Array,
	Spread,
	Map,
	Annotation,
	Binary,
	ScopeLookup,
	Call,
	Index,
	Member,
	Block,
	Return,
	Yield,
	Break,
	Continue,
	Variable,
	FunctionType,
	Lambda,
	Function,
	Struct,
	Exports,
	Import,
	For,
	If,
	While,
	TypeAlias
};
struct Expr
{
	ExprKind kind;
	Location location;
	bool program_owned = false;
	explicit Expr(ExprKind kind, Location location) : kind(kind), location(std::move(location)) {}
	virtual ~Expr() = default;
};
struct Parameter
{
	std::string name;
	Expr* type_expr;
	Expr* default_value = nullptr;
	bool variadic = false, convert = false;
};
struct Name : Expr
{
	std::string value;
	Name(Location l, std::string v);
};
struct Integer : Expr
{
	std::uint64_t magnitude;
	bool negative;
	Integer(Location l, std::uint64_t magnitude, bool negative);
	Integer(Location l, long long value);
};
struct UnsignedInteger : Expr
{
	std::uint64_t value;
	UnsignedInteger(Location l, std::uint64_t v);
};
struct SignedInteger : Expr
{
	std::int64_t value;
	SignedInteger(Location l, std::int64_t v);
};
struct Float : Expr
{
	double value;
	Float(Location l, double v);
};
struct String : Expr
{
	std::string value;
	String(Location l, std::string v);
};
struct MarkupText : Expr
{
	std::string value;
	MarkupText(Location l, std::string v);
};
struct MarkupField : Expr
{
	Expr* value;
	bool escaped;
	bearer::MarkupContext context;
	MarkupField(Location l, Expr* v, bool e, bearer::MarkupContext context);
};
struct Markup : Expr
{
	std::vector<Expr*> parts;
	explicit Markup(Location l);
};
struct ArrayLiteral : Expr
{
	std::vector<Expr*> items;
	Expr* explicit_element_type = nullptr;
	explicit ArrayLiteral(Location l);
};
struct Spread : Expr
{
	Expr* value;
	Expr* target_element_type = nullptr;
	Spread(Location l, Expr* v);
};
struct MapLiteral : Expr
{
	std::vector<std::pair<std::string, Expr*>> entries;
	explicit MapLiteral(Location l);
};
struct Annotation : Expr
{
	Expr* value;
	Expr* type_expr;
	bool convert;
	Annotation(Location l, Expr* v, Expr* t, bool convert = false);
};
struct Binary : Expr
{
	std::string operator_;
	Expr *left, *right;
	Binary(Location l, std::string op, Expr* a, Expr* b);
};
struct ScopeLookup : Expr
{
	Expr* value;
	std::string member;
	ScopeLookup(Location l, Expr* v, std::string m);
};
struct Call : Expr
{
	Expr* function;
	std::vector<Expr*> arguments;
	Call(Location l, Expr* f);
};
struct Index : Expr
{
	Expr *value, *index;
	Index(Location l, Expr* v, Expr* i);
};
struct Member : Expr
{
	Expr* value;
	std::string member;
	Member(Location l, Expr* v, std::string m);
};
struct Block : Expr
{
	std::vector<Expr*> items;
	explicit Block(Location l);
};
struct Return : Expr
{
	Expr* value;
	Return(Location l, Expr* v);
};
struct Yield : Expr
{
	Expr* value;
	Yield(Location l, Expr* v);
};
struct Break : Expr
{
	explicit Break(Location l);
};
struct Continue : Expr
{
	explicit Continue(Location l);
};
struct Variable : Expr
{
	std::string name;
	Expr *annotation, *value;
	bool inferred;
	Variable(Location l, std::string n, Expr* a, Expr* v, bool i);
};
struct FunctionType : Expr
{
	std::vector<Parameter> parameters;
	Expr* return_type;
	explicit FunctionType(Location l);
};
struct Lambda : Expr
{
	std::vector<Parameter> parameters;
	Expr* return_type;
	Block* body;
	explicit Lambda(Location l);
};
struct Function : Expr
{
	std::string name;
	std::vector<Parameter> parameters;
	Expr* return_type;
	Block* body = nullptr;
	bool host = false, trace_host = false;
	Function(Location l, std::string n);
};
struct Struct : Expr
{
	std::string name;
	std::vector<Expr*> members;
	Struct(Location l, std::string n);
};
struct Exports : Expr
{
	std::vector<std::string> names;
	explicit Exports(Location l);
};
struct Import : Expr
{
	std::string path;
	std::string alias;
	Import(Location l, std::string p, std::string a);
};
struct TypeAlias : Expr
{
	std::string name;
	Expr* value;
	TypeAlias(Location l, std::string n, Expr* v);
};
struct For : Expr
{
	std::vector<std::string> names;
	Expr* iterable;
	Block* body;
	explicit For(Location l);
};
struct If : Expr
{
	Expr* condition;
	Block *then_body, *else_body;
	explicit If(Location l);
};
struct While : Expr
{
	Expr* condition;
	Block* body;
	explicit While(Location l);
};

struct Program
{
	std::vector<std::unique_ptr<Expr>> storage;
	std::vector<Expr*> items;
	template <typename T, typename... A> T* make(A&&... args)
	{
		auto p = std::make_unique<T>(std::forward<A>(args)...);
		auto r = p.get();
		r->program_owned = true;
		storage.push_back(std::move(p));
		return r;
	}
};
struct Parser
{
	Parser(std::vector<Token> tokens, CancellationCallback cancelled = {});
	Program parse();
	std::vector<Token> tokens_;
	std::size_t position_ = 0;
	Program program_;
	CancellationCallback cancelled_;
	Token& token();
	Token take();
	bool match(std::string_view);
	Token require(std::string_view);
	Token require_identifier(std::string_view);
	void skip_separators();
	Expr* expression(int minimum = 0, bool stop_at_newline = false);
	Expr* prefix();
	Expr* parenthesized(Location);
	Expr* array_literal(Location);
	Expr* map_literal(Location);
	Expr* finish_call(Expr*);
	Block* block(Location);
	Expr* function_expression(Location);
	Expr* function(Location, bool host = false, bool trace_host = false);
	std::vector<Parameter> parameters();
	Expr* structure(Location);
	Expr* exports(Location);
	Expr* import_directive(Location);
	Expr* type_alias(Location);
	Expr* variable(Location);
	Expr* result_expr(Location, bool yield);
	Expr* for_expr(Location);
	Expr* if_expr(Location);
	Expr* while_expr(Location);
};
Program parse(std::string_view source, std::string file = "<input>", CancellationCallback cancelled = {});
std::string type_name(const Expr& expression);
struct FunctionKey
{
	std::string name;
	std::vector<std::string> parameter_types;
	bool operator==(const FunctionKey&) const = default;
};
struct FunctionKeyHash
{
	std::size_t operator()(const FunctionKey&) const;
};
struct DeclarationIndex
{
	std::unordered_map<FunctionKey, Function*, FunctionKeyHash> functions;
	void add_program(const Program& program);
};

} // namespace capy
