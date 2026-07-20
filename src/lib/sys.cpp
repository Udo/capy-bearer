#ifdef __UCE_WASM_CORE__
#include <cmath>
#include "types.h"
#include "functionlib.h"
#include "hash.h"
#include "uri.h"
#include "sys.h"

extern "C" {
uint64_t uce_host_time(void);
double uce_host_time_precise(void);
size_t uce_host_env(const char* key, size_t key_len, char* buf, size_t cap);
size_t uce_host_random(char* buf, size_t len);
void uce_host_log(int level, const char* buf, size_t len);
// read-only file membrane, policy-gated host-side to the site tree;
// relative paths resolve against the current unit's directory (the native
// cwd convention); file_read uses the length-query convention
int uce_host_file_exists(const char* path, size_t path_len, const char* current, size_t current_len);
size_t uce_host_file_read(const char* path, size_t path_len, const char* current, size_t current_len, char* buf, size_t cap);
int uce_host_file_write(const char* path, size_t path_len, const char* current, size_t current_len, const char* content, size_t content_len, int append);
void uce_host_file_unlink(const char* path, size_t path_len, const char* current, size_t current_len);
size_t uce_host_file_list(const char* path, size_t path_len, const char* current, size_t current_len, char* buf, size_t cap);
int uce_host_file_mkdir(const char* path, size_t path_len, const char* current, size_t current_len);
int64_t uce_host_file_mtime(const char* path, size_t path_len, const char* current, size_t current_len);
uint64_t uce_host_file_open(const char* path, size_t path_len, const char* current, size_t current_len, const char* mode, size_t mode_len);
size_t uce_host_file_handle_read(uint64_t handle, uint64_t len, char* buf, size_t cap);
size_t uce_host_file_handle_pread(uint64_t handle, uint64_t offset, uint64_t len, char* buf, size_t cap);
uint64_t uce_host_file_handle_write(uint64_t handle, const char* data, size_t data_len);
uint64_t uce_host_file_handle_pwrite(uint64_t handle, uint64_t offset, const char* data, size_t data_len);
int64_t uce_host_file_handle_seek(uint64_t handle, int64_t offset, int whence);
int64_t uce_host_file_handle_tell(uint64_t handle);
void uce_host_file_handle_close(uint64_t handle);
size_t uce_host_file_stat(const char* path, size_t path_len, const char* current, size_t current_len, char* buf, size_t cap);
size_t uce_host_dir_list(const char* path, size_t path_len, const char* current, size_t current_len, char* buf, size_t cap);
int uce_host_file_rename(const char* from, size_t from_len, const char* to, size_t to_len, const char* current, size_t current_len);
int uce_host_file_copy(const char* from, size_t from_len, const char* to, size_t to_len, const char* current, size_t current_len);
int uce_host_file_truncate(const char* path, size_t path_len, const char* current, size_t current_len, uint64_t size);
int uce_host_dir_remove(const char* path, size_t path_len, const char* current, size_t current_len, int recursive);
size_t uce_host_file_temp(const char* prefix, size_t prefix_len, const char* current, size_t current_len, char* buf, size_t cap);
int uce_host_file_chmod(const char* path, size_t path_len, const char* current, size_t current_len, uint32_t mode);
int uce_host_file_symlink(const char* target, size_t target_len, const char* linkpath, size_t linkpath_len, const char* current, size_t current_len);
int uce_host_file_fsync(uint64_t handle);
int uce_host_task_spawn(const char* key, size_t key_len, uint64_t callback_id, double interval, uint64_t timeout, int repeat);
int uce_host_task_pid(const char* key, size_t key_len);
int uce_host_task_kill(int pid, int sig);
unsigned int uce_host_sleep_us(uint64_t usec);
uint64_t uce_host_socket_connect(const char* host, size_t host_len, int port);
void uce_host_socket_close(uint64_t sockfd);
int uce_host_socket_write(uint64_t sockfd, const char* data, size_t data_len);
size_t uce_host_socket_read(uint64_t sockfd, uint32_t max_length, uint32_t timeout, char* buf, size_t cap);
int uce_host_server_start_http(const char* key, size_t key_len, const char* bind, size_t bind_len, const char* file, size_t file_len, const char* function, size_t function_len, const char* current, size_t current_len);
int uce_host_server_stop(const char* key, size_t key_len);
size_t uce_host_memcache_command(uint64_t sockfd, const char* command, size_t command_len, char* buf, size_t cap);
size_t uce_host_mysql(const char* in, size_t in_len, char* out, size_t cap);
size_t uce_host_request_perf(const char* in, size_t in_len, char* out, size_t cap);
size_t uce_host_shell_exec(const char* cmd, size_t cmd_len, char* buf, size_t cap);
size_t uce_host_sha256(const char* data, size_t data_len, char* out, size_t cap);
size_t uce_host_sha256_hex(const char* data, size_t data_len, char* out, size_t cap);
size_t uce_host_hmac_sha256(const char* key, size_t key_len, const char* data, size_t data_len, char* out, size_t cap);
size_t uce_host_hmac_sha256_hex(const char* key, size_t key_len, const char* data, size_t data_len, char* out, size_t cap);
size_t uce_host_base64_encode(const char* data, size_t data_len, char* out, size_t cap);
size_t uce_host_base64_decode(const char* data, size_t data_len, char* out, size_t cap);
int uce_host_crypto_equal(const char* a, size_t a_len, const char* b, size_t b_len);
size_t uce_host_password_hash(const char* password, size_t password_len, char* out, size_t cap);
int uce_host_password_verify(const char* password, size_t password_len, const char* encoded, size_t encoded_len);
int uce_host_password_needs_rehash(const char* encoded, size_t encoded_len);
size_t uce_host_http_request(const char* in, size_t in_len, char* out, size_t cap);
uint64_t uce_host_http_request_async(const char* in, size_t in_len);
size_t uce_host_shell_exec_dv(const char* in, size_t in_len, char* out, size_t cap);
uint64_t uce_host_shell_spawn(const char* in, size_t in_len);
size_t uce_host_job_status(uint64_t job_id, char* out, size_t cap);
size_t uce_host_job_result(uint64_t job_id, char* out, size_t cap);
size_t uce_host_job_await(uint64_t job_id, uint64_t timeout_ms, char* out, size_t cap);
int uce_host_job_cancel(uint64_t job_id);
size_t uce_host_path_real(const char* path, size_t path_len, char* buf, size_t cap);
int uce_host_path_is_within(const char* path, size_t path_len, const char* root, size_t root_len);
size_t uce_host_cwd_get(char* buf, size_t cap);
int uce_host_cwd_set(const char* path, size_t path_len);
size_t uce_host_process_start_directory(char* buf, size_t cap);
size_t uce_host_last_trap_trace(char* buf, size_t cap);
}

static String wasm_current_unit_file()
{
	return(context ? context->resources.current_unit_file : String(""));
}

