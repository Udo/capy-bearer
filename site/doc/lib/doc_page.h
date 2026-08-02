#pragma once
#include "capy_signatures.generated.h"

struct DocPage {
	String title;
	String content;
	DValue sig_lines;
	DValue capy_sig_lines;
	DValue param_lines;
	DValue example_blocks;
	DValue example_pairs;
	DValue cpp_only_examples;
	DValue guide_examples;
	String capy_status;
	bool has_legacy_examples = false;
	String example_parse_error;
	DValue see_lines;
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

bool doc_page_name_is_safe(String name)
{
	if(name == "")
		return(false);
	for(char c : name)
		if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ' || c == '+'))
			return(false);
	return(true);
}

DValue doc_guide_redirects()
{
	DValue redirects;
	redirects["01-getting-started"] = "01-install-and-first-program";
	redirects["02-basic-syntax"] = "02-source-structure-and-syntax";
	redirects["03-types"] = "03-values-and-types";
	redirects["04-variables-scope-and-expressions"] = "04-expressions-and-control-flow";
	redirects["05-operators-and-control-flow"] = "04-expressions-and-control-flow";
	redirects["06-functions"] = "05-functions-and-closures";
	redirects["07-strings-and-markup"] = "06-strings-and-markup";
	redirects["08-arrays-tuples-and-structs"] = "07-collections-and-records";
	redirects["09-function-values-closures-and-memory"] = "05-functions-and-closures";
	redirects["10-dvalues"] = "08-dynamic-values";
	redirects["11-web-handlers"] = "09-web-handlers-and-requests";
	redirects["12-components-and-units"] = "10-units-components-and-exports";
	redirects["13-tasks-and-jobs"] = "11-tasks-and-jobs";
	redirects["14-errors-debugging-and-style"] = "12-errors-testing-and-style";
	return(redirects);
}

String doc_canonical_page(String page)
{
	if(page.rfind("capy-", 0) != 0)
		return(page);
	String target = doc_guide_redirects()[page.substr(5)].to_string();
	return(target == "" ? page : "capy-" + target);
}

String doc_page_path(String name)
{
	if(!doc_page_name_is_safe(name))
		return("");
	name = doc_canonical_page(name);
	if(name.rfind("capy-", 0) != 0)
		return("pages/" + name + ".txt");
	String canonical_path = "capy/" + name.substr(5) + ".txt";
	if(file_exists(canonical_path))
		return(canonical_path);
	for(String file_name : ls("capy/"))
	{
		String source = nibble(file_name, ".");
		if(doc_canonical_page("capy-" + source) == name)
			return("capy/" + source + ".txt");
	}
	return("");
}

String doc_page_source(String name)
{
	String path = doc_page_path(name);
	return(path == "" ? "" : file_get_contents(path));
}

String doc_page_title_from_source(String source)
{
	u64 start = source.find(":title\n");
	if(start == String::npos)
		return("");
	start += 7;
	u64 end = source.find("\n:", start);
	return(trim(source.substr(start, end == String::npos ? String::npos : end - start)));
}

DValue doc_page_names()
{
	DValue names;
	for(String file_name : ls("pages/").sort())
	{
		String remaining = file_name;
		names.push(nibble(remaining, "."));
	}
	DValue canonical_guides;
	for(String file_name : ls("capy/").sort())
	{
		String remaining = file_name;
		String guide = doc_canonical_page("capy-" + nibble(remaining, "."));
		if(canonical_guides[guide].to_string() == "")
		{
			canonical_guides[guide] = "Y";
			names.push(guide);
		}
	}
	return(names);
}

bool doc_has_page(String name)
{
	String path = doc_page_path(name);
	return(path != "" && file_exists(path));
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

bool doc_example_entry_is_valid(String entry)
{
	return(entry == "render" || entry == "cli" || entry == "component" || entry == "init" || entry == "once" || entry == "ws");
}

String doc_example_language_label(String language)
{
	return(language == "capy" ? "Capy" : "C++ (.uce)");
}

String doc_example_handler_name(String entry)
{
	if(entry == "render")
		return("RENDER");
	if(entry == "cli")
		return("CLI");
	if(entry == "component")
		return("COMPONENT");
	if(entry == "init")
		return("INIT");
	if(entry == "once")
		return("ONCE");
	return("WS");
}

void doc_flush_section(DocPage& result, String page, String section, DValue& section_lines, DValue& content_lines)
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
	else if(section == "output")
	{
		if(result.guide_examples.size() == 0)
			result.example_parse_error = "output must follow a Capy guide example";
		else
			result.guide_examples[result.guide_examples.size() - 1]["output"] = join(section_lines, "\n");
	}
	else
	{
		for(String line : section_lines)
			content_lines.push_back(line);
	}
}

void doc_add_example(DocPage& result, String page, String language, String entry, String body, String& pending_entry, String& pending_body)
{
	if(language == "legacy")
	{
		result.has_legacy_examples = true;
		result.example_blocks.push_back(body);
		return;
	}
	if(!doc_example_entry_is_valid(entry))
	{
		result.example_parse_error = "unknown example entry: " + entry;
		return;
	}
	if(pending_entry == "")
	{
		if(page.rfind("capy-", 0) == 0 && language == "capy")
		{
			DValue example;
			example["entry"] = entry;
			example["body"] = body;
			result.guide_examples.push_back(example);
		}
		else if(language == "cpp" && result.capy_status != "")
		{
			DValue example;
			example["entry"] = entry;
			example["body"] = body;
			result.cpp_only_examples[result.cpp_only_examples.size()] = example;
		}
		else if(language != "capy")
			result.example_parse_error = "typed examples must start with capy, got " + language;
		else
		{
			pending_entry = entry;
			pending_body = body;
		}
		return;
	}
	if(language != "cpp" || entry != pending_entry)
	{
		result.example_parse_error = "expected contiguous :example cpp " + pending_entry;
		return;
	}
	DValue pair;
	pair["entry"] = pending_entry;
	pair["capy_body"] = pending_body;
	pair["cpp_body"] = body;
	result.example_pairs[result.example_pairs.size()] = pair;
	pending_entry = "";
	pending_body = "";
}

DocPage load_doc_page(String page)
{
	page = doc_canonical_page(page);
	DocPage result;
	DValue lines = split(doc_page_source(page), "\n");
	String current_section = "";
	DValue current_lines;
	DValue content_lines;
	String pending_entry;
	String pending_body;
	String example_language;
	String example_entry;
	bool in_typed_example = false;

	for(auto line : lines)
	{
		if(current_section == "output" && line.rfind("## ", 0) == 0)
		{
			doc_flush_section(result, page, current_section, current_lines, content_lines);
			current_lines.clear();
			current_section = "content";
		}
		if(line != "" && line.substr(0, 1) == ":")
		{
			if(current_section == "example")
			{
				String example_body = join(current_lines, "\n");
				if(trim(example_body) == "")
					result.example_parse_error = "empty example block";
				else
					doc_add_example(result, page, example_language, example_entry, example_body, pending_entry, pending_body);
			}
			else
				doc_flush_section(result, page, current_section, current_lines, content_lines);
			current_lines.clear();

			String section = trim(line.substr(1));
			if(section == "example" || section.substr(0, 8) == "example ")
			{
				current_section = "example";
				example_language = "";
				example_entry = "";
				if(section == "example")
				{
					example_language = "legacy";
					example_entry = "render";
				}
				else
				{
					String header = trim(section.substr(8));
					String language = trim(nibble(header, " "));
					String entry = trim(header);
					if((language == "capy" || language == "cpp") && entry != "" && entry.find(" ") == String::npos)
					{
						example_language = language;
						example_entry = entry;
						in_typed_example = true;
					}
					else
						result.example_parse_error = "invalid example header: " + section;
				}
				continue;
			}

			if(in_typed_example && pending_entry != "")
				result.example_parse_error = "expected contiguous :example cpp " + pending_entry;
			if(section.substr(0, 12) == "capy-status ")
			{
				String status = trim(section.substr(12));
				if(status == "unsupported" || status == "cpp-specific")
					result.capy_status = status;
				else
					result.example_parse_error = "unknown Capy status: " + status;
				current_section = "";
				continue;
			}
			if(section == "title" || section == "sig" || section == "params" || section == "content" || section == "see" || section == "output")
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

	if(current_section == "example")
	{
		String example_body = join(current_lines, "\n");
		if(trim(example_body) == "")
			result.example_parse_error = "empty example block";
		else
			doc_add_example(result, page, example_language, example_entry, example_body, pending_entry, pending_body);
	}
	else
		doc_flush_section(result, page, current_section, current_lines, content_lines);
	if(pending_entry != "")
		result.example_parse_error = "expected contiguous :example cpp " + pending_entry;
	result.content = join(content_lines, "\n");
	result.title = trim(result.title);
	if(page.rfind("capy-", 0) != 0)
		result.capy_sig_lines = doc_capy_signature_lines(page);
	return(result);
}
