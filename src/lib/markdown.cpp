DValue markdown_to_ast(String src, DValue options);
String markdown_to_html(String src, DValue options);

bool markdown_has_key(DValue& tree, String key)
{
	return(tree.type == 'M' && tree._map.count(key) > 0);
}

DValue markdown_get_value(DValue& tree, String key)
{
	if(markdown_has_key(tree, key))
		return(tree._map[key]);
	return(DValue());
}

String markdown_get_string(DValue& tree, String key, String default_value = "")
{
	if(markdown_has_key(tree, key))
		return(tree._map[key].to_string());
	return(default_value);
}

bool markdown_truthy(DValue value, bool default_value = false)
{
	switch(value.type)
	{
		case('B'):
			return(value._bool);
		case('F'):
			return(value._float != 0);
		case('S'):
		{
			String raw = to_lower(trim(value._String));
			if(raw == "true" || raw == "1" || raw == "yes" || raw == "on" || raw == "(true)")
				return(true);
			if(raw == "false" || raw == "0" || raw == "no" || raw == "off" || raw == "(false)")
				return(false);
			return(default_value);
		}
		case('M'):
			return(!value._map.empty());
		default:
			return(default_value);
	}
}

bool markdown_get_bool(DValue& tree, String key, bool default_value = false)
{
	if(markdown_has_key(tree, key))
		return(markdown_truthy(tree._map[key], default_value));
	return(default_value);
}

bool markdown_gfm_enabled(DValue& options)
{
	return(markdown_get_bool(options, "gfm", true));
}

bool markdown_allow_html(DValue& options)
{
	return(markdown_get_bool(options, "allow_html", false));
}

String markdown_get_component_target(DValue& options, String hook)
{
	if(!markdown_has_key(options, "components"))
		return("");
	DValue components = options._map["components"];
	if(!markdown_has_key(components, hook))
		return("");
	return(trim(components._map[hook].to_string()));
}

DValue markdown_make_node(String type)
{
	DValue node;
	node["type"] = type;
	return(node);
}

void markdown_push(DValue& parent, DValue child)
{
	parent.push(child);
}

bool markdown_is_blank(String line)
{
	return(trim(line) == "");
}

s64 markdown_count_indent(String line)
{
	s64 result = 0;
	while(result < (s64)line.length() && line[result] == ' ')
		result += 1;
	return(result);
}

bool markdown_is_digits(String s)
{
	if(s == "")
		return(false);
	for(auto c : s)
	{
		if(!isdigit((unsigned char)c))
			return(false);
	}
	return(true);
}

bool markdown_is_identifier(String s)
{
	if(s == "")
		return(false);
	for(auto c : s)
	{
		if(!(isalnum((unsigned char)c) || c == '_' || c == '-'))
			return(false);
	}
	return(true);
}

String markdown_strip_newlines(String s)
{
	s = replace(s, "\r\n", "\n");
	s = replace(s, "\r", "\n");
	return(s);
}

String markdown_trim_quotes(String s)
{
	s = trim(s);
	if(s.length() >= 2)
	{
		char first = s.front();
		char last = s.back();
		if((first == '"' && last == '"') || (first == '\'' && last == '\''))
			return(s.substr(1, s.length() - 2));
	}
	return(s);
}

String markdown_read_quoted(String raw, s64& i)
{
	String result = "";
	if(i >= (s64)raw.length())
		return(result);
	char quote = raw[i];
	i += 1;
	while(i < (s64)raw.length())
	{
		char c = raw[i];
		if(c == '\\' && i + 1 < (s64)raw.length())
		{
			result.append(1, raw[i + 1]);
			i += 2;
			continue;
		}
		if(c == quote)
		{
			i += 1;
			return(result);
		}
		result.append(1, c);
		i += 1;
	}
	return(result);
}

DValue markdown_parse_attrs(String raw, String& argument_out)
{
	DValue attrs;
	argument_out = "";
	s64 i = 0;
	while(i < (s64)raw.length())
	{
		while(i < (s64)raw.length() && isspace((unsigned char)raw[i]))
			i += 1;
		if(i >= (s64)raw.length())
			break;

		String token = "";
		if(raw[i] == '"' || raw[i] == '\'')
		{
			token = markdown_read_quoted(raw, i);
			if(argument_out != "")
				argument_out += " ";
			argument_out += token;
			continue;
		}

		s64 token_start = i;
		while(i < (s64)raw.length() && !isspace((unsigned char)raw[i]))
		{
			if(raw[i] == '=')
				break;
			i += 1;
		}
		token = raw.substr(token_start, i - token_start);

		if(i < (s64)raw.length() && raw[i] == '=' && markdown_is_identifier(token))
		{
			i += 1;
			String value = "";
			if(i < (s64)raw.length() && (raw[i] == '"' || raw[i] == '\''))
				value = markdown_read_quoted(raw, i);
			else
			{
				s64 value_start = i;
				while(i < (s64)raw.length() && !isspace((unsigned char)raw[i]))
					i += 1;
				value = raw.substr(value_start, i - value_start);
			}
			attrs[token] = markdown_trim_quotes(value);
		}
		else
		{
			if(argument_out != "")
				argument_out += " ";
			argument_out += token;
		}
	}
	return(attrs);
}