String shell_exec(String cmd)
{
	size_t required = uce_host_shell_exec(cmd.data(), cmd.size(), 0, 0);
	if(required == 0)
		return("");
	String output(required, 0);
	size_t got = uce_host_shell_exec(cmd.data(), cmd.size(), &output[0], required);
	output.resize(got <= required ? got : 0);
	return(output);
}
String shell_escape(String raw)
{
	String result;
	for(auto c : raw)
	{
		if(c == '\'')
			result.append("'\\''");
		else
			result.append(1, c);
	}
	return("'" + result + "'");
}
String basename(String fn) { while(fn.find("/") != String::npos) fn = fn.substr(fn.find("/") + 1); return(fn); }
String dirname(String fn) { auto pos = fn.find_last_of('/'); return(pos == String::npos ? "" : fn.substr(0, pos)); }
String path_join(String base, String child) { if(base == "") return(child); if(child == "") return(base); if(child[0] == '/') return(child); return(base + (base.back() == '/' ? "" : "/") + child); }
String path_real(String path)
{
	size_t required = uce_host_path_real(path.data(), path.size(), 0, 0);
	if(required == 0)
		return("");
	String resolved(required, 0);
	size_t got = uce_host_path_real(path.data(), path.size(), &resolved[0], required);
	resolved.resize(got <= required ? got : 0);
	return(resolved);
}
bool path_is_within(String path, String root)
{
	return(uce_host_path_is_within(path.data(), path.size(), root.data(), root.size()) != 0);
}
bool mkdir(String path)
{
	String current = wasm_current_unit_file();
	return(uce_host_file_mkdir(path.data(), path.size(), current.data(), current.size()) != 0);
}
bool file_exists(String path)
{
	String current = wasm_current_unit_file();
	return(uce_host_file_exists(path.data(), path.size(), current.data(), current.size()) != 0);
}
String file_get_contents(String file_name)
{
	String current = wasm_current_unit_file();
	size_t required = uce_host_file_read(file_name.data(), file_name.size(), current.data(), current.size(), 0, 0);
	if(required == 0)
		return("");
	String content(required, 0);
	size_t got = uce_host_file_read(file_name.data(), file_name.size(), current.data(), current.size(), &content[0], required);
	content.resize(got <= required ? got : 0);
	return(content);
}
bool file_put_contents(String file_name, String content)
{
	String current = wasm_current_unit_file();
	return(uce_host_file_write(file_name.data(), file_name.size(), current.data(), current.size(), content.data(), content.size(), 0) != 0);
}
bool file_append(String file_name, String content)
{
	String current = wasm_current_unit_file();
	return(uce_host_file_write(file_name.data(), file_name.size(), current.data(), current.size(), content.data(), content.size(), 1) != 0);
}
u64 file_open(String path, String mode)
{
	String current = wasm_current_unit_file();
	return((u64)uce_host_file_open(path.data(), path.size(), current.data(), current.size(), mode.data(), mode.size()));
}
String file_read(u64 h, u64 len)
{
	size_t required = uce_host_file_handle_read(h, len, 0, 0);
	if(required == 0)
		return("");
	String content(required, 0);
	size_t got = uce_host_file_handle_read(h, len, &content[0], required);
	content.resize(got <= required ? got : 0);
	return(content);
}
String file_pread(u64 h, u64 offset, u64 len)
{
	size_t required = uce_host_file_handle_pread(h, offset, len, 0, 0);
	if(required == 0)
		return("");
	String content(required, 0);
	size_t got = uce_host_file_handle_pread(h, offset, len, &content[0], required);
	content.resize(got <= required ? got : 0);
	return(content);
}
u64 file_write(u64 h, String data) { return(uce_host_file_handle_write(h, data.data(), data.size())); }
u64 file_pwrite(u64 h, u64 offset, String data) { return(uce_host_file_handle_pwrite(h, offset, data.data(), data.size())); }
s64 file_seek(u64 h, s64 offset, s64 whence) { return((s64)uce_host_file_handle_seek(h, offset, (int)whence)); }
s64 file_tell(u64 h) { return((s64)uce_host_file_handle_tell(h)); }
void file_close(u64 h) { uce_host_file_handle_close(h); }
static DValue wasm_decode_dvalue_result(String encoded)
{
	DValue out;
	String error;
	ucb_decode(encoded, out, &error);
	return(out);
}
DValue file_stat(String path)
{
	String current = wasm_current_unit_file();
	size_t required = uce_host_file_stat(path.data(), path.size(), current.data(), current.size(), 0, 0);
	String encoded(required, 0);
	size_t got = required ? uce_host_file_stat(path.data(), path.size(), current.data(), current.size(), &encoded[0], required) : 0;
	encoded.resize(got <= required ? got : 0);
	return(wasm_decode_dvalue_result(encoded));
}
DValue dir_list(String path)
{
	String current = wasm_current_unit_file();
	size_t required = uce_host_dir_list(path.data(), path.size(), current.data(), current.size(), 0, 0);
	String encoded(required, 0);
	size_t got = required ? uce_host_dir_list(path.data(), path.size(), current.data(), current.size(), &encoded[0], required) : 0;
	encoded.resize(got <= required ? got : 0);
	return(wasm_decode_dvalue_result(encoded));
}
bool file_rename(String from, String to)
{
	String current = wasm_current_unit_file();
	return(uce_host_file_rename(from.data(), from.size(), to.data(), to.size(), current.data(), current.size()) != 0);
}
bool file_copy(String from, String to)
{
	String current = wasm_current_unit_file();
	return(uce_host_file_copy(from.data(), from.size(), to.data(), to.size(), current.data(), current.size()) != 0);
}
bool file_truncate(String path, u64 size)
{
	String current = wasm_current_unit_file();
	return(uce_host_file_truncate(path.data(), path.size(), current.data(), current.size(), size) != 0);
}
bool dir_remove(String path, bool recursive)
{
	String current = wasm_current_unit_file();
	return(uce_host_dir_remove(path.data(), path.size(), current.data(), current.size(), recursive ? 1 : 0) != 0);
}
String file_temp(String prefix)
{
	String current = wasm_current_unit_file();
	size_t required = uce_host_file_temp(prefix.data(), prefix.size(), current.data(), current.size(), 0, 0);
	if(required == 0) return("");
	String out(required, 0);
	size_t got = uce_host_file_temp(prefix.data(), prefix.size(), current.data(), current.size(), &out[0], required);
	out.resize(got <= required ? got : 0);
	return(out);
}
bool file_chmod(String path, u32 mode)
{
	String current = wasm_current_unit_file();
	return(uce_host_file_chmod(path.data(), path.size(), current.data(), current.size(), mode) != 0);
}
bool file_symlink(String target, String linkpath)
{
	String current = wasm_current_unit_file();
	return(uce_host_file_symlink(target.data(), target.size(), linkpath.data(), linkpath.size(), current.data(), current.size()) != 0);
}
bool file_fsync(u64 h) { return(uce_host_file_fsync(h) != 0); }


static String wasm_string_hostcall_1(size_t (*fn)(const char*, size_t, char*, size_t), String data)
{
	size_t required = fn(data.data(), data.size(), 0, 0);
	String out(required, 0);
	size_t got = required ? fn(data.data(), data.size(), &out[0], required) : 0;
	out.resize(got <= required ? got : 0);
	return(out);
}
String sha256(String data) { return(wasm_string_hostcall_1(uce_host_sha256, data)); }
String sha256_hex(String data) { return(wasm_string_hostcall_1(uce_host_sha256_hex, data)); }
String hmac_sha256(String key, String data)
{
	size_t required = uce_host_hmac_sha256(key.data(), key.size(), data.data(), data.size(), 0, 0);
	String out(required, 0); size_t got = required ? uce_host_hmac_sha256(key.data(), key.size(), data.data(), data.size(), &out[0], required) : 0; out.resize(got <= required ? got : 0); return(out);
}
String hmac_sha256_hex(String key, String data)
{
	size_t required = uce_host_hmac_sha256_hex(key.data(), key.size(), data.data(), data.size(), 0, 0);
	String out(required, 0); size_t got = required ? uce_host_hmac_sha256_hex(key.data(), key.size(), data.data(), data.size(), &out[0], required) : 0; out.resize(got <= required ? got : 0); return(out);
}
String password_hash(String password)
{
	String encoded(256, 0);
	size_t got = uce_host_password_hash(password.data(), password.size(), &encoded[0], encoded.size());
	if(got > encoded.size())
		return("");
	encoded.resize(got);
	return(encoded);
}
bool password_verify(String password, String encoded) { return(uce_host_password_verify(password.data(), password.size(), encoded.data(), encoded.size()) != 0); }
bool password_needs_rehash(String encoded) { return(uce_host_password_needs_rehash(encoded.data(), encoded.size()) != 0); }
String base64_decode(String raw) { return(wasm_string_hostcall_1(uce_host_base64_decode, raw)); }
String random_bytes(u64 n)
{
	if(n > 1024 * 1024) n = 1024 * 1024;
	String out(n, 0); size_t got = n ? uce_host_random(&out[0], n) : 0; out.resize(got <= n ? got : 0); return(out);
}
bool crypto_equal(String a, String b) { return(uce_host_crypto_equal(a.data(), a.size(), b.data(), b.size()) != 0); }


DValue http_request(DValue req)
{
	String encoded = ucb_encode(req);
	size_t required = uce_host_http_request(encoded.data(), encoded.size(), 0, 0);
	String out(required, 0); size_t got = required ? uce_host_http_request(encoded.data(), encoded.size(), &out[0], required) : 0; out.resize(got <= required ? got : 0); return(wasm_decode_dvalue_result(out));
}
u64 http_request_async(DValue req)
{
	String encoded = ucb_encode(req);
	return((u64)uce_host_http_request_async(encoded.data(), encoded.size()));
}

DValue shell_exec(DValue spec)
{
	String encoded = ucb_encode(spec);
	size_t required = uce_host_shell_exec_dv(encoded.data(), encoded.size(), 0, 0);
	String out(required, 0);
	size_t got = required ? uce_host_shell_exec_dv(encoded.data(), encoded.size(), &out[0], required) : 0;
	out.resize(got <= required ? got : 0);
	return(wasm_decode_dvalue_result(out));
}
u64 shell_spawn(DValue spec)
{
	String encoded = ucb_encode(spec);
	return((u64)uce_host_shell_spawn(encoded.data(), encoded.size()));
}
DValue job_status(u64 job_id)
{
	size_t required = uce_host_job_status(job_id, 0, 0);
	String out(required, 0);
	size_t got = required ? uce_host_job_status(job_id, &out[0], required) : 0;
	out.resize(got <= required ? got : 0);
	return(wasm_decode_dvalue_result(out));
}
DValue job_result(u64 job_id)
{
	size_t required = uce_host_job_result(job_id, 0, 0);
	String out(required, 0);
	size_t got = required ? uce_host_job_result(job_id, &out[0], required) : 0;
	out.resize(got <= required ? got : 0);
	return(wasm_decode_dvalue_result(out));
}
DValue job_await(u64 job_id, u64 timeout_ms)
{
	size_t required = uce_host_job_await(job_id, timeout_ms, 0, 0);
	String out(required, 0);
	size_t got = required ? uce_host_job_await(job_id, timeout_ms, &out[0], required) : 0;
	out.resize(got <= required ? got : 0);
	return(wasm_decode_dvalue_result(out));
}
bool job_cancel(u64 job_id) { return(uce_host_job_cancel(job_id) != 0); }

