
#include <cctype>
#include <cmath>
#include <limits>
#include <cstring>
#include <iomanip>

namespace {

template <typename TreePtr>
TreePtr dv_resolve_reference(TreePtr tree)
{
	u32 depth = 0;
	while(tree && tree->type == 'R' && depth < 16)
	{
		TreePtr target = reinterpret_cast<TreePtr>(tree->_ptr);
		if(target == 0 || target == tree)
			break;
		tree = target;
		depth += 1;
	}
	return(tree);
}

bool dv_key_is_index(String key)
{
	if(key == "")
		return(false);
	for(auto c : key)
	{
		if(!isdigit(c))
			return(false);
	}
	// Only canonical index strings count ("1", not "01"), so each numeric
	// value has exactly one representation.
	if(key.length() > 1 && key[0] == '0')
		return(false);
	return(true);
}

String dv_trim(String raw)
{
	if(raw == "")
		return(raw);

	size_t start = 0;
	while(start < raw.length() && isspace(raw[start]))
		start += 1;

	size_t end = raw.length();
	while(end > start && isspace(raw[end - 1]))
		end -= 1;

	return(raw.substr(start, end - start));
}

String dv_lower(String raw)
{
	for(auto& c : raw)
		c = (char)tolower(c);
	return(raw);
}

bool dv_string_to_bool_value(String raw, bool& value_out)
{
	raw = dv_lower(dv_trim(raw));
	if(raw == "")
		return(false);
	if(raw == "1" || raw == "true" || raw == "(true)" || raw == "yes" || raw == "on")
	{
		value_out = true;
		return(true);
	}
	if(raw == "0" || raw == "false" || raw == "(false)" || raw == "no" || raw == "off" || raw == "null")
	{
		value_out = false;
		return(true);
	}
	return(false);
}

bool dv_string_to_f64_value(String raw, f64& value_out)
{
	raw = dv_trim(raw);
	if(raw == "")
		return(false);

	bool bool_value = false;
	if(dv_string_to_bool_value(raw, bool_value))
	{
		value_out = (bool_value ? 1.0 : 0.0);
		return(true);
	}

	char* end = 0;
	value_out = strtod(raw.c_str(), &end);
	if(end == raw.c_str())
		return(false);
	while(end && *end != 0)
	{
		if(!isspace(*end))
			return(false);
		end += 1;
	}
	return(std::isfinite(value_out));
}

const DValue* dv_scalar_map_value(const DValue& tree)
{
	if(tree.type != 'M' || tree._map.size() != 1)
		return(0);
	auto it = tree._map.begin();
	if(it == tree._map.end())
		return(0);
	return(&it->second.deref());
}

f64 dv_clamp_to_f64_range(long double value)
{
	if(value > std::numeric_limits<f64>::max())
		return(std::numeric_limits<f64>::max());
	if(value < -std::numeric_limits<f64>::max())
		return(-std::numeric_limits<f64>::max());
	return((f64)value);
}

s64 dv_clamp_to_s64_range(long double value)
{
	if(value > (long double)std::numeric_limits<s64>::max())
		return(std::numeric_limits<s64>::max());
	if(value < (long double)std::numeric_limits<s64>::min())
		return(std::numeric_limits<s64>::min());
	return((s64)value);
}

u64 dv_clamp_to_u64_range(long double value)
{
	if(value <= 0)
		return(0);
	if(value > (long double)std::numeric_limits<u64>::max())
		return(std::numeric_limits<u64>::max());
	return((u64)value);
}

}

void DValue::each(std::function <void (const DValue& t, String key)> f) const
{
	const DValue& target = deref();
	switch(target.type)
	{
		case('M'):
			// Lists iterate in numeric index order (string keys sort "10"
			// before "2"), matching the json/xml/yaml encoders.
			if(target.is_list())
			{
				for(u64 i = 0; i < target._map.size(); i++)
				{
					auto it = target._map.find(std::to_string(i));
					if(it == target._map.end())
						break;
					f(it->second, it->first);
				}
				break;
			}
			for (auto it = target._map.begin(); it != target._map.end(); ++it)
			{
				f(it->second, it->first);
			}
			break;
		default:
			f(*this, "");
			break;
	}
}

StringList DValue::keys() const
{
	StringList result;
	each([&](const DValue& item, String key) {
		(void)item;
		if(key != "")
			result.push_back(key);
	});
	return(result);
}

