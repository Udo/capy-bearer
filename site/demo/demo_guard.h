String test_demo_normalize_ip(String ip)
{
	if(ip.find(",") != String::npos)
		ip = trim(nibble(ip, ","));
	ip = trim(ip);
	if(str_starts_with(ip, "::ffff:"))
		ip = ip.substr(7);
	return(ip);
}

bool test_demo_ip_is_private(String ip)
{
	ip = trim(ip);
	if(ip == "" || ip == "localhost" || ip == "::1")
		return(true);
	if(str_starts_with(ip, "127."))
		return(true);
	if(str_starts_with(ip, "10."))
		return(true);
	if(str_starts_with(ip, "192.168."))
		return(true);
	if(str_starts_with(ip, "fc") || str_starts_with(ip, "fd") || str_starts_with(ip, "fe80:"))
		return(true);

	auto parts = split(ip, ".");
	if(parts.size() == 4 && parts[0] == "172")
	{
		s64 second = int_val(parts[1]);
		if(second >= 16 && second <= 31)
			return(true);
	}

	return(false);
}

String test_demo_request_ip(Request& context)
{
	String remote_ip = test_demo_normalize_ip(context.params["REMOTE_ADDR"]);
	String forwarded_ip = test_demo_normalize_ip(first(context.params["HTTP_X_FORWARDED_FOR"], context.params["HTTP_X_REAL_IP"]));

	if(remote_ip != "" && !test_demo_ip_is_private(remote_ip))
		return(remote_ip);
	if(forwarded_ip != "")
		return(forwarded_ip);
	return(remote_ip);
}

bool test_demo_request_allowed(Request& context)
{
	return(test_demo_ip_is_private(test_demo_request_ip(context)));
}

void test_demo_render_restricted_html(Request& context, String title, String risk, String style_href = "style.css", String back_href = "index.uce")
{
	context.set_status(403, "Restricted");
	String request_ip = first(test_demo_request_ip(context), "unknown");
	print("<html><head>");
	print("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></meta>");
	print("<link rel=\"stylesheet\" href='", html_escape(style_href), "?v=", time(), "'></link>");
	print("</head><body>");
	print("<h1><a href=\"", html_escape(back_href), "\">BEARER Test</a>: ", html_escape(title), "</h1>");
	print("<p>This test page is disabled for public access because it can ", html_escape(risk), ".</p>");
	print("<p>It remains available from localhost or a private network for local development and server administration.</p>");
	print("<p>Detected request source: <code>", html_escape(request_ip), "</code></p>");
	print("</body></html>");
}

void test_demo_render_restricted_text(Request& context, String title, String risk)
{
	context.set_status(403, "Restricted");
	context.header["Content-Type"] = "text/plain; charset=utf-8";
	print(title, ": restricted on public access\n");
	print("Reason: this test can ", risk, ".\n");
	print("Detected request source: ", first(test_demo_request_ip(context), "unknown"), "\n");
}

void test_demo_render_restricted_json(Request& context, String title, String risk)
{
	context.set_status(403, "Restricted");
	context.header["Content-Type"] = "application/json; charset=utf-8";
	DValue payload;
	payload["ok"].set_bool(false);
	payload["error"] = "restricted";
	payload["title"] = title;
	payload["reason"] = "This test is disabled for public access because it can " + risk + ".";
	payload["request_ip"] = first(test_demo_request_ip(context), "unknown");
	print(json_encode(payload));
}
