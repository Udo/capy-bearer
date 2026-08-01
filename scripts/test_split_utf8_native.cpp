#include <iostream>

#include "src/lib/types.cpp"
#include "src/lib/dvalue.cpp"
#include "src/lib/functionlib.cpp"

static bool bytes(const std::vector<String>& values, const String& source)
{
	String joined;
	for(const auto& value : values)
		joined += value;
	return(joined == source);
}

int main()
{
	bool ok = true;
	auto need = [&](bool condition, const char* name) {
		if(!condition)
		{
			std::cerr << name << "\n";
			ok = false;
		}
	};
	for(String input : {String("\xc2", 1), String("\xe2\x82", 2), String("\xf0\x9f\x92", 3), String("\x80", 1), String("\xc0\x80", 2), String("\xed\xa0\x80", 3)})
	{
		auto values = split_utf8_strings(input);
		need(values.size() == input.size() && bytes(values, input), "invalid UTF-8 byte boundaries");
	}
	String zwj = "\xe2\x80\x8d";
	String selector = "\xef\xb8\x8f";
	auto leading_zwj = split_utf8_strings(zwj + "A", true);
	need(leading_zwj.size() == 2 && leading_zwj[0] == zwj && leading_zwj[1] == "A", "leading ZWJ");
	auto leading_selector = split_utf8_strings(selector + "A", true);
	need(leading_selector.size() == 2 && leading_selector[0] == selector && leading_selector[1] == "A", "leading variation selector");
	auto joined = split_utf8_strings("A" + selector + zwj + "B", true);
	need(joined.size() == 1 && joined[0] == "A" + selector + zwj + "B", "compound UTF-8 sequence");
	return(ok ? 0 : 1);
}
