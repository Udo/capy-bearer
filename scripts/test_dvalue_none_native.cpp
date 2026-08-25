#include <iostream>
#include <utility>

#include "src/lib/types.cpp"
#include "src/lib/dvalue.cpp"
#include "src/lib/functionlib.cpp"

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

	DValue empty;
	need(empty.type == 'S' && !empty.is_none() && empty.to_string("empty") == "empty" && empty.to_json() == "\"\"", "default empty string");

	DValue none;
	none["stale"] = "value";
	none.set_none();
	need(none.type == 'N' && none.is_none() && none.get_type_name() == "none" && none.size() == 0 && !none.is_array() && !none.is_list() && !none.is_reference() && none._String == "" && none._map.empty() && none._ptr == 0, "none state");
	need(none.to_string("string fallback") == "string fallback" && none.to_s64(-42) == -42 && none.to_u64(42) == 42 && none.to_f64(4.25) == 4.25 && none.to_bool(true) && none.to_stringmap().empty(), "none fallbacks");

	DValue copied;
	copied.set(none);
	DValue moved;
	moved.set(std::move(copied));
	need(copied.is_none() && moved.is_none(), "none copy and move");

	String none_brrb = brb_encode(none);
	need(none_brrb == String("BRRB\x02\x00N\x00\x00", 9), "none BRRB bytes");
	DValue decoded;
	String error;
	need(brb_decode(none_brrb, decoded, &error) && decoded.is_none(), "none BRRB decode");
	auto malformed = [&](String wire) { DValue output; String message; return(!brb_decode(wire, output, &message) && message != ""); };
	need(malformed(String("BRRB\x02\x00N\x01x\x00", 10)), "none BRRB payload rejection");
	need(malformed(String("BRRB\x02\x00N\x00\x01", 9)), "none BRRB children rejection");
	need(malformed(String("BRRB\x02\x01N\x00\x00", 9)), "none BRRB list flag rejection");
	need(malformed(String("BRRB\x02\x02N\x00\x00", 9)), "none BRRB flags rejection");

	DValue callable;
	callable.set_type('C');
	callable._ptr = (void*)(uintptr_t)4;
	callable._array_index = 7;
	String local_callable = brb_encode_local(callable);
	need(brb_encode(callable) == none_brrb, "public callable projection");
	need(malformed(local_callable), "private callable rejection");
	DValue local_decoded;
	need(brb_decode_local(local_callable, local_decoded, &error) && local_decoded.type == 'C' && local_decoded._ptr == callable._ptr && local_decoded._array_index == callable._array_index, "local callable decode");
	need(malformed(String("BRRB\x02\x00C\x00\x00", 9)), "malformed callable rejection");

	DValue tree;
	tree["none"].set_none();
	tree["list"].set_array();
	tree["list"].push(none);
	DValue round_trip;
	need(brb_decode(brb_encode(tree), round_trip, &error) && round_trip["none"].is_none() && round_trip["list"].is_list() && round_trip["list"][0].is_none(), "nested none BRRB round trip");

	need(none.to_json() == "null" && json_encode(none) == "null" && json_encode(tree) == "{\"list\": [null], \"none\": null}", "none JSON encode");
	need(json_decode("null").is_none() && json_decode("{\"value\":null}")["value"].is_none(), "none JSON decode");
	need(yaml_encode(none) == "null\n" && yaml_encode(tree) == "list:\n  - null\nnone: null\n" && yaml_decode("null").is_none() && yaml_decode("~").is_none() && yaml_decode("value: null\n")["value"].is_none(), "none YAML");
	need(xml_encode(none, "none") == "<none/>" && !xml_decode(xml_encode(none, "none")).is_none(), "none XML boundary");

	u32 iterations = 0;
	none.each([&](const DValue&, String) { iterations += 1; });
	DValue filtered = none.filter([&](const DValue&, String) { iterations += 1; return(true); });
	DValue mapped = none.map([&](const DValue&, String) { iterations += 1; return(DValue("value")); });
	need(iterations == 0 && filtered.size() == 0 && mapped.size() == 0, "none iteration");

	DValue target;
	target.set_none();
	DValue reference;
	reference.set_reference(&target);
	need(reference.is_reference() && reference.deref().is_none() && reference.is_none(), "none reference");
	none["after"] = "mutation";
	need(none.type == 'M' && none.has("after") && none["after"].to_string() == "mutation", "none map mutation");

	return(ok ? 0 : 1);
}
