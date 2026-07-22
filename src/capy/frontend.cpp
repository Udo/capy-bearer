#include "frontend.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace capy
{
namespace
{

std::size_t utf8_size(unsigned char byte)
{
	if (byte < 0x80)
		return 1;
	if ((byte & 0xe0) == 0xc0)
		return 2;
	if ((byte & 0xf0) == 0xe0)
		return 3;
	if ((byte & 0xf8) == 0xf0)
		return 4;
	return 1;
}
unsigned codepoint(std::string_view text, std::size_t position)
{
	unsigned char first = text[position];
	if (first < 0x80)
		return first;
	std::size_t size = utf8_size(first);
	unsigned value = first & ((1u << (8 - size - 1)) - 1);
	for (std::size_t i = 1; i < size && position + i < text.size(); ++i)
		value = (value << 6) | (static_cast<unsigned char>(text[position + i]) & 63);
	return value;
}
bool is_alpha(unsigned value)
{
	return (value < 128 && std::isalpha(value)) || value >= 128;
}
bool is_digit(unsigned value)
{
	return value < 128 && std::isdigit(value);
}
bool is_alnum(unsigned value)
{
	return is_alpha(value) || is_digit(value);
}
[[noreturn]] void fail(Location location, std::string message)
{
	throw Error(std::move(location), std::move(message));
}

} // namespace

std::string format_location(const Location& location)
{
	return location.file + ":" + std::to_string(location.line) + ":" + std::to_string(location.column);
}

Error::Error(Location source_location, std::string source_message)
	: std::runtime_error(format_location(source_location) + ": " + source_message), location(std::move(source_location)), message(std::move(source_message))
{
}

Lexer::Lexer(std::string_view source, std::string file, std::size_t line, std::size_t column, std::size_t base_offset, CancellationCallback cancelled)
	: source_(source), file_(std::move(file)), line_(line), column_(column), base_offset_(base_offset), cancelled_(std::move(cancelled))
{
}

Location Lexer::location() const
{
	return {file_, line_, column_, base_offset_ + offset_};
}
bool Lexer::starts(std::string_view text) const
{
	return source_.substr(byte_, text.size()) == text;
}

std::string Lexer::advance()
{
	if (byte_ - checked_byte_ >= 4096)
	{
		checked_byte_ = byte_;
		if (cancelled_ && cancelled_())
			throw Error(location(), "Capy compilation cancelled");
	}
	std::size_t size = std::min(utf8_size(static_cast<unsigned char>(source_[byte_])), source_.size() - byte_);
	std::string result(source_.substr(byte_, size));
	byte_ += size;
	++offset_;
	if (result == "\n")
	{
		++line_;
		column_ = 1;
	}
	else
		++column_;
	return result;
}

Token Lexer::string_token()
{
	Location start = location();
	advance();
	std::string value;
	while (byte_ < source_.size())
	{
		std::string character = advance();
		if (character == "\"")
			return {TokenKind::string, value, start, {}};
		if (character != "\\")
		{
			value += character;
			continue;
		}
		if (byte_ >= source_.size())
			break;
		std::string escaped = advance();
		if (escaped == "n")
			value += '\n';
		else if (escaped == "r")
			value += '\r';
		else if (escaped == "t")
			value += '\t';
		else if (escaped == "\"" || escaped == "\\")
			value += escaped;
		else
			fail(start, "unsupported string escape \\" + escaped);
	}
	fail(start, "unterminated string literal");
}

Token Lexer::markup_token()
{
	Location start = location();
	advance();
	advance();
	std::vector<MarkupTokenPart> parts;
	std::string literal;
	Location literal_location = location();
	int depth = 0;
	auto flush_literal = [&]
	{
		if (!literal.empty())
			parts.push_back({"text", literal, literal_location});
		literal.clear();
		literal_location = location();
	};
	while (byte_ < source_.size())
	{
		if (starts("<?=") || starts("<?:"))
		{
			flush_literal();
			bool escaped = starts("<?=");
			Location marker_location = location();
			advance();
			advance();
			advance();
			Location field_location = location();
			std::string field, quote;
			bool line_comment = false;
			int markup_depth = 0, nested_fields = 0;
			while (byte_ < source_.size())
			{
				if (line_comment)
				{
					std::string c = advance();
					field += c;
					if (c == "\n")
						line_comment = false;
					continue;
				}
				if (!quote.empty())
				{
					std::string c = advance();
					field += c;
					if (c == "\\" && byte_ < source_.size())
						field += advance();
					else if (c == quote)
						quote.clear();
					continue;
				}
				if (starts("//"))
				{
					field += advance();
					field += advance();
					line_comment = true;
					continue;
				}
				if (starts("<>"))
				{
					field += advance();
					field += advance();
					++markup_depth;
					continue;
				}
				if (markup_depth && starts("</>"))
				{
					field += advance();
					field += advance();
					field += advance();
					--markup_depth;
					continue;
				}
				if (markup_depth && (starts("<?=") || starts("<?:")))
				{
					field += advance();
					field += advance();
					field += advance();
					++nested_fields;
					continue;
				}
				if (starts("?>"))
				{
					if (nested_fields)
					{
						field += advance();
						field += advance();
						--nested_fields;
						continue;
					}
					if (!markup_depth)
						break;
				}
				std::string c = advance();
				field += c;
				if (c == "\"")
					quote = c;
			}
			if (byte_ >= source_.size())
				fail(marker_location, "unterminated markup interpolation");
			advance();
			advance();
			if (std::all_of(field.begin(), field.end(), [](unsigned char c) { return std::isspace(c); }))
				fail(marker_location, "empty markup interpolation");
			parts.push_back({escaped ? "escaped" : "raw", field, field_location});
			literal_location = location();
			continue;
		}
		if (starts("\\</>"))
		{
			advance();
			literal += advance();
			literal += advance();
			literal += advance();
			continue;
		}
		if (starts("\\<>"))
		{
			advance();
			literal += advance();
			literal += advance();
			continue;
		}
		if (starts("<>"))
		{
			++depth;
			advance();
			advance();
			continue;
		}
		if (starts("</>"))
		{
			if (!depth)
			{
				flush_literal();
				advance();
				advance();
				advance();
				return {TokenKind::markup, "", start, std::move(parts)};
			}
			--depth;
			advance();
			advance();
			advance();
			continue;
		}
		literal += advance();
	}
	fail(start, "unterminated markup expression");
}

std::vector<Token> Lexer::tokens()
{
	static const std::unordered_set<std::string> two = {"::", ":=", "..", "==", "!=", "<=", ">=", "&&", "||"};
	static const std::string one = "(){}[],:=+-*/%<>.!;";
	std::vector<Token> result;
	while (byte_ < source_.size())
	{
		char character = source_[byte_];
		if (character == ' ' || character == '\t' || character == '\r')
		{
			advance();
			continue;
		}
		if (character == '\n')
		{
			Location here = location();
			advance();
			result.push_back({TokenKind::newline, "\n", here, {}});
			continue;
		}
		if (starts("//"))
		{
			while (byte_ < source_.size() && source_[byte_] != '\n')
				advance();
			continue;
		}
		Location here = location();
		if (starts("<>"))
		{
			result.push_back(markup_token());
			continue;
		}
		unsigned value = codepoint(source_, byte_);
		if (is_alpha(value) || character == '_')
		{
			std::size_t begin = byte_;
			while (byte_ < source_.size() && (is_alnum(codepoint(source_, byte_)) || source_[byte_] == '_'))
				advance();
			result.push_back({TokenKind::identifier, std::string(source_.substr(begin, byte_ - begin)), here, {}});
			continue;
		}
		if (is_digit(value))
		{
			std::size_t begin = byte_;
			while (byte_ < source_.size() && is_digit(codepoint(source_, byte_)))
				advance();
			result.push_back({TokenKind::integer, std::string(source_.substr(begin, byte_ - begin)), here, {}});
			continue;
		}
		if (character == '\"')
		{
			result.push_back(string_token());
			continue;
		}
		if (character == '#')
		{
			advance();
			std::size_t begin = byte_;
			while (byte_ < source_.size() && (is_alnum(codepoint(source_, byte_)) || source_[byte_] == '_'))
				advance();
			if (begin == byte_)
				fail(here, "expected directive name after '#'");
			result.push_back({TokenKind::directive, std::string(source_.substr(begin, byte_ - begin)), here, {}});
			continue;
		}
		if (starts("..."))
		{
			advance();
			advance();
			advance();
			result.push_back({TokenKind::symbol, "...", here, {}});
			continue;
		}
		std::string pair(source_.substr(byte_, 2));
		if (two.contains(pair))
		{
			advance();
			advance();
			result.push_back({TokenKind::symbol, pair, here, {}});
			continue;
		}
		if (one.find(character) != std::string::npos)
		{
			advance();
			result.push_back({TokenKind::symbol, std::string(1, character), here, {}});
			continue;
		}
		fail(here, "unexpected character '" + std::string(1, character) + "'");
	}
	result.push_back({TokenKind::eof, "", location(), {}});
	return result;
}

Name::Name(Location l, std::string v) : Expr(ExprKind::Name, std::move(l)), value(std::move(v)) {}
Integer::Integer(Location l, long long v) : Expr(ExprKind::Integer, std::move(l)), value(v) {}
String::String(Location l, std::string v) : Expr(ExprKind::String, std::move(l)), value(std::move(v)) {}
MarkupText::MarkupText(Location l, std::string v) : Expr(ExprKind::MarkupText, std::move(l)), value(std::move(v)) {}
MarkupField::MarkupField(Location l, Expr* v, bool e) : Expr(ExprKind::MarkupField, std::move(l)), value(v), escaped(e) {}
Markup::Markup(Location l) : Expr(ExprKind::Markup, std::move(l)) {}
TupleExpr::TupleExpr(Location l) : Expr(ExprKind::Tuple, std::move(l)) {}
ArrayLiteral::ArrayLiteral(Location l) : Expr(ExprKind::Array, std::move(l)) {}
MapLiteral::MapLiteral(Location l) : Expr(ExprKind::Map, std::move(l)) {}
Annotation::Annotation(Location l, Expr* v, Expr* t) : Expr(ExprKind::Annotation, std::move(l)), value(v), type_expr(t) {}
Binary::Binary(Location l, std::string op, Expr* a, Expr* b) : Expr(ExprKind::Binary, std::move(l)), operator_(std::move(op)), left(a), right(b) {}
Cast::Cast(Location l, Expr* v, Expr* t) : Expr(ExprKind::Cast, std::move(l)), value(v), target_type(t) {}
ScopeLookup::ScopeLookup(Location l, Expr* v, std::string m) : Expr(ExprKind::ScopeLookup, std::move(l)), value(v), member(std::move(m)) {}
Call::Call(Location l, Expr* f) : Expr(ExprKind::Call, std::move(l)), function(f) {}
Index::Index(Location l, Expr* v, Expr* i) : Expr(ExprKind::Index, std::move(l)), value(v), index(i) {}
Member::Member(Location l, Expr* v, std::string m) : Expr(ExprKind::Member, std::move(l)), value(v), member(std::move(m)) {}
Block::Block(Location l) : Expr(ExprKind::Block, std::move(l)) {}
Return::Return(Location l, Expr* v) : Expr(ExprKind::Return, std::move(l)), value(v) {}
Break::Break(Location l) : Expr(ExprKind::Break, std::move(l)) {}
Continue::Continue(Location l) : Expr(ExprKind::Continue, std::move(l)) {}
Variable::Variable(Location l, std::string n, Expr* a, Expr* v, bool i)
	: Expr(ExprKind::Variable, std::move(l)), name(std::move(n)), annotation(a), value(v), inferred(i)
{
}
FunctionType::FunctionType(Location l) : Expr(ExprKind::FunctionType, std::move(l)), return_type(nullptr) {}
Lambda::Lambda(Location l) : Expr(ExprKind::Lambda, std::move(l)), return_type(nullptr), body(nullptr) {}
Function::Function(Location l, std::string n) : Expr(ExprKind::Function, std::move(l)), name(std::move(n)), return_type(nullptr), body(nullptr) {}
Struct::Struct(Location l, std::string n) : Expr(ExprKind::Struct, std::move(l)), name(std::move(n)) {}
For::For(Location l) : Expr(ExprKind::For, std::move(l)), iterable(nullptr), body(nullptr) {}
If::If(Location l) : Expr(ExprKind::If, std::move(l)), condition(nullptr), then_body(nullptr), else_body(nullptr) {}
While::While(Location l) : Expr(ExprKind::While, std::move(l)), condition(nullptr), body(nullptr) {}

Token& Parser::token()
{
	return tokens_[position_];
}
Token Parser::take()
{
	if ((position_ & 255) == 0 && cancelled_ && cancelled_())
		throw Error(token().location, "Capy compilation cancelled");
	return tokens_[position_++];
}
bool Parser::match(std::string_view text)
{
	if (token().kind != TokenKind::string && token().text == text)
	{
		take();
		return true;
	}
	return false;
}
Token Parser::require(std::string_view text)
{
	if (token().kind == TokenKind::string || token().text != text)
		fail(token().location, "expected '" + std::string(text) + "', found '" + (token().text.empty() ? "end of file" : token().text) + "'");
	return take();
}
Token Parser::require_identifier(std::string_view purpose)
{
	if (token().kind != TokenKind::identifier)
		fail(token().location, "expected " + std::string(purpose) + ", found '" + (token().text.empty() ? "end of file" : token().text) + "'");
	return take();
}
Parser::Parser(std::vector<Token> tokens, CancellationCallback cancelled) : tokens_(std::move(tokens)), cancelled_(std::move(cancelled)) {}
void Parser::skip_separators()
{
	while (token().kind == TokenKind::newline || (token().kind == TokenKind::symbol && token().text == ";"))
		take();
}

Program Parser::parse()
{
	std::vector<Expr*> items;
	skip_separators();
	while (token().kind != TokenKind::eof)
	{
		items.push_back(expression());
		if (token().kind != TokenKind::newline && token().kind != TokenKind::eof && token().text != ";" && token().text != "}")
			fail(token().location, "expected expression separator, found '" + token().text + "'");
		skip_separators();
	}
	program_.items = std::move(items);
	return std::move(program_);
}

Expr* Parser::expression(int minimum)
{
	while (token().kind == TokenKind::newline)
		take();
	Expr* left = prefix();
	if ((left->kind == ExprKind::Break || left->kind == ExprKind::Continue) && token().kind != TokenKind::newline && token().kind != TokenKind::eof &&
		token().text != "}" && token().text != ";")
		fail(token().location, std::string(left->kind == ExprKind::Break ? "break" : "continue") + " does not accept arguments or operators");
	static const std::unordered_map<std::string, int> infix = {{"=", 5},   {":=", 5},  {":", 10}, {"..", 20}, {"||", 25}, {"&&", 30},
															   {"==", 35}, {"!=", 35}, {"<", 40}, {"<=", 40}, {"as", 45}, {">", 40},
															   {">=", 40}, {"+", 50},  {"-", 50}, {"*", 60},  {"/", 60},  {"%", 60}};
	while (true)
	{
		Token next = token();
		if (next.kind == TokenKind::newline || next.kind == TokenKind::eof || next.text == "," || next.text == ")" || next.text == "]" || next.text == "}" ||
			next.text == ";" || next.text == "{")
			break;
		if (next.text == "(")
		{
			if (80 < minimum)
				break;
			left = finish_call(left);
			continue;
		}
		if (next.text == "[")
		{
			if (80 < minimum)
				break;
			Location location = take().location;
			Expr* index = expression();
			require("]");
			left = program_.make<Index>(location, left, index);
			continue;
		}
		if (next.text == ".")
		{
			if (80 < minimum)
				break;
			Location location = take().location;
			left = program_.make<Member>(location, left, require_identifier("member name").text);
			continue;
		}
		if (next.text == "::")
		{
			if (80 < minimum)
				break;
			Location location = take().location;
			left = program_.make<ScopeLookup>(location, left, require_identifier("scope member").text);
			continue;
		}
		auto found = infix.find(next.text);
		if (found == infix.end() || found->second < minimum)
			break;
		Token op = take();
		if (op.text == ":")
			return program_.make<Annotation>(op.location, left, expression(81));
		if (op.text == "as")
		{
			left = program_.make<Cast>(op.location, left, expression(81));
			continue;
		}
		left = program_.make<Binary>(op.location, op.text, left, expression(found->second + (op.text == "=" || op.text == ":=" ? 0 : 1)));
	}
	return left;
}

Expr* Parser::prefix()
{
	Token current = take();
	if (current.kind == TokenKind::integer)
		return program_.make<Integer>(current.location, std::stoll(current.text));
	if (current.kind == TokenKind::string)
		return program_.make<String>(current.location, current.text);
	if (current.kind == TokenKind::markup)
	{
		Markup* markup = program_.make<Markup>(current.location);
		for (const MarkupTokenPart& part : current.markup)
		{
			if (part.kind == "text")
			{
				markup->parts.push_back(program_.make<MarkupText>(part.location, part.source));
				continue;
			}
			Parser field_parser(Lexer(part.source, part.location.file, part.location.line, part.location.column, part.location.offset, cancelled_).tokens(),
								cancelled_);
			Expr* value = field_parser.expression();
			field_parser.skip_separators();
			if (field_parser.token().kind != TokenKind::eof)
				fail(field_parser.token().location, "markup interpolation must contain one expression");
			for (auto& node : field_parser.program_.storage)
				program_.storage.push_back(std::move(node));
			markup->parts.push_back(program_.make<MarkupField>(part.location, value, part.kind == "escaped"));
		}
		return markup;
	}
	if (current.kind == TokenKind::directive)
	{
		if (current.text == "compile" || current.text == "callsite")
			fail(current.location, "#" + current.text + " compile-time metaprogramming is deferred beyond Capy phase 3");
		fail(current.location, "unknown compiler directive #" + current.text);
	}
	if (current.kind == TokenKind::identifier)
	{
		if (current.text == "function")
			return token().kind == TokenKind::identifier ? function(current.location) : function_expression(current.location);
		if (current.text == "struct")
			return structure(current.location);
		if (current.text == "var")
			return variable(current.location);
		if (current.text == "return")
			return return_expr(current.location);
		if (current.text == "break")
			return program_.make<Break>(current.location);
		if (current.text == "continue")
			return program_.make<Continue>(current.location);
		if (current.text == "for")
			return for_expr(current.location);
		if (current.text == "if")
			return if_expr(current.location);
		if (current.text == "while")
			return while_expr(current.location);
		return program_.make<Name>(current.location, current.text);
	}
	if (current.text == "(")
		return parenthesized(current.location);
	if (current.text == "[")
		return array_literal(current.location);
	if (current.text == "{")
	{
		skip_separators();
		if (token().text == ":" || ((token().kind == TokenKind::identifier || token().kind == TokenKind::string) && position_ + 1 < tokens_.size() &&
									tokens_[position_ + 1].text == ":"))
			return map_literal(current.location);
		return block(current.location);
	}
	if (current.text == "-" || current.text == "!")
		return program_.make<Binary>(current.location, "unary" + current.text, program_.make<Integer>(current.location, 0), expression(70));
	fail(current.location, "expected expression, found '" + (current.text.empty() ? "end of file" : current.text) + "'");
}

Expr* Parser::parenthesized(Location location)
{
	TupleExpr* tuple = program_.make<TupleExpr>(location);
	skip_separators();
	if (match(")"))
		return tuple;
	while (true)
	{
		tuple->items.push_back(expression());
		skip_separators();
		if (!match(","))
			break;
		skip_separators();
	}
	require(")");
	return tuple->items.size() == 1 ? tuple->items[0] : tuple;
}
Expr* Parser::array_literal(Location location)
{
	ArrayLiteral* array = program_.make<ArrayLiteral>(location);
	skip_separators();
	if (!match("]"))
	{
		while (true)
		{
			array->items.push_back(expression());
			skip_separators();
			if (!match(","))
				break;
			skip_separators();
		}
		require("]");
	}
	return array;
}
Expr* Parser::map_literal(Location location)
{
	MapLiteral* map = program_.make<MapLiteral>(location);
	skip_separators();
	if (match(":"))
	{
		require("}");
		return map;
	}
	while (true)
	{
		Token key = take();
		if (key.kind != TokenKind::identifier && key.kind != TokenKind::string)
			fail(key.location, "DValue map key must be an identifier or string");
		require(":");
		map->entries.emplace_back(key.text, expression());
		skip_separators();
		if (!match(","))
			break;
		skip_separators();
	}
	require("}");
	return map;
}
Expr* Parser::finish_call(Expr* function_value)
{
	require("(");
	Call* call = program_.make<Call>(function_value->location, function_value);
	skip_separators();
	if (!match(")"))
	{
		while (true)
		{
			call->arguments.push_back(expression());
			skip_separators();
			if (!match(","))
				break;
			skip_separators();
		}
		require(")");
	}
	return call;
}
Block* Parser::block(Location location)
{
	Block* result = program_.make<Block>(location);
	skip_separators();
	while (!match("}"))
	{
		if (token().kind == TokenKind::eof)
			fail(location, "unterminated code block");
		result->items.push_back(expression());
		if (token().kind != TokenKind::newline && token().kind != TokenKind::eof && token().text != ";" && token().text != "}")
			fail(token().location, "expected expression separator, found '" + token().text + "'");
		skip_separators();
	}
	return result;
}

Expr* Parser::function_expression(Location location)
{
	if (token().text != "(")
		fail(token().location, "anonymous function requires a parameter expression");
	std::vector<Parameter> parameter_list = parameters(parenthesized(take().location));
	Expr* return_type = token().text == "{" ? nullptr : expression(81);
	if (token().text != "{")
	{
		FunctionType* type = program_.make<FunctionType>(location);
		type->parameters = std::move(parameter_list);
		type->return_type = return_type;
		return type;
	}
	Lambda* lambda = program_.make<Lambda>(location);
	lambda->parameters = std::move(parameter_list);
	lambda->return_type = return_type;
	lambda->body = block(require("{").location);
	return lambda;
}
Expr* Parser::function(Location location)
{
	Token name = require_identifier("function name");
	std::vector<Expr*> header;
	while (token().text != "{")
	{
		if (token().kind == TokenKind::newline || token().kind == TokenKind::eof)
			fail(token().location, "expected function code block");
		header.push_back(token().text == "(" ? parenthesized(take().location) : expression());
		if (header.size() > 2)
			fail(header.back()->location, "function declaration has more than parameter and return expressions");
	}
	Function* result = program_.make<Function>(location, name.text);
	Location body_location = require("{").location;
	result->body = block(body_location);
	Expr* parameter_expression;
	if (header.size() == 1 && !is_parameter_expression(header[0]))
	{
		parameter_expression = program_.make<TupleExpr>(location);
		result->return_type = header[0];
	}
	else
	{
		parameter_expression = header.empty() ? static_cast<Expr*>(program_.make<TupleExpr>(location)) : header[0];
		result->return_type = header.size() == 2 ? header[1] : nullptr;
	}
	result->parameters = parameters(parameter_expression);
	return result;
}
bool Parser::is_parameter_expression(Expr* expression_value) const
{
	std::vector<Expr*> values =
		expression_value->kind == ExprKind::Tuple ? static_cast<TupleExpr*>(expression_value)->items : std::vector<Expr*>{expression_value};
	return std::all_of(values.begin(), values.end(),
					   [](Expr* value) { return value->kind == ExprKind::Annotation && static_cast<Annotation*>(value)->value->kind == ExprKind::Name; });
}
std::vector<Parameter> Parser::parameters(Expr* expression_value)
{
	std::vector<Expr*> values =
		expression_value->kind == ExprKind::Tuple ? static_cast<TupleExpr*>(expression_value)->items : std::vector<Expr*>{expression_value};
	std::vector<Parameter> result;
	std::unordered_set<std::string> names;
	for (Expr* value : values)
	{
		if (value->kind != ExprKind::Annotation || static_cast<Annotation*>(value)->value->kind != ExprKind::Name)
			fail(value->location, "function parameter expression must contain name:type annotations");
		Annotation* annotation = static_cast<Annotation*>(value);
		std::string name = static_cast<Name*>(annotation->value)->value;
		if (!names.insert(name).second)
			fail(value->location, "function parameter '" + name + "' is already declared");
		result.push_back({std::move(name), annotation->type_expr});
	}
	return result;
}
Expr* Parser::structure(Location location)
{
	Token name = require_identifier("struct name");
	require("{");
	Struct* result = program_.make<Struct>(location, name.text);
	result->members = block(location)->items;
	return result;
}
Expr* Parser::variable(Location location)
{
	Token name = require_identifier("variable name");
	Expr* annotation = nullptr;
	bool inferred = false;
	if (match(":="))
		inferred = true;
	else
	{
		require(":");
		annotation = expression(11);
		require("=");
	}
	return program_.make<Variable>(location, name.text, annotation, expression(), inferred);
}
Expr* Parser::return_expr(Location location)
{
	if (token().kind == TokenKind::newline || token().kind == TokenKind::eof || token().text == ";" || token().text == "}")
		return program_.make<Return>(location, nullptr);
	std::vector<Expr*> values = {expression()};
	while (match(","))
		values.push_back(expression());
	if (values.size() == 1)
		return program_.make<Return>(location, values[0]);
	TupleExpr* tuple = program_.make<TupleExpr>(location);
	tuple->items = std::move(values);
	return program_.make<Return>(location, tuple);
}
Expr* Parser::for_expr(Location location)
{
	For* result = program_.make<For>(location);
	result->names.push_back(require_identifier("loop variable").text);
	if (match(","))
		result->names.push_back(require_identifier("second loop variable").text);
	if (!(match("=") || match("in")))
		fail(token().location, "expected '=' or 'in' after loop variable");
	result->iterable = expression();
	result->body = block(require("{").location);
	return result;
}
Expr* Parser::if_expr(Location location)
{
	If* result = program_.make<If>(location);
	result->condition = expression();
	result->then_body = block(require("{").location);
	std::size_t separator_position = position_;
	skip_separators();
	if (match("else"))
		result->else_body = block(require("{").location);
	else
		position_ = separator_position;
	return result;
}
Expr* Parser::while_expr(Location location)
{
	While* result = program_.make<While>(location);
	result->condition = expression();
	result->body = block(require("{").location);
	return result;
}

Program parse(std::string_view source, std::string file, CancellationCallback cancelled)
{
	auto tokens = Lexer(source, std::move(file), 1, 1, 0, cancelled).tokens();
	return Parser(std::move(tokens), std::move(cancelled)).parse();
}
std::string type_name(const Expr& expression_value)
{
	if (expression_value.kind == ExprKind::Name)
		return static_cast<const Name&>(expression_value).value;
	if (expression_value.kind == ExprKind::ScopeLookup)
	{
		const auto& value = static_cast<const ScopeLookup&>(expression_value);
		return type_name(*value.value) + "::" + value.member;
	}
	if (expression_value.kind == ExprKind::FunctionType)
	{
		const auto& value = static_cast<const FunctionType&>(expression_value);
		std::string result = "function(";
		for (std::size_t i = 0; i < value.parameters.size(); ++i)
		{
			if (i)
				result += ',';
			result += type_name(*value.parameters[i].type_expr);
		}
		return result + ") " + (value.return_type ? type_name(*value.return_type) : "void");
	}
	if (expression_value.kind == ExprKind::Tuple || expression_value.kind == ExprKind::Array)
	{
		const std::vector<Expr*>& values = expression_value.kind == ExprKind::Tuple ? static_cast<const TupleExpr&>(expression_value).items
																					: static_cast<const ArrayLiteral&>(expression_value).items;
		std::string result = expression_value.kind == ExprKind::Tuple ? "(" : "[";
		for (std::size_t i = 0; i < values.size(); ++i)
		{
			if (i)
				result += ',';
			result += type_name(*values[i]);
		}
		return result + (expression_value.kind == ExprKind::Tuple ? ")" : "]");
	}
	fail(expression_value.location, "expected type expression");
}
std::size_t FunctionKeyHash::operator()(const FunctionKey& key) const
{
	std::size_t result = std::hash<std::string>{}(key.name);
	for (const std::string& type : key.parameter_types)
		result ^= std::hash<std::string>{}(type) + 0x9e3779b9 + (result << 6) + (result >> 2);
	return result;
}
void DeclarationIndex::add_program(const Program& program)
{
	for (Expr* item : program.items)
	{
		if (item->kind != ExprKind::Function)
			continue;
		Function* function_value = static_cast<Function*>(item);
		FunctionKey key{function_value->name, {}};
		for (const Parameter& parameter : function_value->parameters)
			key.parameter_types.push_back(type_name(*parameter.type_expr));
		auto existing = functions.find(key);
		if (existing != functions.end())
		{
			std::string types;
			for (std::size_t i = 0; i < key.parameter_types.size(); ++i)
			{
				if (i)
					types += ", ";
				types += key.parameter_types[i];
			}
			Function* previous = existing->second;
			fail(function_value->location, "duplicate overload " + function_value->name + "(" + types +
											   "); return type does not distinguish overloads (first declared at " + std::to_string(previous->location.line) +
											   ":" + std::to_string(previous->location.column) + ")");
		}
		functions.emplace(std::move(key), function_value);
	}
}

} // namespace capy
