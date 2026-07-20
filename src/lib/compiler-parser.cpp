#include "compiler-parser.h"

#include "compiler.h"

namespace {

const char* UCE_NAMED_RENDER_SYMBOL = "__uce_render";
const char* UCE_NAMED_COMPONENT_SYMBOL = "__uce_component";
const char* UCE_NAMED_SERVE_HTTP_SYMBOL = "__uce_serve_http";

struct CompilerCodeState
{
	char quote_char = '\0';
	bool inside_quote = false;
	bool inside_line_comment = false;
	bool inside_block_comment = false;
};

bool compiler_code_state_is_neutral(const CompilerCodeState& state)
{
	return(!state.inside_quote && !state.inside_line_comment && !state.inside_block_comment);
}

String compiler_cpp_raw_string_delimiter(const String& content)
{
	StringList candidates = {
		"",
		"UCE",
		"UCE_LITERAL",
		"uce_literal_0",
		"uce_literal_1"
	};

	u64 hash = 1469598103934665603ULL;
	for(unsigned char c : content)
	{
		hash ^= c;
		hash *= 1099511628211ULL;
	}

	for(u32 i = 0; i < 64; i += 1)
	{
		String suffix = to_hex<u64>(hash ^ ((u64)i * 0x9E3779B97F4A7C15ULL), 12);
		candidates.push_back("UCE" + suffix);
	}

	for(auto& delimiter : candidates)
	{
		String terminator = ")" + delimiter + "\"";
		if(content.find(terminator) == String::npos)
			return(delimiter);
	}

	return("");
}

String compiler_cpp_string_literal(const String& content)
{
	String delimiter = compiler_cpp_raw_string_delimiter(content);
	if(delimiter != "" || content.find(")\"") == String::npos)
		return("R\"" + delimiter + "(" + content + ")" + delimiter + "\"");

	// This fallback is only reachable for deliberately adversarial content that
	// contains every generated raw-string delimiter candidate.
	return(json_escape(content));
}

void compiler_code_state_consume(CompilerCodeState& state, String& buffer, const String& content, u32& i)
{
	char c = content[i];
	char c1 = (i + 1 < content.length()) ? content[i + 1] : '\0';

	buffer.append(1, c);

	if(state.inside_quote)
	{
		if(state.quote_char == c && (i == 0 || content[i-1] != '\\'))
			state.inside_quote = false;
		return;
	}

	if(state.inside_line_comment)
	{
		if(c == '\n')
			state.inside_line_comment = false;
		return;
	}

	if(state.inside_block_comment)
	{
		if(c == '*' && c1 == '/')
		{
			buffer.append(1, c1);
			state.inside_block_comment = false;
			i += 1;
		}
		return;
	}

	if(c == '/' && c1 == '/')
	{
		buffer.append(1, c1);
		state.inside_line_comment = true;
		i += 1;
		return;
	}

	if(c == '/' && c1 == '*')
	{
		buffer.append(1, c1);
		state.inside_block_comment = true;
		i += 1;
		return;
	}

	if(c == '"' || c == '\'')
	{
		state.inside_quote = true;
		state.quote_char = c;
	}
}

String compiler_capture_markup_literal(const String& content, u32& i)
{
	String buffer = "";
	u32 depth = 0;
	bool inside_code = false;
	CompilerCodeState code_state;
	u32 j = i + 2;

	while(j < content.length())
	{
		char c = content[j];
		char c1 = (j + 1 < content.length()) ? content[j + 1] : '\0';
		char c2 = (j + 2 < content.length()) ? content[j + 2] : '\0';

		if(inside_code)
		{
			if(compiler_code_state_is_neutral(code_state) && c == '?' && c1 == '>')
			{
				buffer.append(1, c);
				buffer.append(1, c1);
				inside_code = false;
				j += 2;
				continue;
			}

			compiler_code_state_consume(code_state, buffer, content, j);
			j += 1;
			continue;
		}

		if(c == '<' && c1 == '?')
		{
			inside_code = true;
			code_state = CompilerCodeState();
			buffer.append(1, c);
			buffer.append(1, c1);
			j += 2;
			continue;
		}

		if(c == '<' && c1 == '/' && c2 == '>')
		{
			if(depth > 0)
			{
				depth -= 1;
				buffer.append("</>");
				j += 3;
				continue;
			}

			i = j + 2;
			return(buffer);
		}

		if(c == '<' && c1 == '>')
		{
			depth += 1;
			buffer.append("<>");
			j += 2;
			continue;
		}

		buffer.append(1, c);
		j += 1;
	}

	i = content.length();
	return(buffer);
}

String compiler_capture_code_until_php_close(const String& content, u32& i)
{
	String buffer = "";
	CompilerCodeState code_state;

	while(i < content.length())
	{
		char c = content[i];
		char c1 = (i + 1 < content.length()) ? content[i + 1] : '\0';

		if(compiler_code_state_is_neutral(code_state) && c == '?' && c1 == '>')
		{
			i += 1;
			return(buffer);
		}

		compiler_code_state_consume(code_state, buffer, content, i);
		i += 1;
	}

	return(buffer);
}

void compiler_append_text_literal_output(String& parsed_content, String& literal_buffer)
{
	if(literal_buffer == "")
		return;
	parsed_content.append("print(" + compiler_cpp_string_literal(literal_buffer) + ");");
	literal_buffer.clear();
}

String compiler_process_text_literal(Request* context, SharedUnit* su, String content)
{
	String parsed_content;
	String code_buffer = "";
	String literal_buffer = "";
	CompilerCodeState code_state;
	bool inside_code = false;
	bool is_field = false;
	bool escape_field = false;

	for(u32 i = 0; i < content.length(); i++)
	{
		char c = content[i];
		char c1 = (i + 1 < content.length()) ? content[i + 1] : '\0';
		char c2 = (i + 2 < content.length()) ? content[i + 2] : '\0';

		if(!inside_code)
		{
			if(c == '<' && c1 == '?')
			{
				inside_code = true;
				code_buffer = "";
				code_state = CompilerCodeState();
				compiler_append_text_literal_output(parsed_content, literal_buffer);
				if(c2 == '=')
				{
					is_field = true;
					escape_field = true;
					i += 2;
				}
				else if(c2 == ':')
				{
					is_field = true;
					escape_field = false;
					i += 2;
				}
				else
				{
					is_field = false;
					escape_field = false;
					i += 1;
				}
				continue;
			}

			literal_buffer.append(1, c);
			continue;
		}

		if(compiler_code_state_is_neutral(code_state) && c == '?' && c1 == '>')
		{
			inside_code = false;
			i += 1;
			if(is_field)
			{
				if(escape_field)
				{
					parsed_content.append(
						"print(html_escape( " +
						code_buffer +
						" )); "
					);
				}
				else
				{
					parsed_content.append(
						"print( " +
						code_buffer +
						" ); "
					);
				}
			}
			else
			{
				parsed_content.append(code_buffer);
			}
			continue;
		}

		if(compiler_code_state_is_neutral(code_state) && c == '<' && c1 == '>')
		{
			String nested_markup = compiler_capture_markup_literal(content, i);
			code_buffer.append(compiler_process_text_literal(context, su, nested_markup));
			continue;
		}

		compiler_code_state_consume(code_state, code_buffer, content, i);
	}

	if(literal_buffer != "")
		compiler_append_text_literal_output(parsed_content, literal_buffer);

	return(parsed_content);
}

bool compiler_line_starts_entrypoint(String trimmed, String macro_name)
{
	if(trimmed.rfind(macro_name + "(", 0) == 0)
		return(true);
	return(trimmed.rfind(macro_name + ":", 0) == 0 && trimmed.find("(") != String::npos);
}

bool compiler_line_starts_fragmentable_entrypoint(String trimmed, String& kind)
{
	if(compiler_line_starts_entrypoint(trimmed, "ONCE"))
	{
		kind = "ONCE";
		return(true);
	}
	if(compiler_line_starts_entrypoint(trimmed, "RENDER"))
	{
		kind = "RENDER";
		return(true);
	}
	if(compiler_line_starts_entrypoint(trimmed, "COMPONENT"))
	{
		kind = "COMPONENT";
		return(true);
	}
	return(false);
}

String compiler_fragment_capture_prelude(String slot)
{
	return("\nUceFragmentCapture __uce_fragment_capture(context, " + compiler_cpp_string_literal(slot) + ");\n");
}

String compiler_rewrite_fragment_attributes(String content)
{
	StringList lines = split(content, "\n");
	String result;
	for(u32 i = 0; i < lines.size(); i++)
	{
		String line = lines[i];
		String kind;
		if(!compiler_line_starts_fragmentable_entrypoint(trim(line), kind))
		{
			result += line;
			if(i + 1 < lines.size())
				result += "\n";
			continue;
		}

		String slot = (kind == "ONCE" ? "once" : "");
		bool has_fragment_attr = false;
		u32 j = i + 1;
		while(j < lines.size())
		{
			String attr_line = lines[j];
			String attr_trimmed = trim(attr_line);
			if(attr_trimmed.rfind("@fragment", 0) == 0 && (attr_trimmed.length() == 9 || isspace((unsigned char)attr_trimmed[9])))
			{
				slot = trim(attr_trimmed.substr(9));
				has_fragment_attr = true;
				j++;
				continue;
			}
			break;
		}

		bool should_capture = (slot != "") && (kind == "ONCE" || has_fragment_attr);
		if(!should_capture)
		{
			result += line;
			if(j < lines.size())
				result += "\n";
			i = j - 1;
			continue;
		}

		auto declaration_brace_pos = line.find("{");
		if(declaration_brace_pos != String::npos)
		{
			result += line.substr(0, declaration_brace_pos + 1) + compiler_fragment_capture_prelude(slot) + line.substr(declaration_brace_pos + 1);
			if(j < lines.size())
				result += "\n";
			i = j - 1;
			continue;
		}

		// The body must open on the next non-blank line after the attribute
		// lines; anything else (e.g. literal text that merely starts with
		// "ONCE(") is not an entry point and passes through untouched.
		u32 body_index = j;
		while(body_index < lines.size() && trim(lines[body_index]) == "")
			body_index++;
		bool body_opens = body_index < lines.size() && trim(lines[body_index]).rfind("{", 0) == 0;
		if(!body_opens)
		{
			result += line;
			if(i + 1 < lines.size())
				result += "\n";
			continue;
		}

		result += line;
		result += "\n";
		for(u32 k = j; k < body_index; k++)
			result += lines[k] + "\n";
		String body_line = lines[body_index];
		auto brace_pos = body_line.find("{");
		result += body_line.substr(0, brace_pos + 1) + compiler_fragment_capture_prelude(slot) + body_line.substr(brace_pos + 1);
		if(body_index + 1 < lines.size())
			result += "\n";
		i = body_index;
	}
	return(result);
}

bool compiler_rewrite_named_entrypoint_line(String& line, String macro_prefix, String symbol_prefix)
{
	u32 indent_length = 0;
	while(indent_length < line.length() && isspace(line[indent_length]))
		indent_length += 1;

	String indent = line.substr(0, indent_length);
	String trimmed = trim(line);
	if(trimmed.rfind(macro_prefix, 0) != 0)
		return(false);

	String signature = trimmed.substr(macro_prefix.length());
	auto open_paren_pos = signature.find("(");
	if(open_paren_pos == String::npos)
		return(false);

	String render_name = trim(signature.substr(0, open_paren_pos));
	String render_signature = signature.substr(open_paren_pos);
	if(render_name == "")
		return(false);

	line = indent + "EXPORT void " + symbol_prefix + "_" + safe_name(render_name) + render_signature;
	return(true);
}

String compiler_rewrite_named_render_syntax(String content)
{
	String result = "";
	String current_line = "";

	auto flush_line = [&]() {
		if(current_line.length() == 0)
			return;

		String line = current_line;
		String line_break = "";
		if(line.length() > 0 && line.back() == '\n')
		{
			line_break = "\n";
			line.pop_back();
		}

		compiler_rewrite_named_entrypoint_line(line, "RENDER:", UCE_NAMED_RENDER_SYMBOL) ||
		compiler_rewrite_named_entrypoint_line(line, "COMPONENT:", UCE_NAMED_COMPONENT_SYMBOL) ||
		compiler_rewrite_named_entrypoint_line(line, "SERVE_HTTP:", UCE_NAMED_SERVE_HTTP_SYMBOL);

		result += line + line_break;
		current_line = "";
	};

	for(auto c : content)
	{
		current_line.append(1, c);
		if(c == '\n')
			flush_line();
	}
	flush_line();

	return(result);
}

String compiler_preprocess_shared_unit_char_wise(Request* context, SharedUnit* su, String content)
{
	String parsed_content =
		"#include \"uce_lib.h\" \n"+
		file_get_contents(
			context->server->config["COMPILER_SYS_PATH"] + "/" + context->server->config["SETUP_TEMPLATE"]
			)+
		"#line 1 " + json_escape(su->file_name) + "\n";
	CompilerCodeState code_state;
	String current_line = "";
	String literal_buffer = "";
	bool inside_literal = false;

	for(u32 i = 0; i < content.length(); i++)
	{
		char c = content[i];
		char c1 = (i + 1 < content.length()) ? content[i + 1] : '\0';
		char c2 = (i + 2 < content.length()) ? content[i + 2] : '\0';

		if(inside_literal)
		{
			if(c == '<' && c1 == '?' && (c2 == '=' || c2 == ':'))
			{
				compiler_append_text_literal_output(parsed_content, literal_buffer);
				bool escape_field = (c2 == '=');
				i += 3;
				String field_code = compiler_capture_code_until_php_close(content, i);
				if(escape_field)
					parsed_content.append("print(html_escape( " + field_code + " )); ");
				else
					parsed_content.append("print( " + field_code + " ); ");
				continue;
			}

			if(c == '<' && c1 == '?')
			{
				compiler_append_text_literal_output(parsed_content, literal_buffer);
				inside_literal = false;
				code_state = CompilerCodeState();
				i += 1;
				continue;
			}

			if(c == '<' && c1 == '/' && c2 == '>')
			{
				compiler_append_text_literal_output(parsed_content, literal_buffer);
				inside_literal = false;
				i += 2;
				continue;
			}

			literal_buffer.append(1, c);
			continue;
		}

		if(compiler_code_state_is_neutral(code_state) && ((c == '<' && c1 == '>') || (c == '?' && c1 == '>')))
		{
			inside_literal = true;
			literal_buffer = "";
			current_line = "";
			i += 1;
			continue;
		}

		current_line.append(1, c);

		compiler_code_state_consume(code_state, parsed_content, content, i);

		if(c == 10 && current_line.substr(0, 6) == "#load ")
		{
			parsed_content.resize(parsed_content.length() - current_line.length());
			nibble(current_line, "\"");
			String unit_name = nibble(current_line, "\"");
			String resolved_unit = unit_name;
			if(resolved_unit != "" && resolved_unit[0] != '/')
				resolved_unit = expand_path(resolved_unit, su->src_path);
			SharedUnit* sub_su = (resolved_unit == "" ? 0 : get_shared_unit_for_preprocess(context, resolved_unit));
			if(sub_su)
				parsed_content.append("#include \"" + sub_su->bin_path + "/" + sub_su->pre_file_name + "\"\n");
		}
		else
		{
			String trimmed_line = trim(current_line);
			if(c == 10 && trimmed_line.length() > 7 && trimmed_line.substr(0, 6) == "EXPORT" && isspace(trimmed_line[6]))
			{
				current_line = "";
				auto end_declaration_pos = content.find("{", i);
				if(end_declaration_pos != std::string::npos)
				{
					parsed_content.append(1, '\n');
					String declaration = trim(content.substr(i, end_declaration_pos - i));
					su->api_declarations.push_back(declaration+";\n");
				}
			}
		}

		if(c == 10)
			current_line = "";
	}

	if(inside_literal)
		compiler_append_text_literal_output(parsed_content, literal_buffer);

	return(parsed_content);
}

}

String compiler_preprocess_source(Request* context, SharedUnit* su, String content)
{
	content = compiler_rewrite_fragment_attributes(content);
	content = compiler_rewrite_named_render_syntax(content);
	return(compiler_preprocess_shared_unit_char_wise(context, su, content));
}
