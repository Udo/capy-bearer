#include "../demo/demo_guard.h"

String site_tests_status_class(String status)
{
	if(status == "pass")
		return("status-ok");
	if(status == "fail")
		return("status-error");
	return("status-warn");
}

String site_tests_status_label(String status)
{
	if(status == "pass")
		return("PASS");
	if(status == "fail")
		return("FAIL");
	return("SKIP");
}

void site_tests_page_start(String title, String description = "")
{
	print("<html><head>");
	print("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></meta>");
	print("<link rel=\"stylesheet\" href='../demo/style.css?v=", time(), "'></link>");
	print("<style>");
	print(".tests-grid { display:grid; gap:1rem; grid-template-columns:repeat(auto-fit, minmax(18rem, 1fr)); margin:1.5rem 0; }");
	print(".tests-card { display:block; padding:1rem; border:1px solid #ccc; border-radius:0.75rem; background:#fff; color:inherit; text-decoration:none; }");
	print(".tests-card strong { display:block; margin-bottom:0.5rem; }");
	print(".tests-card span { display:block; color:#444; }");
	print(".tests-tags { margin-top:0.75rem; font-size:0.9rem; color:#666; }");
	print(".tests-summary { display:flex; flex-wrap:wrap; gap:0.75rem; margin:1rem 0 1.5rem; }");
	print(".tests-summary .status-badge { font-size:0.95rem; }");
	print(".tests-cases { display:grid; gap:0.75rem; margin:1.5rem 0; }");
	print(".tests-case { border:1px solid #d8d8d8; border-radius:0.75rem; background:#fff; padding:0.9rem 1rem; }");
	print(".tests-case-header { display:flex; gap:0.75rem; align-items:center; justify-content:space-between; }");
	print(".tests-case pre { margin:0.75rem 0 0; white-space:pre-wrap; }");
	print(".tests-section { margin:1.5rem 0; }");
	print(".tests-note { padding:1rem; border-left:4px solid #888; background:#f7f7f7; }");
	print(".tests-inline-code { font-family:monospace; }");
	print(".tests-component { border:1px solid #ccc; border-radius:0.75rem; padding:1rem; background:#fafafa; }");
	print(".tests-component .accent { color:#0a6; font-weight:bold; }");
	print(".tests-warning { border-left:4px solid #d97706; padding:0.75rem 1rem; background:#fff7ed; }");
	print(".tests-code-block { padding:0.75rem 1rem; background:#111827; color:#f9fafb; overflow:auto; }");
	print("</style></head><body>");
	print("<h1><a href=\"index.uce\">BEARER Site Tests</a><a class=\"docs-link\" href=\"../doc/index.uce\">API Docs &rarr;</a></h1>");
	print("<h2>", html_escape(title), "</h2>");
	if(description != "")
		print("<p>", html_escape(description), "</p>");
}

void site_tests_page_end()
{
	print("</body></html>");
}

void site_tests_card(String href, String title, String description, String tags = "")
{
	print("<a class=\"tests-card\" href=\"", html_escape(href), "\">");
	print("<strong>", html_escape(title), "</strong>");
	print("<span>", html_escape(description), "</span>");
	if(tags != "")
		print("<div class=\"tests-tags\">", html_escape(tags), "</div>");
	print("</a>");
}

DValue site_tests_manifest(String manifest_path = "manifest.txt")
{
	DValue manifest;
	for(String line : split(file_get_contents(manifest_path), "\n"))
	{
		line = trim(line);
		if(line == "" || line[0] == '#')
			continue;
		StringList parts = split(line, "|");
		if(parts.size() < 7)
			continue;
		String file = trim(parts[0]);
		manifest[file]["file"] = file;
		manifest[file]["title"] = trim(parts[1]);
		manifest[file]["description"] = trim(parts[2]);
		manifest[file]["tags"] = trim(parts[3]);
		manifest[file]["expected"] = trim(parts[4]);
		manifest[file]["suite"] = trim(parts[5]);
		manifest[file]["index"] = trim(parts[6]);
	}
	return(manifest);
}

void site_tests_summary(u64 passed, u64 failed, u64 skipped, String note = "")
{
	print("<div class=\"tests-summary\">");
	print("<span class=\"status-badge status-ok\">passed ", std::to_string(passed), "</span>");
	print("<span class=\"status-badge status-error\">failed ", std::to_string(failed), "</span>");
	print("<span class=\"status-badge status-warn\">skipped ", std::to_string(skipped), "</span>");
	print("</div>");
	if(note != "")
		print("<div class=\"tests-note\">", html_escape(note), "</div>");
}

void site_tests_case(String name, String status, String detail)
{
	String css = site_tests_status_class(status);
	String label = site_tests_status_label(status);
	print("<section class=\"tests-case\"><div class=\"tests-case-header\">");
	print("<strong>", html_escape(name), "</strong>");
	print("<span class=\"status-badge ", html_escape(css), "\">", html_escape(label), "</span>");
	print("</div><pre>", html_escape(detail), "</pre></section>");
}

void site_tests_restricted(Request& context, String title, String risk)
{
	test_demo_render_restricted_html(context, title, risk, "../demo/style.css", "index.uce");
}