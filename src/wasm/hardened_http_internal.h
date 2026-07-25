// Internal worker-local implementation for opt-in generic hardened HTTP.
#pragma once
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <signal.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>

static u64 hardened_http_monotonic_ms() { timespec ts{}; clock_gettime(CLOCK_MONOTONIC,&ts); return (u64)ts.tv_sec*1000ull+(u64)ts.tv_nsec/1000000ull; }
static void hardened_http_close_inherited_fds()
{
#ifdef SYS_close_range
	if(syscall(SYS_close_range,4u,~0u,0u)==0) return;
#endif
	long max_fd=sysconf(_SC_OPEN_MAX); if(max_fd<4) max_fd=4;
	for(int fd=4;fd<max_fd;fd++) close(fd);
}

struct HardenedHttpExecResult { int exit_code=-1; bool timed_out=false, output_limited=false; String headers_text, body_text, stderr_text; };
struct HardenedHttpHooks {
	std::function<std::vector<String>(String)> resolve;
	std::function<HardenedHttpExecResult(std::vector<String>, String, std::vector<String>, u64, size_t)> execute;
};

// Hardened requests deliberately support public IPv4 answers only. Every IPv6
// answer fails closed until this generic transport has a reviewed IPv6 policy.
// IPv4 must be globally unicast: reject IANA special-purpose 0/8, 10/8,
// 100.64/10, 127/8, 169.254/16, 172.16/12, 192.0.0/24, 192.0.2/24,
// 192.31.196/24, 192.52.193/24, 192.88.99/24, 192.175.48/24, 192.168/16,
// 198.18/15, 198.51.100/24, 203.0.113/24, and all 224/4 multicast/reserved.
static bool hardened_http_public_address(String text)
{
	in_addr v4{};
	if(inet_pton(AF_INET,text.c_str(),&v4)!=1) return false; // Includes every AAAA candidate.
	u32 x=ntohl(v4.s_addr); u8 a=x>>24,b=x>>16,c=x>>8;
	if(a==0||a==10||a==127||a>=224||(a==100&&b>=64&&b<=127)||(a==169&&b==254)||(a==172&&b>=16&&b<=31)||(a==192&&(b==0||b==2||b==168))||(a==192&&b==31&&c==196)||(a==192&&b==52&&c==193)||(a==192&&b==88&&c==99)||(a==192&&b==175&&c==48)||(a==198&&(b==18||b==19||b==51))||(a==203&&b==0&&c==113)) return false;
	return true;
}
// Three private pipes keep curl's -D headers and -o body distinct. The child
// inherits only stdin/stdout/stderr and the header pipe on fd 3. Async workers
// retain their own process group so job_cancel can kill curl and its descendants.
static HardenedHttpExecResult hardened_http_exec_argv_capture(std::vector<String> argv, String input, u64 timeout_ms, size_t output_limit, bool clean_env=true, bool own_process_group=true)
{
	HardenedHttpExecResult r; if(argv.empty()) return r;
	int in[2], body[2], headers[2], err[2];
	if(pipe(in)||pipe(body)||pipe(headers)||pipe(err)) return r;
	pid_t pid=fork();
	if(pid==0) {
		if(own_process_group) setpgid(0,0);
		dup2(in[0],0); dup2(body[1],1); dup2(err[1],2); dup2(headers[1],3);
		hardened_http_close_inherited_fds();
		std::vector<char*> args; for(String& a:argv) args.push_back((char*)a.c_str()); args.push_back(0);
		if(clean_env) { clearenv(); setenv("PATH","/usr/bin:/bin",1); execv(args[0],args.data()); } else execvp(args[0],args.data());
		_exit(127);
	}
	auto close_all=[&](){ for(int fd: {in[0],in[1],body[0],body[1],headers[0],headers[1],err[0],err[1]}) close(fd); };
	if(pid<0) { close_all(); return r; }
	if(own_process_group) setpgid(pid,pid);
	close(in[0]); close(body[1]); close(headers[1]); close(err[1]);
	for(int fd: {in[1],body[0],headers[0],err[0]}) fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);
	size_t input_off=0, total=0; bool in_open=true,body_open=true,headers_open=true,err_open=true,exited=false,killed=false; int status=0; u64 deadline=hardened_http_monotonic_ms()+timeout_ms;
	auto terminate=[&](bool timeout){ if(timeout) r.timed_out=true; if(!killed) { if(own_process_group) kill(-pid,SIGKILL); else kill(-getpgrp(),SIGKILL); kill(pid,SIGKILL); killed=true; } };
	auto drain=[&](int fd,bool& open,String& dst) { char buf[4096]; ssize_t n; while((n=read(fd,buf,sizeof(buf)))>0) { if(total+(size_t)n>output_limit) { r.output_limited=true; terminate(false); } else { dst+=String(buf,n); total+=(size_t)n; } } if(n==0) { close(fd); open=false; } };
	while(in_open||body_open||headers_open||err_open||!exited) {
		if(!exited) { pid_t w=waitpid(pid,&status,WNOHANG); if(w==pid) exited=true; }
		if(in_open) { if(input_off<input.size()) { ssize_t n=write(in[1],input.data()+input_off,input.size()-input_off); if(n>0) input_off+=(size_t)n; else if(n<0&&errno!=EINTR&&errno!=EAGAIN&&errno!=EWOULDBLOCK) { close(in[1]); in_open=false; } } else { close(in[1]); in_open=false; } }
		drain(body[0],body_open,r.body_text); drain(headers[0],headers_open,r.headers_text); drain(err[0],err_open,r.stderr_text);
		if(!killed&&hardened_http_monotonic_ms()>=deadline) terminate(true);
		if(killed&&!exited) { while(waitpid(pid,&status,0)<0&&errno==EINTR) {} exited=true; }
		if(in_open||body_open||headers_open||err_open||!exited) usleep(1000);
	}
	if(WIFEXITED(status)) r.exit_code=WEXITSTATUS(status); else if(WIFSIGNALED(status)) r.exit_code=128+WTERMSIG(status);
	return r;
}

