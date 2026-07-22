#include "frontend.h"
#include <cassert>
#include <fstream>
#include <iostream>
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
		Program p = parse("function CLI { print(\";\", \",\", \"}\") }\n", "test.capy");
		auto* f = static_cast<Function*>(p.items[0]);
		assert(static_cast<Call*>(f->body->items[0])->arguments.size() == 3);
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
		Program p = parse("function value(x : s32) s32 { return x }\nfunction value(x : string) string { return x }\n", "overload.capy");
		DeclarationIndex index;
		index.add_program(p);
		assert(index.functions.size() == 2);
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
	expect_error("function bad(s32) s32 { return 1 }\n", "name:type annotations");
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
	expect_error("function meta(x : any) { #compile { emit(x) } }\n", "#compile compile-time metaprogramming is deferred beyond Capy phase 3");
	expect_error("#wat", "unknown compiler directive #wat");
	expect_error("@", "unexpected character '@'");
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
