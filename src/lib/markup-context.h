#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace bearer
{
enum class MarkupContext
{
	html_text,
	html_attribute,
	javascript_value,
	css_value,
	dynamic_name,
	unquoted_attribute,
	javascript_quoted,
	javascript_comment,
	javascript_boundary,
	css_quoted,
	css_comment,
	css_boundary,
	markup_declaration
};

inline const char* markup_context_error(MarkupContext context)
{
	switch (context)
	{
		case MarkupContext::dynamic_name: return "markup interpolation is not allowed in a tag or attribute name";
		case MarkupContext::unquoted_attribute: return "markup interpolation requires a quoted attribute value";
		case MarkupContext::javascript_quoted: return "markup interpolation must not be inside a JavaScript string or template literal";
		case MarkupContext::javascript_comment: return "markup interpolation must not be inside a JavaScript comment";
		case MarkupContext::javascript_boundary: return "markup interpolation must start at a JavaScript value boundary";
		case MarkupContext::css_quoted: return "markup interpolation must not be inside a CSS string";
		case MarkupContext::css_comment: return "markup interpolation must not be inside a CSS comment";
		case MarkupContext::css_boundary: return "markup interpolation must start at a CSS value boundary";
		case MarkupContext::markup_declaration: return "markup interpolation is not allowed in an HTML comment or declaration";
		default: return "";
	}
}

inline bool markup_context_is_safe(MarkupContext context)
{
	return context == MarkupContext::html_text || context == MarkupContext::html_attribute ||
		context == MarkupContext::javascript_value || context == MarkupContext::css_value;
}

class MarkupContextScanner
{
	enum class State
	{
		data,
		tag_open,
		tag_name,
		before_attribute,
		attribute_name,
		after_attribute_name,
		before_attribute_value,
		attribute_double,
		attribute_single,
		attribute_unquoted,
		declaration,
		comment,
		script,
		style
	};

	State state_ = State::data;
	std::string tag_;
	std::string attribute_;
	bool closing_tag_ = false;
	char raw_quote_ = 0;
	bool raw_escape_ = false;
	bool raw_line_comment_ = false;
	bool raw_block_comment_ = false;
	bool raw_pending_slash_ = false;
	bool raw_pending_star_ = false;
	char raw_last_significant_ = 0;
	char raw_comment_boundary_ = 0;
	std::string raw_word_;
	std::string raw_comment_word_;

	static char lower(char value)
	{
		return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
	}

	static bool name_character(char value)
	{
		return std::isalnum(static_cast<unsigned char>(value)) || value == '-' || value == '_' || value == ':';
	}

	static bool starts_case_insensitive(std::string_view text, std::size_t position, std::string_view expected)
	{
		if (text.size() - position < expected.size()) return false;
		for (std::size_t i = 0; i < expected.size(); ++i)
			if (lower(text[position + i]) != lower(expected[i])) return false;
		return true;
	}

	static bool raw_close(std::string_view text, std::size_t position, std::string_view tag)
	{
		if (!starts_case_insensitive(text, position, "</") || !starts_case_insensitive(text, position + 2, tag)) return false;
		const std::size_t end = position + 2 + tag.size();
		return end == text.size() || std::isspace(static_cast<unsigned char>(text[end])) || text[end] == '>' || text[end] == '/';
	}

	void finish_tag()
	{
		if (!closing_tag_ && tag_ == "script") state_ = State::script;
		else if (!closing_tag_ && tag_ == "style") state_ = State::style;
		else state_ = State::data;
		tag_.clear();
		attribute_.clear();
		closing_tag_ = false;
		raw_quote_ = 0;
		raw_escape_ = false;
		raw_line_comment_ = false;
		raw_block_comment_ = false;
		raw_pending_slash_ = false;
		raw_pending_star_ = false;
		raw_last_significant_ = 0;
		raw_comment_boundary_ = 0;
		raw_word_.clear();
		raw_comment_word_.clear();
	}

	void consume_raw(char value, bool javascript)
	{
		if (raw_line_comment_)
		{
			if (value == '\n' || value == '\r') raw_line_comment_ = false;
			return;
		}
		if (raw_block_comment_)
		{
			if (raw_pending_star_ && value == '/') raw_block_comment_ = false;
			raw_pending_star_ = value == '*';
			return;
		}
		if (raw_quote_)
		{
			if (raw_escape_) raw_escape_ = false;
			else if (value == '\\') raw_escape_ = true;
			else if (value == raw_quote_) { raw_last_significant_ = value; raw_word_.clear(); raw_quote_ = 0; }
			return;
		}
		if (raw_pending_slash_)
		{
			raw_pending_slash_ = false;
			if (javascript && value == '/') { raw_line_comment_ = true; raw_last_significant_ = raw_comment_boundary_; raw_word_ = raw_comment_word_; return; }
			if (value == '*') { raw_block_comment_ = true; raw_pending_star_ = false; raw_last_significant_ = raw_comment_boundary_; raw_word_ = raw_comment_word_; return; }
		}
		if (value == '/')
		{
			raw_comment_boundary_ = raw_last_significant_;
			raw_comment_word_ = raw_word_;
			raw_pending_slash_ = true;
			raw_last_significant_ = value;
			raw_word_.clear();
			return;
		}
		if (value == '"' || value == '\'' || (javascript && value == '`')) { raw_quote_ = value; raw_last_significant_ = value; raw_word_.clear(); }
		else if (std::isalnum(static_cast<unsigned char>(value)) || value == '_' || value == '$')
		{
			if (!(std::isalnum(static_cast<unsigned char>(raw_last_significant_)) || raw_last_significant_ == '_' || raw_last_significant_ == '$')) raw_word_.clear();
			raw_word_ += lower(value);
			raw_last_significant_ = value;
		}
		else if (!std::isspace(static_cast<unsigned char>(value))) { raw_last_significant_ = value; raw_word_.clear(); }
	}

	MarkupContext attribute_context() const
	{
		return MarkupContext::html_attribute;
	}

	bool javascript_value_boundary() const
	{
		if (raw_last_significant_ == 0 || std::string_view("=([{,:;?!+-*/%&|<>").find(raw_last_significant_) != std::string_view::npos) return true;
		return raw_word_ == "return" || raw_word_ == "throw" || raw_word_ == "case" || raw_word_ == "new" || raw_word_ == "typeof" ||
			raw_word_ == "void" || raw_word_ == "delete" || raw_word_ == "yield" || raw_word_ == "await" || raw_word_ == "in" || raw_word_ == "of" ||
			raw_word_ == "instanceof" || raw_word_ == "extends";
	}

	static bool css_value_boundary(char value)
	{
		return value == 0 || std::string_view("{:,;(/").find(value) != std::string_view::npos;
	}

public:
	void consume(std::string_view text)
	{
		for (std::size_t i = 0; i < text.size(); ++i)
		{
			char value = text[i];
			if (state_ == State::script || state_ == State::style)
			{
				if (state_ == State::script && raw_line_comment_ && i + 2 < text.size() && (unsigned char)value == 0xe2 &&
					(unsigned char)text[i + 1] == 0x80 && ((unsigned char)text[i + 2] == 0xa8 || (unsigned char)text[i + 2] == 0xa9))
				{
					consume_raw('\n', true);
					i += 2;
					continue;
				}
				const std::string_view tag = state_ == State::script ? std::string_view("script") : std::string_view("style");
				if (raw_close(text, i, tag))
				{
					state_ = State::data;
					raw_quote_ = 0;
					raw_escape_ = raw_line_comment_ = raw_block_comment_ = raw_pending_slash_ = raw_pending_star_ = false;
					raw_last_significant_ = 0;
					raw_comment_boundary_ = 0;
					raw_word_.clear();
					raw_comment_word_.clear();
					--i;
					continue;
				}
				consume_raw(value, state_ == State::script);
				continue;
			}

			switch (state_)
			{
				case State::data:
					if (value == '<') state_ = State::tag_open;
					break;
				case State::tag_open:
					if (value == '/') { closing_tag_ = true; tag_.clear(); state_ = State::tag_name; }
					else if (value == '!')
					{
						if (text.size() - i >= 3 && text.substr(i, 3) == "!--") { state_ = State::comment; i += 2; }
						else state_ = State::declaration;
					}
					else if (name_character(value)) { closing_tag_ = false; tag_.assign(1, lower(value)); state_ = State::tag_name; }
					else state_ = State::data;
					break;
				case State::tag_name:
					if (name_character(value)) tag_.push_back(lower(value));
					else if (value == '>') finish_tag();
					else if (std::isspace(static_cast<unsigned char>(value))) state_ = State::before_attribute;
					else if (value == '/') state_ = State::before_attribute;
					break;
				case State::before_attribute:
					if (value == '>') finish_tag();
					else if (!std::isspace(static_cast<unsigned char>(value)) && value != '/') { attribute_.assign(1, lower(value)); state_ = State::attribute_name; }
					break;
				case State::attribute_name:
					if (name_character(value)) attribute_.push_back(lower(value));
					else if (value == '=') state_ = State::before_attribute_value;
					else if (value == '>') finish_tag();
					else if (std::isspace(static_cast<unsigned char>(value))) state_ = State::after_attribute_name;
					break;
				case State::after_attribute_name:
					if (value == '=') state_ = State::before_attribute_value;
					else if (value == '>') finish_tag();
					else if (!std::isspace(static_cast<unsigned char>(value)) && value != '/') { attribute_.assign(1, lower(value)); state_ = State::attribute_name; }
					break;
				case State::before_attribute_value:
					if (value == '"') state_ = State::attribute_double;
					else if (value == '\'') state_ = State::attribute_single;
					else if (value == '>') finish_tag();
					else if (!std::isspace(static_cast<unsigned char>(value))) state_ = State::attribute_unquoted;
					break;
				case State::attribute_double:
					if (value == '"') state_ = State::before_attribute;
					break;
				case State::attribute_single:
					if (value == '\'') state_ = State::before_attribute;
					break;
				case State::attribute_unquoted:
					if (value == '>') finish_tag();
					else if (std::isspace(static_cast<unsigned char>(value))) state_ = State::before_attribute;
					break;
				case State::declaration:
					if (value == '>') state_ = State::data;
					break;
				case State::comment:
					if (value == '>' && i >= 2 && text[i - 1] == '-' && text[i - 2] == '-') state_ = State::data;
					break;
				case State::script:
				case State::style:
					break;
			}
		}
	}

	MarkupContext context() const
	{
		switch (state_)
		{
			case State::data: return MarkupContext::html_text;
			case State::attribute_double:
			case State::attribute_single: return attribute_context();
			case State::before_attribute_value:
			case State::attribute_unquoted: return MarkupContext::unquoted_attribute;
			case State::script:
				if (raw_quote_) return MarkupContext::javascript_quoted;
				if (raw_line_comment_ || raw_block_comment_) return MarkupContext::javascript_comment;
				if (!javascript_value_boundary()) return MarkupContext::javascript_boundary;
				return MarkupContext::javascript_value;
			case State::style:
				if (raw_quote_) return MarkupContext::css_quoted;
				if (raw_block_comment_) return MarkupContext::css_comment;
				if (!css_value_boundary(raw_last_significant_)) return MarkupContext::css_boundary;
				return MarkupContext::css_value;
			case State::comment:
			case State::declaration: return MarkupContext::markup_declaration;
			default: return MarkupContext::dynamic_name;
		}
	}
};
}