String cwd_get()
{
	size_t required = uce_host_cwd_get(0, 0);
	if(required == 0)
		return("");
	String cwd(required, 0);
	size_t got = uce_host_cwd_get(&cwd[0], required);
	cwd.resize(got <= required ? got : 0);
	return(cwd);
}
void cwd_set(String path) { uce_host_cwd_set(path.data(), path.size()); }
String process_start_directory()
{
	size_t required = uce_host_process_start_directory(0, 0);
	if(required == 0)
		return("");
	String cwd(required, 0);
	size_t got = uce_host_process_start_directory(&cwd[0], required);
	cwd.resize(got <= required ? got : 0);
	return(cwd);
}
u64 file_mtime(String file_name)
{
	String current = wasm_current_unit_file();
	return((u64)uce_host_file_mtime(file_name.data(), file_name.size(), current.data(), current.size()));
}
void file_unlink(String file_name)
{
	String current = wasm_current_unit_file();
	uce_host_file_unlink(file_name.data(), file_name.size(), current.data(), current.size());
}
String expand_path(String path, String relative_to_path) { return(path_join(relative_to_path, path)); }
StringList ls(String dir)
{
	String current = wasm_current_unit_file();
	size_t required = uce_host_file_list(dir.data(), dir.size(), current.data(), current.size(), 0, 0);
	if(required == 0)
		return(StringList());
	String listing(required, 0);
	size_t got = uce_host_file_list(dir.data(), dir.size(), current.data(), current.size(), &listing[0], required);
	listing.resize(got <= required ? got : 0);
	if(listing == "")
		return(StringList());
	return(split(listing, "\n"));
}
DValue request_perf()
{
	size_t required = uce_host_request_perf("", 0, 0, 0);
	if(required == 0)
		return(DValue());
	String encoded_response(required, 0);
	size_t got = uce_host_request_perf("", 0, &encoded_response[0], required);
	if(got == 0 || got > required)
		return(DValue());
	DValue response;
	String decode_error;
	if(ucb_decode(encoded_response, response, &decode_error))
		return(response);
	return(DValue());
}

f64 time_precise() { return(uce_host_time_precise()); }
u64 time() { return(uce_host_time()); }
// The native build shells out to `date`; the wasm core has no shell, so it
// formats with wasi-libc strftime (no TZ data → local == UTC, acceptable).
static String wasm_time_strftime(String format, u64 timestamp, bool utc)
{
	if(timestamp == 0)
		timestamp = time();
	time_t t = (time_t)timestamp;
	struct tm tmv;
	if(utc)
		gmtime_r(&t, &tmv);
	else
		localtime_r(&t, &tmv);
	char buffer[512];
	size_t n = strftime(buffer, sizeof(buffer), format.c_str(), &tmv);
	return(String(buffer, n));
}
String time_format_local(String format, u64 timestamp) { return(wasm_time_strftime(format, timestamp, false)); }
String time_format_utc(String format, u64 timestamp)
{
	if(format == "RFC1123")
		format = "%a, %d %b %Y %T GMT";
	return(wasm_time_strftime(format, timestamp, true));
}
static String wasm_time_expand_delta(String format, u64 timestamp, u64 now_timestamp)
{
	u64 delta_seconds = now_timestamp > timestamp ? now_timestamp - timestamp : 0;
	format = replace(format, "%deltaY", std::to_string(delta_seconds / (60 * 60 * 24 * 365)));
	format = replace(format, "%deltam", std::to_string(delta_seconds / (60 * 60 * 24 * 30)));
	format = replace(format, "%deltad", std::to_string(delta_seconds / (60 * 60 * 24)));
	format = replace(format, "%deltaH", std::to_string(delta_seconds / (60 * 60)));
	format = replace(format, "%deltaM", std::to_string(delta_seconds / 60));
	format = replace(format, "%deltaS", std::to_string(delta_seconds));
	return(format);
}
String time_format_relative(u64 timestamp, String format_very_recent, u64 medium_recency_seconds, String format_medium_recent, u64 not_recent_seconds, String format_not_recent)
{
	u64 now_timestamp = time();
	u64 delta_seconds = now_timestamp > timestamp ? now_timestamp - timestamp : 0;
	format_very_recent = first(format_very_recent, "just now");
	medium_recency_seconds = medium_recency_seconds > 0 ? medium_recency_seconds : 90;
	format_medium_recent = first(format_medium_recent, "%deltaM minutes ago");
	not_recent_seconds = not_recent_seconds > 0 ? not_recent_seconds : 90 * 60;
	format_not_recent = first(format_not_recent, "%deltaH hours ago");
	if(delta_seconds < medium_recency_seconds)
		return(wasm_time_expand_delta(format_very_recent, timestamp, now_timestamp));
	if(delta_seconds < not_recent_seconds)
		return(wasm_time_expand_delta(format_medium_recent, timestamp, now_timestamp));
	return(wasm_time_expand_delta(format_not_recent, timestamp, now_timestamp));
}
u64 time_parse(String time_String)
{
	time_String = trim(time_String);
	if(time_String == "")
		return(0);
	if(time_String.size() == 19 || (time_String.size() == 20 && time_String[19] == 'Z') || (time_String.size() == 23 && time_String.substr(19) == " UTC"))
	{
		struct tm parsed = {};
		String calendar = time_String.substr(0, 19);
		const char* end = strptime(calendar.c_str(), calendar[10] == 'T' ? "%Y-%m-%dT%H:%M:%S" : "%Y-%m-%d %H:%M:%S", &parsed);
		if(end && *end == 0)
		{
			char normalized[20];
			strftime(normalized, sizeof(normalized), "%Y-%m-%d %H:%M:%S", &parsed);
			calendar[10] = ' ';
			if(calendar == normalized)
			{
				time_t timestamp = timegm(&parsed);
				if(timestamp >= 0)
					return((u64)timestamp);
			}
		}
	}
	return(int_val(trim(shell_exec("date -u -d " + shell_escape(time_String) + " +'%s'"))));
}
u64 socket_connect(String host, u16 port)
{
	return(uce_host_socket_connect(host.data(), host.size(), (int)port));
}
void socket_close(u64 sockfd) { uce_host_socket_close(sockfd); }
bool socket_write(u64 sockfd, String data)
{
	return(uce_host_socket_write(sockfd, data.data(), data.size()) != 0);
}
String socket_read(u64 sockfd, u32 max_length, u32 timeout)
{
	size_t required = uce_host_socket_read(sockfd, max_length, timeout, 0, 0);
	if(required == 0)
		return("");
	String content(required, 0);
	size_t got = uce_host_socket_read(sockfd, max_length, timeout, &content[0], required);
	content.resize(got <= required ? got : 0);
	return(content);
}
String ws_message() { return(context ? context->in : ""); }
String ws_connection_id() { return(context ? context->resources.websocket_connection_id : ""); }
String ws_scope() { return(context ? context->resources.websocket_scope : ""); }
u8 ws_opcode() { return(context ? context->resources.websocket_opcode : 0); }
bool ws_is_binary() { return(context && context->resources.websocket_is_binary); }
StringList ws_connections(String scope) { (void)scope; return(context ? context->resources.websocket_scope_connection_ids : StringList()); }
u64 ws_connection_count(String scope) { return(ws_connections(scope).size()); }
// The wasm workspace owns no connections; ws_send/ws_close record dispatch
// commands (same shape as the native websocket_exec capture) that the host
// carries back to the broker, which sends them over the client connections.
bool ws_send(String message, bool binary, String scope)
{
	if(!context)
		return(false);
	DValue command;
	command["action"] = "broadcast";
	command["binary"].set_bool(binary);
	command["message_b64"] = base64_encode(message);
	command["scope"] = scope;
	context->resources.websocket_dispatch_commands.push(command);
	return(true);
}
bool ws_send_to(String connection_id, String message, bool binary)
{
	if(!context)
		return(false);
	DValue command;
	command["action"] = "send_to";
	command["binary"].set_bool(binary);
	command["message_b64"] = base64_encode(message);
	command["connection_id"] = connection_id;
	context->resources.websocket_dispatch_commands.push(command);
	return(true);
}
bool ws_close(String connection_id)
{
	if(!context)
		return(false);
	if(connection_id == "")
		connection_id = ws_connection_id();
	DValue command;
	command["action"] = "close";
	command["connection_id"] = connection_id;
	command["status_code"] = (f64)1000;
	command["reason"] = "";
	context->resources.websocket_dispatch_commands.push(command);
	return(true);
}
String backtrace_get_frames(void* const* frames, size_t size, u32 skip_frames) { (void)frames; (void)size; (void)skip_frames; return(backtrace_capture(0, 0)); }
String backtrace_capture(u32 max_frames, u32 skip_frames)
{
	(void)max_frames;
	(void)skip_frames;
	size_t required = uce_host_last_trap_trace(0, 0);
	if(required == 0)
		return("");
	String trace(required, 0);
	size_t got = uce_host_last_trap_trace(&trace[0], required);
	trace.resize(got <= required ? got : 0);
	return(trace);
}
String signal_name(s32 sig)
{
	switch(sig)
	{
		case 6: return("SIGABRT");
		case 7: return("SIGBUS");
		case 8: return("SIGFPE");
		case 4: return("SIGILL");
		case 2: return("SIGINT");
		case 11: return("SIGSEGV");
		case 15: return("SIGTERM");
		default: return("");
	}
}
String memcache_escape_key(String key)
{
	String result;
	for(auto c : key)
	{
		if(isspace((unsigned char)c))
			c = '_';
		result.append(1, c);
	}
	return(result);
}
StringList memcache_escape_keys(StringList keys)
{
	StringList result;
	for(auto s : keys)
		result.push_back(memcache_escape_key(s));
	return(result);
}
u64 memcache_connect(String host, u16 port)
{
	if(host == "")
		host = "127.0.0.1";
	if(port == 0)
		port = 11211;
	return(socket_connect(host, port));
}
String memcache_command(u64 connection, String command)
{
	size_t required = uce_host_memcache_command(connection, command.data(), command.size(), 0, 0);
	if(required == 0)
		return("");
	String content(required, 0);
	size_t got = uce_host_memcache_command(connection, command.data(), command.size(), &content[0], required);
	content.resize(got <= required ? got : 0);
	return(content);
}
bool memcache_set(u64 connection, String key, String value, u64 expires_in)
{
	String response = memcache_command(
		connection,
		String("set ") + memcache_escape_key(key) + " 0 " + std::to_string(expires_in) + " " + std::to_string(value.length()) + "\r\n" + value
	);
	return("STORED" == trim(response));
}
bool memcache_delete(u64 connection, String key)
{
	String response = memcache_command(connection, String("delete ") + memcache_escape_key(key));
	return("DELETED" == trim(response));
}
String memcache_get(u64 connection, String key, String default_value)
{
	String res = memcache_command(connection, String("get ") + memcache_escape_key(key));
	String header_end = "\r\n";
	size_t header_pos = res.find(header_end);
	if(res.rfind("VALUE ", 0) != 0 || header_pos == String::npos)
		return(default_value);
	String header = res.substr(0, header_pos);
	StringList parts = split(header, " ");
	if(parts.size() < 4)
		return(default_value);
	u64 length = int_val(parts[3]);
	size_t data_pos = header_pos + header_end.size();
	if(res.size() < data_pos + length)
		return(default_value);
	return(res.substr(data_pos, length));
}
StringMap memcache_get_multiple(u64 connection, StringList keys)
{
	StringMap result;
	String res = memcache_command(connection, String("get ") + join(memcache_escape_keys(keys), " "));
	while(res.rfind("VALUE ", 0) == 0)
	{
		size_t header_pos = res.find("\r\n");
		if(header_pos == String::npos)
			break;
		StringList parts = split(res.substr(0, header_pos), " ");
		if(parts.size() < 4)
			break;
		String key = parts[1];
		u64 length = int_val(parts[3]);
		size_t data_pos = header_pos + 2;
		if(res.size() < data_pos + length)
			break;
		result[key] = res.substr(data_pos, length);
		res = res.substr(data_pos + length);
		if(res.rfind("\r\n", 0) == 0)
			res = res.substr(2);
	}
	return(result);
}
void on_segfault(int sig) { (void)sig; }