DValue DValue::values() const
{
	DValue result;
	result.set_array();
	each([&](const DValue& item, String key) {
		(void)key;
		result.push(item);
	});
	return(result);
}

DValue DValue::filter(StringList keys) const
{
	DValue result;
	const DValue& target = deref();
	for(auto key : keys)
	{
		const DValue* item = target.key(key);
		if(item)
			result[key] = *item;
	}
	return(result);
}

DValue DValue::filter(std::function<bool (const DValue&, String)> f) const
{
	DValue result;
	bool input_is_list = is_list();
	if(input_is_list)
		result.set_array();
	each([&](const DValue& item, String key) {
		if(!f(item, key))
			return;
		if(key != "" && !input_is_list)
			result[key] = item;
		else
			result.push(item);
	});
	return(result);
}

DValue DValue::map(std::function<DValue (const DValue&, String)> f) const
{
	DValue result;
	bool input_is_list = is_list();
	if(input_is_list)
		result.set_array();
	each([&](const DValue& item, String key) {
		DValue mapped = f(item, key);
		if(key != "" && !input_is_list)
			result[key] = mapped;
		else
			result.push(mapped);
	});
	return(result);
}

bool DValue::is_array() const
{
	return(deref().type == 'M');
}

bool DValue::is_list() const
{
	const DValue& target = deref();
	if(target.type != 'M')
		return(false);
	if(target._map.size() == 0)
		return(target._list_mode);
	// The map iterates in string order ("10" before "2"), so the check must
	// be order-independent: n unique canonical index keys with maximum n-1
	// are exactly 0..n-1.
	s64 max_index = -1;
	for(const auto& entry : target._map)
	{
		if(!dv_key_is_index(entry.first))
			return(false);
		s64 index = strtoll(entry.first.c_str(), 0, 10);
		if(index > max_index)
			max_index = index;
	}
	return(max_index == (s64)target._map.size() - 1);
}

String DValue::to_string(String default_value) const
{
	const DValue& target = deref();
	switch(target.type)
	{
		case('S'):
			if(target._String == "")
				return(default_value);
			return(target._String);
		case('F'):
			return(std::to_string(target._float));
		case('B'):
			return(target._bool ? "(true)" : "(false)");
		case('M'):
			return(default_value);
		case('P'):
			return(std::to_string((u64)target._ptr));
		case('R'):
			return(default_value);
	}
	return(default_value);
}

s64 DValue::to_s64(s64 default_value) const
{
	const DValue& target = deref();
	switch(target.type)
	{
		case('S'):
		{
			f64 value = 0;
			if(!dv_string_to_f64_value(target._String, value))
				return(default_value);
			return(dv_clamp_to_s64_range((long double)value));
		}
		case('F'):
			return(dv_clamp_to_s64_range((long double)target._float));
		case('B'):
			return(target._bool ? 1 : 0);
		case('M'):
		{
			const DValue* item = dv_scalar_map_value(target);
			if(item)
				return(item->to_s64(default_value));
			return(default_value);
		}
		case('P'):
			return(dv_clamp_to_s64_range((long double)(u64)target._ptr));
		case('R'):
			return(default_value);
	}
	return(default_value);
}

u64 DValue::to_u64(u64 default_value) const
{
	const DValue& target = deref();
	switch(target.type)
	{
		case('S'):
		{
			f64 value = 0;
			if(!dv_string_to_f64_value(target._String, value))
				return(default_value);
			return(dv_clamp_to_u64_range((long double)value));
		}
		case('F'):
			return(dv_clamp_to_u64_range((long double)target._float));
		case('B'):
			return(target._bool ? 1 : 0);
		case('M'):
		{
			const DValue* item = dv_scalar_map_value(target);
			if(item)
				return(item->to_u64(default_value));
			return(default_value);
		}
		case('P'):
			return((u64)target._ptr);
		case('R'):
			return(default_value);
	}
	return(default_value);
}

f64 DValue::to_f64(f64 default_value) const
{
	const DValue& target = deref();
	switch(target.type)
	{
		case('S'):
		{
			f64 value = 0;
			if(!dv_string_to_f64_value(target._String, value))
				return(default_value);
			return(value);
		}
		case('F'):
			return(target._float);
		case('B'):
			return(target._bool ? 1.0 : 0.0);
		case('M'):
		{
			const DValue* item = dv_scalar_map_value(target);
			if(item)
				return(item->to_f64(default_value));
			return(default_value);
		}
		case('P'):
			return(dv_clamp_to_f64_range((long double)(u64)target._ptr));
		case('R'):
			return(default_value);
	}
	return(default_value);
}