StringList markdown_sorted_keys(DValue tree)
{
	StringList keys;
	if(tree.type != 'M')
		return(keys);
	for(auto& it : tree._map)
		keys.push_back(it.first);
	std::sort(keys.begin(), keys.end(), [] (String a, String b) {
		bool a_numeric = markdown_is_digits(a);
		bool b_numeric = markdown_is_digits(b);
		if(a_numeric && b_numeric)
			return(int_val(a) < int_val(b));
		if(a_numeric != b_numeric)
			return(a_numeric);
		return(a < b);
	});
	return(keys);
}

StringList markdown_split_pipe_row(String raw)
{
	StringList result;
	String cell = "";
	bool escaped = false;
	for(auto c : raw)
	{
		if(escaped)
		{
			cell.append(1, c);
			escaped = false;
			continue;
		}
		if(c == '\\')
		{
			escaped = true;
			continue;
		}
		if(c == '|')
		{
			result.push_back(trim(cell));
			cell = "";
		}
		else
		{
			cell.append(1, c);
		}
	}
	result.push_back(trim(cell));
	if(result.size() > 0 && result.front() == "")
		result.erase(result.begin());
	if(result.size() > 0 && result.back() == "")
		result.pop_back();
	return(result);
}

bool markdown_parse_table_separator(String raw, StringList& alignments)
{
	alignments.clear();
	StringList cells = markdown_split_pipe_row(raw);
	if(cells.size() == 0)
		return(false);
	for(auto cell : cells)
	{
		cell = trim(cell);
		if(cell == "")
			return(false);
		String align = "";
		bool left = cell.front() == ':';
		bool right = cell.back() == ':';
		String test = cell;
		if(left)
			test = test.substr(1);
		if(right && test.length() > 0)
			test = test.substr(0, test.length() - 1);
		test = trim(test);
		if(test.length() < 3)
			return(false);
		for(auto c : test)
		{
			if(c != '-')
				return(false);
		}
		if(left && right)
			align = "center";
		else if(left)
			align = "left";
		else if(right)
			align = "right";
		else
			align = "";
		alignments.push_back(align);
	}
	return(true);
}

bool markdown_parse_fence(String raw, char& fence_char, s64& fence_length, String& info)
{
	String trimmed = trim(raw);
	if(trimmed.length() < 3)
		return(false);
	char c = trimmed[0];
	if(c != '`' && c != '~')
		return(false);
	s64 i = 0;
	while(i < (s64)trimmed.length() && trimmed[i] == c)
		i += 1;
	if(i < 3)
		return(false);
	fence_char = c;
	fence_length = i;
	info = trim(trimmed.substr(i));
	return(true);
}

bool markdown_is_hr(String raw)
{
	String trimmed = trim(raw);
	if(trimmed.length() < 3)
		return(false);
	char marker = trimmed[0];
	if(marker != '-' && marker != '*' && marker != '_')
		return(false);
	s64 mark_count = 0;
	for(auto c : trimmed)
	{
		if(c == marker)
			mark_count += 1;
		else if(!isspace((unsigned char)c))
			return(false);
	}
	return(mark_count >= 3);
}

bool markdown_parse_list_marker(String raw, bool& ordered, s64& marker_length, s64& start_number, String& content)
{
	ordered = false;
	marker_length = 0;
	start_number = 1;
	content = "";
	if(raw.length() < 2)
		return(false);
	char first = raw[0];
	if((first == '-' || first == '*' || first == '+') && raw.length() >= 2 && raw[1] == ' ')
	{
		ordered = false;
		marker_length = 2;
		content = raw.substr(2);
		return(true);
	}

	s64 i = 0;
	while(i < (s64)raw.length() && isdigit((unsigned char)raw[i]))
		i += 1;
	if(i == 0 || i + 1 >= (s64)raw.length())
		return(false);
	if((raw[i] == '.' || raw[i] == ')') && raw[i + 1] == ' ')
	{
		ordered = true;
		start_number = int_val(raw.substr(0, i));
		marker_length = i + 2;
		content = raw.substr(marker_length);
		return(true);
	}
	return(false);
}

bool markdown_parse_task_prefix(String& content, bool& checked)
{
	if(content.length() < 4 || content[0] != '[' || content[2] != ']' || content[3] != ' ')
		return(false);
	char marker = to_lower(String().append(1, content[1]))[0];
	if(marker != 'x' && marker != ' ')
		return(false);
	checked = (marker == 'x');
	content = content.substr(4);
	return(true);
}