static u64 wasm_next_task_callback_id = 1;
static std::map<u64, std::function<void()>> wasm_task_callbacks;

extern "C" int uce_wasm_task_run(uint64_t callback_id)
{
	auto it = wasm_task_callbacks.find(callback_id);
	if(it == wasm_task_callbacks.end())
		return(1);
	it->second();
	return(0);
}

int task_kill(pid_t pid, s32 sig) { return(uce_host_task_kill(pid, (int)sig)); }
String runtime_safe_key(String key, String label)
{
	(void)label;
	key = trim(key);
	if(key == "")
		return("");
	return(gen_sha1(key));
}
pid_t task(String key, std::function<void()> exec_after_spawn, u64 timeout)
{
	u64 id = wasm_next_task_callback_id++;
	wasm_task_callbacks[id] = exec_after_spawn;
	return((pid_t)uce_host_task_spawn(key.data(), key.size(), id, 0.0, timeout, 0));
}
pid_t task_repeat(String key, f64 interval, std::function<void()> exec_after_spawn, u64 timeout)
{
	if(!(interval > 0) || !std::isfinite(interval))
		return(0);
	u64 id = wasm_next_task_callback_id++;
	wasm_task_callbacks[id] = exec_after_spawn;
	return((pid_t)uce_host_task_spawn(key.data(), key.size(), id, interval, timeout, 1));
}
pid_t task_pid(String key) { return((pid_t)uce_host_task_pid(key.data(), key.size())); }
extern "C" unsigned int sleep(unsigned int seconds) { return(uce_host_sleep_us((uint64_t)seconds * 1000000ull)); }
extern "C" s32 usleep(u32 usec) { uce_host_sleep_us(usec); return(0); }
pid_t server_start_http(String key, String socket_fn_or_port, String call_uce_filename, String call_function)
{
	String current = wasm_current_unit_file();
	return((pid_t)uce_host_server_start_http(
		key.data(), key.size(),
		socket_fn_or_port.data(), socket_fn_or_port.size(),
		call_uce_filename.data(), call_uce_filename.size(),
		call_function.data(), call_function.size(),
		current.data(), current.size()
	));
}
bool server_stop(String key) { return(uce_host_server_stop(key.data(), key.size()) != 0); }
StringMap default_config()
{
	StringMap cfg;
	cfg["SESSION_TIME"] = std::to_string(60*60*24*30);
	cfg["MAX_MEMORY"] = std::to_string(1024*1024*16);
	return(cfg);
}

#else
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <execinfo.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <errno.h>
#include "sys.h"
#include "hash.h"
#include "uri.h"

String sha256(String data) { return(sha256_native(data)); }
String sha256_hex(String data) { return(sha256_hex_native(data)); }
String hmac_sha256(String key, String data) { return(hmac_sha256_native(key, data)); }
String hmac_sha256_hex(String key, String data) { return(hmac_sha256_hex_native(key, data)); }
String base64_decode(String raw) { bool ok=false; return(::base64_decode(raw, ok)); }
String random_bytes(u64 n) { if(n > 1024*1024) n = 1024*1024; String out(n, 0); int fd=open("/dev/urandom", O_RDONLY); if(fd<0) return(""); size_t off=0; while(off<n) { ssize_t got=read(fd, &out[off], n-off); if(got<0 && errno==EINTR) continue; if(got<=0) break; off += (size_t)got; } close(fd); out.resize(off); return(out); }
bool crypto_equal(String a, String b) { return(crypto_equal_native(a, b)); }
String password_hash(String password) { return(password_hash_native(password)); }
bool password_verify(String password, String encoded) { return(password_verify_native(password, encoded)); }
bool password_needs_rehash(String encoded) { return(password_needs_rehash_native(encoded)); }

// Single definitions for the native split build (declared extern in sys.h).
pid_t parent_pid = 0;
pid_t my_pid = 0;

namespace {

u64 file_lock_timeout_ms()
{
	const char* raw = getenv("UCE_FILE_LOCK_TIMEOUT_MS");
	if(!raw || !*raw)
		return(2000);
	char* end = 0;
	unsigned long long parsed = strtoull(raw, &end, 10);
	if(end == raw)
		return(2000);
	return((u64)parsed);
}

u64 monotonic_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return((u64)ts.tv_sec * 1000ull + (u64)ts.tv_nsec / 1000000ull);
}

int open_locked_file(String file_name, int open_flags, int lock_type, int create_mode = 0644)
{
	int fd = open(file_name.c_str(), open_flags, create_mode);
	if(fd == -1)
		return(-1);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	u64 timeout = file_lock_timeout_ms();
	u64 deadline = monotonic_ms() + timeout;
	while(true)
	{
		if(flock(fd, lock_type | LOCK_NB) == 0)
			return(fd);
		if(errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
		{
			close(fd);
			return(-1);
		}
		if(timeout == 0 || monotonic_ms() >= deadline)
		{
			fprintf(stderr, "(!) file lock timeout after %llums: %s\n", (unsigned long long)timeout, file_name.c_str());
			close(fd);
			return(-1);
		}
		usleep(10000);
	}
}

void close_locked_file(int fd)
{
	if(fd == -1)
		return;
	flock(fd, LOCK_UN);
	close(fd);
}

}

String backtrace_get_frames(void* const* frames, size_t size, u32 skip_frames)
{
	if(size == 0)
		return("");

	char** symbols = backtrace_symbols(frames, size);
	if(!symbols)
		return("");

	String trace;
	for(size_t i = skip_frames; i < size; i++)
	{
		trace += symbols[i];
		trace += "\n";
	}
	free(symbols);
	return(trace);
}

String backtrace_capture(u32 max_frames, u32 skip_frames)
{
	if(max_frames == 0)
		return("");

	std::vector<void*> frames(max_frames);
	size_t size = backtrace(frames.data(), max_frames);
	return(backtrace_get_frames(frames.data(), size, skip_frames));
}

String signal_name(s32 sig)
{
	switch(sig)
	{
		case SIGABRT: return("SIGABRT");
		case SIGBUS: return("SIGBUS");
		case SIGFPE: return("SIGFPE");
		case SIGILL: return("SIGILL");
		case SIGINT: return("SIGINT");
		case SIGSEGV: return("SIGSEGV");
		case SIGTERM: return("SIGTERM");
		default: return("");
	}
}

