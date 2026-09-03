#pragma once

#if defined(__BEARER_WASM_CORE__) || defined(__BEARER_WASM_UNIT__)
#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_UN 8
#endif
#ifndef SIGABRT
#define SIGABRT 6
#endif
#ifndef SIGSEGV
#define SIGSEGV 11
#endif
typedef int pid_t;
extern "C" {
int raise(int);
unsigned int sleep(unsigned int seconds);
s32 usleep(u32 usec);
}
#else
#include <sys/file.h>
#include <signal.h>
inline constexpr u64 PROCESS_OUTPUT_LIMIT_DEFAULT = 16ull * 1024 * 1024;
unsigned int child_exit_status_snapshot();
bool child_exit_status_take(pid_t pid, int& status, unsigned int since);
void reap_child_exits();
#endif
#include <ctime>
#include <sstream>

struct DValue;

String shell_exec(String cmd);
#if !defined(__BEARER_WASM_CORE__) && !defined(__BEARER_WASM_UNIT__)
String process_identity_start_ticks(pid_t pid);
DValue process_exec(String cmd, String input, StringMap env, u64 timeout_ms, u64 output_limit = PROCESS_OUTPUT_LIMIT_DEFAULT);
#endif
DValue http_request(DValue req);
u64 http_request_async(DValue req);
DValue shell_exec(String command, DValue flags);
DValue job_status(u64 job_id);
DValue job_result(u64 job_id);
DValue job_await(u64 job_id, u64 timeout_ms);
bool job_cancel(u64 job_id);
String sha256(String data);
String hmac_sha256(String key, String data);
String base64_decode(String raw);
String random_bytes(u64 n);
bool crypto_equal(String a, String b);
String password_hash(String password);
bool password_verify(String password, String encoded);
bool password_needs_rehash(String encoded);
DValue crypto_operation(DValue request);
String shell_escape(String raw);
String basename(String fn);
String dirname(String fn);
String path_join(String base, String child);
String path_normalize(String path, DValue opt = {});
bool mkdir(String path);
bool file_exists(String path);
String file_get_contents(String file_name);
bool file_put_contents(String file_name, String content);
bool file_append(String file_name, String content);
u64 file_open(String path, String mode);
String file_read(u64 h, u64 len);
String file_pread(u64 h, u64 offset, u64 len);
u64 file_write(u64 h, String data);
u64 file_pwrite(u64 h, u64 offset, String data);
s64 file_seek(u64 h, s64 offset, s64 whence);
s64 file_tell(u64 h);
void file_close(u64 h);
DValue file_stat(String path);
DValue dir_list(String path);
bool file_rename(String from, String to);
bool file_copy(String from, String to);
bool file_truncate(String path, u64 size);
bool dir_remove(String path, bool recursive = false);
String file_temp(String prefix);
bool file_chmod(String path, u32 mode);
bool file_symlink(String target, String linkpath);
bool file_fsync(u64 h);
template <typename... Ts>
inline bool file_append(String file_name, Ts... args)
{
	std::ostringstream out;
	((out << args), ...);
	return(file_append(file_name, out.str()));
}
String cwd_get();
void cwd_set(String path);
String process_start_directory();
u64 file_mtime(String file_name);
void file_unlink(String file_name);
DValue ls(String dir);
f64 time_precise();
u64 time();
String time_format_local(String format = "", u64 timestamp = 0);

// Runtime timing/profiling snapshot for the active wasm request/workspace.
DValue request_perf();

String time_format_utc(String format = "", u64 timestamp = 0);
String time_format_relative(u64 timestamp, String format_very_recent = "", u64 medium_recency_seconds = 0, String format_medium_recent = "", u64 not_recent_seconds = 0, String format_not_recent = "");
u64 time_parse(String time_String);

u64 socket_connect(String host, u16 port);
void socket_close(u64 sockfd);
bool socket_write(u64 sockfd, String data);
String socket_read(u64 sockfd, u32 max_length = 1024*128, u32 timeout = 1);

String ws_message();
String ws_connection_id();
String ws_scope();
u8 ws_opcode();
bool ws_is_binary();
DValue ws_connections(String scope = "");
u64 ws_connection_count(String scope = "");
bool ws_send(String message, bool binary = false, String scope = "");
bool ws_send_to(String connection_id, String message, bool binary = false);
bool ws_close(String connection_id = "");

String backtrace_get_frames(void* const* frames, size_t size, u32 skip_frames = 0);
String backtrace_capture(u32 max_frames = 32, u32 skip_frames = 0);
String signal_name(s32 sig);

u64 memcache_connect(String host = "127.0.0.1", u16 port = 11211);
String memcache_command(u64 connection, String command);
bool memcache_set(u64 connection, String key, String value, u64 expires_in = 60*60);
bool memcache_delete(u64 connection, String key);
String memcache_get(u64 connection, String key, String default_value = "");
StringMap memcache_get_multiple(u64 connection, DValue keys);

// Memcached's own default item limit is 1 MB, and one retrieval can return several items.
// This bounds a single response the way WS_BROKER_MAX_OUTBOUND_BYTES bounds one forward.
const u32 memcache_max_response_bytes = 4 * 1024 * 1024;

// A Memcache text response is framed, not delimited. A "VALUE <key> <flags> <bytes>" line
// (with a trailing <cas> field for gets/gats) is followed by exactly <bytes> payload octets
// and CRLF, so a payload that itself contains "END\r\n" must not end the read. Every other
// line is terminal, except "STAT " lines, which continue until the terminator arrives.
// Returns false while octets are still outstanding, so a reader knows to read again.
// A malformed header reports complete rather than waiting for octets that never arrive.
inline bool memcache_response_complete(const String& response)
{
	size_t pos = 0;
	while(true)
	{
		size_t line_end = response.find("\r\n", pos);
		if(line_end == String::npos)
			return(false);
		String line = response.substr(pos, line_end - pos);
		pos = line_end + 2;
		if(line.rfind("STAT ", 0) == 0)
			continue;
		if(line.rfind("VALUE ", 0) != 0)
			return(true);
		// <bytes> is field 3 in both the four-field VALUE line and the five-field gets form.
		size_t start = 0;
		u64 length = 0;
		bool have_length = false;
		for(size_t field = 0; field <= 3 && start <= line.size(); field++)
		{
			size_t space = line.find(' ', start);
			String token = space == String::npos ? line.substr(start) : line.substr(start, space - start);
			if(field == 3)
			{
				if(token == "" || token.find_first_not_of("0123456789") != String::npos || token.size() > 19)
					return(true);
				for(char digit : token)
					length = length * 10 + (u64)(digit - '0');
				have_length = true;
				break;
			}
			if(space == String::npos)
				break;
			start = space + 1;
		}
		if(!have_length)
			return(true);
		if(response.size() < pos + (size_t)length + 2)
			return(false);
		pos += (size_t)length + 2;
	}
}

// Defined once in sys.cpp for the native split_strings build (core/wasm/main); the wasm
// core/unit builds keep the in-place definition (single TU / loader-resolved).
#if defined(__BEARER_WASM_CORE__) || defined(__BEARER_WASM_UNIT__)
pid_t parent_pid = 0;
pid_t my_pid = 0;
#else
extern pid_t parent_pid;
extern pid_t my_pid;
#endif

void on_segfault(int sig);

String runtime_safe_key(String key, String label = "runtime key");
String task(String target, DValue props);
DValue task_status(String id);
DValue task_await(String id, u64 timeout_ms);
DValue task_cancel(String id);
pid_t server_start_http(String key, String socket_fn_or_port, String call_bearer_filename, String call_function = "");
bool server_stop(String key);
