#pragma once

String json_escape(String s, char quote_char = '"');

// DValue is UCE's general-purpose structured value container.
// It stores scalar values, nested map/list-like values, and internal references.
// Numeric and boolean reads are intentionally permissive so request data,
// JSON-decoded values, and metadata trees can be consumed without repetitive
// manual parsing at each call site.
struct DValue {
	DValue() = default;
	DValue(const DValue&) = default;
	DValue(DValue&&) = default;

	char type = 'S';

	String 	_String;
	f64 	_float = 0;
	s64 	_array_index = 0;
	bool	_bool = false;
	bool	_list_mode = false;
	void*	_ptr = 0;
	std::map<String, DValue> _map;

	// Read accessors are const and never create or modify nodes. The to_*
	// conversions take an optional default that is returned when the value is
	// missing (empty) or cannot be converted to the requested type.
	void each(std::function <void (const DValue& t, String key)> f) const;
	StringList keys() const;
	DValue values() const;
	DValue filter(StringList keys) const;
	DValue filter(std::function<bool (const DValue&, String)> f) const;
	DValue map(std::function<DValue (const DValue&, String)> f) const;
	bool is_array() const;
	bool is_list() const;
	String to_string(String default_value = "") const;
	s64 to_s64(s64 default_value = 0) const;
	u64 to_u64(u64 default_value = 0) const;
	f64 to_f64(f64 default_value = 0) const;
	bool to_bool(bool default_value = false) const;
	StringMap to_stringmap() const;
	String to_json(char quote_char = '"') const;
	String get_type_name() const;
	DValue get_by_path(String path, String delim = "/") const;
	bool is_reference() const;
	DValue* reference_target();
	const DValue* reference_target() const;
	DValue& deref();
	const DValue& deref() const;
	void set_type(char t);
	void set(String s);
	void set(void* p);
	void set(f64 f);
	void set_bool(bool b);
	void set(const DValue& source);
	void set(DValue&& source);
	void set(StringMap source);
	void set_array();
	void set_reference(DValue* target);
	bool has(String s) const;
	DValue* key(String s);
	const DValue* key(String s) const;
	DValue* get_or_create(String s);
	DValue& operator [] (String s);
	void operator = (String v);
	void operator = (f64 v);
	void operator = (void* v);
	void operator = (DValue v);
	void operator = (StringMap v);

	void push(const DValue& child);
	DValue pop();
	void remove(String s);
	void clear();
};

String to_String(DValue t);
String var_dump(const DValue& map, String prefix = "", String postfix = "\n");

String ucb_encode(const DValue& value);
String ucb_encode_flat_string_map(const StringMap& value);
DValue ucb_decode(const String& encoded);
bool ucb_decode(const String& encoded, DValue& out, String* error_out = 0);

extern "C" {

typedef struct DValue uce_dvalue;

typedef struct uce_dv_iter
{
	size_t position;
	size_t reserved[3];
} uce_dv_iter;

uce_dvalue* uce_dv_root(void);
uce_dvalue* uce_dv_get(uce_dvalue* value, const char* key, size_t key_len);
uce_dvalue* uce_dv_find(uce_dvalue* value, const char* key, size_t key_len);
const char* uce_dv_value(uce_dvalue* value, size_t* len_out);
void uce_dv_set_value(uce_dvalue* value, const char* bytes, size_t len);
size_t uce_dv_count(uce_dvalue* value);
int uce_dv_is_list(uce_dvalue* value);
uce_dv_iter uce_dv_iter_begin(uce_dvalue* value);
int uce_dv_iter_next(uce_dvalue* value, uce_dv_iter* iter, const char** key_out, size_t* key_len_out, uce_dvalue** child_out);
size_t uce_dv_encode(uce_dvalue* value, char* buf, size_t cap);
uce_dvalue* uce_dv_decode(const char* buf, size_t len);
const char* uce_dv_last_error(void);

}
