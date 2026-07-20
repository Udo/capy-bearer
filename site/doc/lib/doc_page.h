#pragma once

struct DocPage {
	String title;
	String content;
	StringList sig_lines;
	StringList param_lines;
	StringList example_blocks;
	StringList see_lines;
};

enum class DocPageKind
{
	function,
	struct_page,
	directive,
	method,
	info
};

String doc_method_label(String page)
{
	String label = page;
	nibble(label, "_");
	String class_name = nibble(label, "_");
	if(label == "")
		return(class_name);
	return(class_name + "::" + label);
}

String doc_default_title(String page)
{
	String page_title = page;
	if(page.substr(0, 2) == "2_")
		return(doc_method_label(page));
	if(page_title.length() > 1 && page_title[1] == '_')
		nibble(page_title, "_");
	return(page_title);
}

String doc_markdown_inline(String text)
{
	text = trim(text);
	if(text == "")
		return("");
	String html = markdown_to_html(text);
	if(html.length() >= 7 && html.substr(0, 3) == "<p>" && html.substr(html.length() - 4) == "</p>")
		return(html.substr(3, html.length() - 7));
	return(html);
}

String doc_legacy_heading(String section)
{
	if(section == "desc")
		return("");
	if(section == "related")
		return("## PHP & JS Equivalents");
	return("## " + section);
}

bool doc_has_area(String name)
{
	return(file_exists("areas/" + name + ".txt"));
}

bool doc_has_page(String name)
{
	return(file_exists("pages/" + name + ".txt"));
}

DocPageKind doc_page_kind(String page)
{
	if(page.substr(0, 2) == "0_")
		return(DocPageKind::struct_page);
	if(page.substr(0, 2) == "1_")
		return(DocPageKind::directive);
	if(page.substr(0, 2) == "2_")
		return(DocPageKind::method);
	if(page.substr(0, 2) == "3_")
		return(DocPageKind::info);
	return(DocPageKind::function);
}

String doc_page_kind_badge(DocPageKind kind)
{
	if(kind == DocPageKind::struct_page)
		return("struct");
	if(kind == DocPageKind::directive)
		return("directive");
	if(kind == DocPageKind::method)
		return("method");
	if(kind == DocPageKind::info)
		return("info");
	return("");
}

String doc_index_label(String page)
{
	String label = page;
	auto kind = doc_page_kind(page);
	if(kind == DocPageKind::method)
		return(doc_method_label(page));
	if(kind == DocPageKind::struct_page || kind == DocPageKind::directive || kind == DocPageKind::info)
		nibble(label, "_");
	return(label);
}

void doc_flush_section(DocPage& result, String page, String section, StringList& section_lines, StringList& content_lines)
{
	if(section == "")
		return;
	if(section == "title")
	{
		String title = trim(join(section_lines, "\n"));
		if(title != page)
			result.title = title;
	}
	else if(section == "sig")
	{
		for(String line : section_lines)
			result.sig_lines.push_back(line);
	}
	else if(section == "params")
	{
		for(String line : section_lines)
			result.param_lines.push_back(line);
	}
	else if(section == "see")
	{
		for(String line : section_lines)
		{
			line = trim(line);
			if(line != "")
				result.see_lines.push_back(line);
		}
	}
	else if(section == "example")
	{
		String example = join(section_lines, "\n");
		if(trim(example) != "")
			result.example_blocks.push_back(example);
	}
	else
	{
		for(String line : section_lines)
			content_lines.push_back(line);
	}
}

DocPage load_doc_page(String page)
{
	DocPage result;
	StringList lines = split(file_get_contents("pages/" + page + ".txt"), "\n");
	String current_section = "";
	StringList current_lines;
	StringList content_lines;

	for(auto line : lines)
	{
		if(line != "" && line.substr(0, 1) == ":")
		{
			doc_flush_section(result, page, current_section, current_lines, content_lines);
			current_lines.clear();

			String section = trim(line.substr(1));
			if(section == "title" || section == "sig" || section == "params" || section == "content" || section == "example" || section == "see")
			{
				current_section = section;
				continue;
			}

			current_section = "legacy";
			String heading = doc_legacy_heading(section);
			if(heading != "")
			{
				if(content_lines.size() > 0 && content_lines.back() != "")
					content_lines.push_back("");
				content_lines.push_back(heading);
				content_lines.push_back("");
			}
			continue;
		}

		current_lines.push_back(line);
	}

	doc_flush_section(result, page, current_section, current_lines, content_lines);
	result.content = join(content_lines, "\n");
	result.title = trim(result.title);
	return(result);
}