bool markdown_parse_atx_heading(String raw, s64& level, String& content)
{
	String trimmed = trim(raw);
	if(trimmed == "" || trimmed[0] != '#')
		return(false);
	level = 0;
	while(level < (s64)trimmed.length() && trimmed[level] == '#')
		level += 1;
	if(level == 0 || level > 6)
		return(false);
	if(level >= (s64)trimmed.length() || !isspace((unsigned char)trimmed[level]))
		return(false);
	content = trim(trimmed.substr(level));
	while(content.length() > 0 && content.back() == '#')
	{
		String stripped = trim(content.substr(0, content.length() - 1));
		if(stripped != "")
			content = stripped;
		else
			break;
	}
	return(true);
}

bool markdown_parse_setext_heading(String current, String next, s64& level, String& content)
{
	current = trim(current);
	next = trim(next);
	if(current == "" || next.length() < 3)
		return(false);
	bool all_equals = true;
	bool all_dashes = true;
	for(auto c : next)
	{
		if(c != '=' && !isspace((unsigned char)c))
			all_equals = false;
		if(c != '-' && !isspace((unsigned char)c))
			all_dashes = false;
	}
	if(!all_equals && !all_dashes)
		return(false);
	level = all_equals ? 1 : 2;
	content = current;
	return(true);
}

bool markdown_parse_directive_open(String raw, String& name, String& argument, DValue& attrs)
{
	String trimmed = trim(raw);
	if(trimmed.rfind(":::", 0) != 0 || trimmed == ":::")
		return(false);
	String payload = trim(trimmed.substr(3));
	StringList parts = split_space(payload);
	if(parts.size() == 0)
		return(false);
	name = parts[0];
	if(!markdown_is_identifier(name))
		return(false);
	String remainder = trim(payload.substr(name.length()));
	attrs = markdown_parse_attrs(remainder, argument);
	return(true);
}

bool markdown_parse_autolink(String raw, s64 start, s64& end, String& href, String& text)
{
	String rest = raw.substr(start);
	if(rest.rfind("https://", 0) != 0 && rest.rfind("http://", 0) != 0)
		return(false);
	end = start;
	while(end < (s64)raw.length())
	{
		char c = raw[end];
		if(isspace((unsigned char)c) || c == '<' || c == '>')
			break;
		end += 1;
	}
	if(end <= start)
		return(false);
	href = raw.substr(start, end - start);
	while(href.length() > 0)
	{
		char last = href.back();
		if(last == '.' || last == ',' || last == '!' || last == '?' || last == ';' || last == ':')
		{
			href.pop_back();
			end -= 1;
		}
		else
		{
			break;
		}
	}
	if(href == "")
		return(false);
	text = href;
	return(true);
}

bool markdown_parse_link_target(String raw, String& url, String& title)
{
	raw = trim(raw);
	if(raw == "")
		return(false);
	if(raw.find(" ") == String::npos)
	{
		url = markdown_trim_quotes(raw);
		title = "";
		return(true);
	}
	String work = raw;
	url = markdown_trim_quotes(trim(nibble(work, " ")));
	title = markdown_trim_quotes(trim(work));
	return(url != "");
}

DValue markdown_parse_inline(String raw, DValue options);

void markdown_flush_text(String& buffer, DValue& children)
{
	if(buffer == "")
		return;
	DValue node = markdown_make_node("text");
	node["text"] = buffer;
	markdown_push(children, node);
	buffer = "";
}

