#include "cli.h"

DValue cli_input(Request& context)
{
	DValue input;

	for(auto& item : context.get)
		input[item.first] = item.second;
	for(auto& item : context.post)
		input[item.first] = item.second;

	String content_type_info = context.params["CONTENT_TYPE"];
	String content_type = to_lower(trim(nibble(content_type_info, ";")));
	if(content_type == "application/json" || str_ends_with(content_type, "+json"))
	{
		String body = trim(context.in);
		if(body != "")
		{
			DValue parsed = json_decode(body);
			if(parsed.type == 'M')
			{
				for(auto& item : parsed._map)
					input[item.first] = item.second;
			}
			else
			{
				input["_"] = parsed;
			}
		}
	}

	return(input);
}

String cli_arg(Request& context, String key, String default_value)
{
	DValue input = cli_input(context);
	DValue* value = input.key(key);
	if(!value)
		return(default_value);
	String result = value->to_string();
	if(result == "")
		return(default_value);
	return(result);
}
