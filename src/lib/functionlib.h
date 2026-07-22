
#pragma once

u8 char_to_u8(char input);
u8 hex_to_u8(String src);
u64 int_val(String s, u32 base = 10);
f64 float_val(String s);
u64 to_u64(String s, u64 fallback = 0);
s64 to_s64(String s, s64 fallback = 0);
f64 to_f64(String s, f64 fallback = 0);
bool to_bool(String s, bool fallback = false);
String to_lower(String s);
String to_upper(String s);
String substr(String s, s64 start_pos);
String substr(String s, s64 start_pos, s64 length);
s64 strpos(String haystack, String needle, s64 offset = 0);
bool str_starts_with(String haystack, String needle);
bool str_ends_with(String haystack, String needle);
bool contains(String haystack, String needle);
String replace(String s, String search, String replace_with);
bool regex_match(String pattern, String subject, String flags = "");
DValue regex_search(String pattern, String subject, String flags = "");
DValue regex_search_all(String pattern, String subject, String flags = "");
String regex_replace(String pattern, String replacement, String subject, String flags = "");
StringList regex_split(String pattern, String subject, String flags = "");

String trim(String raw);
StringList split_space(String str);
StringList split(String str, String delim);
StringList split_utf8(String s, bool compound_characters = false);
StringMap split_kv(String s, char separator = '=', bool trim_whitespace = true, bool uppercase_keys = false);
StringMap split_http_headers(String s);
String join(StringList l, String delim = "\n");
String nibble(String& haystack, String delim);
void json_consume_space(String s, u32& i);

inline String to_string(StringList l) {
	String result;
	u32 i = 0;
	for(auto& s : l)
	{
		if(i > 0)
			result.append("\n");
		result.append(s);
		i += 1;
	}
	return(result);
}

inline String to_string(SharedUnit* u) {
	String result;

	result += String("SharedUnit( \n")+
		"Source:"+(u->file_name)+"\n"+
		"Wasm:"+(u->wasm_name)+"\n"+
		"API:"+(u->api_file_name)+"\n"+
		to_string(u->api_declarations);

	return(result);
}

// NB: header-defined function templates must be inline — wasm side modules
// compile with -fvisibility-inlines-hidden so instantiations bind locally
// instead of becoming unresolvable self-imports (WASM-PROPOSAL §6)
template <typename ITYPE>
inline String to_hex(ITYPE w, size_t hex_len = sizeof(ITYPE)<<1)
{
    static const char* digits = "0123456789ABCDEF";
    String rc(hex_len,'0');
    for (size_t i=0, j=(hex_len-1)*4 ; i<hex_len; ++i,j-=4)
        rc[i] = digits[(w>>j) & 0x0f];
    return(rc);
}

template<typename T, typename F>
inline std::vector<T> filter(std::vector<T> items, F f)
{
	std::vector<T> new_items;
	for(auto item : items)
	{
		if(f(item))
			new_items.push_back(item);
	}
	return(new_items);
}

template<typename T, typename F>
inline auto map(std::vector<T> items, F f)
{
	using ResultType = decltype(f(items[0]));
	std::vector<ResultType> new_items;
	for(auto item : items)
		new_items.push_back(f(item));
	return(new_items);
}

template <typename ...Args>
inline String first(Args... args)
{
    std::vector<String> vec = {args...};
    for(auto s : vec)
		if(trim(s) != "")
			return(s);
	return("");
}

String html_escape(String s);
String html_escape(u64 a);
String html_escape(f64 a);

String json_encode(String s, char quote_char = '"');
String json_encode(DValue t, char quote_char = '"');
DValue json_decode(String s);
String xml_encode(DValue t, String root_name = "root");
DValue xml_decode(String s);
String yaml_encode(DValue t);
DValue yaml_decode(String s);

String var_dump(StringMap map, String prefix = "", String postfix = "\n");
String var_dump(StringList slist, String prefix = "", String postfix = "\n");
StringMap array_merge(StringMap a, StringMap b);
DValue array_merge(DValue a, DValue b);

void ob_start();
void ob_close();
String ob_get();
String ob_get_close();

// RAII capture used by the preprocessor's @fragment rewrite: buffers the
// handler's output and appends it to context.call["fragments"][slot].
struct BearerFragmentCapture
{
	Request& context;
	String slot;

	BearerFragmentCapture(Request& c, String s) : context(c), slot(s)
	{
		ob_start();
	}

	~BearerFragmentCapture()
	{
		String html = ob_get_close();
		if(html != "")
			context.call["fragments"][slot] = context.call["fragments"][slot].to_string() + html;
	}
};

String safe_name(String raw);
String ascii_safe_name(String raw);

#define is_bit_set(var,pos) ((var) & (1<<(pos)))