DValue markdown_parse_inline(String raw, DValue options)
{
	DValue children;
	String buffer = "";
	bool gfm = markdown_gfm_enabled(options);
	bool allow_html = markdown_allow_html(options);

	for(s64 i = 0; i < (s64)raw.length(); i++)
	{
		char c = raw[i];
		char c1 = (i + 1 < (s64)raw.length()) ? raw[i + 1] : '\0';

		if(c == '\\' && i + 1 < (s64)raw.length())
		{
			buffer.append(1, raw[i + 1]);
			i += 1;
			continue;
		}

		if(c == '`')
		{
			s64 tick_count = 1;
			while(i + tick_count < (s64)raw.length() && raw[i + tick_count] == '`')
				tick_count += 1;
			String ticks(tick_count, '`');
			auto close_pos = raw.find(ticks, i + tick_count);
			if(close_pos != String::npos)
			{
				markdown_flush_text(buffer, children);
				DValue node = markdown_make_node("code");
				node["text"] = raw.substr(i + tick_count, close_pos - (i + tick_count));
				markdown_push(children, node);
				i = close_pos + tick_count - 1;
				continue;
			}
		}

		if(c == '!' && c1 == '[')
		{
			auto label_end = raw.find("]", i + 2);
			if(label_end != String::npos && label_end + 1 < (s64)raw.length() && raw[label_end + 1] == '(')
			{
				auto target_end = raw.find(")", label_end + 2);
				if(target_end != String::npos)
				{
					String url = "";
					String title = "";
					if(markdown_parse_link_target(raw.substr(label_end + 2, target_end - (label_end + 2)), url, title))
					{
						markdown_flush_text(buffer, children);
						DValue node = markdown_make_node("image");
						node["alt"] = raw.substr(i + 2, label_end - (i + 2));
						node["src"] = url;
						node["title"] = title;
						markdown_push(children, node);
						i = target_end;
						continue;
					}
				}
			}
		}

		if(c == '[')
		{
			auto label_end = raw.find("]", i + 1);
			if(label_end != String::npos && label_end + 1 < (s64)raw.length() && raw[label_end + 1] == '(')
			{
				auto target_end = raw.find(")", label_end + 2);
				if(target_end != String::npos)
				{
					String url = "";
					String title = "";
					if(markdown_parse_link_target(raw.substr(label_end + 2, target_end - (label_end + 2)), url, title))
					{
						markdown_flush_text(buffer, children);
						DValue node = markdown_make_node("link");
						node["href"] = url;
						node["title"] = title;
						node["children"] = markdown_parse_inline(raw.substr(i + 1, label_end - (i + 1)), options);
						markdown_push(children, node);
						i = target_end;
						continue;
					}
				}
			}
		}

		if(gfm && c == '~' && c1 == '~')
		{
			auto close_pos = raw.find("~~", i + 2);
			if(close_pos != String::npos)
			{
				markdown_flush_text(buffer, children);
				DValue node = markdown_make_node("strike");
				node["children"] = markdown_parse_inline(raw.substr(i + 2, close_pos - (i + 2)), options);
				markdown_push(children, node);
				i = close_pos + 1;
				continue;
			}
		}

		if((c == '*' && c1 == '*') || (c == '_' && c1 == '_'))
		{
			String delim = raw.substr(i, 2);
			auto close_pos = raw.find(delim, i + 2);
			if(close_pos != String::npos)
			{
				markdown_flush_text(buffer, children);
				DValue node = markdown_make_node("strong");
				node["children"] = markdown_parse_inline(raw.substr(i + 2, close_pos - (i + 2)), options);
				markdown_push(children, node);
				i = close_pos + 1;
				continue;
			}
		}

		if(c == '*' || c == '_')
		{
			String delim = raw.substr(i, 1);
			auto close_pos = raw.find(delim, i + 1);
			if(close_pos != String::npos)
			{
				markdown_flush_text(buffer, children);
				DValue node = markdown_make_node("em");
				node["children"] = markdown_parse_inline(raw.substr(i + 1, close_pos - (i + 1)), options);
				markdown_push(children, node);
				i = close_pos;
				continue;
			}
		}

		if(gfm)
		{
			s64 end = 0;
			String href = "";
			String text = "";
			if(markdown_parse_autolink(raw, i, end, href, text))
			{
				markdown_flush_text(buffer, children);
				DValue node = markdown_make_node("link");
				node["href"] = href;
				node["title"] = "";
				DValue link_children;
				DValue text_node = markdown_make_node("text");
				text_node["text"] = text;
				markdown_push(link_children, text_node);
				node["children"] = link_children;
				markdown_push(children, node);
				i = end - 1;
				continue;
			}
		}

		if(allow_html && c == '<')
		{
			auto close_pos = raw.find(">", i + 1);
			if(close_pos != String::npos)
			{
				String html = raw.substr(i, close_pos - i + 1);
				if(html.length() >= 3)
				{
					markdown_flush_text(buffer, children);
					DValue node = markdown_make_node("raw_html");
					node["html"] = html;
					markdown_push(children, node);
					i = close_pos;
					continue;
				}
			}
		}

		buffer.append(1, c);
	}

	markdown_flush_text(buffer, children);
	return(children);
}

DValue markdown_parse_blocks(StringList lines, DValue options);

DValue markdown_parse_paragraph(StringList& lines, s64& idx, s64 base_indent, DValue& options)
{
	StringList paragraph_lines;
	while(idx < (s64)lines.size())
	{
		if(markdown_is_blank(lines[idx]))
			break;
		if(markdown_count_indent(lines[idx]) < base_indent)
			break;
		if(paragraph_lines.size() > 0)
		{
			String current = lines[idx].substr(base_indent);
			s64 heading_level = 0;
			String heading_content = "";
			char fence_char = '\0';
			s64 fence_length = 0;
			String fence_info = "";
			bool ordered = false;
			s64 marker_length = 0;
			s64 start_number = 1;
			String list_content = "";
			String directive_name = "";
			String directive_argument = "";
			DValue directive_attrs;
			if(markdown_parse_atx_heading(current, heading_level, heading_content) ||
				markdown_parse_fence(current, fence_char, fence_length, fence_info) ||
				markdown_is_hr(current) ||
				trim(current).rfind(">", 0) == 0 ||
				markdown_parse_list_marker(current, ordered, marker_length, start_number, list_content) ||
				markdown_parse_directive_open(current, directive_name, directive_argument, directive_attrs))
				break;
			if(markdown_gfm_enabled(options) && idx + 1 < (s64)lines.size() && current.find("|") != String::npos &&
				markdown_count_indent(lines[idx + 1]) >= base_indent)
			{
				StringList alignments;
				if(markdown_parse_table_separator(lines[idx + 1].substr(base_indent), alignments))
					break;
			}
		}
		paragraph_lines.push_back(lines[idx].substr(base_indent));
		idx += 1;
	}

	DValue node = markdown_make_node("paragraph");
	String text = join(paragraph_lines, "\n");
	node["text"] = text;
	node["children"] = markdown_parse_inline(text, options);
	return(node);
}

