#include "frontend.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
using namespace capy;

static void expect_error(const std::string& source, const std::string& text, const std::string& file = "bad.capy")
{
	try
	{
		parse(source, file);
		std::cerr << "expected error: " << text << " for " << source << "\n";
		assert(false);
	}
	catch (const Error& e)
	{
		assert(e.what() == std::string(e.what()));
		assert(e.message.find(text) != std::string::npos);
	}
}

int main(int argc, char** argv)
{
	{
		Program p = parse("const answer : s32 = 42\n", "constant.capy");
		auto* constant = static_cast<Constant*>(p.items[0]);
		assert(constant->name == "answer" && type_name(*constant->annotation) == "s32" && static_cast<Integer*>(constant->value)->value == 42);
	}
	{
		Program p = parse("trace host function __bearer_trace(value : s32) string\n", "host.capy");
		auto* f = static_cast<Function*>(p.items[0]);
		assert(f->host && f->trace_host && !f->body);
	}
	{
		Program p = parse("function pair value : s32 (s32, string) { return (value, \"x\") }\n", "test.capy");
		auto* f = static_cast<Function*>(p.items[0]);
		assert(f->parameters.size() == 1 && type_name(*f->parameters[0].type_expr) == "s32" && type_name(*f->return_type) == "(s32,string)");
	}
	{
		Program p = parse("function pair (s32, string) { return (1, \"x\") }\n", "test.capy");
		auto* f = static_cast<Function*>(p.items[0]);
		assert(f->parameters.empty() && type_name(*f->return_type) == "(s32,string)");
	}
	{
		Program p = parse("var callback : function(value : s32) s32 = function(value : s32) s32 { return value + 1 }\n", "test.capy");
		auto* v = static_cast<Variable*>(p.items[0]);
		assert(v->annotation->kind == ExprKind::FunctionType && v->value->kind == ExprKind::Lambda && type_name(*v->annotation) == "function(s32) s32");
	}
	{
		Program p = parse("var callback : function(__bearer_value : s32) s32 = function(__bearer_value : s32) s32 { __bearer_value }\nfor __bearer_key, value = dval({:}) { value }\n", "private-binders.capy");
		assert(static_cast<Lambda*>(static_cast<Variable*>(p.items[0])->value)->parameters[0].name == "__bearer_value");
		assert(static_cast<For*>(p.items[1])->names[0] == "__bearer_key");
	}
	{
		Program p = parse("function CLI { print(\";\", \",\", \"}\") }\n", "test.capy");
		auto* f = static_cast<Function*>(p.items[0]);
		assert(static_cast<Call*>(f->body->items[0])->arguments.size() == 3);
	}
	{
		Program p = parse("function CLI { box.callback(7) }\n", "member-call.capy");
		auto* call = static_cast<Call*>(static_cast<Function*>(p.items[0])->body->items[0]);
		auto* member = static_cast<Member*>(call->function);
		assert(member->member == "callback" && static_cast<Name*>(member->value)->value == "box" && call->arguments.size() == 1 &&
			call->location.file == "member-call.capy" && call->location.line == 1 && call->location.column == 19);
	}
	{
		Program p = parse("var a := 1 // first\nvar b := 2\n", "comments.capy");
		assert(p.items.size() == 2);
	}
	{
		Program p = parse("var a : s32 = test(10)\nvar b := test(10, 20)\n", "vars.capy");
		auto* a = static_cast<Variable*>(p.items[0]);
		auto* b = static_cast<Variable*>(p.items[1]);
		assert(!a->inferred && b->inferred && !b->annotation);
	}
	{
		Program p = parse("var value := dval({\"name\": \"Ada\", age: 42})\nfor key, item = value { print(key) }\n", "map.capy");
		auto* c = static_cast<Call*>(static_cast<Variable*>(p.items[0])->value);
		assert(static_cast<MapLiteral*>(c->arguments[0])->entries.size() == 2);
		assert(static_cast<For*>(p.items[1])->names.size() == 2);
	}
	{
		Program p = parse("0..10\n", "range.capy");
		assert(static_cast<Binary*>(p.items[0])->operator_ == "..");
	}
	{
		Program p = parse("-2147483648\n2147483647\n", "integers.capy");
		assert(static_cast<Integer*>(p.items[0])->value == -2147483648LL);
		assert(static_cast<Integer*>(p.items[1])->value == 2147483647LL);
	}
	{
		Program p = parse("0u64; 18446744073709551615u64; -9223372036854775808s64; 9223372036854775807s64; 1.5; 2e3; 1..3", "wide.capy");
		assert(static_cast<UnsignedInteger*>(p.items[0])->value == 0);
		assert(static_cast<UnsignedInteger*>(p.items[1])->value == std::numeric_limits<std::uint64_t>::max());
		assert(static_cast<SignedInteger*>(p.items[2])->value == std::numeric_limits<std::int64_t>::min());
		assert(static_cast<SignedInteger*>(p.items[3])->value == std::numeric_limits<std::int64_t>::max());
		assert(static_cast<Float*>(p.items[4])->value == 1.5 && static_cast<Float*>(p.items[5])->value == 2000.0);
		assert(static_cast<Binary*>(p.items[6])->operator_ == "..");
	}
	{
		Program p = parse("\"one and\n  two\"\n", "string.capy");
		assert(static_cast<String*>(p.items[0])->value == "one and\n  two");
	}
	{
		Program p = parse("<><h1><?= title ?></h1><?: trusted ?></>\n", "markup.capy");
		auto* m = static_cast<Markup*>(p.items[0]);
		assert(m->parts.size() == 4 && static_cast<MarkupText*>(m->parts[0])->value == "<h1>" && static_cast<MarkupField*>(m->parts[1])->escaped &&
			   !static_cast<MarkupField*>(m->parts[3])->escaped);
	}
	{
		Program p = parse("<><div><><span>x</span></></div></>\n", "markup.capy");
		auto* m = static_cast<Markup*>(p.items[0]);
		assert(m->parts.size() == 1 && static_cast<MarkupText*>(m->parts[0])->value == "<div><span>x</span></div>");
	}
	{
		Program p = parse("<><p><?= <><b><?= clone(\"x\") ?></b></> ?></p></>\n", "markup.capy");
		auto* m = static_cast<Markup*>(p.items[0]);
		assert(static_cast<MarkupField*>(m->parts[1])->value->kind == ExprKind::Markup);
	}
	{
		Program p = parse("<><script>const close = \"\\</>\";</script></>\n", "markup.capy");
		assert(static_cast<MarkupText*>(static_cast<Markup*>(p.items[0])->parts[0])->value == "<script>const close = \"</>\";</script>");
	}
	{
		Program p = parse("EXPORTS first, second\nEXPORTS third\n", "exports.capy");
		auto* first = static_cast<Exports*>(p.items[0]);
		auto* second = static_cast<Exports*>(p.items[1]);
		assert(first->names == std::vector<std::string>({"first", "second"}) && second->names == std::vector<std::string>({"third"}));
	}
	{
		Program p = parse("function value(x : s32) s32 { return x }\nfunction value(x : as string) string { return x }\n", "overload.capy");
		auto* converted = static_cast<Function*>(p.items[1]);
		assert(!static_cast<Function*>(p.items[0])->parameters[0].convert && converted->parameters[0].convert);
		DeclarationIndex index;
		index.add_program(p);
		assert(index.functions.size() == 2);
	}
	{
		Program p = parse("function gather(prefix : string, ...values : as string) [string] { return values }\nfunction CLI { gather(\"x\", ...[\"a\", \"b\"]) }\n", "variadic.capy");
		auto* gather = static_cast<Function*>(p.items[0]);
		assert(gather->parameters.size() == 2 && gather->parameters[1].variadic && gather->parameters[1].convert);
		auto* call = static_cast<Call*>(static_cast<Function*>(p.items[1])->body->items[0]);
		assert(call->arguments.size() == 2 && call->arguments[1]->kind == ExprKind::Spread);
	}
	{
		Program p = parse("function value(x : s32) s32 { return x }\nfunction value(x : s32) string { return x }\n", "overload.capy");
		try
		{
			DeclarationIndex().add_program(p);
			assert(false);
		}
		catch (const Error& e)
		{
			assert(e.message.find("return type does not distinguish overloads") != std::string::npos);
		}
	}
	expect_error("function CLI { const answer : s32 = 42 }\n", "const declarations are top-level only");
	expect_error("function bad(s32) s32 { return 1 }\n", "name:type annotations");
	expect_error("function bad(...values : string, tail : string) {}\n", "variadic parameter must be last");
	expect_error("function CLI { var value := 1 as s64 }\n", "call the target type constructor instead");
	expect_error("trace function bad() string {}\n", "trace modifier applies only to host function declarations");
	expect_error("function bad(value : any, value : any) value::type { value }\n", "function parameter 'value' is already declared");
	try
	{
		parse("\"broken", "bad.capy");
		assert(false);
	}
	catch (const Error& e)
	{
		assert(std::string(e.what()) == "bad.capy:1:1: unterminated string literal");
	}
	expect_error("<><?=   ?></>", "empty markup interpolation");
	expect_error("<><?= value </>", "unterminated markup interpolation");
	expect_error("<><p>missing", "unterminated markup expression");
	expect_error("function CLI { break junk }\n", "break does not accept arguments or operators");
	expect_error("function CLI { continue() }\n", "continue does not accept arguments or operators");
	expect_error("EXPORTS\n", "EXPORTS requires at least one function name");
	expect_error("EXPORTS first,\n", "expected exported function name after ','");
	expect_error("function CLI { EXPORTS first }\n", "EXPORTS is a reserved top-level directive");
	expect_error("function meta(x : any) { #compile { emit(x) } }\n", "#compile compile-time metaprogramming is deferred beyond Capy phase 3");
	expect_error("#wat", "unknown compiler directive #wat");
	expect_error("@", "unexpected character '@'");
	expect_error("18446744073709551616u64", "outside the u64 range");
	expect_error("9223372036854775808s64", "outside the s64 range");
	expect_error("-9223372036854775809s64", "outside the s64 range");
	expect_error("1u64x", "invalid numeric suffix");
	expect_error("1e", "invalid f64 exponent");
	expect_error("2147483648", "outside the s32 range");
	expect_error("-2147483649", "outside the s32 range");
	expect_error("999999999999999999999999999999", "outside the s32 range");
	try
	{
		Parser({}).token();
		assert(false);
	}
	catch (const Error& e)
	{
		assert(e.message == "parser received no tokens");
	}
	{
		Lexer lexer("hé\n", "utf8.capy");
		auto tokens = lexer.tokens();
		assert(tokens[0].location.column == 1 && tokens[1].location.column == 3 && tokens[1].location.offset == 2);
	}
	for (int index = 1; index < argc; ++index)
	{
		std::ifstream input(argv[index], std::ios::binary);
		assert(input && "fixture must be readable");
		std::ostringstream source;
		source << input.rdbuf();
		parse(source.str(), argv[index]);
	}
	std::cout << "native Capy frontend tests passed";
	if (argc > 1)
		std::cout << " and " << argc - 1 << " tracked fixtures parsed";
	std::cout << '\n';
}
