#pragma once

DValue markdown_to_ast(String src);
DValue markdown_to_ast(String src, DValue options);
String markdown_to_html(String src);
String markdown_to_html(String src, DValue options);