DValue markdown_parse_list(StringList& lines, s64& idx, s64 base_indent, DValue& options)
{
	bool ordered = false;
	s64 marker_length = 0;
	s64 start_number = 1;
	String content = "";
	String raw = lines[idx].substr(base_indent);
	markdown_parse_list_marker(raw, ordered, marker_length, start_number, content);

	DValue node = markdown_make_node("list");
	node["ordered"].set_bool(ordered);
	node["start"] = (f64)start_number;

	while(idx < (s64)lines.size())
	{
		if(markdown_is_blank(lines[idx]))
			break;
		if(markdown_count_indent(lines[idx]) < base_indent)
			break;

		String line_raw = lines[idx].substr(base_indent);
		bool item_ordered = false;
		s64 item_marker_length = 0;
		s64 item_start_number = 1;
		String item_content = "";
		if(!markdown_parse_list_marker(line_raw, item_ordered, item_marker_length, item_start_number, item_content) || item_ordered != ordered)
			break;

		DValue item = markdown_make_node("list_item");
		if(markdown_gfm_enabled(options))
		{
			bool checked = false;
			String task_content = item_content;
			if(markdown_parse_task_prefix(task_content, checked))
			{
				item["task"].set_bool(true);
				item["checked"].set_bool(checked);
				item_content = task_content;
			}
		}

		StringList child_lines;
		child_lines.push_back(item_content);
		idx += 1;

		while(idx < (s64)lines.size())
		{
			if(markdown_is_blank(lines[idx]))
			{
				child_lines.push_back("");
				idx += 1;
				continue;
			}
			s64 indent = markdown_count_indent(lines[idx]);
			if(indent < base_indent + 2)
				break;

			String next_raw = lines[idx].substr(base_indent);
			bool next_item_ordered = false;
			s64 next_marker_length = 0;
			s64 next_start_number = 1;
			String next_content = "";
			if(indent == base_indent &&
				markdown_parse_list_marker(next_raw, next_item_ordered, next_marker_length, next_start_number, next_content) &&
				next_item_ordered == ordered)
				break;

			s64 consume_indent = std::min<s64>((s64)lines[idx].length(), base_indent + 2);
			child_lines.push_back(lines[idx].substr(consume_indent));
			idx += 1;
		}

		DValue child_doc = markdown_parse_blocks(child_lines, options);
		item["children"] = markdown_get_value(child_doc, "children");
		markdown_push(node["children"], item);
	}

	return(node);
}

DValue markdown_parse_blockquote(StringList& lines, s64& idx, s64 base_indent, DValue& options)
{
	StringList quote_lines;
	while(idx < (s64)lines.size())
	{
		if(markdown_is_blank(lines[idx]))
		{
			quote_lines.push_back("");
			idx += 1;
			continue;
		}
		if(markdown_count_indent(lines[idx]) < base_indent)
			break;
		String raw = lines[idx].substr(base_indent);
		String trimmed = trim(raw);
		if(trimmed.rfind(">", 0) != 0)
			break;
		s64 marker_pos = raw.find(">");
		String stripped = raw.substr(marker_pos + 1);
		if(stripped.rfind(" ", 0) == 0)
			stripped = stripped.substr(1);
		quote_lines.push_back(stripped);
		idx += 1;
	}

	DValue node = markdown_make_node("blockquote");
	DValue child_doc = markdown_parse_blocks(quote_lines, options);
	node["children"] = markdown_get_value(child_doc, "children");
	return(node);
}

DValue markdown_parse_code_block(StringList& lines, s64& idx, s64 base_indent)
{
	char fence_char = '\0';
	s64 fence_length = 0;
	String info = "";
	String raw = lines[idx].substr(base_indent);
	markdown_parse_fence(raw, fence_char, fence_length, info);
	String lang = trim(nibble(info, " "));
	String meta = trim(info);
	idx += 1;

	StringList body;
	while(idx < (s64)lines.size())
	{
		String current = lines[idx];
		String current_raw = current;
		if(markdown_count_indent(current) >= base_indent)
			current_raw = current.substr(base_indent);
		String trimmed = trim(current_raw);
		s64 close_length = 0;
		while(close_length < (s64)trimmed.length() && trimmed[close_length] == fence_char)
			close_length += 1;
		if(close_length >= fence_length)
		{
			idx += 1;
			break;
		}
		body.push_back(current_raw);
		idx += 1;
	}

	DValue node = markdown_make_node("code_block");
	node["text"] = join(body, "\n");
	node["lang"] = lang;
	node["meta"] = meta;
	return(node);
}