namespace {

String time_format_expand_delta_tokens(String format, u64 timestamp, u64 now_timestamp)
{
	u64 delta_seconds = 0;
	if(now_timestamp > timestamp)
		delta_seconds = now_timestamp - timestamp;

	format = replace(format, "%deltaY", std::to_string(delta_seconds / (60 * 60 * 24 * 365)));
	format = replace(format, "%deltam", std::to_string(delta_seconds / (60 * 60 * 24 * 30)));
	format = replace(format, "%deltad", std::to_string(delta_seconds / (60 * 60 * 24)));
	format = replace(format, "%deltaH", std::to_string(delta_seconds / (60 * 60)));
	format = replace(format, "%deltaM", std::to_string(delta_seconds / 60));
	format = replace(format, "%deltaS", std::to_string(delta_seconds));
	return(format);
}

String time_format_shell(String format, u64 timestamp, bool use_utc)
{
	String ts;
	String fmt;
	u64 effective_timestamp = (timestamp > 0 ? timestamp : time());
	String expanded_format = time_format_expand_delta_tokens(format, effective_timestamp, time());

	if(timestamp > 0)
		ts = String("-d '@")+std::to_string(timestamp)+"'";
	if(expanded_format != "")
		fmt = String("+'")+expanded_format+"'";

	return(trim(shell_exec(String("date ") + (use_utc ? "-u " : "") + ts + " " + fmt)));
}

}

String shell_exec(String cmd)
{
	//printf("(i) shell_exec(%s)\n", cmd.c_str());
	String data;
	FILE * stream;
	const int max_buffer = 256;
	char buffer[max_buffer];
	cmd.append(" 2>&1");

	stream = popen(cmd.c_str(), "r");

	if (stream) {
		while (!feof(stream))
			if (fgets(buffer, max_buffer, stream) != NULL) data.append(buffer);
		pclose(stream);
	}
	return data;
}

String shell_escape(String raw)
{
	String result = "";
	for(auto c : raw)
	{
		if(c == '\'')
		{
			result.append("'\\''");
		}
		else
		{
			result.append(1, c);
		}
	}
	return("\'" + result + "\'");
	/*
	`	U+0060 (Grave Accent)	Backtick	Command substitution
~	U+007E	Tilde	Tilde expansion
!	U+0021	Exclamation mark	History expansion
#	U+0023 Number sign	Hash	Comments
$	U+0024	Dollar sign	Parameter expansion
&	U+0026	Ampersand	Background commands
*	U+002A	Asterisk	Filename expansion and globbing
(	U+0028	Left Parenthesis	Subshells
)	U+0029	Right Parenthesis	Subshells
   	U+0009	Tab (⇥)	Word splitting (whitespace)
{	U+007B Left Curly Bracket	Left brace	Brace expansion
[	U+005B	Left Square Bracket	Filename expansion and globbing
|	U+007C Vertical Line	Vertical bar	Pipelines
\	U+005C Reverse Solidus	Backslash	Escape character
;	U+003B	Semicolon	Separating commands
'	U+0027 Apostrophe	Single quote	String quoting
"	U+0022 Quotation Mark	Double quote	String quoting with interpolation
↩	U+000A Line Feed	Newline	Line break
<	U+003C	Less than	Input redirection
>	U+003E	Greater than	Output redirection
?	U+003F	Question mark	Filename expansion and globbing
  	U+0020	Space	Word splitting1 (whitespace)
 */
}

String basename(String fn)
{
	String result;
	while(fn.length() > 0)
		result = nibble("/", fn);
	//printf("basename(%s) %s\n", fn.c_str(), result.c_str());
	return(result);
}

String dirname(String fn)
{
	String result;
	auto seg = split(fn, "/");
	seg.pop_back();
	result = join(seg, "/");
	//printf("dirname(%s) %s seg#%i\n", fn.c_str(), result.c_str(), seg.size());
	return(result);
}

String path_join(String base, String child)
{
	if(base == "")
		return(child);
	if(child == "")
		return(base);
	if(child[0] == '/')
		return(child);
	if(base[base.length() - 1] == '/')
		return(base + child);
	return(base + "/" + child);
}

String path_real(String path)
{
	char resolved[PATH_MAX];
	if(realpath(path.c_str(), resolved))
		return(String(resolved));
	return("");
}

bool path_is_within(String path, String root)
{
	String real_path = path_real(path);
	String real_root = path_real(root);
	if(real_path == "" || real_root == "")
		return(false);
	if(real_path == real_root)
		return(true);
	if(real_root[real_root.length() - 1] != '/')
		real_root += "/";
	return(str_starts_with(real_path, real_root));
}

bool mkdir(String path)
{
	if(path == "")
		return(false);
	std::error_code ec;
	if(std::filesystem::exists(path, ec))
		return(std::filesystem::is_directory(path, ec));
	return(std::filesystem::create_directories(path, ec) || std::filesystem::is_directory(path, ec));
}

bool file_exists(String path)
{
	std::filesystem::path fp{ path };
	return(std::filesystem::exists(fp));
}

namespace {

String file_read_all(int fd)
{
	if(fd == -1)
		return("");

	char buf[512];
	String content;
	s64 bytes_read = 0;
	lseek(fd, 0, SEEK_SET);
	while((bytes_read = read(fd, buf, sizeof(buf))) > 0)
		content.append(buf, bytes_read);
	return(content);
}


bool file_write_all(int fd, const char* data, size_t remaining)
{
	while(remaining > 0)
	{
		auto bytes_written = write(fd, data, remaining);
		if(bytes_written < 0)
		{
			if(errno == EINTR)
				continue;
			return(false);
		}
		if(bytes_written == 0)
			return(false);
		data += bytes_written;
		remaining -= bytes_written;
	}
	return(true);
}

}

String file_get_contents(String file_name)
{
	s32 fd = open_locked_file(file_name, O_RDONLY, LOCK_SH);
	if(fd == -1)
	{
		printf("(!) Could not read %s\n", file_name.c_str());
		return("");
	}
	String content = file_read_all(fd);
	close_locked_file(fd);
	return(content);
}

bool file_put_contents(String file_name, String content)
{
	s32 fd = open_locked_file(file_name, O_RDWR | O_CREAT, LOCK_EX, 0644);
	if(fd == -1)
	{
		printf("(!) Could not write %s\n", file_name.c_str());
		return(false);
	}
	lseek(fd, 0, SEEK_SET);
	bool ok = ftruncate(fd, 0) == 0 && file_write_all(fd, content.data(), content.length());
	close_locked_file(fd);
	if(!ok)
	{
		printf("(!) Could not fully write %s\n", file_name.c_str());
		return(false);
	}
	return(true);
}

bool file_append(String file_name, String content)
{
	s32 fd = open_locked_file(file_name, O_RDWR | O_CREAT, LOCK_EX, 0644);
	if(fd == -1)
	{
		printf("(!) Could not append %s\n", file_name.c_str());
		return(false);
	}
	lseek(fd, 0, SEEK_END);
	bool ok = file_write_all(fd, content.data(), content.length());
	close_locked_file(fd);
	if(!ok)
	{
		printf("(!) Could not fully append %s\n", file_name.c_str());
		return(false);
	}
	return(true);
}

String cwd_get()
{
	return(std::filesystem::current_path());
}

String process_start_directory()
{
	// Primed in main() before any unit handler can chdir; fault recovery and
	// config-relative path resolution anchor to this instead of the volatile
	// per-unit working directory.
	static String start_directory = cwd_get();
	return(start_directory);
}

void cwd_set(String path)
{
	chdir(path.c_str());
}

u64 file_mtime(String file_name)
{
	struct stat info;
	if (stat(file_name.c_str(), &info) != 0)
	{
		return(0);
	}
	else
	{
		return(info.st_mtime);
	}
}

void file_unlink(String file_name)
{
	remove(file_name.c_str());
}

String expand_path(String path, String relative_to_path)
{
	String result;

	if(relative_to_path == "")
		relative_to_path = cwd_get();

	auto base_path = split(relative_to_path, "/");
	auto rel_path = split(path, "/");

	for(auto& s : rel_path)
	{
		if(s == "..")
		{
			base_path.pop_back();
		}
		else if(s == ".")
		{

		}
		else
		{
			base_path.push_back(s);
		}
	}

	return(join(base_path, "/"));
}

f64 time_precise()
{
	return ((f64)std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::high_resolution_clock::now().time_since_epoch()).count()) / 1000000;
}

u64 time()
{
	return(std::time(0));
}

String time_format_local(String format, u64 timestamp)
{
	return(time_format_shell(format, timestamp, false));
}

String time_format_utc(String format, u64 timestamp)
{
	if(format == "RFC1123")
		format = "%a, %d %b %Y %T GMT";
	return(time_format_shell(format, timestamp, true));
}

String time_format_relative(u64 timestamp, String format_very_recent, u64 medium_recency_seconds, String format_medium_recent, u64 not_recent_seconds, String format_not_recent)
{
	u64 now_timestamp = time();
	u64 delta_seconds = 0;
	if(now_timestamp > timestamp)
		delta_seconds = now_timestamp - timestamp;

	format_very_recent = first(format_very_recent, "just now");
	medium_recency_seconds = (medium_recency_seconds > 0 ? medium_recency_seconds : 90);
	format_medium_recent = first(format_medium_recent, "%deltaM minutes ago");
	not_recent_seconds = (not_recent_seconds > 0 ? not_recent_seconds : 90 * 60);
	format_not_recent = first(format_not_recent, "%deltaH hours ago");

	if(delta_seconds < medium_recency_seconds)
		return(time_format_expand_delta_tokens(format_very_recent, timestamp, now_timestamp));
	if(delta_seconds < not_recent_seconds)
		return(time_format_expand_delta_tokens(format_medium_recent, timestamp, now_timestamp));
	return(time_format_expand_delta_tokens(format_not_recent, timestamp, now_timestamp));
}