bool DValue::to_bool(bool default_value) const
{
	const DValue& target = deref();
	switch(target.type)
	{
		case('S'):
		{
			bool value = false;
			if(dv_string_to_bool_value(target._String, value))
				return(value);
			f64 numeric_value = 0;
			if(dv_string_to_f64_value(target._String, numeric_value))
				return(numeric_value != 0);
			// Non-empty unparseable strings stay truthy; only a missing/empty
			// value falls back to the default.
			if(dv_trim(target._String) != "")
				return(true);
			return(default_value);
		}
		case('F'):
			return(target._float != 0);
		case('B'):
			return(target._bool);
		case('M'):
		{
			const DValue* item = dv_scalar_map_value(target);
			if(item)
				return(item->to_bool(default_value));
			return(target._map.size() > 0);
		}
		case('P'):
			return(target._ptr != 0);
		case('R'):
			return(default_value);
	}
	return(default_value);
}

StringMap DValue::to_stringmap() const
{
	const DValue& target = deref();
	StringMap result;
	switch(target.type)
	{
		case('M'):
			for(const auto& entry : target._map)
				result[entry.first] = entry.second.deref().to_string();
			break;
		case('S'):
			if(dv_trim(target._String) != "")
				result["value"] = target._String;
			break;
		case('F'):
		case('B'):
		case('P'):
			result["value"] = target.to_string();
			break;
		case('R'):
			break;
	}
	return(result);
}

String DValue::to_json(char quote_char) const
{
	const DValue& target = deref();
	switch(target.type)
	{
		case('S'):
			return(json_escape(target._String, quote_char));
			break;
		case('F'):
			return(std::to_string(target._float));
			break;
		case('B'):
			return(target._bool ? "true" : "false");
			break;
		case('M'):
			return("\"(array)\"");
			break;
		case('P'):
			return("\"(pointer)\"");
			break;
		case('R'):
			return("\"(reference)\"");
			break;
	}
	return("\"(unknown)\"");
}

String DValue::get_type_name() const
{
	const DValue& target = deref();
	switch(target.type)
	{
		case('S'):
			return("String");
			break;
		case('F'):
			return("f64");
			break;
		case('B'):
			return("bool");
			break;
		case('M'):
			return("array");
			break;
		case('P'):
			return("pointer");
			break;
		case('R'):
			return("reference");
			break;
	}
	return("unknown");
}

DValue DValue::get_by_path(String path, String delim) const
{
	const DValue* current = &deref();
	if(path == "")
		return(*current);
	size_t start = 0;
	while(start <= path.length())
	{
		size_t end = path.find(delim, start);
		String segment;
		if(end == String::npos)
			segment = path.substr(start);
		else
			segment = path.substr(start, end - start);
		if(segment == "")
		{
			if(end == String::npos)
				break;
			start = end + delim.length();
			continue;
		}
		current = &current->deref();
		if(current->type != 'M')
			return(DValue());
		auto it = current->_map.find(segment);
		if(it == current->_map.end())
			return(DValue());
		current = &it->second;
		if(end == String::npos)
			break;
		start = end + delim.length();
	}
	return(current->deref());
}

bool DValue::is_reference() const
{
	return(type == 'R');
}

DValue* DValue::reference_target()
{
	if(type != 'R')
		return(0);
	DValue* target = dv_resolve_reference(this);
	if(target == 0 || target == this || target->type == 'R')
		return(0);
	return(target);
}

const DValue* DValue::reference_target() const
{
	if(type != 'R')
		return(0);
	const DValue* target = dv_resolve_reference(this);
	if(target == 0 || target == this || target->type == 'R')
		return(0);
	return(target);
}

DValue& DValue::deref()
{
	DValue* target = dv_resolve_reference(this);
	if(target == 0)
		return(*this);
	return(*target);
}

const DValue& DValue::deref() const
{
	const DValue* target = dv_resolve_reference(this);
	if(target == 0)
		return(*this);
	return(*target);
}

void DValue::set_type(char t)
{
	DValue* target = reference_target();
	if(target)
	{
		target->set_type(t);
		return;
	}
	if(type != t)
	{
		type = t;
		switch(type)
		{
			case('M'):
				_map.clear();
				_array_index = 0;
				_list_mode = false;
				break;
		}
	}
}