DValue markdown_parse_table(StringList& lines, s64& idx, s64 base_indent, DValue& options)
{
	DValue node = markdown_make_node("table");
	String header_line = lines[idx].substr(base_indent);
	String separator_line = lines[idx + 1].substr(base_indent);
	StringList alignments;
	markdown_parse_table_separator(separator_line, alignments);

	StringList header_cells = markdown_split_pipe_row(header_line);
	for(auto cell_text : header_cells)
	{
		DValue cell = markdown_make_node("table_cell");
		cell["text"] = cell_text;
		cell["children"] = markdown_parse_inline(cell_text, options);
		markdown_push(node["header"], cell);
	}

	for(auto align : alignments)
	{
		DValue cell_align;
		cell_align = align;
		markdown_push(node["align"], cell_align);
	}

	idx += 2;
	while(idx < (s64)lines.size())
	{
		if(markdown_is_blank(lines[idx]))
			break;
		if(markdown_count_indent(lines[idx]) < base_indent)
			break;
		String raw = lines[idx].substr(base_indent);
		if(raw.find("|") == String::npos)
			break;
		StringList cells = markdown_split_pipe_row(raw);
		if(cells.size() == 0)
			break;
		DValue row;
		for(auto cell_text : cells)
		{
			DValue cell = markdown_make_node("table_cell");
			cell["text"] = cell_text;
			cell["children"] = markdown_parse_inline(cell_text, options);
			markdown_push(row, cell);
		}
		markdown_push(node["rows"], row);
		idx += 1;
	}

	return(node);
}

DValue markdown_parse_directive(StringList& lines, s64& idx, s64 base_indent, DValue& options)
{
	String name = "";
	String argument = "";
	DValue attrs;
	markdown_parse_directive_open(lines[idx].substr(base_indent), name, argument, attrs);

	StringList body_lines;
	s64 depth = 1;
	idx += 1;
	while(idx < (s64)lines.size())
	{
		String current = lines[idx];
		String raw = current;
		if(markdown_count_indent(current) >= base_indent)
			raw = current.substr(base_indent);
		String trimmed = trim(raw);
		if(trimmed.rfind(":::", 0) == 0)
		{
			if(trimmed == ":::")
			{
				depth -= 1;
				if(depth == 0)
				{
					idx += 1;
					break;
				}
				body_lines.push_back(raw);
				idx += 1;
				continue;
			}
			String nested_name = "";
			String nested_argument = "";
			DValue nested_attrs;
			if(markdown_parse_directive_open(raw, nested_name, nested_argument, nested_attrs))
				depth += 1;
		}
		body_lines.push_back(raw);
		idx += 1;
	}

	DValue node = markdown_make_node("directive");
	node["name"] = name;
	node["argument"] = argument;
	node["attrs"] = attrs;
	node["body_source"] = join(body_lines, "\n");
	DValue child_doc = markdown_parse_blocks(body_lines, options);
	node["children"] = markdown_get_value(child_doc, "children");
	return(node);
}

DValue markdown_parse_raw_html_block(StringList& lines, s64& idx, s64 base_indent)
{
	StringList html_lines;
	while(idx < (s64)lines.size())
	{
		if(markdown_is_blank(lines[idx]))
			break;
		if(markdown_count_indent(lines[idx]) < base_indent)
			break;
		String raw = lines[idx].substr(base_indent);
		if(trim(raw).rfind("<", 0) != 0)
			break;
		html_lines.push_back(raw);
		idx += 1;
	}
	DValue node = markdown_make_node("raw_html");
	node["html"] = join(html_lines, "\n");
	return(node);
}