static bool hardened_http_parse_headers(String text, u64& status, DValue& filtered)
{
	status=0; filtered.set_array(); size_t pos=0; bool found=false;
	while(pos<text.size()) {
		size_t end=text.find("\r\n\r\n",pos); size_t sep=4; if(end==String::npos) { end=text.find("\n\n",pos); sep=2; } if(end==String::npos) return false;
		String block=replace(text.substr(pos,end-pos),"\r",""); size_t nl=block.find('\n'); String first=nl==String::npos?block:block.substr(0,nl);
		if(first.size()<12||first.rfind("HTTP/",0)!=0||first[8]!=' '||!isdigit((unsigned char)first[9])||!isdigit((unsigned char)first[10])||!isdigit((unsigned char)first[11])) return false;
		status=strtoull(first.substr(9,3).c_str(),0,10); DValue current; current.set_array();
		if(nl!=String::npos) for(String line:split_strings(block.substr(nl+1),"\n")) { size_t c=line.find(':'); if(c==String::npos) return false; String k=to_lower(trim(line.substr(0,c))); if(k=="content-type"||k=="cache-control") current[k]=trim(line.substr(c+1)); }
		filtered=current; found=true; pos=end+sep;
	}
	return found;
}

static bool hardened_http_token(String text)
{
	if(text=="") return false;
	for(unsigned char c:text) if(!(isalnum(c)||c=='-'||c=='_')) return false;
	return true;
}
static bool hardened_http_header_value(String text)
{
	if(text.size()>8192||text.find('\0')!=String::npos||text.find('\r')!=String::npos||text.find('\n')!=String::npos) return false;
	return true;
}
static bool hardened_http_ip_literal(String text)
{
	in_addr v4{}; in6_addr v6{};
	return inet_pton(AF_INET,text.c_str(),&v4)==1 || inet_pton(AF_INET6,text.c_str(),&v6)==1;
}
static bool hardened_http_url(String url, String& host, String& port)
{
	if(url.find('\0')!=String::npos||url.find_first_of("\\ \r\n\t")!=String::npos||url.rfind("https://",0)!=0) return false;
	String rest=url.substr(8); size_t slash=rest.find('/'); String authority=slash==String::npos?rest:rest.substr(0,slash);
	if(authority==""||authority.find('@')!=String::npos||authority.find('[')!=String::npos||authority.find(']')!=String::npos) return false;
	size_t colon=authority.rfind(':'); host=colon==String::npos?authority:authority.substr(0,colon); port=colon==String::npos?"443":authority.substr(colon+1);
	if(host==""||hardened_http_ip_literal(host)||!hardened_http_token(port)) return false;
	for(unsigned char c:host) if(!(isalnum(c)||c=='.'||c=='-')) return false;
	char* end=0; unsigned long n=strtoul(port.c_str(),&end,10); return end!=port.c_str()&&*end==0&&n>0&&n<=65535;
}
static bool hardened_http_security_requested(const DValue* security)
{
	return security && (security->key("https_only") || security->key("public_dns_only") || security->key("pin_dns") || security->key("isolated_curl") || security->key("no_redirects"));
}
static bool hardened_http_true(const DValue* value)
{
	return value && value->get_type_name()=="bool" && value->to_bool();
}
static DValue hardened_http_request_internal(const DValue& req, u64 timeout_ms, const HardenedHttpHooks& hooks)
{
	DValue r; r["status"]=(f64)0; r["headers"].set_array(); r["body"]=""; r["error"]="";
	const DValue* sec=req.key("security");
	if(req.key("follow_redirects") && req.key("follow_redirects")->to_bool()) { r["error"]="invalid_request"; return r; }
	timeout_ms=std::min<u64>(std::max<u64>(1,timeout_ms),10000);
	if(!sec || !hardened_http_true(sec->key("https_only")) || !hardened_http_true(sec->key("public_dns_only")) || !hardened_http_true(sec->key("pin_dns")) || !hardened_http_true(sec->key("isolated_curl")) || !hardened_http_true(sec->key("no_redirects"))) { r["error"]="invalid_request"; return r; }
	String method=req.key("method")?req.key("method")->to_string():"GET"; String url=req.key("url")?req.key("url")->to_string():""; String host,port;
	if((method!="GET"&&method!="POST"&&method!="PUT"&&method!="PATCH"&&method!="DELETE"&&method!="HEAD"&&method!="OPTIONS")||!hardened_http_url(url,host,port)) { r["error"]="invalid_request"; return r; }
	String body=req.key("body")?req.key("body")->to_string():""; if(body.size()>65536||body.find('\0')!=String::npos) { r["error"]="invalid_request"; return r; }
	std::vector<String> argv={"/usr/bin/curl","--disable","-sS","--http1.1","--proto","=https","--proto-redir","=https","--noproxy","*","--proxy","","--alt-svc","","--hsts","","--cacert","/etc/ssl/certs/ca-certificates.crt","--connect-timeout","3","--max-time",std::to_string(std::max<u64>(1,timeout_ms/1000)),"--max-filesize","65536","-X",method,"-D","/proc/self/fd/3","-o","/proc/self/fd/1"};
	const DValue* hs=req.key("headers"); if(hs) { bool valid=true; hs->each([&](const DValue& v,String k) { String value=v.to_string(), lower=to_lower(k); if(!hardened_http_token(k)||!hardened_http_header_value(value)||lower=="host"||lower=="content-length"||lower=="transfer-encoding"||lower=="connection"||lower=="proxy-connection"||lower=="expect") valid=false; else { argv.push_back("-H"); argv.push_back(k+": "+value); } }); if(!valid) { r["error"]="invalid_request"; return r; } }
	std::vector<String> answers=hooks.resolve(host); String address; for(String a:answers) { if(!hardened_http_public_address(a)) { r["error"]="unsafe_dns"; return r; } if(address=="") address=a; } if(address=="") { r["error"]="unsafe_dns"; return r; }
	argv.push_back("--resolve"); argv.push_back(host+":"+port+":"+address); if(req.key("body")) { argv.push_back("--data-binary"); argv.push_back("@-"); } argv.push_back(url);
	HardenedHttpExecResult pr=hooks.execute(argv,body,{"PATH=/usr/bin:/bin"},timeout_ms,72*1024); if(pr.output_limited||pr.body_text.size()>65536||pr.headers_text.size()>8192) { r["error"]="response_too_large"; return r; }
	u64 status; DValue headers; if(!hardened_http_parse_headers(pr.headers_text,status,headers)) { r["error"]=pr.timed_out?"timeout":"malformed_output"; return r; }
	r["status"]=(f64)status; r["headers"]=headers; r["body"]=pr.body_text;
	if(status/100==3) r["error"]="redirect_not_allowed"; else if(pr.exit_code!=0) r["error"]=pr.timed_out?"timeout":"network_failure"; else if(status/100!=2) r["error"]="http_status"; return r;
}