void DValue::set(String s)
{
	DValue* target = reference_target();
	if(target)
	{
		target->set(s);
		return;
	}
	set_type('S');
	_String = s;
	_list_mode = false;
}

void DValue::set(void* p)
{
	DValue* target = reference_target();
	if(target)
	{
		target->set(p);
		return;
	}
	set_type('P');
	_ptr = p;
	_list_mode = false;
}

void DValue::set(f64 f)
{
	DValue* target = reference_target();
	if(target)
	{
		target->set(f);
		return;
	}
	set_type('F');
	_float = f;
	_list_mode = false;
}

void DValue::set_bool(bool b)
{
	DValue* target = reference_target();
	if(target)
	{
		target->set_bool(b);
		return;
	}
	set_type('B');
	_bool = b;
	_list_mode = false;
}

void DValue::set(const DValue& source)
{
	DValue* target = reference_target();
	if(target)
	{
		target->set(source);
		return;
	}
	set_type(source.type);
	switch(type)
	{
		case('S'):
			_String = source._String;
			_list_mode = false;
			break;
		case('F'):
			_float = source._float;
			_list_mode = false;
			break;
		case('B'):
			_bool = source._bool;
			_list_mode = false;
			break;
		case('M'):
			_map = source._map;
			_array_index = source._array_index;
			_list_mode = source._list_mode;
			break;
		case('P'):
			_ptr = source._ptr;
			_list_mode = false;
			break;
		case('R'):
			_ptr = source._ptr;
			_list_mode = false;
			break;
	}
}

void DValue::set(DValue&& source)
{
	DValue* target = reference_target();
	if(target)
	{
		target->set(std::move(source));
		return;
	}
	set_type(source.type);
	switch(type)
	{
		case('S'):
			_String = std::move(source._String);
			_list_mode = false;
			break;
		case('F'):
			_float = source._float;
			_list_mode = false;
			break;
		case('B'):
			_bool = source._bool;
			_list_mode = false;
			break;
		case('M'):
			_map = std::move(source._map);
			_array_index = source._array_index;
			_list_mode = source._list_mode;
			break;
		case('P'):
		case('R'):
			_ptr = source._ptr;
			_list_mode = false;
			break;
	}
}

void DValue::set(StringMap source)
{
	DValue* target = reference_target();
	if(target)
	{
		target->set(source);
		return;
	}
	set_type('M');
	_map.clear();
	_array_index = 0;
	_list_mode = false;
	for (auto it = source.begin(); it != source.end(); ++it)
	{
		_map[it->first] = it->second;
	}
}

void DValue::set_array()
{
	DValue* target = reference_target();
	if(target)
	{
		target->set_array();
		return;
	}
	type = 'M';
	_map.clear();
	_array_index = 0;
	_list_mode = true;
}

void DValue::set_reference(DValue* target)
{
	type = 'R';
	_ptr = target;
}

bool DValue::has(String s) const
{
	const DValue& target = deref();
	if(target.type != 'M')
		return(false);
	return(target._map.find(s) != target._map.end());
}

DValue* DValue::key(String s)
{
	DValue* target = reference_target();
	if(target)
		return(target->key(s));
	if(type != 'M')
		return(0);
	auto it = _map.find(s);
	if(it == _map.end())
		return(0);
	return(&it->second);
}

const DValue* DValue::key(String s) const
{
	const DValue& target = deref();
	if(target.type != 'M')
		return(0);
	auto it = target._map.find(s);
	if(it == target._map.end())
		return(0);
	return(&it->second);
}

DValue* DValue::get_or_create(String s)
{
	DValue* target = reference_target();
	if(target)
		return(target->get_or_create(s));
	set_type('M');
	if(_list_mode && !dv_key_is_index(s))
		_list_mode = false;
	return(&_map[s]);
}

DValue& DValue::operator [] (String s) {
	DValue* target = reference_target();
	if(target)
		return((*target)[s]);
	return(*get_or_create(s));
}

void DValue::operator = (String v) { set(v); }
void DValue::operator = (f64 v) { set(v); }
void DValue::operator = (void* v) { set(v); }
void DValue::operator = (DValue v) { set(std::move(v)); }
void DValue::operator = (StringMap v) { set(v); }