DValue markdown_parse_blocks(StringList lines, DValue options)
{
	DValue document = markdown_make_node("document");
	document["source"] = join(lines, "\n");
	s64 idx = 0;

	while(idx < (s64)lines.size())
	{
		if(markdown_is_blank(lines[idx]))
		{
			idx += 1;
			continue;
		}

		s64 base_indent = markdown_count_indent(lines[idx]);
		String raw = lines[idx].substr(base_indent);
		String trimmed = trim(raw);

		s64 atx_level = 0;
		String heading_content = "";
		if(markdown_parse_atx_heading(raw, atx_level, heading_content))
		{
			DValue node = markdown_make_node("heading");
			node["level"] = (f64)atx_level;
			node["text"] = heading_content;
			node["children"] = markdown_parse_inline(heading_content, options);
			markdown_push(document["children"], node);
			idx += 1;
			continue;
		}

		if(idx + 1 < (s64)lines.size() && markdown_count_indent(lines[idx + 1]) >= base_indent)
		{
			s64 setext_level = 0;
			String setext_content = "";
			if(markdown_parse_setext_heading(raw, lines[idx + 1].substr(base_indent), setext_level, setext_content))
			{
				DValue node = markdown_make_node("heading");
				node["level"] = (f64)setext_level;
				node["text"] = setext_content;
				node["children"] = markdown_parse_inline(setext_content, options);
				markdown_push(document["children"], node);
				idx += 2;
				continue;
			}
		}

		char fence_char = '\0';
		s64 fence_length = 0;
		String fence_info = "";
		if(markdown_parse_fence(raw, fence_char, fence_length, fence_info))
		{
			DValue node = markdown_parse_code_block(lines, idx, base_indent);
			markdown_push(document["children"], node);
			continue;
		}

		if(markdown_is_hr(raw))
		{
			DValue node = markdown_make_node("hr");
			markdown_push(document["children"], node);
			idx += 1;
			continue;
		}

		String directive_name = "";
		String directive_argument = "";
		DValue directive_attrs;
		if(markdown_parse_directive_open(raw, directive_name, directive_argument, directive_attrs))
		{
			DValue node = markdown_parse_directive(lines, idx, base_indent, options);
			markdown_push(document["children"], node);
			continue;
		}

		if(trimmed.rfind(">", 0) == 0)
		{
			DValue node = markdown_parse_blockquote(lines, idx, base_indent, options);
			markdown_push(document["children"], node);
			continue;
		}

		bool ordered = false;
		s64 marker_length = 0;
		s64 start_number = 1;
		String list_content = "";
		if(markdown_parse_list_marker(raw, ordered, marker_length, start_number, list_content))
		{
			DValue node = markdown_parse_list(lines, idx, base_indent, options);
			markdown_push(document["children"], node);
			continue;
		}

		if(markdown_gfm_enabled(options) && idx + 1 < (s64)lines.size() && raw.find("|") != String::npos &&
			markdown_count_indent(lines[idx + 1]) >= base_indent)
		{
			StringList alignments;
			if(markdown_parse_table_separator(lines[idx + 1].substr(base_indent), alignments))
			{
				DValue node = markdown_parse_table(lines, idx, base_indent, options);
				markdown_push(document["children"], node);
				continue;
			}
		}

		if(markdown_allow_html(options) && trimmed.rfind("<", 0) == 0)
		{
			DValue node = markdown_parse_raw_html_block(lines, idx, base_indent);
			markdown_push(document["children"], node);
			continue;
		}

		DValue node = markdown_parse_paragraph(lines, idx, base_indent, options);
		markdown_push(document["children"], node);
	}

	return(document);
}

String markdown_render_children(DValue children, DValue& options);
String markdown_render_node(DValue node, DValue& options);

String markdown_render_cell_children(DValue cell, DValue& options)
{
	return(markdown_render_children(markdown_get_value(cell, "children"), options));
}

String markdown_render_with_component_hook(DValue node, DValue& options, String hook, String children_html, String default_html)
{
	String type = markdown_get_string(node, "type");
	String target = "";
	String exact_hook = "";
	if(type == "directive")
	{
		exact_hook = ":::" + markdown_get_string(node, "name");
		target = markdown_get_component_target(options, exact_hook);
	}
	if(target == "")
		target = markdown_get_component_target(options, hook);
	if(target == "" || !context)
		return(default_html);
	if(target != "" && exact_hook != "")
		hook = exact_hook;

	DValue props;
	props["hook"] = hook;
	props["target"] = target;
	props["default_html"] = default_html;
	props["children_html"] = children_html;
	props["node"] = node;
	props["type"] = type;
	props["name"] = markdown_get_string(node, "name");
	props["argument"] = markdown_get_string(node, "argument");
	props["text"] = markdown_get_string(node, "text");
	props["lang"] = markdown_get_string(node, "lang");
	props["href"] = markdown_get_string(node, "href");
	props["src"] = markdown_get_string(node, "src");
	props["title"] = markdown_get_string(node, "title");
	props["options"] = options;
	return(component(target, props, *context));
}

String markdown_render_children(DValue children, DValue& options)
{
	String result = "";
	for(auto key : markdown_sorted_keys(children))
		result += markdown_render_node(children._map[key], options);
	return(result);
}

