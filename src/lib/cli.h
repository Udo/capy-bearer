#pragma once

DValue cli_input(Request& context);
String cli_arg(Request& context, String key, String default_value = "");