void DValue::push(const DValue& child)
{
	DValue* target = reference_target();
	if(target)
	{
		target->push(child);
		return;
	}
	set_type('M');
	if(_map.size() == 0)
	{
		_list_mode = true;
		_array_index = 0;
	}
	else
	{
		if(is_list())
		{
			_list_mode = true;
			_array_index = _map.size();
		}
		else
		{
			_list_mode = false;
			while(_map.find(std::to_string(_array_index)) != _map.end())
				_array_index += 1;
		}
	}
	_map[std::to_string(_array_index)] = child;
	_array_index += 1;
}

DValue DValue::pop()
{
	DValue* target = reference_target();
	if(target)
		return(target->pop());
	set_type('M');
	if(_map.empty())
	{
		_array_index = 0;
		return(DValue());
	}
	auto last = _map.rbegin();
	DValue result = last->second;
	_map.erase(last->first);
	if(_list_mode)
		_array_index = _map.size();
	return(result);
}

void DValue::remove(String s)
{
	DValue* target = reference_target();
	if(target)
	{
		target->remove(s);
		return;
	}
	set_type('M');
	_map.erase(s);
	if(_map.size() == 0)
		_array_index = 0;
}

void DValue::clear()
{
	DValue* target = reference_target();
	if(target)
	{
		target->clear();
		return;
	}
	set_type('M');
	_map.clear();
	_array_index = 0;
}