String markdown_render_node(DValue node, DValue& options)
{
	String type = markdown_get_string(node, "type");
	String hook = "node." + type;
	String children_html = markdown_render_children(markdown_get_value(node, "children"), options);
	String html = "";

	if(type == "document")
	{
		html = children_html;
	}
	else if(type == "paragraph")
	{
		html = "<p>" + children_html + "</p>";
	}
	else if(type == "heading")
	{
		s64 level = int_val(markdown_get_string(node, "level", "1"));
		s64 heading_offset = int_val(markdown_get_string(options, "heading_offset", "0"));
		if(heading_offset < -5) heading_offset = -5;
		if(heading_offset > 5) heading_offset = 5;
		level += heading_offset;
		if(level < 1) level = 1;
		if(level > 6) level = 6;
		html = String("<h") + level + ">" + children_html + String("</h") + level + ">";
	}
	else if(type == "text")
	{
		html = html_escape(markdown_get_string(node, "text"));
	}
	else if(type == "code")
	{
		html = "<code>" + html_escape(markdown_get_string(node, "text")) + "</code>";
	}
	else if(type == "strong")
	{
		html = "<strong>" + children_html + "</strong>";
	}
	else if(type == "em")
	{
		html = "<em>" + children_html + "</em>";
	}
	else if(type == "strike")
	{
		html = "<del>" + children_html + "</del>";
	}
	else if(type == "link")
	{
		String attrs = " href=\"" + html_escape(markdown_get_string(node, "href")) + "\"";
		String title = markdown_get_string(node, "title");
		if(title != "")
			attrs += " title=\"" + html_escape(title) + "\"";
		html = "<a" + attrs + ">" + children_html + "</a>";
	}
	else if(type == "image")
	{
		String attrs = " src=\"" + html_escape(markdown_get_string(node, "src")) + "\"";
		attrs += " alt=\"" + html_escape(markdown_get_string(node, "alt")) + "\"";
		String title = markdown_get_string(node, "title");
		if(title != "")
			attrs += " title=\"" + html_escape(title) + "\"";
		html = "<img" + attrs + "/>";
	}
	else if(type == "raw_html")
	{
		html = markdown_get_string(node, "html");
	}
	else if(type == "hr")
	{
		html = "<hr/>";
	}
	else if(type == "blockquote")
	{
		html = "<blockquote>" + children_html + "</blockquote>";
	}
	else if(type == "list")
	{
		bool ordered = markdown_get_bool(node, "ordered", false);
		String tag = ordered ? "ol" : "ul";
		String attrs = "";
		if(ordered)
		{
			s64 start = int_val(markdown_get_string(node, "start", "1"));
			if(start > 1)
				attrs = String(" start=\"") + start + "\"";
		}
		html = "<" + tag + attrs + ">" + children_html + "</" + tag + ">";
	}
	else if(type == "list_item")
	{
		String attrs = "";
		if(markdown_get_bool(node, "task", false))
			attrs = " class=\"task-list-item\"";
		html = "<li" + attrs + ">";
		if(markdown_get_bool(node, "task", false))
		{
			html += "<input type=\"checkbox\" disabled";
			if(markdown_get_bool(node, "checked", false))
				html += " checked";
			html += "/> ";
		}
		html += children_html + "</li>";
	}
	else if(type == "code_block")
	{
		String lang = markdown_get_string(node, "lang");
		String class_attr = "";
		if(lang != "")
			class_attr = " class=\"language-" + html_escape(lang) + "\"";
		html = "<pre><code" + class_attr + ">" + html_escape(markdown_get_string(node, "text")) + "</code></pre>";
	}
	else if(type == "table")
	{
		String header_html = "";
		StringList align_keys = markdown_sorted_keys(markdown_get_value(node, "align"));
		StringList header_keys = markdown_sorted_keys(markdown_get_value(node, "header"));
		for(s64 i = 0; i < (s64)header_keys.size(); i++)
		{
			String align = (i < (s64)align_keys.size()) ? node["align"]._map[align_keys[i]].to_string() : "";
			String align_attr = align != "" ? " align=\"" + html_escape(align) + "\"" : "";
			header_html += "<th" + align_attr + ">" + markdown_render_cell_children(node["header"]._map[header_keys[i]], options) + "</th>";
		}

		String body_html = "";
		for(auto row_key : markdown_sorted_keys(markdown_get_value(node, "rows")))
		{
			DValue row = node["rows"]._map[row_key];
			String row_html = "";
			StringList row_keys = markdown_sorted_keys(row);
			for(s64 i = 0; i < (s64)row_keys.size(); i++)
			{
				String align = (i < (s64)align_keys.size()) ? node["align"]._map[align_keys[i]].to_string() : "";
				String align_attr = align != "" ? " align=\"" + html_escape(align) + "\"" : "";
				row_html += "<td" + align_attr + ">" + markdown_render_cell_children(row._map[row_keys[i]], options) + "</td>";
			}
			body_html += "<tr>" + row_html + "</tr>";
		}

		html = "<table><thead><tr>" + header_html + "</tr></thead><tbody>" + body_html + "</tbody></table>";
	}
	else if(type == "directive")
	{
		String name = markdown_get_string(node, "name");
		String title = markdown_get_string(node["attrs"], "title", markdown_get_string(node, "argument"));
		String title_html = "";
		if(title != "")
			title_html = "<div class=\"md-directive-title\">" + markdown_render_children(markdown_parse_inline(title, options), options) + "</div>";
		html = "<div class=\"md-directive md-directive-" + safe_name(name) + "\">" + title_html + children_html + "</div>";
	}
	else
	{
		html = children_html;
	}

	return(markdown_render_with_component_hook(node, options, hook, children_html, html));
}

DValue markdown_to_ast(String src)
{
	DValue options;
	return(markdown_to_ast(src, options));
}

DValue markdown_to_ast(String src, DValue options)
{
	src = markdown_strip_newlines(src);
	return(markdown_parse_blocks(split(src, "\n"), options));
}

String markdown_to_html(String src)
{
	DValue options;
	return(markdown_to_html(src, options));
}

String markdown_to_html(String src, DValue options)
{
	DValue ast = markdown_to_ast(src, options);
	return(markdown_render_node(ast, options));
}
