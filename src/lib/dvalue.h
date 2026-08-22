#pragma once

String json_escape(String s, char quote_char = '"');

// DValue is BEARER's general-purpose structured value container.
// It stores scalar values, nested map/list-like values, and internal references.
// Numeric and boolean reads are intentionally permissive so request data,
// JSON-decoded values, and metadata trees can be consumed without repetitive
// manual parsing at each call site.
struct DValue {
	DValue() = default;
	DValue(String value) { set(value); }
	DValue(std::initializer_list<String> values);
	DValue(const DValue&) = default;
	DValue(DValue&&) = default;

	// Preserves the range-for ABI after StringList became DValue.
	class StringIterator {
		const DValue* value;
		size_t position;
	public:
		StringIterator(const DValue* value, size_t position) : value(value), position(position) {}
		const String& operator*() const;
		StringIterator& operator++() { ++position; return(*this); }
		bool operator != (const StringIterator& other) const { return(position != other.position); }
	};

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
	StringIterator begin() const;
	StringIterator end() const;
	void each(std::function <void (const DValue& t, String key)> f) const;
	DValue keys() const;
	DValue values() const;
	DValue filter(DValue keys) const;
	DValue filter(std::function<bool (const DValue&, String)> f) const;
	DValue map(std::function<DValue (const DValue&, String)> f) const;
	// String-list operations live on the general list value; callers that need
	// strings explicitly convert children with to_string().
	DValue filter(std::function<bool (String)> f) const;
	DValue map(std::function<String (String)> f) const;
	DValue unique() const;
	DValue sort() const;
	bool some(std::function<bool (String)> f) const;
	bool every(std::function<bool (String)> f) const;
	String find(std::function<bool (String)> f, String fallback = "") const;
	void each(std::function<void (String)> f) const;
	size_t size() const;
	void pop_back();
	String front(String fallback = "") const;
	String back(String fallback = "") const;
	bool is_array() const;
	bool is_list() const;
	bool is_none() const;
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
	void set_none();
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
	DValue& operator [] (u64 index) { return((*this)[std::to_string(index)]); }
	const DValue& operator [] (u64 index) const;
	operator String() const { return(to_string()); }
	void operator = (String v);
	void operator = (f64 v);
	void operator = (void* v);
	void operator = (DValue v);
	void operator = (StringMap v);

	void push(const DValue& child);
	void push_back(String value);
	DValue pop();
	void remove(String s);
	void clear();
};

String to_String(DValue t);
String var_dump(const DValue& map, String prefix = "", String postfix = "\n");

String brb_encode(const DValue& value);
String brb_encode_flat_string_map(const StringMap& value);
DValue brb_decode(const String& encoded);
bool brb_decode(const String& encoded, DValue& out, String* error_out = 0);

extern "C" {

typedef struct DValue bearer_dvalue;

typedef struct bearer_dv_iter
{
	size_t position;
	size_t reserved[3];
} bearer_dv_iter;

bearer_dvalue* bearer_dv_root(void);
bearer_dvalue* bearer_dv_get(bearer_dvalue* value, const char* key, size_t key_len);
bearer_dvalue* bearer_dv_find(bearer_dvalue* value, const char* key, size_t key_len);
const char* bearer_dv_value(bearer_dvalue* value, size_t* len_out);
void bearer_dv_set_value(bearer_dvalue* value, const char* bytes, size_t len);
size_t bearer_dv_count(bearer_dvalue* value);
int bearer_dv_is_list(bearer_dvalue* value);
bearer_dv_iter bearer_dv_iter_begin(bearer_dvalue* value);
int bearer_dv_iter_next(bearer_dvalue* value, bearer_dv_iter* iter, const char** key_out, size_t* key_len_out, bearer_dvalue** child_out);
size_t bearer_dv_encode(bearer_dvalue* value, char* buf, size_t cap);
bearer_dvalue* bearer_dv_decode(const char* buf, size_t len);
const char* bearer_dv_last_error(void);

}