namespace {

const char* UCEB_MAGIC = "UCEB";
const u8 UCEB_VERSION = 2;
const u8 UCEB_FLAG_LIST = 1;
const u32 UCEB_MAX_NESTING_DEPTH = 64;

thread_local String uce_dv_last_error_text;
thread_local String uce_dv_value_result;
thread_local DValue uce_dv_decode_result;

bool ucb_append_varint(String& out, u64 value)
{
	while(value >= 0x80)
	{
		out.push_back((char)((value & 0x7f) | 0x80));
		value >>= 7;
	}
	out.push_back((char)value);
	return(true);
}

bool ucb_read_varint(const char* src, size_t src_size, size_t& offset, u64& value_out)
{
	value_out = 0;
	u32 shift = 0;
	while(offset < src_size && shift <= 63)
	{
		u8 byte = (u8)src[offset++];
		value_out |= ((u64)(byte & 0x7f) << shift);
		if((byte & 0x80) == 0)
			return(true);
		shift += 7;
	}
	return(false);
}

bool ucb_read_varint(const String& src, size_t& offset, u64& value_out)
{
	return(ucb_read_varint(src.data(), src.size(), offset, value_out));
}

char ucb_node_type(const DValue& value)
{
	const DValue& target = value.deref();
	switch(target.type)
	{
		case('M'):
		case('S'):
		case('F'):
		case('B'):
			return(target.type);
		default:
			// Raw pointers/references are not meaningful across the native/wasm
			// membrane; preserve the historical wire behavior as an empty scalar.
			return('S');
	}
}

String ucb_node_scalar(const DValue& value)
{
	const DValue& target = value.deref();
	switch(target.type)
	{
		case('S'):
			return(target._String);
		case('F'):
		{
			std::ostringstream out;
			out << std::setprecision(std::numeric_limits<f64>::max_digits10) << target._float;
			return(out.str());
		}
		case('B'):
			return(target._bool ? "1" : "0");
		case('P'):
			return("");
		default:
			return("");
	}
}

bool ucb_decode_scalar(char node_type, const String& scalar, DValue& out, String& error)
{
	switch(node_type)
	{
		case('S'):
			out = scalar;
			return(true);
		case('F'):
		{
			const char* begin = scalar.c_str();
			char* end = 0;
			f64 value = strtod(begin, &end);
			if(end == begin || end != begin + scalar.size() || !std::isfinite(value))
			{
				error = "invalid UCEB2 f64 scalar";
				return(false);
			}
			out = value;
			return(true);
		}
		case('B'):
			if(scalar == "1" || scalar == "true" || scalar == "(true)")
			{
				out.set_bool(true);
				return(true);
			}
			if(scalar == "0" || scalar == "false" || scalar == "(false)")
			{
				out.set_bool(false);
				return(true);
			}
			error = "invalid UCEB2 bool scalar";
			return(false);
	}
	error = "invalid UCEB2 scalar type tag";
	return(false);
}

void ucb_encode_node(String& out, const DValue& value)
{
	const DValue& target = value.deref();
	u8 flags = target.is_list() ? UCEB_FLAG_LIST : 0;
	out.push_back((char)flags);
	out.push_back(ucb_node_type(target));
	String scalar = ucb_node_scalar(target);
	ucb_append_varint(out, scalar.size());
	out.append(scalar.data(), scalar.size());

	if(target.type != 'M')
	{
		ucb_append_varint(out, 0);
		return;
	}
	ucb_append_varint(out, target._map.size());
	target.each([&](const DValue& child, String key) {
		ucb_append_varint(out, key.size());
		out.append(key.data(), key.size());
		ucb_encode_node(out, child);
	});
}

bool ucb_decode_node(const String& src, size_t& offset, DValue& out, String& error, u32 depth = 0)
{
	if(depth >= UCEB_MAX_NESTING_DEPTH)
	{
		error = "UCEB2 nesting limit exceeded";
		return(false);
	}
	if(offset > src.size() || src.size() - offset < 2)
	{
		error = "unexpected end of UCEB2 node";
		return(false);
	}
	u8 flags = (u8)src[offset++];
	char node_type = src[offset++];
	if(node_type != 'M' && node_type != 'S' && node_type != 'F' && node_type != 'B')
	{
		error = "invalid UCEB2 node type tag";
		return(false);
	}
	u64 scalar_len = 0;
	if(!ucb_read_varint(src, offset, scalar_len))
	{
		error = "invalid UCEB2 scalar length";
		return(false);
	}
	if(offset > src.size() || scalar_len > src.size() - offset)
	{
		error = "UCEB2 scalar length exceeds input";
		return(false);
	}
	String scalar(src.data() + offset, (size_t)scalar_len);
	offset += (size_t)scalar_len;

	u64 child_count = 0;
	if(!ucb_read_varint(src, offset, child_count))
	{
		error = "invalid UCEB2 child count";
		return(false);
	}

	out.clear();
	if(node_type != 'M')
	{
		if(child_count != 0 || (flags & UCEB_FLAG_LIST) != 0)
		{
			error = "UCEB2 scalar node cannot have children or list flag";
			return(false);
		}
		return(ucb_decode_scalar(node_type, scalar, out, error));
	}
	if((flags & UCEB_FLAG_LIST) != 0)
		out.set_array();

	for(u64 i = 0; i < child_count; i++)
	{
		u64 key_len = 0;
		if(!ucb_read_varint(src, offset, key_len))
		{
			error = "invalid UCEB2 child key length";
			return(false);
		}
		if(offset > src.size() || key_len > src.size() - offset)
		{
			error = "UCEB2 child key length exceeds input";
			return(false);
		}
		String key(src.data() + offset, (size_t)key_len);
		offset += (size_t)key_len;
		if(depth + 1 >= UCEB_MAX_NESTING_DEPTH)
		{
			error = "UCEB2 nesting limit exceeded";
			return(false);
		}
		DValue child;
		if(!ucb_decode_node(src, offset, child, error, depth + 1))
			return(false);
		out[key] = std::move(child);
	}
	return(true);
}

String uce_dv_key(const char* key, size_t key_len)
{
	if(key == 0)
		return("");
	return(String(key, key_len));
}

DValue* uce_dv_target(uce_dvalue* value)
{
	if(value == 0)
		return(0);
	return(reinterpret_cast<DValue*>(value)->reference_target() ? reinterpret_cast<DValue*>(value)->reference_target() : reinterpret_cast<DValue*>(value));
}

const DValue* uce_dv_target_const(uce_dvalue* value)
{
	if(value == 0)
		return(0);
	return(&reinterpret_cast<DValue*>(value)->deref());
}

}

String ucb_encode(const DValue& value)
{
	String out;
	out.append(UCEB_MAGIC, 4);
	out.push_back((char)UCEB_VERSION);
	ucb_encode_node(out, value);
	return(out);
}

// Internal request/config transport uses the same UCEB2 map representation
// without first building a duplicate DValue tree.
String ucb_encode_flat_string_map(const StringMap& value)
{
	String out;
	out.append(UCEB_MAGIC, 4);
	out.push_back((char)UCEB_VERSION);
	out.push_back(0);
	out.push_back('M');
	ucb_append_varint(out, 0);
	ucb_append_varint(out, value.size());
	for(auto& child : value)
	{
		ucb_append_varint(out, child.first.size());
		out.append(child.first.data(), child.first.size());
		out.push_back(0);
		out.push_back('S');
		ucb_append_varint(out, child.second.size());
		out.append(child.second.data(), child.second.size());
		ucb_append_varint(out, 0);
	}
	return(out);
}