u64 time_parse(String time_String)
{
	time_String = trim(time_String);
	if(time_String == "")
		return(0);
	return(int_val(trim(shell_exec("date -u -d "+shell_escape(time_String)+" +'%s'"))));
}

u64 socket_connect(String host, u16 port)
{

	/*String addrinfo {
		int              ai_flags;
		int              ai_family;
		int              ai_socktype;
		int              ai_protocol;
		socklen_t        ai_addrlen;
		String sockaddr *ai_addr;
		char            *ai_canonname;
		String addrinfo *ai_next;
	};*/

    auto sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(sockfd < 0)
    {
		print("SOCKET ERROR (could not open socket)\n");
		perror("SOCKET ERROR ");
		return(0);
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(host.c_str());

    if(connect(sockfd, (struct sockaddr*) &addr, sizeof(addr)) < 0)
    {
		print("SOCKET ERROR (could not connect to address " + String(host) + ":" + std::to_string(port) + ")\n");
		perror("SOCKET ERROR ");
		close(sockfd);
		return(0);
	}
	context->resources.sockets.push_back(sockfd);
	return(sockfd);
}

void socket_close(u64 sockfd)
{
	close(sockfd);
}

bool socket_write(u64 sockfd, String data)
{
	return(write(sockfd, data.c_str(), data.length()) >= 0);
}

String socket_read(u64 sockfd, u32 max_length, u32 timeout)
{
	struct timeval tv;
	tv.tv_sec = timeout;
	tv.tv_usec = 0;
	setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
	if(max_length == 0)
		return("");
	std::vector<char> buf(max_length);
	auto byte_count = recv(sockfd, buf.data(), buf.size(), 0);
	if(byte_count > 0)
		return(String(buf.data(), byte_count));
	return("");
}

String memcache_escape_key(String key)
{
	String result;
	for(auto c : key)
	{
		if(isspace((unsigned char)c))
			c = '_';
		result.append(1, c);
	}
	return(result);
}

StringList memcache_escape_keys(StringList keys)
{
	StringList result;
	for(auto s : keys)
	{
		result.push_back(memcache_escape_key(s));
	}
	return(result);
}

u64 memcache_connect(String host, u16 port)
{
	return(socket_connect(host, port));
}

String memcache_command(u64 connection, String command)
{
	socket_write(connection, command+"\r\n");
	return(socket_read(connection)); // FIXME: do multi-chunk until END line is received!
}

bool memcache_set(u64 connection, String key, String value, u64 expires_in)
{
	socket_write(connection,
		// set KEY META_DATA EXPIRY_TIME LENGTH_IN_BYTES
		String("set ") + memcache_escape_key(key) + " 0 " + std::to_string(expires_in) + " " + std::to_string(value.length()) + "\r\n" +
		value + "\r\n");
	return("STORED" == trim(socket_read(connection)));
}

bool memcache_delete(u64 connection, String key)
{
	socket_write(connection,
		// set KEY META_DATA EXPIRY_TIME LENGTH_IN_BYTES
		String("delete ") + memcache_escape_key(key) + "\r\n"
		);
	return("DELETED" == trim(socket_read(connection)));
}

String memcache_get(u64 connection, String key, String default_value)
{
	auto res = memcache_command(connection, String("get ")+memcache_escape_key(key));
	String t = nibble(res, " ");
	if(t == "VALUE")
	{
		String key = nibble(res, " ");
		String meta = nibble(res, " ");
		u32 length = stoi(nibble(res, "\r\n"));
		return(res.substr(0, length));
	}
	return(default_value);
}

StringMap memcache_get_multiple(u64 connection, StringList keys)
{
	StringMap result;
	// to do: escape key String
	auto res = memcache_command(connection, String("get ")+join(memcache_escape_keys(keys), " "));
	while(res.length() > 0)
	{
		String t = nibble(res, " ");
		if(t == "VALUE")
		{
			String key = nibble(res, " ");
			String meta = nibble(res, " ");
			u32 length = stoi(nibble(res, "\r\n"));
			result[key] = res.substr(0, length);
			res = res.substr(length+2);
		}
	}
	return(result);
}

void on_segfault(int sig)
{
	String trace = backtrace_capture(32, 1);
	String sig_label = signal_name(sig);
	if(sig_label != "")
		fprintf(stderr, "SEG FAULT: %d (%s):\n%s", sig, sig_label.c_str(), trace.c_str());
	else
		fprintf(stderr, "SEG FAULT: %d:\n%s", sig, trace.c_str());
	exit(1);
}

struct Worker {
	pid_t pid;
};

std::map<pid_t, Worker> workers;
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/prctl.h>

// The process-wide reaper may run on a runtime thread while a request thread
// synchronously owns a child. Preserve unknown statuses until that owner takes
// them instead of forcing it to guess success after waitpid reports ECHILD.
static const size_t child_exit_status_capacity = 256;
static volatile sig_atomic_t child_exit_status_cursor = 0;
static volatile sig_atomic_t child_exit_status_pids[child_exit_status_capacity] = {0};
static volatile sig_atomic_t child_exit_status_values[child_exit_status_capacity] = {0};
static volatile sig_atomic_t child_exit_status_sequences[child_exit_status_capacity] = {0};

static void child_exit_status_publish(pid_t pid, int status)
{
	sig_atomic_t cursor = __atomic_fetch_add(&child_exit_status_cursor, 1, __ATOMIC_RELAXED);
	size_t slot = (size_t)(unsigned int)cursor % child_exit_status_capacity;
	__atomic_store_n(&child_exit_status_pids[slot], 0, __ATOMIC_RELEASE);
	__atomic_store_n(&child_exit_status_values[slot], (sig_atomic_t)status, __ATOMIC_RELAXED);
	__atomic_store_n(&child_exit_status_sequences[slot], cursor + 1, __ATOMIC_RELAXED);
	__atomic_store_n(&child_exit_status_pids[slot], (sig_atomic_t)pid, __ATOMIC_RELEASE);
}

unsigned int child_exit_status_snapshot()
{
	return((unsigned int)__atomic_load_n(&child_exit_status_cursor, __ATOMIC_ACQUIRE));
}

bool child_exit_status_take(pid_t pid, int& status, unsigned int since)
{
	for(size_t slot = 0; slot < child_exit_status_capacity; slot++)
	{
		unsigned int sequence = (unsigned int)__atomic_load_n(&child_exit_status_sequences[slot], __ATOMIC_ACQUIRE);
		unsigned int distance = sequence - since;
		if(sequence == 0 || distance == 0 || distance >= 0x80000000u)
			continue;
		sig_atomic_t expected = (sig_atomic_t)pid;
		if(__atomic_compare_exchange_n(&child_exit_status_pids[slot], &expected, 0, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		{
			status = (int)__atomic_load_n(&child_exit_status_values[slot], __ATOMIC_RELAXED);
			return(true);
		}
	}
	return(false);
}

namespace {

class ProcessSigchldBlock
{
	sigset_t previous;
	bool blocked = false;
public:
	ProcessSigchldBlock()
	{
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, SIGCHLD);
		blocked = pthread_sigmask(SIG_BLOCK, &mask, &previous) == 0;
	}
	void restore()
	{
		if(blocked)
		{
			pthread_sigmask(SIG_SETMASK, &previous, 0);
			blocked = false;
		}
	}
	~ProcessSigchldBlock() { restore(); }
};

}

DValue process_exec(String cmd, String input, StringMap env, u64 timeout_ms, u64 output_limit)
{
	DValue result;
	result["exit_code"] = (f64)-1;
	result["stdout"] = "";
	result["stderr"] = "";
	result["timed_out"].set_bool(false);
	result["output_truncated"].set_bool(false);
	if(timeout_ms == 0)
		timeout_ms = 5000;
	int inpipe[2] = {-1, -1}, outpipe[2] = {-1, -1}, errpipe[2] = {-1, -1};
	if(pipe(inpipe) != 0 || pipe(outpipe) != 0 || pipe(errpipe) != 0)
	{
		for(int fd : {inpipe[0], inpipe[1], outpipe[0], outpipe[1], errpipe[0], errpipe[1]})
			if(fd >= 0)
				close(fd);
		result["stderr"] = "pipe failed";
		return(result);
	}
	ProcessSigchldBlock sigchld;
	unsigned int child_status_snapshot_value = child_exit_status_snapshot();
	pid_t pid = fork();
	if(pid < 0)
	{
		close(inpipe[0]); close(inpipe[1]); close(outpipe[0]); close(outpipe[1]); close(errpipe[0]); close(errpipe[1]);
		result["stderr"] = "fork failed";
		return(result);
	}
	if(pid == 0)
	{
		sigchld.restore();
		setpgid(0, 0);
		pid_t expected_parent = getppid();
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if(getppid() != expected_parent)
			_exit(127);
		dup2(inpipe[0], 0); dup2(outpipe[1], 1); dup2(errpipe[1], 2);
		close(inpipe[0]); close(inpipe[1]); close(outpipe[0]); close(outpipe[1]); close(errpipe[0]); close(errpipe[1]);
		for(auto& value : env)
			setenv(value.first.c_str(), value.second.c_str(), 1);
		execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)0);
		_exit(127);
	}
	setpgid(pid, pid);
	close(inpipe[0]); close(outpipe[1]); close(errpipe[1]);
	fcntl(inpipe[1], F_SETFL, fcntl(inpipe[1], F_GETFL, 0) | O_NONBLOCK);
	fcntl(outpipe[0], F_SETFL, fcntl(outpipe[0], F_GETFL, 0) | O_NONBLOCK);
	fcntl(errpipe[0], F_SETFL, fcntl(errpipe[0], F_GETFL, 0) | O_NONBLOCK);
	size_t input_offset = 0;
	bool input_open = true, output_open = true, error_open = true, exited = false, status_valid = false;
	int status = 0;
	u64 now_ms = monotonic_ms();
	u64 deadline = timeout_ms > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + timeout_ms;
	auto append_output = [&](String key, const char* data, size_t length) {
		String current = result[key].to_string();
		u64 captured = result["stdout"].to_string().size() + result["stderr"].to_string().size();
		size_t accepted = length;
		if(output_limit > 0)
		{
			u64 available = captured < output_limit ? output_limit - captured : 0;
			accepted = std::min<size_t>(accepted, (size_t)available);
			if(accepted < length)
				result["output_truncated"].set_bool(true);
		}
		if(accepted > 0)
			result[key] = current + String(data, accepted);
	};
	while(output_open || error_open || !exited)
	{
		if(!exited)
		{
			pid_t waited = waitpid(pid, &status, WNOHANG);
			if(waited == pid)
			{
				exited = true;
				status_valid = true;
			}
			else if(waited < 0 && errno == ECHILD)
			{
				u64 transfer_deadline = monotonic_ms() + 50;
				do
				{
					status_valid = child_exit_status_take(pid, status, child_status_snapshot_value);
					if(!status_valid)
						sched_yield();
				} while(!status_valid && monotonic_ms() < transfer_deadline);
				exited = true;
				if(!status_valid)
					result["stderr"] = result["stderr"].to_string() + "lost child exit status";
			}
		}
		if(input_open)
		{
			if(input_offset < input.size())
			{
				ssize_t written = write(inpipe[1], input.data() + input_offset, input.size() - input_offset);
				if(written > 0)
					input_offset += (size_t)written;
				else if(written < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
				{
					close(inpipe[1]);
					input_open = false;
				}
			}
			else
			{
				close(inpipe[1]);
				input_open = false;
			}
		}
		char buffer[4096];
		ssize_t length;
		while((length = read(outpipe[0], buffer, sizeof(buffer))) > 0)
			append_output("stdout", buffer, (size_t)length);
		if(length == 0 && output_open)
		{
			close(outpipe[0]);
			output_open = false;
		}
		while((length = read(errpipe[0], buffer, sizeof(buffer))) > 0)
			append_output("stderr", buffer, (size_t)length);
		if(length == 0 && error_open)
		{
			close(errpipe[0]);
			error_open = false;
		}
		if((output_open || error_open || !exited) && monotonic_ms() >= deadline)
		{
			result["timed_out"].set_bool(true);
			kill(-pid, SIGKILL);
			kill(pid, SIGKILL);
			if(!exited)
				status_valid = waitpid(pid, &status, 0) == pid;
			exited = true;
			if(input_open) { close(inpipe[1]); input_open = false; }
			if(output_open) { close(outpipe[0]); output_open = false; }
			if(error_open) { close(errpipe[0]); error_open = false; }
		}
		if(output_open || error_open || !exited)
		{
			u64 remaining_ms = deadline > monotonic_ms() ? deadline - monotonic_ms() : 0;
			if(remaining_ms > 0)
				usleep((useconds_t)std::min<u64>(1000, remaining_ms * 1000));
		}
	}
	if(status_valid && WIFEXITED(status))
		result["exit_code"] = (f64)WEXITSTATUS(status);
	else if(status_valid && WIFSIGNALED(status))
		result["exit_code"] = (f64)(128 + WTERMSIG(status));
	return(result);
}

pid_t spawn_subprocess(std::function<void()> exec_after_spawn)
{
	parent_pid = getpid();
	pid_t p;
	p = fork();
	if(p == 0)
	{
		my_pid = getpid();
		//printf("(C) child procress started, PID:%i\n", my_pid);
		prctl(PR_SET_PDEATHSIG, SIGHUP);
		exec_after_spawn();
		return(0);
	}
	else
	{
		Worker w;
		w.pid = p;
		workers[w.pid] = w;
		printf("(P) child procress spawned: PID %i\n", p);
		return(p);
	}
}

String runtime_safe_key(String key, String label)
{
	key = trim(key);
	if(key == "")
		throw std::runtime_error(label + " cannot be empty");
	return(gen_sha1(key));
}

String task_file_prefix(String key)
{
	return(path_join(context->server->config["BIN_DIRECTORY"], "task-" + runtime_safe_key(key, "task key")));
}

struct TaskStatus {
	pid_t pid = 0;
	String process_start_ticks = "";
};

TaskStatus task_status_parse(String status_file)
{
	TaskStatus status;
	auto lines = split(trim(status_file), "\n");
	if(lines.size() > 0)
		status.pid = (pid_t)int_val(trim(lines[0]));
	if(lines.size() > 1)
		status.process_start_ticks = trim(lines[1]);
	return(status);
}

String task_process_start_ticks(pid_t pid)
{
	if(pid <= 0)
		return("");
	String stat_file_name = "/proc/" + std::to_string(pid) + "/stat";
	int fd = open(stat_file_name.c_str(), O_RDONLY);
	if(fd == -1)
		return("");
	char buffer[4096];
	ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
	close(fd);
	if(bytes_read <= 0)
		return("");
	buffer[bytes_read] = '\0';
	String stat = buffer;
	size_t command_end = stat.rfind(") ");
	if(command_end == String::npos)
		return("");
	String after_command = stat.substr(command_end + 2);
	auto fields = split_space(after_command);
	if(fields.size() <= 19)
		return("");
	return(fields[19]);
}

String task_status_content(pid_t pid)
{
	return(std::to_string(pid) + "\n" + task_process_start_ticks(pid) + "\n");
}

bool task_status_is_alive(TaskStatus status)
{
	if(status.pid <= 0)
		return(false);
	if(kill(status.pid, 0) != 0)
		return(false);
	if(status.process_start_ticks == "")
		return(true);
	return(task_process_start_ticks(status.pid) == status.process_start_ticks);
}

void task_close_inherited_fds()
{
	long max_fd = sysconf(_SC_OPEN_MAX);
	if(max_fd < 0 || max_fd > 65536)
		max_fd = 4096;
	for(int fd = 3; fd < max_fd; fd++)
		close(fd);
}

int task_kill(pid_t pid, s32 sig)
{
	if(pid <= 0)
	{
		errno = EINVAL;
		return(-1);
	}
	return(kill(pid, sig));
}

pid_t task_pid(String key)
{
	String status_file_name = task_file_prefix(key);
	String lock_file_name = status_file_name + ".lock";
	int lock_fd = open_locked_file(lock_file_name, O_RDWR | O_CREAT, LOCK_EX, 0644);
	if(lock_fd == -1)
	{
		fprintf(stderr, "task_pid(): could not lock task key '%s'\n", key.c_str());
		return(0);
	}
	String status_file = file_exists(status_file_name) ? file_get_contents(status_file_name) : "";
	if(status_file != "")
	{
		TaskStatus status = task_status_parse(status_file);
		if(task_status_is_alive(status))
		{
			close_locked_file(lock_fd);
			return(status.pid);
		}
		file_unlink(status_file_name);
	}
	close_locked_file(lock_fd);
	return(0);
}

pid_t task(String key, std::function<void()> exec_after_spawn, u64 timeout)
{
	String status_file_name = task_file_prefix(key);
	String lock_file_name = status_file_name + ".lock";
	int lock_fd = open_locked_file(lock_file_name, O_RDWR | O_CREAT, LOCK_EX, 0644);
	if(lock_fd == -1)
	{
		fprintf(stderr, "task(): could not lock task key '%s'\n", key.c_str());
		return(0);
	}
	String status_file = file_exists(status_file_name) ? file_get_contents(status_file_name) : "";
	pid_t p = 0;
	if(status_file != "")
	{
		TaskStatus status = task_status_parse(status_file);
		if(task_status_is_alive(status))
		{
			printf("(P) worker process '%s' already running: PID %i\n", key.c_str(), status.pid);
			close_locked_file(lock_fd);
			return(status.pid);
		}
		file_unlink(status_file_name);
	}
	p = fork();
	if(p < 0)
	{
		fprintf(stderr, "task(): fork failed for key '%s': %s\n", key.c_str(), strerror(errno));
		close_locked_file(lock_fd);
		return(0);
	}
	if(p == 0)
	{
		close_locked_file(lock_fd);
		my_pid = getpid();
		// The FastCGI worker handles termination to drain accepted requests.
		// Generic task children do not run that drain loop, so inheriting those
		// handlers would turn task_kill(SIGTERM) into a no-op.
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		signal(SIGALRM, SIG_DFL);
		if(timeout > 0)
			alarm(timeout);

		if(context->resources.client_socket > 0)
		{
			close(context->resources.client_socket);
			context->resources.client_socket = 0;
		}
		task_close_inherited_fds();
		exec_after_spawn();
		int exit_lock_fd = open_locked_file(lock_file_name, O_RDWR | O_CREAT, LOCK_EX, 0644);
		if(exit_lock_fd != -1)
		{
			file_unlink(status_file_name);
			close_locked_file(exit_lock_fd);
		}
		else
		{
			fprintf(stderr, "task(): could not lock task key '%s' during child cleanup\n", key.c_str());
		}
		printf("(P) worker process '%s' terminated: PID %i\n", key.c_str(), my_pid);
		exit(0);
	}

	if(!file_put_contents(status_file_name, task_status_content(p)))
	{
		fprintf(stderr, "task(): could not write status file for key '%s'; terminating child PID %i\n", key.c_str(), p);
		kill(p, SIGTERM);
		close_locked_file(lock_fd);
		return(0);
	}
	close_locked_file(lock_fd);
	printf("(P) worker process '%s' spawned: PID %i\n", key.c_str(), p);
	return(p);
}

#include <unistd.h>
pid_t task_repeat(String key, f64 interval, std::function<void()> exec_after_spawn, u64 timeout)
{
	if(interval <= 0)
		throw std::runtime_error("task_repeat(): interval must be greater than zero");
	auto repeater_function = [key, interval, exec_after_spawn, timeout]() {
		f64 started_at = time_precise();
		while (timeout == 0 || time_precise() - started_at < (f64)timeout)
		{
			exec_after_spawn();
			f64 elapsed = time_precise() - started_at;
			if(timeout > 0 && elapsed >= (f64)timeout)
				break;
			f64 sleep_seconds = interval;
			if(timeout > 0 && elapsed + sleep_seconds > (f64)timeout)
				sleep_seconds = (f64)timeout - elapsed;
			printf("(P) worker process '%s' sleeping\n", key.c_str());
			if(sleep_seconds > 0)
				usleep((useconds_t)(sleep_seconds * 1000000.0));
		}
	};
	return(task(key, repeater_function, timeout));
}

void on_child_exit(int sig)
{
	(void)sig;
	pid_t pid;
	int status;
	while((pid = waitpid(-1, &status, WNOHANG)) > 0)
	{
		if(workers.count(pid) > 0)
		{
			workers.erase(pid);
			printf("(P) child terminated (PID:%i)\n", pid);
		}
		else
		{
			child_exit_status_publish(pid, status);
			printf("(P) task child reaped (PID:%i)\n", pid);
		}
	}
}

StringList ls(String dir)
{
	StringList entries;
	std::error_code ec;
	if(!std::filesystem::is_directory(dir, ec))
		return(entries);
	for(auto const& entry : std::filesystem::directory_iterator(dir, ec))
	{
		if(ec)
			break;
		entries.push_back(entry.path().filename().string());
	}
	std::sort(entries.begin(), entries.end());
	return(entries);
}

StringMap make_server_settings()
{
	StringMap cfg;

	cfg["BIN_DIRECTORY"] = "/tmp/uce/work";
	cfg["WASM_COMPILE_SCRIPT"] = "scripts/compile_wasm_unit";
	cfg["WASM_BACKEND_VERBOSE"] = "0";
	cfg["WASM_PROFILE_HOSTCALL_CPU"] = "0";
	cfg["WASM_PROFILE_THREAD_RUNTIME"] = "0";
	cfg["WASM_CORE_PATH"] = "";
	cfg["WASM_MEMORY_LIMIT_BYTES"] = std::to_string(512ull * 1024 * 1024);
	cfg["WASM_EPOCH_DEADLINE_TICKS"] = "200";
	cfg["WASM_EPOCH_PERIOD_MS"] = "50";
	cfg["SETUP_TEMPLATE"] = "scripts/setup.h.template";
	cfg["LIT_ESC"] = "3d5b5_1";
	cfg["CONTENT_TYPE"] = "text/html; charset=utf-8";
	cfg["FCGI_SOCKET_PATH"] = "/run/uce/fastcgi.sock";
	cfg["FCGI_SOCKET_MODE"] = "0666";
	cfg["CLI_SOCKET_PATH"] = "/run/uce/cli.sock";
	cfg["CLI_SOCKET_MODE"] = "0600";
	// Command socket the WS broker listens on; workers flush ws_* dispatch
	// command batches here at workspace teardown.
	cfg["WS_BROKER_SOCKET_PATH"] = "/run/uce/ws-broker.sock";
	cfg["WS_BROKER_OUTBOUND_TIMEOUT_SECONDS"] = "30";
	// Comma-separated uce_host_* names a sysadmin disables; empty = nothing blocked.
	cfg["UCE_HOSTCALL_BLOCKLIST"] = "";
	cfg["TMP_UPLOAD_PATH"] = "/tmp/uce/uploads";
	cfg["SESSION_PATH"] = "/tmp/uce/sessions";
	cfg["SESSION_COOKIE_SECURE"] = "0";
	cfg["COMPILER_SYS_PATH"] = ".";
	cfg["PRECOMPILE_FILES_IN"] = "";
	cfg["PRECOMPILE_JOBS"] = "2";
	cfg["SITE_DIRECTORY"] = "site";
	cfg["JIT_COMPILE_ON_REQUEST"] = "1";
	cfg["SHOW_DYNAMIC_COMPILE_ERRORS"] = "1";
	cfg["SERVE_LAST_KNOWN_GOOD"] = "0";
	cfg["PROACTIVE_COMPILE_ENABLED"] = "1";
	cfg["PROACTIVE_COMPILE_JOBS"] = "2";
	cfg["COMPILE_FAILURE_RETRY_SECONDS"] = std::to_string(10);
	cfg["PROACTIVE_COMPILE_CHECK_INTERVAL"] = std::to_string(60);
	cfg["TRANSPORT_MAX_CLIENT_CONNECTIONS"] = std::to_string(256);
	cfg["TRANSPORT_MAX_HTTP_HEADER_BYTES"] = std::to_string(16 * 1024);
	cfg["TRANSPORT_MAX_HTTP_BODY_BYTES"] = std::to_string(1024 * 1024);
	cfg["TRANSPORT_MAX_WEBSOCKET_FRAME_BYTES"] = std::to_string(1024 * 1024);
	cfg["TRANSPORT_MAX_WEBSOCKET_MESSAGE_BYTES"] = std::to_string(1024 * 1024);
	cfg["TRANSPORT_MAX_WEBSOCKET_OUTPUT_BYTES"] = std::to_string(4 * 1024 * 1024);
	cfg["TRANSPORT_MAX_RESPONSE_BYTES"] = std::to_string(8 * 1024 * 1024);
	cfg["TRANSPORT_HTTP_REQUEST_TIMEOUT_SECONDS"] = "15";
	cfg["TRANSPORT_CONNECTION_IDLE_TIMEOUT_SECONDS"] = "120";
	cfg["HTTP_DOCUMENT_ROOT"] = "";
	cfg["CUSTOM_SERVER_MAX_SERVERS"] = "16";
	cfg["CUSTOM_SERVER_MIN_PORT"] = "1024";
	cfg["CUSTOM_SERVER_MAX_PORT"] = "65535";
	cfg["CUSTOM_SERVER_ALLOW_PUBLIC_BIND"] = "0";
	cfg["CUSTOM_SERVER_UNIX_SOCKET_PREFIX"] = "/tmp/uce/custom-servers/";
	cfg["CUSTOM_SERVER_HANDLER_TIMEOUT_SECONDS"] = "30";
	cfg["CUSTOM_SERVER_UCE_ROOT"] = "";
	cfg["ARCHIVE_MAX_INPUT_BYTES"] = std::to_string(64 * 1024 * 1024);
	cfg["ARCHIVE_MAX_OUTPUT_BYTES"] = std::to_string(64 * 1024 * 1024);
	cfg["ARCHIVE_MAX_ZIP_ENTRIES"] = "4096";

	cfg["HTTP_PORT"] = std::to_string(8080);
	cfg["SESSION_TIME"] = std::to_string(60*60*24*30);
	cfg["WORKER_COUNT"] = std::to_string(4);
	cfg["MAX_MEMORY"] = std::to_string(1024*1024*16);

	for(auto& it : split_kv(file_get_contents("/etc/uce/settings.cfg")))
	{
		cfg[it.first] = it.second;
	}

	if(cfg["FCGI_SOCKET_PATH"] == "" && cfg["SOCKET_PATH"] != "")
		cfg["FCGI_SOCKET_PATH"] = cfg["SOCKET_PATH"];
	if(cfg["SOCKET_PATH"] == "" && cfg["FCGI_SOCKET_PATH"] != "")
		cfg["SOCKET_PATH"] = cfg["FCGI_SOCKET_PATH"];

	if(cfg["FCGI_PORT"] == "" && cfg["LISTEN_PORT"] != "")
		cfg["FCGI_PORT"] = cfg["LISTEN_PORT"];
	if(cfg["LISTEN_PORT"] == "" && cfg["FCGI_PORT"] != "")
		cfg["LISTEN_PORT"] = cfg["FCGI_PORT"];

	return(cfg);
}
#endif