bool ucb_decode(const String& encoded, DValue& out, String* error_out)
{
	String error;
	if(encoded.size() < 5 || encoded.compare(0, 4, UCEB_MAGIC) != 0)
		error = "missing UCEB magic header";
	else if((u8)encoded[4] != UCEB_VERSION)
		error = "unsupported UCEB version";
	else
	{
		size_t offset = 5;
		DValue decoded;
		if(ucb_decode_node(encoded, offset, decoded, error) && offset == encoded.size())
		{
			out = std::move(decoded);
			if(error_out)
				*error_out = "";
			return(true);
		}
		if(error == "")
			error = "trailing bytes after UCEB2 document";
	}
	if(error_out)
		*error_out = error;
	return(false);
}

#ifdef __UCE_WASM_CORE__
static bool ucb_decode_flat_string_map(const char* encoded, size_t encoded_size, StringMap& out, String* error_out)
{
	String error;
	StringMap decoded;
	size_t offset = 0;
	auto fail = [&](String message) {
		error = message;
		return(false);
	};
	if(encoded_size < 5 || memcmp(encoded, UCEB_MAGIC, 4) != 0)
		fail("missing UCEB magic header");
	else if((u8)encoded[4] != UCEB_VERSION)
		fail("unsupported UCEB version");
	else
	{
		offset = 5;
		if(encoded_size - offset < 2)
			fail("unexpected end of UCEB2 string map");
		else
		{
			u8 flags = (u8)encoded[offset++];
			char type = encoded[offset++];
			u64 scalar_len = 0, child_count = 0;
			if(flags != 0 || type != 'M')
				fail("UCEB2 string map root must be a map");
			else if(!ucb_read_varint(encoded, encoded_size, offset, scalar_len) || scalar_len != 0)
				fail("UCEB2 string map root must have no scalar");
			else if(!ucb_read_varint(encoded, encoded_size, offset, child_count))
				fail("invalid UCEB2 string map child count");
			else
			{
				for(u64 i = 0; error == "" && i < child_count; i++)
				{
					u64 key_len = 0, value_len = 0, value_children = 0;
					if(!ucb_read_varint(encoded, encoded_size, offset, key_len) || key_len > encoded_size - offset)
					{
						fail("invalid UCEB2 string map key");
						break;
					}
					String key(encoded + offset, (size_t)key_len);
					offset += (size_t)key_len;
					if(encoded_size - offset < 2)
					{
						fail("unexpected end of UCEB2 string map value");
						break;
					}
					u8 value_flags = (u8)encoded[offset++];
					char value_type = encoded[offset++];
					if(value_flags != 0 || value_type != 'S' || !ucb_read_varint(encoded, encoded_size, offset, value_len) || value_len > encoded_size - offset)
					{
						fail("UCEB2 string map value must be a string scalar");
						break;
					}
					String value(encoded + offset, (size_t)value_len);
					offset += (size_t)value_len;
					if(!ucb_read_varint(encoded, encoded_size, offset, value_children) || value_children != 0)
					{
						fail("UCEB2 string map value cannot have children");
						break;
					}
					decoded[std::move(key)] = std::move(value);
				}
				if(error == "" && offset != encoded_size)
					fail("trailing bytes after UCEB2 string map");
			}
		}
	}
	if(error != "")
	{
		if(error_out)
			*error_out = error;
		return(false);
	}
	out = std::move(decoded);
	if(error_out)
		*error_out = "";
	return(true);
}
#endif

DValue ucb_decode(const String& encoded)
{
	DValue out;
	String error;
	ucb_decode(encoded, out, &error);
	return(out);
}

extern "C" {

uce_dvalue* uce_dv_root(void)
{
	if(context == 0)
		return(0);
	return(reinterpret_cast<uce_dvalue*>(&context->call));
}

uce_dvalue* uce_dv_get(uce_dvalue* value, const char* key, size_t key_len)
{
	DValue* target = uce_dv_target(value);
	if(target == 0)
		return(0);
	return(reinterpret_cast<uce_dvalue*>(target->get_or_create(uce_dv_key(key, key_len))));
}

uce_dvalue* uce_dv_find(uce_dvalue* value, const char* key, size_t key_len)
{
	DValue* target = uce_dv_target(value);
	if(target == 0)
		return(0);
	return(reinterpret_cast<uce_dvalue*>(target->key(uce_dv_key(key, key_len))));
}

const char* uce_dv_value(uce_dvalue* value, size_t* len_out)
{
	const DValue* target = uce_dv_target_const(value);
	if(target == 0)
	{
		if(len_out)
			*len_out = 0;
		return(0);
	}
	uce_dv_value_result = ucb_node_scalar(*target);
	if(len_out)
		*len_out = uce_dv_value_result.size();
	return(uce_dv_value_result.data());
}

void uce_dv_set_value(uce_dvalue* value, const char* bytes, size_t len)
{
	DValue* target = uce_dv_target(value);
	if(target == 0)
		return;
	if(bytes == 0 && len > 0)
	{
		target->set("");
		return;
	}
	target->set(String(bytes ? bytes : "", len));
}

size_t uce_dv_count(uce_dvalue* value)
{
	const DValue* target = uce_dv_target_const(value);
	if(target == 0 || target->type != 'M')
		return(0);
	return(target->_map.size());
}

int uce_dv_is_list(uce_dvalue* value)
{
	const DValue* target = uce_dv_target_const(value);
	return(target && target->is_list() ? 1 : 0);
}

uce_dv_iter uce_dv_iter_begin(uce_dvalue* value)
{
	uce_dv_iter iter;
	iter.position = 0;
	iter.reserved[0] = 0;
	iter.reserved[1] = 0;
	iter.reserved[2] = 0;
	return(iter);
}

int uce_dv_iter_next(uce_dvalue* value, uce_dv_iter* iter, const char** key_out, size_t* key_len_out, uce_dvalue** child_out)
{
	const DValue* target = uce_dv_target_const(value);
	if(target == 0 || target->type != 'M' || iter == 0)
		return(0);
	std::map<String, DValue>::const_iterator entry;
	if(target->is_list())
	{
		entry = target->_map.find(std::to_string((u64)iter->position));
		if(entry == target->_map.end())
			return(0);
	}
	else
	{
		if(iter->position >= target->_map.size())
			return(0);
		entry = target->_map.begin();
		std::advance(entry, iter->position);
	}
	if(key_out)
		*key_out = entry->first.data();
	if(key_len_out)
		*key_len_out = entry->first.size();
	if(child_out)
		*child_out = reinterpret_cast<uce_dvalue*>(const_cast<DValue*>(&entry->second));
	iter->position += 1;
	return(1);
}

size_t uce_dv_encode(uce_dvalue* value, char* buf, size_t cap)
{
	const DValue* target = uce_dv_target_const(value);
	if(target == 0)
		return(0);
	String encoded = ucb_encode(*target);
	if(buf != 0 && cap > 0)
	{
		size_t copy_len = encoded.size() < cap ? encoded.size() : cap;
		memcpy(buf, encoded.data(), copy_len);
	}
	return(encoded.size());
}

uce_dvalue* uce_dv_decode(const char* buf, size_t len)
{
	String encoded(buf ? buf : "", buf ? len : 0);
	String error;
	DValue decoded;
	if(!ucb_decode(encoded, decoded, &error))
	{
		uce_dv_last_error_text = error;
		return(0);
	}
	uce_dv_last_error_text = "";
	uce_dv_decode_result = decoded;
	return(reinterpret_cast<uce_dvalue*>(&uce_dv_decode_result));
}

const char* uce_dv_last_error(void)
{
	return(uce_dv_last_error_text.c_str());
}

}

String to_String(DValue t)
{
	return(t.to_string());
}

String var_dump(const DValue& map, String prefix, String postfix)
{
	String result = "";
	if(!map.is_array())
		return(map.to_string());
	map.each([&] (const DValue& item, String key) {
		result += prefix + key + ": " + item.to_string() + postfix;
		if(item.is_array())
			result += var_dump(item, prefix + "\t");
	});
	return(result);
}

String json_escape(String s, char quote_char)
{
	//return(String("\"")+s+"\"");
	String result;
	u32 i = 0;
	result.append(1, quote_char);
	while(i < s.length())
	{
		char c = s[i];
		if(c == quote_char)
		{
			result.append(1, '\\');
			result.append(1, quote_char);
		}
		else switch(c)
		{
			case('\t'):
				result.append("\\t");
				break;
			case('\n'):
				result.append("\\n");
				break;
			case('"'):
				result.append("\\\"");
				break;
			case('\r'):
				result.append("\\r");
				break;
			case('\\'):
				result.append("\\\\");
				break;
			case('\b'):
				result.append("\\b");
				break;
			case('\f'):
				result.append("\\f");
				break;
			default:
				result.append(1, c);
				break;
		}
		i += 1;
	}
	result.append(1, quote_char);
	return(result);
}
