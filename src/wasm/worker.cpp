// W3 — production wasm workspace runtime + membrane (WASM-PROPOSAL §6, §7).
//
// One workspace per request: a fresh Wasmtime store holding the core module
// instance (production bearer_lib carve-out, owns memory/allocator/DValue) plus unit
// PIC side modules loaded lazily — including mid-request via the
// bearer_host_component_resolve hostcall, which is how component()/unit_render()
// inside the guest trigger dynamic loading.
//
// Designed for amalgamation-include (like bearer_lib.cpp): the including TU must
// provide String/DValue/brb_encode/brb_decode (types.cpp + dvalue.cpp) and
// wasm_trace.h. The production FastCGI worker includes this through backend.cpp.
//
// Loader rules carried from the spike phases, now enforced as policy:
//  - import discipline: units may import only env / GOT.mem / GOT.func
//    (no WASI — the core is zero-WASI for BEARER's own calls, and units get
//    everything from the core); units defining allocator symbols are rejected
//  - GOT.mem of a PIC module's data exports are __memory_base-relative
//    offsets; the loader adds the owning module's base (Phase 0 erratum)
//  - GOT.func resolves host-side: the target function's funcref is placed
//    into the shared table and the slot index becomes the GOT value
//  - export name collisions: core wins, then first-loading unit wins
//    (mirrors native dlopen global-symbol interposition for identical
//    vague-linkage template instantiations)
//  - bearer.abi stamp must match the core ABI version; fail loudly, never guess

#include <wasmtime.hh>

#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <map>
#include <set>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <cerrno>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>

struct WasmDylinkInfo
{
	u32 mem_size = 0;
	u32 mem_align = 0;
	u32 table_size = 0;
	u32 table_align = 0;
	bool found = false;
};

struct WasmAbiInfo
{
	u32 version = 0;
	String toolchain;
	String module_name;
	bool found = false;
};

struct WasmSourceMap
{
	struct Row
	{
		u64 address = 0;
		u32 file = 0;
		u32 line = 0;
		u32 column = 0;
	};
	String module_name;
	std::map<u32, String> files;
	std::vector<Row> rows;
};

enum class WasmUnitImportKind
{
	Memory,
	Table,
	StackPointer,
	MemoryBase,
	TableBase,
	Function,
	GotMemory,
	GotFunction
};

struct WasmUnitImport
{
	WasmUnitImportKind kind;
	String name;
};

struct WasmUnitModule
{
	String source_path;   // absolute .uce path
	String wasm_path;     // artifact in the unit cache
	u64 modified_ns = 0;
	u64 changed_ns = 0;
	u64 size = 0;
	WasmDylinkInfo dylink;
	WasmAbiInfo abi;
	std::optional<wasmtime::Module> module;
	std::vector<WasmUnitImport> imports;
	size_t got_memory_imports = 0;
};

struct WasmUnitModuleLoadProfile
{
	bool cache_hit = false;
	bool serialized_cache_hit = false;
	bool timed_out = false;
	u64 lookup_us = 0;
	u64 read_us = 0;
	u64 read_bytes = 0;
	u64 read_count = 0;
	u64 parse_us = 0;
	u64 compile_us = 0;
	u64 classify_us = 0;
};

struct WasmInstancePreDeleter
{
	void operator()(wasmtime_instance_pre_t* instance_pre) const
	{
		wasmtime_instance_pre_delete(instance_pre);
	}
};

struct WasmWorkerConfig
{
	String core_wasm_path = "bin/wasm/core.wasm";
	String site_root;                  // absolute
	String cache_root = "/tmp/bearer/work";
	std::vector<String> write_roots;   // absolute prefixes the write membrane allows
	int64_t memory_limit = 512ll * 1024 * 1024;
	u32 table_headroom = 4096;
	u64 epoch_deadline_ticks = 200;    // ticker period × ticks = CPU budget
	u64 epoch_period_ms = 50;
	u64 invocation_timeout_ms = 30000;
	u64 mysql_persistent_pool_size = 8;
	bool profile_hostcall_cpu = false;
	bool profile_thread_runtime = false;
	bool verbose = false;
	// bearer_host_* names (bare, without the "bearer_host_" prefix) the sysadmin has
	// disabled via BEARER_HOSTCALL_BLOCKLIST. A blocked hostcall resolves to a trap
	// stub at workspace birth (see make_host_import); empty = feature off.
	std::set<String> hostcall_blocklist;
};

struct WasmMySQLOperation
{
	String op;
	String source;
	u64 elapsed_us = 0;
};

struct WasmUnitModuleOperation
{
	String unit;
	String kind;
	String source;
	u64 total_us = 0;
	u64 materialize_us = 0;
	u64 lookup_us = 0;
	u64 read_us = 0;
	u64 read_bytes = 0;
	u64 read_count = 0;
	u64 parse_us = 0;
	u64 build_us = 0;
	u64 classify_us = 0;
	u64 allocate_us = 0;
	u64 import_us = 0;
	u64 symbol_resolve_count = 0;
	u64 symbol_resolve_us = 0;
	u64 instantiate_us = 0;
	u64 initialize_us = 0;
};

struct WasmRequestProfile
{
	static const u64 HOSTCALL_OPERATION_MAX = 96;
	u64 dispatch_us = 0;
	u64 workspace_setup_us = 0;
	u64 workspace_setup_cpu_us = 0;
	u64 workspace_birth_us = 0;
	u64 workspace_birth_cpu_us = 0;
	u64 birth_policy_us = 0;
	u64 birth_import_us = 0;
	u64 birth_instantiate_us = 0;
	u64 birth_exports_us = 0;
	u64 birth_initialize_us = 0;
	u64 context_apply_us = 0;
	u64 context_apply_cpu_us = 0;
	u64 context_bytes = 0;
	u64 server_config_bytes = 0;
	u64 context_encode_us = 0;
	u64 context_allocate_us = 0;
	u64 context_write_us = 0;
	u64 context_guest_apply_us = 0;
	u64 context_free_us = 0;
	u64 entry_invoke_us = 0;
	u64 entry_load_us = 0;
	u64 entry_presence_us = 0;
	u64 entry_link_us = 0;
	u64 entry_dispatch_us = 0;
	u64 output_collect_us = 0;
	u64 workspace_complete_us = 0;
	u64 component_resolve_count = 0;
	u64 component_loaded_reuse_count = 0;
	u64 component_resolve_total_us = 0;
	u64 component_path_total_us = 0;
	u64 component_artifact_total_us = 0;
	u64 component_load_total_us = 0;
	u64 component_link_total_us = 0;
	u64 unit_load_count = 0;
	u64 entry_unit_load_count = 0;
	u64 entry_unit_materialize_total_us = 0;
	u64 dynamic_include_load_count = 0;
	u64 dynamic_include_materialize_total_us = 0;
	u64 unit_module_total_us = 0;
	u64 unit_module_cache_hit_count = 0;
	u64 unit_module_cache_miss_count = 0;
	u64 unit_module_serialized_cache_hit_count = 0;
	u64 unit_module_compile_count = 0;
	u64 unit_module_lookup_total_us = 0;
	u64 unit_module_read_total_us = 0;
	u64 unit_module_read_bytes = 0;
	u64 unit_module_read_count = 0;
	u64 unit_module_parse_total_us = 0;
	u64 unit_module_compile_total_us = 0;
	u64 unit_module_classify_total_us = 0;
	u64 unit_module_operations_dropped = 0;
	std::vector<WasmUnitModuleOperation> unit_module_operations;
	u64 unit_allocate_total_us = 0;
	u64 unit_import_total_us = 0;
	u64 unit_symbol_resolve_count = 0;
	u64 unit_symbol_resolve_total_us = 0;
	u64 unit_instantiate_total_us = 0;
	u64 unit_initialize_total_us = 0;
	u64 hostcall_count = 0;
	u64 hostcall_total_us = 0;
	u64 hostcall_cpu_total_us = 0;
	u64 hostcall_operation_slots = 0;
	u64 hostcall_operation_counts[HOSTCALL_OPERATION_MAX] = {};
	u64 hostcall_operation_us[HOSTCALL_OPERATION_MAX] = {};
	u64 hostcall_operation_cpu_us[HOSTCALL_OPERATION_MAX] = {};
	u64 mysql_hostcall_count = 0;
	u64 mysql_hostcall_total_us = 0;
	u64 mysql_operation_count = 0;
	u64 mysql_operations_dropped = 0;
	u64 mysql_connection_open_count = 0;
	u64 mysql_connection_reuse_count = 0;
	u64 mysql_request_pool_hit_count = 0;
	std::vector<WasmMySQLOperation> mysql_operations;
	u64 memcache_hostcall_count = 0;
	u64 memcache_hostcall_total_us = 0;
};

struct WasmResponse : WasmRequestProfile
{
	bool ok = false;
	bool handler_present = true;  // false → unit has no handler for the requested kind (404)
	String body;
	DValue meta;          // status / headers / cookies / session
	String error;         // collapsed trace or loader error when !ok
};

static u64 wasm_file_lock_timeout_ms()
{
	const char* raw = getenv("BEARER_FILE_LOCK_TIMEOUT_MS");
	if(!raw || !*raw)
		return(2000);
	char* end = 0;
	unsigned long long parsed = strtoull(raw, &end, 10);
	return(end == raw ? 2000 : (u64)parsed);
}

static u64 wasm_monotonic_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return((u64)ts.tv_sec * 1000ull + (u64)ts.tv_nsec / 1000000ull);
}

struct WasmSigchldBlock
{
	sigset_t previous;
	bool blocked = false;
	WasmSigchldBlock()
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
	~WasmSigchldBlock() { restore(); }
};

static u64 wasm_deadline_after_ms(u64 timeout_ms)
{
	u64 now = wasm_monotonic_ms();
	return(timeout_ms > UINT64_MAX - now ? UINT64_MAX : now + timeout_ms);
}

static bool wasm_socket_wait(int fd, short events, u64 deadline)
{
	while(true)
	{
		u64 now = wasm_monotonic_ms();
		if(now >= deadline)
			return(false);
		u64 remaining_ms = deadline - now;
		struct pollfd item = { fd, events, 0 };
		int rc = poll(&item, 1, (int)std::min<u64>(INT_MAX, remaining_ms));
		if(rc > 0)
			return((item.revents & (events | POLLERR | POLLHUP)) != 0);
		if(rc == 0)
			return(false);
		if(errno != EINTR)
			return(false);
	}
}

static u64 wasm_socket_connect_bounded(const String& host, u16 port, u64 timeout_ms)
{
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(fd < 0)
		return(0);
	int flags = fcntl(fd, F_GETFL, 0);
	if(flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
	{
		close(fd);
		return(0);
	}
	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	if(inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
	{
		close(fd);
		return(0);
	}
	int rc = connect(fd, (struct sockaddr*)&address, sizeof(address));
	if(rc != 0 && errno == EINPROGRESS && wasm_socket_wait(fd, POLLOUT, wasm_deadline_after_ms(timeout_ms)))
	{
		int error = 0;
		socklen_t error_size = sizeof(error);
		rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) == 0 && error == 0 ? 0 : -1;
	}
	if(rc != 0)
	{
		close(fd);
		return(0);
	}
	if(fcntl(fd, F_SETFL, flags) != 0)
	{
		close(fd);
		return(0);
	}
	if(fd == 0)
	{
		int moved = dup(fd);
		close(fd);
		fd = moved;
	}
	if(fd <= 0)
		return(0);
	if(context)
		context->resources.sockets.push_back(fd);
	return((u64)fd);
}

static bool wasm_socket_write_bounded(u64 socket_fd, const String& data, u64 timeout_ms)
{
	int fd = (int)socket_fd;
	u64 deadline = wasm_deadline_after_ms(timeout_ms);
	size_t offset = 0;
	while(offset < data.size())
	{
		if(!wasm_socket_wait(fd, POLLOUT, deadline))
			return(false);
		ssize_t written = send(fd, data.data() + offset, data.size() - offset, MSG_DONTWAIT | MSG_NOSIGNAL);
		if(written > 0)
			offset += (size_t)written;
		else if(written < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			return(false);
	}
	return(true);
}

static String wasm_socket_read_bounded(u64 socket_fd, u32 max_length, u64 timeout_ms)
{
	if(max_length == 0 || !wasm_socket_wait((int)socket_fd, POLLIN, wasm_deadline_after_ms(timeout_ms)))
		return("");
	std::vector<char> buffer(max_length);
	ssize_t count = recv((int)socket_fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
	return(count > 0 ? String(buffer.data(), (size_t)count) : String(""));
}

static f64 wasm_thread_cpu_time()
{
	struct timespec ts;
	if(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
		return(0);
	return((f64)ts.tv_sec + (f64)ts.tv_nsec / 1000000000.0);
}

static int wasm_open_locked_file(const String& file_name, int flags, int lock_type, bool truncate_after_lock)
{
	int fd = open(file_name.c_str(), flags, 0644);
	if(fd < 0)
		return(-1);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	u64 timeout = wasm_file_lock_timeout_ms();
	u64 deadline = wasm_monotonic_ms() + timeout;
	while(true)
	{
		if(flock(fd, lock_type | LOCK_NB) == 0)
		{
			if(truncate_after_lock && ftruncate(fd, 0) != 0)
			{
				flock(fd, LOCK_UN);
				close(fd);
				return(-1);
			}
			return(fd);
		}
		if(errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
		{
			close(fd);
			return(-1);
		}
		if(timeout == 0 || wasm_monotonic_ms() >= deadline)
		{
			fprintf(stderr, "[wasm] file lock timeout after %llums: %s\n", (unsigned long long)timeout, file_name.c_str());
			close(fd);
			return(-1);
		}
		usleep(10000);
	}
}

static bool wasm_fd_write_all(int fd, const char* data, size_t remaining, u64* written_out)
{
	u64 total = 0;
	while(remaining > 0)
	{
		ssize_t n = write(fd, data, remaining);
		if(n < 0)
		{
			if(errno == EINTR)
				continue;
			return(false);
		}
		if(n == 0)
			return(false);
		data += n;
		remaining -= (size_t)n;
		total += (u64)n;
	}
	if(written_out)
		*written_out = total;
	return(true);
}



// ---- file-backed async job registry + bounded process execution ----------

static String bearer_job_root()
{
	const char* env = getenv("BEARER_JOB_ROOT");
	String root = (env && *env) ? String(env) : String("/run/bearer/jobs");
	std::error_code ec;
	std::filesystem::create_directories(root, ec);
	if(ec)
	{
		root = "/tmp/bearer/jobs";
		std::filesystem::create_directories(root, ec);
	}
	return(root);
}

static String bearer_job_path(u64 id) { return(bearer_job_root() + "/" + std::to_string(id)); }
static String bearer_read_text(const String& path) { std::ifstream in(path, std::ios::binary); if(!in) return(""); std::ostringstream ss; ss << in.rdbuf(); return(ss.str()); }
static void bearer_write_text(const String& path, const String& data) { std::ofstream out(path, std::ios::binary|std::ios::trunc); out.write(data.data(), (std::streamsize)data.size()); }

static u64 bearer_job_new(const String& kind)
{
	String root = bearer_job_root();
	std::error_code ec;
	std::filesystem::create_directories(root, ec);
	u64 seed = ((u64)time(0) << 32) ^ ((u64)getpid() << 16) ^ (u64)rand();
	for(int i = 0; i < 100; i++)
	{
		u64 id = seed ^ (wasm_monotonic_ms() + (u64)i * 0x9e3779b97f4a7c15ull);
		String dir = root + "/" + std::to_string(id);
		if(mkdir(dir.c_str(), 0700) == 0)
		{
			bearer_write_text(dir + "/kind", kind);
			bearer_write_text(dir + "/created", std::to_string((u64)time(0)));
			bearer_write_text(dir + "/state", "pending");
			return(id);
		}
	}
	return(0);
}

static void bearer_job_reap()
{
	String root = bearer_job_root();
	u64 now = (u64)time(0);
	u64 ttl = 3600;
	if(const char* raw = getenv("BEARER_JOB_TTL_SECONDS")) { char* e=0; unsigned long long v=strtoull(raw,&e,10); if(e!=raw && v>0) ttl=(u64)v; }
	std::error_code ec;
	for(auto& e : std::filesystem::directory_iterator(root, ec))
	{
		if(!e.is_directory()) continue;
		u64 created = strtoull(bearer_read_text(e.path().string()+"/created").c_str(), 0, 10);
		if(created > 0 && now > created + ttl)
			std::filesystem::remove_all(e.path(), ec);
	}
}

static DValue bearer_shell_exec_spec(const DValue& spec)
{
	return(process_exec(spec.key("cmd") ? spec.key("cmd")->to_string() : String(""), spec.key("stdin") ? spec.key("stdin")->to_string() : String(""), spec.key("env") ? spec.key("env")->to_stringmap() : StringMap(), spec.key("timeout_ms") ? spec.key("timeout_ms")->to_u64(5000) : 5000));
}

static void bearer_job_finish(u64 id, DValue result, String final_state="done")
{
	String dir = bearer_job_path(id);
	bearer_write_text(dir + "/result.tmp", brb_encode(result));
	rename((dir + "/result.tmp").c_str(), (dir + "/result").c_str());
	bearer_write_text(dir + "/state", final_state);
}

static u64 bearer_shell_spawn_spec(const DValue& spec)
{
	bearer_job_reap();
	u64 id = bearer_job_new("shell");
	if(!id) return(0);
	pid_t pid = fork();
	if(pid == 0)
	{
		setsid();
		bearer_write_text(bearer_job_path(id) + "/worker_pid", std::to_string((long long)getpid()));
		bearer_write_text(bearer_job_path(id) + "/state", "running");
		DValue result = bearer_shell_exec_spec(spec);
		bearer_job_finish(id, result, "done");
		_exit(0);
	}
	if(pid < 0) { DValue r; r["error"]="fork failed"; bearer_job_finish(id,r,"failed"); return(id); }
	bearer_write_text(bearer_job_path(id) + "/worker_pid", std::to_string((long long)pid));
	bearer_write_text(bearer_job_path(id) + "/state", "running");
	return(id);
}


static DValue bearer_exec_argv_capture(std::vector<String> argv, String input, u64 timeout_ms)
{
	DValue r; r["exit_code"]=(f64)-1; r["stdout"]=""; r["stderr"]=""; r["timed_out"].set_bool(false);
	if(argv.empty()) { r["stderr"]="empty argv"; return(r); }
	if(timeout_ms == 0) timeout_ms = 5000;
	int inpipe[2], outpipe[2], errpipe[2];
	if(pipe(inpipe)||pipe(outpipe)||pipe(errpipe)) { r["stderr"]="pipe failed"; return(r); }
	WasmSigchldBlock sigchld;
	unsigned int child_status_snapshot=child_exit_status_snapshot();
	pid_t pid=fork();
	if(pid==0)
	{
		sigchld.restore();
		setpgid(0,0);
		dup2(inpipe[0],0); dup2(outpipe[1],1); dup2(errpipe[1],2);
		close(inpipe[0]); close(inpipe[1]); close(outpipe[0]); close(outpipe[1]); close(errpipe[0]); close(errpipe[1]);
		std::vector<char*> args; for(auto& a: argv) args.push_back((char*)a.c_str()); args.push_back(0);
		execvp(args[0], args.data()); _exit(127);
	}
	if(pid < 0)
	{
		close(inpipe[0]); close(inpipe[1]); close(outpipe[0]); close(outpipe[1]); close(errpipe[0]); close(errpipe[1]);
		r["stderr"] = "fork failed";
		return(r);
	}
	setpgid(pid,pid);
	close(inpipe[0]); close(outpipe[1]); close(errpipe[1]);
	fcntl(inpipe[1], F_SETFL, fcntl(inpipe[1], F_GETFL, 0)|O_NONBLOCK); fcntl(outpipe[0], F_SETFL, fcntl(outpipe[0], F_GETFL, 0)|O_NONBLOCK); fcntl(errpipe[0], F_SETFL, fcntl(errpipe[0], F_GETFL, 0)|O_NONBLOCK);
	size_t input_off=0; bool in_open=true,out_open=true,err_open=true,exited=false,status_valid=false; int status=0; u64 deadline=wasm_monotonic_ms()+timeout_ms;
	while(out_open || err_open || !exited)
	{
		if(!exited) { pid_t w=waitpid(pid,&status,WNOHANG); if(w==pid) { exited=true; status_valid=true; } else if(w<0&&errno==ECHILD) { u64 transfer_deadline=wasm_deadline_after_ms(50); do { status_valid=child_exit_status_take(pid,status,child_status_snapshot); if(!status_valid)sched_yield(); } while(!status_valid&&wasm_monotonic_ms()<transfer_deadline); exited=true; if(!status_valid)r["stderr"]=r["stderr"].to_string()+"lost child exit status"; } }
		if(in_open) { if(input_off<input.size()) { ssize_t n=write(inpipe[1], input.data()+input_off, input.size()-input_off); if(n>0) input_off+=(size_t)n; else if(n<0 && errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK) { close(inpipe[1]); in_open=false; } } else { close(inpipe[1]); in_open=false; } }
		char buf[4096]; ssize_t n; while((n=read(outpipe[0],buf,sizeof(buf)))>0) r["stdout"] = r["stdout"].to_string()+String(buf,n); if(n==0&&out_open){close(outpipe[0]);out_open=false;}
		while((n=read(errpipe[0],buf,sizeof(buf)))>0) r["stderr"] = r["stderr"].to_string()+String(buf,n); if(n==0&&err_open){close(errpipe[0]);err_open=false;}
		if((out_open || err_open || !exited) && wasm_monotonic_ms() >= deadline) { r["timed_out"].set_bool(true); kill(-pid,SIGKILL); kill(pid,SIGKILL); if(!exited) status_valid=waitpid(pid,&status,0)==pid; exited=true; if(in_open){close(inpipe[1]);in_open=false;} if(out_open){close(outpipe[0]);out_open=false;} if(err_open){close(errpipe[0]);err_open=false;} }
		if(out_open || err_open || !exited) usleep(10000);
	}
	if(status_valid && WIFEXITED(status)) r["exit_code"]=(f64)WEXITSTATUS(status); else if(status_valid && WIFSIGNALED(status)) r["exit_code"]=(f64)(128+WTERMSIG(status));
	return(r);
}

static bool bearer_header_name_safe(String name)
{
	if(name=="") return(false);
	for(unsigned char c: name) if(!(isalnum(c)||c=='-'||c=='_')) return(false);
	return(true);
}

static DValue bearer_http_request_value(const DValue& req)
{
	DValue r; r["status"]=(f64)0; r["headers"].set_array(); r["body"]=""; r["error"]="";
	const DValue* method_value = req.key("method");
	const DValue* url_value = req.key("url");
	const DValue* timeout_value = req.key("timeout_ms");
	const DValue* follow_redirects_value = req.key("follow_redirects");
	const DValue* headers_value = req.key("headers");
	const DValue* body_value = req.key("body");
	String method = method_value ? to_upper(method_value->to_string()) : String("GET");
	String url = url_value ? url_value->to_string() : String("");
	if(url=="" || url.find('\0')!=String::npos) { r["error"]="missing url"; return(r); }
	if(method=="") method="GET";
	u64 timeout_ms = timeout_value ? timeout_value->to_u64(5000) : 5000;
	std::vector<String> argv = {"curl", "-sS", "--http1.0", "-X", method, "-D", "-", "-w", "\nBEARER_HTTP_STATUS:%{http_code}", "--max-time", std::to_string(std::max<u64>(1, timeout_ms / 1000))};
	if(follow_redirects_value && follow_redirects_value->to_bool()) argv.push_back("-L");
	if(headers_value) headers_value->each([&](const DValue& v, String k){ if(bearer_header_name_safe(k)) { argv.push_back("-H"); argv.push_back(k + ": " + replace(replace(v.to_string(), "\r", " "), "\n", " ")); } });
	String body = body_value ? body_value->to_string() : String("");
	if(body_value) { argv.push_back("--data-binary"); argv.push_back("@-"); }
	argv.push_back(url);
	if(access("/usr/bin/curl", X_OK)!=0 && access("/bin/curl", X_OK)!=0) { r["error"]="curl binary not found in runtime PATH"; return(r); }
	DValue pr = bearer_exec_argv_capture(argv, body, timeout_ms);
	String out = pr["stdout"].to_string();
	String marker="\nBEARER_HTTP_STATUS:"; size_t mp=out.rfind(marker);
	if(mp!=String::npos) { r["status"]=(f64)strtoull(out.c_str()+mp+marker.size(),0,10); out=out.substr(0,mp); }
	else r["error"]="curl did not report status";
	String sep="\r\n\r\n"; size_t hp=out.rfind(sep); size_t sep_len=4; if(hp==String::npos) { sep="\n\n"; hp=out.rfind(sep); sep_len=2; }
	String hdrs = hp==String::npos ? String("") : out.substr(0,hp); r["body"] = hp==String::npos ? out : out.substr(hp+sep_len);
	DValue headers;
	for(String line: split(replace(hdrs,"\r",""), "\n")) { size_t c=line.find(':'); if(c!=String::npos) headers[trim(line.substr(0,c))] = trim(line.substr(c+1)); }
	r["headers"] = headers;
	if(pr["exit_code"].to_s64() != 0 && r["error"].to_string()=="") r["error"] = trim(pr["stderr"].to_string());
	return(r);
}

static u64 bearer_http_spawn_spec(const DValue& req)
{
	bearer_job_reap(); u64 id=bearer_job_new("http"); if(!id) return(0);
	pid_t pid=fork();
	if(pid==0) { setsid(); bearer_write_text(bearer_job_path(id)+"/worker_pid", std::to_string((long long)getpid())); bearer_write_text(bearer_job_path(id)+"/state", "running"); DValue result=bearer_http_request_value(req); bearer_job_finish(id,result,result["error"].to_string()==""?"done":"failed"); _exit(0); }
	if(pid<0) { DValue r; r["error"]="fork failed"; bearer_job_finish(id,r,"failed"); return(id); }
	bearer_write_text(bearer_job_path(id)+"/worker_pid", std::to_string((long long)pid)); bearer_write_text(bearer_job_path(id)+"/state", "running"); return(id);
}

static DValue bearer_job_status_value(u64 id)
{
	DValue r; String dir=bearer_job_path(id); r["job_id"]=(f64)id;
	if(id==0 || !std::filesystem::is_directory(dir)) { r["state"]="missing"; return(r); }
	String state=trim(bearer_read_text(dir+"/state")); if(state=="") state="pending"; r["state"]=state;
	r["kind"]=trim(bearer_read_text(dir+"/kind")); r["pid"]=(f64)strtoull(bearer_read_text(dir+"/worker_pid").c_str(),0,10);
	r["done"].set_bool(state=="done"||state=="failed"||state=="cancelled");
	return(r);
}

static DValue bearer_job_result_value(u64 id, u64 timeout_ms)
{
	u64 deadline = wasm_monotonic_ms() + timeout_ms;
	while(timeout_ms > 0 && wasm_monotonic_ms() < deadline)
	{
		DValue st = bearer_job_status_value(id);
		if(st["done"].to_bool()) break;
		usleep(10000);
	}
	DValue r = bearer_job_status_value(id);
	String encoded = bearer_read_text(bearer_job_path(id)+"/result");
	if(encoded != "") { DValue decoded; String err; if(brb_decode(encoded, decoded, &err)) r["result"] = decoded; }
	return(r);
}

static bool bearer_job_cancel_value(u64 id)
{
	DValue st = bearer_job_status_value(id);
	if(st["state"].to_string()=="missing") return(false);
	String state = st["state"].to_string();
	if(state == "done" || state == "failed" || state == "cancelled")
		return(false);
	pid_t pid = (pid_t)st["pid"].to_u64(0);
	if(pid > 0) kill(-pid, SIGKILL);
	DValue result; result["cancelled"].set_bool(true);
	bearer_job_finish(id, result, "cancelled");
	return(true);
}

// ---- module byte parsing (hardened; carried from the phase 3 spike) -------

// Included into the native server TU through backend.cpp. File-scope helpers
// remain static to keep their linkage local to the wasm backend object.
static bool wasm_read_uleb(const std::vector<u8>& buf, size_t& pos, size_t end, u64& out)
{
	out = 0;
	u32 shift = 0;
	while(true)
	{
		if(pos >= end || shift >= 64)
			return(false);
		u8 byte = buf[pos++];
		if(shift == 63 && (byte & 0x7e) != 0)
			return(false);
		out |= ((u64)(byte & 0x7f)) << shift;
		if((byte & 0x80) == 0)
			return(true);
		shift += 7;
	}
}

static bool wasm_parse_sections(const std::vector<u8>& bytes, WasmDylinkInfo& dylink, WasmAbiInfo& abi, String& error,
	bool deadline_active = false, std::chrono::steady_clock::time_point deadline = {}, bool* timed_out = 0)
{
	auto deadline_expired = [&]() {
		if(!deadline_active || std::chrono::steady_clock::now() < deadline)
			return(false);
		if(timed_out)
			*timed_out = true;
		error = "wasm metadata parse timed out";
		return(true);
	};
	if(timed_out)
		*timed_out = false;
	if(bytes.size() < 8 || memcmp(bytes.data(), "\0asm", 4) != 0)
	{
		error = "not a wasm module";
		return(false);
	}
	if(!(bytes[4] == 1 && bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0))
	{
		error = "unsupported wasm binary version";
		return(false);
	}
	size_t pos = 8;
	bool dylink_seen = false;
	bool abi_seen = false;
	bool module_seen = false;
	while(pos < bytes.size())
	{
		if(deadline_expired())
			return(false);
		u8 section_id = bytes[pos++];
		u64 size = 0;
		if(!wasm_read_uleb(bytes, pos, bytes.size(), size) || size > bytes.size() - pos)
		{
			error = "malformed wasm section header";
			return(false);
		}
		size_t end = pos + (size_t)size;
		if(section_id == 0)
		{
			u64 name_len = 0;
			size_t cursor = pos;
			if(!wasm_read_uleb(bytes, cursor, end, name_len) || name_len > end - cursor)
			{
				error = "malformed custom section name";
				return(false);
			}
			if(name_len > 64)
			{
				pos = end;
				continue;
			}
			String name((const char*)bytes.data() + cursor, (size_t)name_len);
			cursor += (size_t)name_len;
			if((name == "dylink.0" || name == "bearer.abi" || name == "bearer.module") && size > 1024 * 1024)
			{
				error = "oversized wasm metadata section";
				return(false);
			}
			if(name == "dylink.0")
			{
				if(dylink_seen)
				{
					error = "duplicate dylink.0 metadata section";
					return(false);
				}
				dylink_seen = true;
				while(cursor < end)
				{
					if(deadline_expired())
						return(false);
					u8 sub = bytes[cursor++];
					u64 sub_len = 0;
					if(!wasm_read_uleb(bytes, cursor, end, sub_len) || sub_len > end - cursor)
					{
						error = "malformed dylink.0 subsection";
						return(false);
					}
					size_t sub_end = cursor + (size_t)sub_len;
					if(sub == 1) // WASM_DYLINK_MEM_INFO
					{
						u64 v[4];
						for(int i = 0; i < 4; i++)
							if(!wasm_read_uleb(bytes, cursor, sub_end, v[i]))
							{
								error = "malformed dylink.0 mem_info";
								return(false);
							}
						if(dylink.found)
						{
							error = "duplicate dylink.0 mem_info";
							return(false);
						}
						if(v[1] >= 31 || v[3] >= 31)
						{
							error = "unsupported dylink alignment";
							return(false);
						}
						dylink.mem_size = (u32)v[0];
						dylink.mem_align = (u32)v[1];
						dylink.table_size = (u32)v[2];
						dylink.table_align = (u32)v[3];
						dylink.found = true;
					}
					cursor = sub_end;
				}
			}
			else if(name == "bearer.abi")
			{
				if(abi_seen)
				{
					error = "duplicate bearer.abi metadata section";
					return(false);
				}
				abi_seen = true;
				String text((const char*)bytes.data() + cursor, end - cursor);
				abi.found = true;
				size_t line_start = 0;
				while(line_start < text.size())
				{
					if(deadline_expired())
						return(false);
					size_t line_end = text.find('\n', line_start);
					if(line_end == String::npos)
						line_end = text.size();
					String line = text.substr(line_start, line_end - line_start);
					if(line.rfind("unit_abi_version=", 0) == 0)
						abi.version = (u32)strtoul(line.c_str() + 17, 0, 10);
					if(line.rfind("toolchain=", 0) == 0)
						abi.toolchain = line.substr(10);
					line_start = line_end + 1;
				}
			}
			else if(name == "bearer.module")
			{
				if(module_seen)
				{
					error = "duplicate bearer.module metadata section";
					return(false);
				}
				module_seen = true;
				abi.module_name.assign((const char*)bytes.data() + cursor, end - cursor);
			}
		}
		pos = end;
	}
	return(true);
}

static bool wasm_read_file(const String& path, std::vector<u8>& out)
{
	std::ifstream in(path, std::ios::binary);
	if(!in)
		return(false);
	in.seekg(0, std::ios::end);
	std::streamoff n = in.tellg();
	if(n < 0)
		return(false);
	in.seekg(0, std::ios::beg);
	out.resize((size_t)n);
	if(n == 0)
		return(true);
	in.read((char*)out.data(), n);
	return((bool)in);
}

static bool wasm_read_file_deadline(const String& path, std::vector<u8>& out, String& error, u64& bytes_read, u64& read_count,
	bool& timed_out, bool deadline_active, std::chrono::steady_clock::time_point deadline, const struct stat& expected_st)
{
	bytes_read = 0;
	read_count = 0;
	timed_out = false;
	auto expired = [&]() {
		if(!deadline_active || std::chrono::steady_clock::now() < deadline)
			return(false);
		timed_out = true;
		error = "wasm artifact read timed out";
		return(true);
	};
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if(fd < 0)
		return(false);
	struct stat st;
	u64 expected_modified_ns = (u64)expected_st.st_mtim.tv_sec * 1000000000ull + (u64)expected_st.st_mtim.tv_nsec;
	u64 expected_changed_ns = (u64)expected_st.st_ctim.tv_sec * 1000000000ull + (u64)expected_st.st_ctim.tv_nsec;
	if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
	{
		close(fd);
		error = "wasm artifact is not a regular file";
		return(false);
	}
	u64 modified_ns = (u64)st.st_mtim.tv_sec * 1000000000ull + (u64)st.st_mtim.tv_nsec;
	u64 changed_ns = (u64)st.st_ctim.tv_sec * 1000000000ull + (u64)st.st_ctim.tv_nsec;
	if(st.st_dev != expected_st.st_dev || st.st_ino != expected_st.st_ino || st.st_mode != expected_st.st_mode ||
		modified_ns != expected_modified_ns || changed_ns != expected_changed_ns || st.st_size != expected_st.st_size)
	{
		close(fd);
		error = "wasm artifact changed while loading";
		return(false);
	}
	out.clear();
	u64 offset = 0;
	while(offset < (u64)st.st_size)
	{
		if(expired())
			break;
		size_t wanted = (size_t)std::min<u64>(64 * 1024, (u64)st.st_size - offset);
		if(offset > out.max_size() || wanted > out.max_size() - (size_t)offset)
		{
			error = "wasm artifact is too large";
			break;
		}
		try
		{
			out.resize((size_t)offset + wanted);
		}
		catch(const std::bad_alloc&)
		{
			error = "cannot allocate wasm artifact buffer";
			break;
		}
		catch(const std::length_error&)
		{
			error = "wasm artifact is too large";
			break;
		}
		if(expired())
			break;
		ssize_t count = pread(fd, out.data() + offset, wanted, (off_t)offset);
		if(count < 0 && errno == EINTR)
			continue;
		if(count <= 0)
		{
			error = "cannot read wasm artifact";
			break;
		}
		offset += (u64)count;
		out.resize((size_t)offset);
		bytes_read += (u64)count;
		read_count++;
		if(expired())
			break;
	}
	struct stat final_st;
	if(error == "" && (fstat(fd, &final_st) != 0 || final_st.st_dev != st.st_dev || final_st.st_ino != st.st_ino ||
		final_st.st_mode != st.st_mode || final_st.st_mtim.tv_sec != st.st_mtim.tv_sec ||
		final_st.st_mtim.tv_nsec != st.st_mtim.tv_nsec || final_st.st_ctim.tv_sec != st.st_ctim.tv_sec ||
		final_st.st_ctim.tv_nsec != st.st_ctim.tv_nsec || final_st.st_size != st.st_size))
		error = "wasm artifact changed while loading";
	close(fd);
	if(error != "")
	{
		out.clear();
		return(false);
	}
	return(true);
}

static bool wasm_source_map_load(const String& path, WasmSourceMap& map)
{
	std::ifstream input(path);
	if(!input)
		return(false);
	String line;
	const String prefix = "BEARER_SOURCE_MAP_V1\t";
	if(!std::getline(input, line) || line.rfind(prefix, 0) != 0)
		return(false);
	map.module_name = line.substr(prefix.size());
	while(std::getline(input, line))
	{
		auto fields = split(line, "\t");
		if(fields.size() == 3 && fields[0] == "F")
			map.files[(u32)strtoul(fields[1].c_str(), 0, 10)] = fields[2];
		else if(fields.size() == 5 && fields[0] == "L")
		{
			WasmSourceMap::Row row;
			row.address = strtoull(fields[1].c_str(), 0, 16);
			row.file = (u32)strtoul(fields[2].c_str(), 0, 10);
			row.line = (u32)strtoul(fields[3].c_str(), 0, 10);
			row.column = (u32)strtoul(fields[4].c_str(), 0, 10);
			map.rows.push_back(row);
		}
	}
	return(!map.rows.empty());
}

static String wasm_source_map_lookup(const WasmSourceMap& map, u64 address)
{
	const WasmSourceMap::Row* found = 0;
	for(auto& row : map.rows)
	{
		if(row.address > address)
			break;
		found = &row;
	}
	if(!found || found->line == 0)
		return("");
	auto file = map.files.find(found->file);
	if(file == map.files.end())
		return("");
	String result = file->second + ":" + std::to_string(found->line);
	if(found->column)
		result += ":" + std::to_string(found->column);
	return(result);
}

struct WasmMetadataReader
{
	int fd = -1;
	u64 file_size = 0;
	u64 bytes_read = 0;
	u64 read_count = 0;
	u64 buffer_offset = 0;
	size_t buffer_size = 0;
	u8 buffer[4096];
	bool deadline_active = false;
	std::chrono::steady_clock::time_point deadline;
	bool timed_out = false;

	bool expired()
	{
		if(!deadline_active || std::chrono::steady_clock::now() < deadline)
			return(false);
		timed_out = true;
		return(true);
	}

	bool read(u64 offset, u8* out, size_t size)
	{
		if(offset > file_size || size > file_size - offset)
			return(false);
		while(size > 0)
		{
			if(expired())
				return(false);
			if(offset < buffer_offset || offset >= buffer_offset + buffer_size)
			{
				buffer_offset = offset;
				buffer_size = 0;
				size_t wanted = (size_t)std::min<u64>(sizeof(buffer), file_size - offset);
				while(true)
				{
					if(expired())
						return(false);
					ssize_t count = pread(fd, buffer, wanted, (off_t)offset);
					if(count < 0 && errno == EINTR)
						continue;
					if(count <= 0)
						return(false);
					buffer_size = (size_t)count;
					bytes_read += (u64)count;
					read_count++;
					if(expired())
						return(false);
					break;
				}
			}
			size_t available = buffer_size - (size_t)(offset - buffer_offset);
			size_t copied = std::min(size, available);
			memcpy(out, buffer + (size_t)(offset - buffer_offset), copied);
			offset += copied;
			out += copied;
			size -= copied;
		}
		return(true);
	}

	bool read_uleb(u64& pos, u64 end, u64& out)
	{
		out = 0;
		u32 shift = 0;
		while(pos < end && shift < 64)
		{
			u8 byte = 0;
			if(!read(pos++, &byte, 1))
				return(false);
			if(shift == 63 && (byte & 0x7e) != 0)
				return(false);
			out |= ((u64)(byte & 0x7f)) << shift;
			if((byte & 0x80) == 0)
				return(true);
			shift += 7;
		}
		return(false);
	}
};

static void wasm_write_uleb(std::vector<u8>& out, u64 value)
{
	do
	{
		u8 byte = (u8)(value & 0x7f);
		value >>= 7;
		out.push_back(value ? byte | 0x80 : byte);
	}
	while(value);
}

static bool wasm_read_metadata_file(const String& path, std::vector<u8>& metadata, String& error, u64& bytes_read, u64& read_count,
	bool& timed_out, bool deadline_active, std::chrono::steady_clock::time_point deadline, const struct stat& expected_st)
{
	bytes_read = 0;
	read_count = 0;
	timed_out = false;
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if(fd < 0)
		return(false);
	struct stat st;
	if(fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 8)
	{
		close(fd);
		error = "not a wasm module";
		return(false);
	}
	u64 modified_ns = (u64)st.st_mtim.tv_sec * 1000000000ull + (u64)st.st_mtim.tv_nsec;
	u64 changed_ns = (u64)st.st_ctim.tv_sec * 1000000000ull + (u64)st.st_ctim.tv_nsec;
	u64 expected_modified_ns = (u64)expected_st.st_mtim.tv_sec * 1000000000ull + (u64)expected_st.st_mtim.tv_nsec;
	u64 expected_changed_ns = (u64)expected_st.st_ctim.tv_sec * 1000000000ull + (u64)expected_st.st_ctim.tv_nsec;
	if(st.st_dev != expected_st.st_dev || st.st_ino != expected_st.st_ino || st.st_mode != expected_st.st_mode ||
		modified_ns != expected_modified_ns || changed_ns != expected_changed_ns || st.st_size != expected_st.st_size)
	{
		close(fd);
		error = "wasm artifact changed while loading metadata";
		return(false);
	}
	u64 file_size = (u64)st.st_size;
	WasmMetadataReader reader;
	reader.fd = fd;
	reader.file_size = file_size;
	reader.deadline_active = deadline_active;
	reader.deadline = deadline;
	u8 header[8];
	if(!reader.read(0, header, sizeof(header)) || memcmp(header, "\0asm\1\0\0\0", sizeof(header)) != 0)
	{
		bytes_read = reader.bytes_read;
		read_count = reader.read_count;
		timed_out = reader.timed_out;
		close(fd);
		error = timed_out ? "wasm metadata read timed out" : "not a supported wasm module";
		return(false);
	}
	metadata.assign(header, header + sizeof(header));
	u64 pos = sizeof(header);
	bool dylink_seen = false;
	bool abi_seen = false;
	bool module_seen = false;
	while(pos < file_size)
	{
		if(reader.expired())
		{
			error = "wasm metadata read timed out";
			break;
		}
		u8 section_id = 0;
		if(!reader.read(pos++, &section_id, 1))
		{
			error = "malformed wasm section header";
			break;
		}
		u64 section_size = 0;
		if(!reader.read_uleb(pos, file_size, section_size) || section_size > file_size - pos)
		{
			error = "malformed wasm section header";
			break;
		}
		u64 section_end = pos + section_size;
		if(section_id == 0)
		{
			u64 cursor = pos;
			u64 name_len = 0;
			if(!reader.read_uleb(cursor, section_end, name_len) || name_len > section_end - cursor)
			{
				error = "malformed custom section name";
				break;
			}
			String name;
			if(name_len <= 64)
			{
				name.resize((size_t)name_len);
				if(name_len && !reader.read(cursor, (u8*)&name[0], (size_t)name_len))
				{
					error = "malformed custom section name";
					break;
				}
			}
			if(name == "dylink.0" || name == "bearer.abi" || name == "bearer.module")
			{
				bool duplicate = (name == "dylink.0" && dylink_seen) || (name == "bearer.abi" && abi_seen) ||
					(name == "bearer.module" && module_seen);
				if(duplicate)
				{
					error = "duplicate " + name + " metadata section";
					break;
				}
				if(name == "dylink.0") dylink_seen = true;
				else if(name == "bearer.abi") abi_seen = true;
				else module_seen = true;
				if(section_size > 1024 * 1024)
				{
					error = "oversized wasm metadata section";
					break;
				}
				std::vector<u8> section((size_t)section_size);
				if(section_size && !reader.read(pos, section.data(), section.size()))
				{
					error = "cannot read wasm metadata section";
					break;
				}
				metadata.push_back(0);
				wasm_write_uleb(metadata, section_size);
				metadata.insert(metadata.end(), section.begin(), section.end());
			}
		}
		pos = section_end;
	}
	bytes_read = reader.bytes_read;
	read_count = reader.read_count;
	timed_out = reader.timed_out || reader.expired();
	if(timed_out)
		error = "wasm metadata read timed out";
	struct stat final_st;
	if(error == "" && (fstat(fd, &final_st) != 0 || final_st.st_dev != st.st_dev || final_st.st_ino != st.st_ino ||
		final_st.st_mtim.tv_sec != st.st_mtim.tv_sec || final_st.st_mtim.tv_nsec != st.st_mtim.tv_nsec ||
		final_st.st_ctim.tv_sec != st.st_ctim.tv_sec || final_st.st_ctim.tv_nsec != st.st_ctim.tv_nsec ||
		final_st.st_size != st.st_size))
		error = "wasm artifact changed while loading metadata";
	close(fd);
	if(error != "")
		return(false);
	return(true);
}

// ---- worker (per process): engine + compiled module caches ----------------

struct WasmWorkspace;

struct WasmWorker
{
	WasmWorkerConfig cfg;

	explicit WasmWorker(WasmWorkerConfig config) : cfg(std::move(config)), engine(make_engine())
	{
	}

#ifdef BEARER_WASM_HOST_CONNECTORS
	~WasmWorker()
	{
		for(auto* db : mysql_persistent_pool)
			delete db;
	}

	MySQL* mysql_checkout(const String& host, const String& username, const String& password, const String& database, bool& reused, bool& persistent)
	{
		reused = false;
		persistent = false;
		for(size_t i = 0; i < mysql_persistent_pool.size(); i++)
		{
			MySQL* db = mysql_persistent_pool[i];
			if(!db || !db->connection || db->request_host != host || db->request_username != username || db->request_password != password || db->request_database != database)
				continue;
			if(db->reset_connection())
			{
				if(i + 1 < mysql_persistent_pool.size())
				{
					mysql_persistent_pool.erase(mysql_persistent_pool.begin() + i);
					mysql_persistent_pool.push_back(db);
				}
				reused = true;
				persistent = true;
				return(db);
			}
			delete db;
			mysql_persistent_pool.erase(mysql_persistent_pool.begin() + i);
			break;
		}

		MySQL* db = new MySQL();
		persistent = cfg.mysql_persistent_pool_size > 0;
		db->worker_persistent = persistent;
		db->request_pooled = true;
		db->request_host = host;
		db->request_username = username;
		db->request_password = password;
		db->request_database = database;
		if(!db->connect(host, username, password, database) || !db->connection)
			return(db);
		if(persistent)
		{
			while(mysql_persistent_pool.size() >= cfg.mysql_persistent_pool_size)
			{
				delete mysql_persistent_pool.front();
				mysql_persistent_pool.erase(mysql_persistent_pool.begin());
			}
			mysql_persistent_pool.push_back(db);
		}
		return(db);
	}

	std::vector<MySQL*> mysql_persistent_pool;
#endif

	String init()
	{
		std::vector<u8> bytes;
		if(!wasm_read_file(cfg.core_wasm_path, bytes))
			return("cannot read core module: " + cfg.core_wasm_path);
		WasmAbiInfo abi_ignored;
		String parse_error;
		if(!wasm_parse_sections(bytes, core_dylink, abi_ignored, parse_error))
			return("core module: " + parse_error);

		// Deployed runtime sources can be root-owned while workers run unprivileged.
		// Keep the derived serialized core in the configured writable cache root.
		String core_cached_path = path_join(cfg.cache_root, ".uce-core.cwasm");
		String compile_error;
		bool serialized_cache_hit = false;
		auto compiled_or_cached = load_or_compile_cached_module(engine, core_cached_path, cfg.core_wasm_path, bytes, compile_error, serialized_cache_hit);
		if(!compiled_or_cached)
			return("core module compile failed: " + compile_error);
		core_module.emplace(std::move(*compiled_or_cached));
		return("");
	}

	wasmtime::Engine engine;
	std::optional<wasmtime::Module> core_module;
	std::optional<wasmtime::Linker> core_linker;
	std::unique_ptr<wasmtime_instance_pre_t, WasmInstancePreDeleter> core_instance_pre;
	DValue server_config_context;
	String server_config_encoded;
	struct CoreImport
	{
		String module;
		String name;
		bool table = false;
	};
	std::vector<CoreImport> core_imports;
	u32 core_table_min = 0;
	bool core_table_imported = false;
	u64 core_host_import_count = 0;
	// A render worker dispatches one workspace at a time. Persistent host Func
	// definitions resolve their request-owned state through this pointer; forked
	// task children inherit the live workspace copy before the parent drops it.
	WasmWorkspace* active_workspace = 0;
	WasmDylinkInfo core_dylink;

	// unit artifact path in the W2 cache: cache_root + <abs source dir> + /<file>.wasm
	String unit_wasm_path(const String& source_path) const
	{
		return(cfg.cache_root + source_path + ".wasm");
	}

	std::shared_ptr<WasmUnitModule> unit_module(const String& source_path, String& error, WasmUnitModuleLoadProfile& profile,
		bool deadline_active, std::chrono::steady_clock::time_point deadline)
	{
		auto deadline_expired = [&]() {
			if(!deadline_active || std::chrono::steady_clock::now() < deadline)
				return(false);
			profile.timed_out = true;
			error = "wasm module load timed out";
			return(true);
		};
		auto lookup_start = std::chrono::steady_clock::now();
		String wasm_path = unit_wasm_path(source_path);
		struct stat st;
		if(stat(wasm_path.c_str(), &st) != 0)
		{
			profile.lookup_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - lookup_start).count();
			error = "no wasm artifact for " + source_path + " (expected " + wasm_path + ")";
			return(nullptr);
		}
		if(!S_ISREG(st.st_mode))
		{
			profile.lookup_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - lookup_start).count();
			error = "wasm artifact is not a regular file: " + wasm_path;
			return(nullptr);
		}
		auto cached = module_cache.find(wasm_path);
		u64 modified_ns = (u64)st.st_mtim.tv_sec * 1000000000ull + (u64)st.st_mtim.tv_nsec;
		u64 changed_ns = (u64)st.st_ctim.tv_sec * 1000000000ull + (u64)st.st_ctim.tv_nsec;
		profile.lookup_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - lookup_start).count();
		if(deadline_expired())
			return(nullptr);
		if(cached != module_cache.end() && cached->second->modified_ns == modified_ns && cached->second->changed_ns == changed_ns && cached->second->size == (u64)st.st_size)
		{
			profile.cache_hit = true;
			return(cached->second);
		}

		auto unit = std::make_shared<WasmUnitModule>();
		unit->source_path = source_path;
		unit->wasm_path = wasm_path;
		unit->modified_ns = modified_ns;
		unit->changed_ns = changed_ns;
		unit->size = (u64)st.st_size;
		String unit_cached_path = cached_wasm_path(wasm_path);
		auto compile_start = std::chrono::steady_clock::now();
		auto compiled_or_cached = load_current_serialized_module(engine, unit_cached_path, wasm_path);
		profile.compile_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - compile_start).count();
		profile.serialized_cache_hit = compiled_or_cached.has_value();
		if(deadline_expired())
			return(nullptr);
		std::vector<u8> bytes;
		auto read_start = std::chrono::steady_clock::now();
		bool read_ok = profile.serialized_cache_hit
			? wasm_read_metadata_file(wasm_path, bytes, error, profile.read_bytes, profile.read_count, profile.timed_out,
				deadline_active, deadline, st)
			: wasm_read_file_deadline(wasm_path, bytes, error, profile.read_bytes, profile.read_count, profile.timed_out,
				deadline_active, deadline, st);
		if(!read_ok)
		{
			profile.read_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - read_start).count();
			if(error == "")
				error = "cannot read " + wasm_path;
			return(nullptr);
		}
		profile.read_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - read_start).count();
		if(deadline_expired())
			return(nullptr);
		auto parse_start = std::chrono::steady_clock::now();
		if(!wasm_parse_sections(bytes, unit->dylink, unit->abi, error, deadline_active, deadline, &profile.timed_out))
		{
			profile.parse_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - parse_start).count();
			error = wasm_path + ": " + error;
			return(nullptr);
		}
		profile.parse_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - parse_start).count();
		if(deadline_expired())
			return(nullptr);
		if(!unit->dylink.found)
		{
			error = wasm_path + ": missing dylink.0 mem_info (not a PIC side module)";
			return(nullptr);
		}
		if(!unit->abi.found)
		{
			error = wasm_path + ": missing bearer.abi stamp";
			return(nullptr);
		}
		String compile_error;
		if(!compiled_or_cached)
		{
			compile_start = std::chrono::steady_clock::now();
			compiled_or_cached = compile_and_cache_module(engine, unit_cached_path, bytes, compile_error);
			profile.compile_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - compile_start).count();
		}
		if(deadline_expired())
			return(nullptr);
		if(!compiled_or_cached)
		{
			error = wasm_path + ": compile failed: " + compile_error;
			return(nullptr);
		}
		unit->module.emplace(std::move(*compiled_or_cached));
		auto classify_start = std::chrono::steady_clock::now();
		for(auto import_type : unit->module->imports())
		{
			if(deadline_expired())
				return(nullptr);
			String mod_name(import_type.module());
			String name(import_type.name());
			auto extern_type = wasmtime::ExternType::from_import(import_type);
			bool is_func_import = std::get_if<wasmtime::FuncType::Ref>(&extern_type) != 0;
			WasmUnitImportKind kind;
			if(mod_name == "env" && name == "memory")
				kind = WasmUnitImportKind::Memory;
			else if(mod_name == "env" && name == "__indirect_function_table")
				kind = WasmUnitImportKind::Table;
			else if(mod_name == "env" && name == "__stack_pointer")
				kind = WasmUnitImportKind::StackPointer;
			else if(mod_name == "env" && name == "__memory_base")
				kind = WasmUnitImportKind::MemoryBase;
			else if(mod_name == "env" && name == "__table_base")
				kind = WasmUnitImportKind::TableBase;
			else if(mod_name == "env" && is_func_import)
				kind = WasmUnitImportKind::Function;
			else if(mod_name == "GOT.mem")
			{
				kind = WasmUnitImportKind::GotMemory;
				unit->got_memory_imports++;
			}
			else if(mod_name == "GOT.func")
				kind = WasmUnitImportKind::GotFunction;
			else
			{
				error = source_path + ": import policy violation: " + mod_name + "." + name;
				return(nullptr);
			}
			unit->imports.push_back({ kind, std::move(name) });
		}
		profile.classify_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - classify_start).count();
		if(deadline_expired())
			return(nullptr);
		module_cache[wasm_path] = unit;
		return(unit);
	}

	static bool serialized_module_needs_refresh(const String& wasm_path)
	{
		struct stat wasm_stat;
		struct stat cached_stat;
		String cached_path = cached_wasm_path(wasm_path);
		if(stat(wasm_path.c_str(), &wasm_stat) != 0)
			return(false);
		if(stat(cached_path.c_str(), &cached_stat) != 0)
			return(true);
		return(cached_stat.st_mtim.tv_sec < wasm_stat.st_mtim.tv_sec ||
			(cached_stat.st_mtim.tv_sec == wasm_stat.st_mtim.tv_sec && cached_stat.st_mtim.tv_nsec <= wasm_stat.st_mtim.tv_nsec));
	}

	static String serialize_module_artifact(const String& wasm_path)
	{
		if(!serialized_module_needs_refresh(wasm_path))
			return("");
		std::vector<u8> bytes;
		if(!wasm_read_file(wasm_path, bytes))
			return("cannot read " + wasm_path);
		wasmtime::Engine engine = make_engine();
		String error;
		auto module = compile_and_cache_module(engine, cached_wasm_path(wasm_path), bytes, error);
		if(!module)
			return(error);
		if(serialized_module_needs_refresh(wasm_path))
			return("cannot publish serialized module for " + wasm_path);
		return("");
	}

	friend struct WasmWorkspace;
	struct ComponentFreshnessState
	{
		std::chrono::steady_clock::time_point checked_at;
		bool stale = false;
		String source_generation;
	};
	struct ComponentResolutionState
	{
		std::chrono::steady_clock::time_point checked_at;
		String resolved;
	};
	std::map<String, ComponentFreshnessState> component_freshness;
	std::map<String, ComponentResolutionState> component_resolutions;

	static String cached_wasm_path(const String& wasm_path)
	{
		if(wasm_path.size() >= 5 && wasm_path.rfind(".wasm", wasm_path.size() - 5) == wasm_path.size() - 5)
			return(wasm_path.substr(0, wasm_path.size() - 5) + ".cwasm");
		return(wasm_path + ".cwasm");
	}

	static std::optional<wasmtime::Module> load_current_serialized_module(wasmtime::Engine& engine, const String& cached_path, const String& wasm_path)
	{
		struct stat wasm_stat;
		struct stat cached_stat;
		if(stat(cached_path.c_str(), &cached_stat) == 0 && stat(wasm_path.c_str(), &wasm_stat) == 0)
		{
			if(cached_stat.st_mtim.tv_sec > wasm_stat.st_mtim.tv_sec ||
				(cached_stat.st_mtim.tv_sec == wasm_stat.st_mtim.tv_sec && cached_stat.st_mtim.tv_nsec > wasm_stat.st_mtim.tv_nsec))
			{
				auto deserialized = wasmtime::Module::deserialize_file(engine, cached_path);
				if(deserialized)
					return(deserialized.ok());
			}
		}
		return(std::nullopt);
	}

	static std::optional<wasmtime::Module> compile_and_cache_module(wasmtime::Engine& engine, const String& cached_path,
		std::vector<u8>& bytes, String& compile_error)
	{
		auto compiled = wasmtime::Module::compile(engine, bytes);
		if(!compiled)
		{
			compile_error = String(compiled.err().message());
			return(std::nullopt);
		}
		auto result = compiled.ok();
		auto serialized = result.serialize();
		if(serialized)
		{
			String tmp = cached_path + "." + std::to_string((long long)getpid()) + ".tmp";
			auto data = serialized.ok();
			{
				std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
				if(out)
				{
					out.write((const char*)data.data(), (std::streamsize)data.size());
					if(out)
					{
						out.flush();
						out.close();
						if(std::rename(tmp.c_str(), cached_path.c_str()) != 0)
							(void)std::remove(tmp.c_str());
					}
				}
				else
					(void)std::remove(tmp.c_str());
			}
		}
		return(result);
	}

	static std::optional<wasmtime::Module> load_or_compile_cached_module(wasmtime::Engine& engine, const String& cached_path, const String& wasm_path,
		std::vector<u8>& bytes, String& compile_error, bool& serialized_cache_hit)
	{
		serialized_cache_hit = false;
		auto cached = load_current_serialized_module(engine, cached_path, wasm_path);
		if(cached)
		{
			serialized_cache_hit = true;
			return(cached);
		}
		return(compile_and_cache_module(engine, cached_path, bytes, compile_error));
	}

	static wasmtime::Engine make_engine()
	{
		wasmtime::Config config;
		wasmtime::PoolAllocationConfig pool;
		// A workspace owns one core plus at most table_headroom side modules;
		// side modules import its single memory/table. Keep allocator capacity at
		// the existing handler-table boundary instead of imposing its 1000-slot default.
		pool.total_core_instances(4097);
		config.pooling_allocation_strategy(pool);
		config.epoch_interruption(true);
		// CRITICAL: the host (linux_fastcgi.cpp) installs its own SIGSEGV/SIGILL/
		// SIGBUS handlers per request (install_request_fault_handlers) with plain
		// signal(), which clobbers Wasmtime's trap-handling handlers. With
		// signals_based_traps on, a guest `unreachable`/OOB surfaces as a host
		// signal that the native on_segfault catches and abort()s the worker —
		// a 502 instead of a clean wasm trap. Disabling it makes Cranelift emit
		// explicit trap checks: guest traps stay pure wasm traps returned as
		// Result errors, never a host signal, so the two never collide.
		config.signals_based_traps(false);
		return(wasmtime::Engine(std::move(config)));
	}

	std::map<String, std::shared_ptr<WasmUnitModule>> module_cache;
	std::vector<String> hostcall_operation_names;
};

// The request envelope keeps app scratch state separate from transport fields.
// Maps use BRRB2 directly so the guest can populate Request StringMaps without
// constructing and then copying a duplicate DValue subtree.
static void wasm_request_envelope_append(String& envelope, const String& value)
{
	u64 size = value.size();
	while(size >= 0x80)
	{
		envelope.push_back((char)((size & 0x7f) | 0x80));
		size >>= 7;
	}
	envelope.push_back((char)size);
	envelope.append(value.data(), value.size());
}

static String wasm_encode_request_envelope(const Request& request, const String& entry_unit, const String& handler)
{
	String envelope = "BRRQ";
	envelope.push_back(1);
	envelope.push_back(12);
	wasm_request_envelope_append(envelope, brb_encode(request.call));
	wasm_request_envelope_append(envelope, brb_encode_flat_string_map(request.params));
	wasm_request_envelope_append(envelope, brb_encode_flat_string_map(request.get));
	wasm_request_envelope_append(envelope, brb_encode_flat_string_map(request.post));
	wasm_request_envelope_append(envelope, brb_encode_flat_string_map(request.cookies));
	wasm_request_envelope_append(envelope, brb_encode_flat_string_map(request.session));
	wasm_request_envelope_append(envelope, request.session_id);
	wasm_request_envelope_append(envelope, request.session_name);
	wasm_request_envelope_append(envelope, request.session_loaded_hash);
	wasm_request_envelope_append(envelope, entry_unit);
	wasm_request_envelope_append(envelope, request.in);
	String websocket;
	if(handler == "websocket")
	{
		DValue ws;
		ws["connection_id"] = request.resources.websocket_connection_id;
		ws["scope"] = request.resources.websocket_scope;
		ws["opcode"] = (f64)request.resources.websocket_opcode;
		ws["binary"].set_bool(request.resources.websocket_is_binary);
		for(auto& id : request.resources.websocket_scope_connection_ids)
		{
			DValue value;
			value = id;
			ws["connections"].push(value);
		}
		ws["connection_state"] = request.connection;
		websocket = brb_encode(ws);
	}
	wasm_request_envelope_append(envelope, websocket);
	return(envelope);
}

// ---- workspace (per request) ----------------------------------------------

static String wasm_host_regex_execute(const String& encoded)
{
	DValue request;
	String decode_error;
	DValue response;
	if(brb_decode(encoded, request, &decode_error))
	{
		String op = request["op"].to_string();
		String pattern = request["pattern"].to_string();
		String subject = request["subject"].to_string();
		String flags = request["flags"].to_string();
		if(op == "match")
			response["bool"].set_bool(regex_match(pattern, subject, flags));
		else if(op == "search")
			response["tree"] = regex_search(pattern, subject, flags);
		else if(op == "search_all")
			response["tree"] = regex_search_all(pattern, subject, flags);
		else if(op == "replace")
			response["text"] = regex_replace(pattern, request["replacement"].to_string(), subject, flags);
		else if(op == "split")
			for(auto& part : regex_split(pattern, subject, flags))
			{
				DValue value;
				value = part;
				response["list"].push(value);
			}
	}
	return(brb_encode(response));
}

struct WasmWorkspace : public WasmRequestProfile
{
	WasmWorker& worker;
	wasmtime::Store store;

	struct FileHandle
	{
		int fd = -1;
		bool writable = false;
	};
	std::vector<FileHandle> file_handles;
	String capy_regex_result;

	struct RequestPerfSnapshot
	{
		u64 worker_pid = 0;
		u64 parent_pid = 0;
		u64 request_count = 0;
		f64 time_init = 0;
		f64 time_params = 0;
		f64 time_input = 0;
		f64 time_start = 0;
		u64 ready_normalize_us = 0;
		u64 ready_mutation_check_us = 0;
		u64 ready_artifact_stat_us = 0;
		u64 ready_freshness_us = 0;
		u64 ready_source_generation_us = 0;
		u64 ready_freshness_full_check_us = 0;
		u64 ready_worker_us = 0;
		u32 ready_check_count = 0;
		u32 ready_freshness_cache_hit_count = 0;
		f64 workspace_wall_start = 0;
		f64 workspace_cpu_start = 0;
		struct rusage thread_runtime_start = {};
		int thread_cpu_start = -1;
		bool thread_runtime_profiled = false;
		bool active = false;
	} request_perf;

	explicit WasmWorkspace(WasmWorker& w) : worker(w), store(w.engine)
	{
		worker.active_workspace = this;
	}

	using InvocationClock = std::chrono::steady_clock;
	bool invocation_active = false;
	InvocationClock::time_point invocation_deadline;
	u64 invocation_budget_ms = 0;

	u64 invocation_remaining_ms(InvocationClock::time_point now = InvocationClock::now()) const
	{
		if(!invocation_active)
			return(UINT64_MAX);
		if(now >= invocation_deadline)
			return(0);
		return((u64)std::chrono::duration_cast<std::chrono::milliseconds>(invocation_deadline - now).count());
	}

	bool invocation_expired(InvocationClock::time_point now = InvocationClock::now()) const
	{
		return(invocation_active && now >= invocation_deadline);
	}

	String invocation_timeout_error() const
	{
		return("BEARER_INVOCATION_TIMEOUT: wasm invocation exceeded " + std::to_string(invocation_budget_ms) + " ms");
	}

	void arm_guest_deadline(wasmtime::Store::Context context)
	{
		u64 ticks = worker.cfg.epoch_deadline_ticks;
		if(invocation_active)
		{
			u64 remaining_ms = invocation_remaining_ms();
			if(remaining_ms == 0)
				ticks = 0;
			else
			{
				u64 remaining_ticks = remaining_ms / worker.cfg.epoch_period_ms +
					(remaining_ms % worker.cfg.epoch_period_ms != 0);
				u64 segment_ms = worker.cfg.epoch_deadline_ticks > UINT64_MAX / worker.cfg.epoch_period_ms ?
					UINT64_MAX : worker.cfg.epoch_deadline_ticks * worker.cfg.epoch_period_ms;
				// One extra tick prevents the engine ticker's current phase from
				// interrupting just before the absolute steady-clock deadline.
				if(remaining_ms <= segment_ms)
					ticks = remaining_ticks == UINT64_MAX ? UINT64_MAX : remaining_ticks + 1;
			}
		}
		context.set_epoch_deadline(ticks);
	}

	u64 bounded_hostcall_timeout_ms(u64 requested_ms) const
	{
		if(!invocation_active)
			return(requested_ms);
		u64 remaining_ms = invocation_remaining_ms();
		return(std::min(requested_ms, remaining_ms));
	}

	struct InvocationScope
	{
		WasmWorkspace& workspace;
		bool replaced = false;
		bool previous_active = false;
		InvocationClock::time_point previous_deadline;
		u64 previous_budget_ms = 0;
		InvocationScope(WasmWorkspace& workspace, u64 timeout_cap_ms = 0, bool force_new = false, u64 reported_budget_ms = 0) : workspace(workspace)
		{
			if(!workspace.invocation_active || force_new)
			{
				replaced = true;
				previous_active = workspace.invocation_active;
				previous_deadline = workspace.invocation_deadline;
				previous_budget_ms = workspace.invocation_budget_ms;
				u64 budget_ms = workspace.worker.cfg.invocation_timeout_ms;
				if(timeout_cap_ms != UINT64_MAX)
					budget_ms = std::min(budget_ms, timeout_cap_ms);
				workspace.invocation_active = true;
				workspace.invocation_budget_ms = reported_budget_ms > 0 ? reported_budget_ms : budget_ms;
				workspace.invocation_deadline = InvocationClock::now() + std::chrono::milliseconds(budget_ms);
			}
			workspace.arm_guest_deadline(workspace.ctx());
		}
		~InvocationScope()
		{
			if(!replaced)
				return;
			workspace.invocation_active = previous_active;
			workspace.invocation_deadline = previous_deadline;
			workspace.invocation_budget_ms = previous_budget_ms;
			workspace.arm_guest_deadline(workspace.ctx());
		}
	};

	void set_perf_snapshot(u64 worker_pid, u64 parent_pid, u64 request_count,
		f64 time_init, f64 time_params, f64 time_input, f64 time_start,
		u64 ready_normalize_us, u64 ready_mutation_check_us, u64 ready_artifact_stat_us,
		u64 ready_freshness_us, u64 ready_source_generation_us, u64 ready_freshness_full_check_us,
		u64 ready_worker_us, u32 ready_check_count, u32 ready_freshness_cache_hit_count,
		f64 workspace_wall_start, f64 workspace_cpu_start)
	{
		request_perf.worker_pid = worker_pid;
		request_perf.parent_pid = parent_pid;
		request_perf.request_count = request_count;
		request_perf.time_init = time_init;
		request_perf.time_params = time_params;
		request_perf.time_input = time_input;
		request_perf.time_start = time_start;
		request_perf.ready_normalize_us = ready_normalize_us;
		request_perf.ready_mutation_check_us = ready_mutation_check_us;
		request_perf.ready_artifact_stat_us = ready_artifact_stat_us;
		request_perf.ready_freshness_us = ready_freshness_us;
		request_perf.ready_source_generation_us = ready_source_generation_us;
		request_perf.ready_freshness_full_check_us = ready_freshness_full_check_us;
		request_perf.ready_worker_us = ready_worker_us;
		request_perf.ready_check_count = ready_check_count;
		request_perf.ready_freshness_cache_hit_count = ready_freshness_cache_hit_count;
		request_perf.workspace_wall_start = workspace_wall_start;
		request_perf.workspace_cpu_start = workspace_cpu_start;
		request_perf.active = true;
	}

#ifdef BEARER_WASM_HOST_CONNECTORS
	// Host-owned resource handle table (§3.1): connections opened by the guest
	// live here and are closed when the workspace drops at request end. This is
	// the wasm-side enforcement of request-scoped DB lifecycle; app code should
	// never cache these opaque handles across requests.
	std::vector<SQLite*> sqlite_handles;
	std::vector<MySQL*> mysql_handles;
	std::vector<MySQL*> mysql_request_pool;
	std::vector<MySQL*> mysql_request_owned;
#endif
	~WasmWorkspace()
	{
		if(worker.active_workspace == this)
			worker.active_workspace = 0;
		for(auto& h : file_handles)
		{
			if(h.fd >= 0)
			{
				flock(h.fd, LOCK_UN);
				close(h.fd);
				h.fd = -1;
			}
		}
#ifdef BEARER_WASM_HOST_CONNECTORS
		for(auto* db : sqlite_handles)
			if(db)
				delete db;   // ~SQLite disconnects
		for(auto* db : mysql_request_pool)
			if(db)
				db->request_leases = 0;
		for(auto* db : mysql_request_owned)
			if(db)
				delete db;   // ~MySQL disconnects
#endif
	}

	// The guest calls a sized hostcall twice (buf=0 to learn the length, then
	// to fetch). For side-effecting ops (sqlite) re-executing on the fetch is
	// wrong, so the result is staged on the first call (keyed on the exact
	// input bytes) and replayed on the second without re-running the op.
	String staged_hostcall_input;
	String staged_hostcall_result;
	String staged_socket_read_key;
	String staged_socket_read_result;
	String staged_memcache_key;
	String staged_memcache_result;
	bool hostcall_staged(const String& input, String& out)
	{
		if(!staged_hostcall_input.empty() && input == staged_hostcall_input)
		{
			out = staged_hostcall_result;
			staged_hostcall_input = "";
			staged_hostcall_result = "";
			return(true);
		}
		return(false);
	}
	void hostcall_stage(const String& input, const String& out)
	{
		staged_hostcall_input = input;
		staged_hostcall_result = out;
	}

	// resolve-kind values shared with the guest core (src/wasm/core.cpp)
	// must match WasmResolveKind in src/wasm/core.cpp

	String birth()
	{
		auto phase_start = std::chrono::steady_clock::now();
		auto phase_us = [&]() -> u64 {
			auto now = std::chrono::steady_clock::now();
			u64 elapsed = (u64)std::chrono::duration_cast<std::chrono::microseconds>(now - phase_start).count();
			phase_start = now;
			return(elapsed);
		};
		if(invocation_expired())
			return(invocation_timeout_error());
		auto cx = ctx();
		store.limiter(worker.cfg.memory_limit, -1, -1, -1, -1);
		arm_guest_deadline(cx);
		birth_policy_us = phase_us();

		auto& module = *worker.core_module;
		if(!worker.core_linker)
		{
			worker.core_linker.emplace(worker.engine);
			for(auto import_type : module.imports())
			{
				String mod(import_type.module());
				String name(import_type.name());
				auto extern_type = wasmtime::ExternType::from_import(import_type);
				if(auto* table_ref = std::get_if<wasmtime::TableType::Ref>(&extern_type))
				{
					if(name.rfind("__indirect_function_table", 0) != 0)
						return("core imports unexpected table " + mod + "." + name);
					worker.core_table_min = table_ref->min();
					worker.core_table_imported = true;
					worker.core_imports.push_back({ mod, name, true });
					continue;
				}
				auto* func_ref = std::get_if<wasmtime::FuncType::Ref>(&extern_type);
				if(!func_ref)
					return("core has unexpected non-func import " + mod + "." + name);
				wasmtime::FuncType func_type(*func_ref);
				String error = make_host_import(*worker.core_linker, mod, name, func_type);
				if(error != "")
					return(error);
				worker.core_imports.push_back({ mod, name, false });
			}
			if(!worker.core_table_imported)
			{
				wasmtime_instance_pre_t* instance_pre = 0;
				wasmtime_error_t* pre_error = wasmtime_linker_instantiate_pre(
					worker.core_linker->capi(), module.capi(), &instance_pre);
				if(pre_error)
					return("core pre-instantiation failed: " + String(wasmtime::Error(pre_error).message()));
				worker.core_instance_pre.reset(instance_pre);
			}
		}
		std::vector<wasmtime::Extern> imports;
		if(worker.core_table_imported)
		{
			u32 total = worker.core_table_min + worker.cfg.table_headroom;
			wasmtime::TableType table_type(wasmtime::ValType::funcref(), total, total);
			auto table_created = wasmtime::Table::create(cx, table_type, wasmtime::Val(std::optional<wasmtime::Func>()));
			if(!table_created)
				return("table create failed: " + String(table_created.err().message()));
			table.emplace(table_created.ok());
			table_next_free = worker.core_table_min;
			for(auto& import : worker.core_imports)
			{
				if(import.table)
				{
					imports.push_back(*table);
					continue;
				}
				auto host_import = worker.core_linker->get(cx, import.module, import.name);
				if(!host_import)
					return("core host import unavailable: " + import.module + "." + import.name);
				imports.push_back(*host_import);
			}
		}
		hostcall_operation_slots = worker.hostcall_operation_names.size();
		birth_import_us = phase_us();

		if(worker.core_instance_pre)
		{
			wasmtime_instance_t instance;
			wasm_trap_t* trap = 0;
			wasmtime_error_t* instantiate_error = wasmtime_instance_pre_instantiate(
				worker.core_instance_pre.get(), cx.capi(), &instance, &trap);
			if(instantiate_error)
				return("core instantiation failed: " + String(wasmtime::Error(instantiate_error).message()));
			if(trap)
				return("core instantiation trapped: " + String(wasmtime::Trap(trap).message()));
			core.emplace(instance);
		}
		else
		{
			auto created = wasmtime::Instance::create(cx, module, imports);
			if(!created)
				return("core instantiation failed: " + trap_text(created.err()));
			core.emplace(created.ok());
		}
		birth_instantiate_us = phase_us();

		if(!table)
		{
			auto exported_table = core->get(cx, "__indirect_function_table");
			if(!exported_table || !std::get_if<wasmtime::Table>(&*exported_table))
				return("core does not export __indirect_function_table");
			table.emplace(std::get<wasmtime::Table>(*exported_table));
			auto grown = table->grow(cx, worker.cfg.table_headroom, wasmtime::Val(std::optional<wasmtime::Func>()));
			if(!grown)
				return("table grow failed: " + String(grown.err().message()));
			table_next_free = (u32)grown.ok();
		}

		auto exported_memory = core->get(cx, "memory");
		if(!exported_memory || !std::get_if<wasmtime::Memory>(&*exported_memory))
			return("core does not export memory");
		memory.emplace(std::get<wasmtime::Memory>(*exported_memory));
		birth_exports_us = phase_us();

		String error = call_core("_initialize", {}, 0);
		if(error != "")
			return(error);
		int32_t rc = 0;
		error = call_core("bearer_wasm_core_init", {}, &rc);
		if(error != "")
			return(error);
		if(rc != 0)
			return("bearer_wasm_core_init returned " + std::to_string(rc));
		int32_t core_abi = 0;
		error = call_core("bearer_wasm_core_abi_version", {}, &core_abi);
		if(error != "")
			return(error);
		abi_version = (u32)core_abi;
		error = call_core("bearer_wasm_request", {}, &request_ptr);
		if(error != "")
			return(error);
		birth_initialize_us = phase_us();
		if(request_ptr == 0)
			return("core returned null Request*");
		return("");
	}

	String apply_context(const Request& request, const String& entry_unit, const String& handler)
	{
		auto phase_start = std::chrono::steady_clock::now();
		auto phase_us = [&]() -> u64 {
			auto now = std::chrono::steady_clock::now();
			u64 elapsed = (u64)std::chrono::duration_cast<std::chrono::microseconds>(now - phase_start).count();
			phase_start = now;
			return(elapsed);
		};
		String encoded = wasm_encode_request_envelope(request, entry_unit, handler);
		server_config_bytes = worker.server_config_encoded.size();
		context_bytes = server_config_bytes + encoded.size();
		context_encode_us = phase_us();
		int32_t guest_ptr = 0;
		String error = call_core("bearer_alloc", { (int32_t)context_bytes }, &guest_ptr);
		context_allocate_us = phase_us();
		if(error != "")
			return(error);
		if(guest_ptr == 0)
			return("guest bearer_alloc failed for context buffer");
		error = guest_write((u32)guest_ptr, worker.server_config_encoded);
		if(error == "")
			error = guest_write((u32)guest_ptr + (u32)server_config_bytes, encoded);
		context_write_us = phase_us();
		if(error != "")
			return(error);
		int32_t rc = 0;
		error = call_core("bearer_wasm_apply_context", { guest_ptr, (int32_t)server_config_bytes,
			guest_ptr + (int32_t)server_config_bytes, (int32_t)encoded.size() }, &rc);
		context_guest_apply_us = phase_us();
		if(error != "")
			return(error);
		call_core("bearer_free", { guest_ptr }, 0);
		context_free_us = phase_us();
		if(rc != 0)
			return("bearer_wasm_apply_context returned " + std::to_string(rc));
		return("");
	}

	// Invoke the entry unit through a named handler ("render"/"cli"/"websocket"/
	// "serve_http:named"). The handler is just an export name — same resolve +
	// ONCE + dispatch path as a component. handler_present, when provided,
	// reports whether the unit exports it (caller maps a missing render to an
	// empty body, a missing cli/serve handler to a 404).
	String invoke_entry(const String& entry_source_path, const String& handler, bool* handler_present = 0)
	{
		InvocationScope invocation(*this);
		auto phase_started = std::chrono::steady_clock::now();
		auto phase_us = [&]() {
			auto now = std::chrono::steady_clock::now();
			u64 elapsed = (u64)std::chrono::duration_cast<std::chrono::microseconds>(now - phase_started).count();
			phase_started = now;
			return(elapsed);
		};
		entry_dir = dir_of(entry_source_path);
		size_t unit_index = 0;
		String error = load_unit(entry_source_path, "entry", unit_index);
		entry_load_us = phase_us();
		if(invocation_expired())
			return(invocation_timeout_error());
		if(error != "")
			return(error);
		String handler_symbol = handler_export_symbol(handler);
		auto handler_fn = unit_func(unit_index, handler_symbol);
		bool present = (bool)handler_fn;
		entry_presence_us = phase_us();
		if(handler_present)
			*handler_present = present;
		if(!present)
			return("");
		// The entry unit is already loaded above. Place its handler and optional
		// ONCE export directly in the shared table, then let the core retain ONCE
		// deduplication and dispatch without resolving the same unit via hostcall.
		auto entry = core_func("bearer_wasm_invoke_loaded_entry");
		if(!entry)
			return("core does not export bearer_wasm_invoke_loaded_entry");
		auto link_handler = [&](const String& symbol, const std::optional<wasmtime::Func>& func, u32& slot) {
			if(!func)
			{
				slot = 0;
				return(String(""));
			}
			String slot_key = entry_source_path + ":" + symbol;
			auto cached = handler_slots.find(slot_key);
			if(cached != handler_slots.end())
			{
				slot = cached->second;
				return(String(""));
			}
			String link_error = place_funcref(*func, slot);
			if(link_error == "")
				handler_slots[slot_key] = slot;
			return(link_error);
		};
		u32 handler_slot = 0, once_slot = 0;
		error = link_handler(handler_symbol, handler_fn, handler_slot);
		String once_symbol = handler_export_symbol("once");
		if(error == "")
			error = link_handler(once_symbol, unit_func(unit_index, once_symbol), once_slot);
		if(error != "")
			return(error);
		entry_link_us = phase_us();
		auto result = call_guest(*entry, { wasmtime::Val((int32_t)handler_slot), wasmtime::Val((int32_t)once_slot) });
		entry_dispatch_us = phase_us();
		if(!result)
			return(trap_text(result.err()));
		return("");
	}

	String collect(WasmResponse& response)
	{
		String error = call_core("bearer_wasm_finish_output", {}, 0);
		if(error != "")
			return(error);
		error = call_core("bearer_wasm_finish_response_meta", {}, 0);
		if(error != "")
			return(error);
		int32_t body_ptr = 0, body_len = 0, meta_ptr = 0, meta_len = 0;
		if((error = call_core("bearer_wasm_output_data", {}, &body_ptr)) != "") return(error);
		if((error = call_core("bearer_wasm_output_size", {}, &body_len)) != "") return(error);
		if((error = call_core("bearer_wasm_response_meta_data", {}, &meta_ptr)) != "") return(error);
		if((error = call_core("bearer_wasm_response_meta_size", {}, &meta_len)) != "") return(error);
		error = guest_read((u32)body_ptr, (u32)body_len, response.body);
		if(error != "")
			return(error);
		String meta_encoded;
		error = guest_read((u32)meta_ptr, (u32)meta_len, meta_encoded);
		if(error != "")
			return(error);
		String decode_error;
		if(!brb_decode(meta_encoded, response.meta, &decode_error))
			return("response meta decode failed: " + decode_error);
		if(stale_component_mutation_blocked)
		{
			response.body = "The requested code is being updated. Retry this request shortly.\n";
			response.meta["status"] = stale_component_mutation_status;
			response.meta["headers"]["Content-Type"] = "text/plain; charset=utf-8";
			response.meta["headers"]["Retry-After"] = "1";
		}
		return("");
	}

	std::optional<wasmtime::Instance> core;
	std::optional<wasmtime::Memory> memory;
	std::optional<wasmtime::Table> table;
	u32 table_next_free = 0;
	u32 abi_version = 0;
	int32_t request_ptr = 0;
	String entry_dir;

	struct LoadedUnit
	{
		std::shared_ptr<WasmUnitModule> mod;
		std::optional<wasmtime::Instance> instance;
		u32 memory_base = 0;
	};
	std::vector<LoadedUnit> units;
	std::map<String, size_t> units_by_source;
	std::map<String, String> component_loaded_paths; // caller + target → request-loaded canonical source
	std::map<String, u32> handler_slots;     // source + ":" + symbol → table slot
	bool stale_component_mutation_blocked = false;
	String stale_component_mutation_status;
	bool component_source_generation_checked = false;
	String component_source_generation;

	wasmtime::Store::Context ctx()
	{
		return(wasmtime::Store::Context(store));
	}

	static String dir_of(const String& path)
	{
		auto pos = path.find_last_of('/');
		return(pos == String::npos ? String("") : path.substr(0, pos));
	}

	String trap_text(const wasmtime::TrapError& error)
	{
		String result = wasm_trace_collapse(String(error.message()));
		bool invocation_deadline_interrupt = invocation_active &&
			(invocation_expired() || invocation_remaining_ms() <= worker.cfg.epoch_period_ms);
		if(invocation_deadline_interrupt && result.find("interrupt") != String::npos && result.find("BEARER_INVOCATION_TIMEOUT:") == String::npos)
			result = invocation_timeout_error() + "\n" + result;
		struct Frame
		{
			String module;
			String function;
			u64 offset = 0;
		};
		std::vector<Frame> frames;
		auto collect = [&](const wasmtime::Trace& trace) {
			if(trace.size() == 0)
				return;
			for(auto& frame : trace)
			{
				Frame item;
				if(auto name = frame.module_name())
					item.module = String(*name);
				if(auto name = frame.func_name())
					item.function = wasm_trace_demangle(String(*name));
				item.offset = frame.module_offset();
				frames.push_back(item);
			}
		};
		if(auto* trap = std::get_if<wasmtime::Trap>(&error.data))
		{
			auto trace = trap->trace();
			collect(trace);
		}
		else if(auto* runtime_error = std::get_if<wasmtime::Error>(&error.data))
		{
			auto trace = runtime_error->trace();
			collect(trace);
		}

		std::map<String, WasmSourceMap> maps;
		std::set<String> unavailable;
		std::vector<String> locations;
		for(size_t index = 0; index < frames.size() && locations.size() < 12; index++)
		{
			const WasmUnitModule* unit = 0;
			for(auto& loaded : units)
				if(loaded.mod->abi.module_name == frames[index].module)
				{
					unit = loaded.mod.get();
					break;
				}
			if(!unit)
				continue;
			String map_path = unit->wasm_path + ".source-map";
			if(unavailable.find(map_path) != unavailable.end())
				continue;
			auto loaded_map = maps.find(map_path);
			if(loaded_map == maps.end())
			{
				WasmSourceMap source_map;
				if(!wasm_source_map_load(map_path, source_map) || source_map.module_name != frames[index].module)
				{
					unavailable.insert(map_path);
					continue;
				}
				loaded_map = maps.emplace(map_path, std::move(source_map)).first;
			}
			String location = wasm_source_map_lookup(loaded_map->second, frames[index].offset);
			if(location == "")
				continue;
			String label = frames[index].function == "" ? "wasm function" : frames[index].function;
			locations.push_back("#" + std::to_string(index) + " " + label + " at " + location);
		}
		if(!locations.empty())
			result += "\nsource locations:\n  " + join(locations, "\n  ");
		return(result);
	}

	// ---- guest memory access (pointer re-derived per call: it moves) ------

	String guest_write(u32 ptr, const String& data)
	{
		auto span = memory->data(ctx());
		if((size_t)ptr + data.size() > span.size())
			return("guest write out of bounds");
		memcpy(span.data() + ptr, data.data(), data.size());
		return("");
	}

	String guest_read(u32 ptr, u32 len, String& out)
	{
		auto span = memory->data(ctx());
		if((size_t)ptr + len > span.size())
			return("guest read out of bounds");
		out.assign((const char*)span.data() + ptr, len);
		return("");
	}

	// ---- calls -------------------------------------------------------------

	std::optional<wasmtime::Func> core_func(const String& name)
	{
		auto exported = core->get(ctx(), std::string_view(name));
		if(!exported)
			return(std::nullopt);
		if(auto* func = std::get_if<wasmtime::Func>(&*exported))
			return(*func);
		return(std::nullopt);
	}

	std::optional<wasmtime::Func> unit_func(size_t unit_index, const String& name)
	{
		auto exported = units[unit_index].instance->get(ctx(), std::string_view(name));
		if(!exported)
			return(std::nullopt);
		if(auto* func = std::get_if<wasmtime::Func>(&*exported))
			return(*func);
		return(std::nullopt);
	}

	wasmtime::TrapResult<std::vector<wasmtime::Val>> call_guest(wasmtime::Func& func, std::vector<wasmtime::Val> args)
	{
		auto context = ctx();
		arm_guest_deadline(context);
		return(func.call(context, args));
	}

	String call_core(const String& name, std::vector<int32_t> argv, int32_t* result_out)
	{
		auto func = core_func(name);
		if(!func)
			return("core does not export " + name);
		std::vector<wasmtime::Val> args;
		for(auto value : argv)
			args.push_back(wasmtime::Val(value));
		auto result = call_guest(*func, args);
		if(!result)
			return(trap_text(result.err()));
		auto values = result.ok();
		if(result_out)
			*result_out = values.empty() ? 0 : values[0].i32();
		return("");
	}

	// ---- symbol registry (core first, then units in load order) -----------

	std::optional<wasmtime::Func> resolve_func(const String& name)
	{
		if(auto func = core_func(name))
			return(func);
		for(size_t i = 0; i < units.size(); i++)
			if(auto func = unit_func(i, name))
				return(func);
		return(std::nullopt);
	}

	// data symbol: value of the exported i32 global + owning module's base
	// (core is non-PIC → base 0; PIC units export __memory_base-relative)
	bool resolve_data(const String& name, u32& address_out)
	{
		auto from_core = core->get(ctx(), std::string_view(name));
		if(from_core)
			if(auto* global = std::get_if<wasmtime::Global>(&*from_core))
			{
				address_out = (u32)global->get(ctx()).i32();
				return(true);
			}
		for(size_t i = 0; i < units.size(); i++)
		{
			auto exported = units[i].instance->get(ctx(), std::string_view(name));
			if(exported)
				if(auto* global = std::get_if<wasmtime::Global>(&*exported))
				{
					address_out = units[i].memory_base + (u32)global->get(ctx()).i32();
					return(true);
				}
		}
		return(false);
	}

	String place_funcref(const wasmtime::Func& func, u32& slot_out)
	{
		auto cx = ctx();
		if(table_next_free >= table->size(cx))
			return("funcref table headroom exhausted");
		auto set = table->set(cx, table_next_free, wasmtime::Val(std::optional<wasmtime::Func>(func)));
		if(!set)
			return("table set failed: " + String(set.err().message()));
		slot_out = table_next_free++;
		return("");
	}

	// ---- unit loading (the §6 sequence) ------------------------------------

	String load_unit(const String& source_path, String kind, size_t& unit_index_out)
	{
		auto known = units_by_source.find(source_path);
		if(known != units_by_source.end())
		{
			unit_index_out = known->second;
			return("");
		}

		unit_load_count++;
		auto materialize_start = std::chrono::steady_clock::now();
		String error;
		auto module_start = std::chrono::steady_clock::now();
		WasmUnitModuleLoadProfile module_profile;
		auto mod = worker.unit_module(source_path, error, module_profile, invocation_active, invocation_deadline);
		u64 module_total_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - module_start).count();
		unit_module_total_us += module_total_us;
		unit_module_cache_hit_count += module_profile.cache_hit ? 1 : 0;
		unit_module_cache_miss_count += module_profile.cache_hit ? 0 : 1;
		unit_module_serialized_cache_hit_count += module_profile.serialized_cache_hit ? 1 : 0;
		unit_module_compile_count += !module_profile.cache_hit && !module_profile.serialized_cache_hit ? 1 : 0;
		unit_module_lookup_total_us += module_profile.lookup_us;
		unit_module_read_total_us += module_profile.read_us;
		unit_module_read_bytes += module_profile.read_bytes;
		unit_module_read_count += module_profile.read_count;
		unit_module_parse_total_us += module_profile.parse_us;
		unit_module_compile_total_us += module_profile.compile_us;
		unit_module_classify_total_us += module_profile.classify_us;
		size_t module_operation_index = unit_module_operations.size();
		if(module_operation_index < 32)
		{
			String unit_name = source_path;
			String site_prefix = worker.cfg.site_root;
			if(site_prefix != "" && site_prefix.back() != '/')
				site_prefix += "/";
			if(site_prefix != "" && unit_name.rfind(site_prefix, 0) == 0)
				unit_name = unit_name.substr(site_prefix.size());
			else if(unit_name.rfind('/') != String::npos)
				unit_name = unit_name.substr(unit_name.rfind('/') + 1);
			WasmUnitModuleOperation operation;
			operation.unit = unit_name;
			operation.kind = kind;
			operation.source = !mod ? "error" : module_profile.cache_hit ? "worker" : module_profile.serialized_cache_hit ? "serialized" : "compiled";
			operation.total_us = module_total_us;
			operation.lookup_us = module_profile.lookup_us;
			operation.read_us = module_profile.read_us;
			operation.read_bytes = module_profile.read_bytes;
			operation.read_count = module_profile.read_count;
			operation.parse_us = module_profile.parse_us;
			operation.build_us = module_profile.compile_us;
			operation.classify_us = module_profile.classify_us;
			unit_module_operations.push_back(operation);
		}
		else
		{
			module_operation_index = 32;
			unit_module_operations_dropped++;
		}
		if(module_profile.timed_out || invocation_expired())
			return(invocation_timeout_error());
		if(!mod)
			return(error);
		// Compiling/deserializing a cold module is host work. Refresh the guest
		// watchdog before the first core call so that wall time cannot make the
		// otherwise harmless malloc/reloc sequence trap immediately.
		auto allocate_start = std::chrono::steady_clock::now();
		arm_guest_deadline(ctx());
		if(mod->abi.version != abi_version)
			return(mod->wasm_path + ": bearer.abi version " + std::to_string(mod->abi.version)
				+ " does not match core ABI " + std::to_string(abi_version));

		auto cx = ctx();
		auto& module = *mod->module;

		// base allocation
		u32 align = 1u << mod->dylink.mem_align;
		int32_t raw_base = 0;
		error = call_core("malloc", { (int32_t)(mod->dylink.mem_size + align) }, &raw_base);
		if(error != "")
			return(error);
		if(raw_base == 0)
			return("core malloc failed for unit data segment");
		u32 memory_base = ((u32)raw_base + (align - 1)) & ~(align - 1);
		u32 table_base = table_next_free;
		if(mod->dylink.table_size > table->size(cx) - table_next_free)
			return("funcref table headroom exhausted by " + source_path);
		table_next_free += mod->dylink.table_size;
		u64 allocate_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - allocate_start).count();
		unit_allocate_total_us += allocate_us;
		if(module_operation_index < unit_module_operations.size())
			unit_module_operations[module_operation_index].allocate_us = allocate_us;

		// import resolution
		auto import_start = std::chrono::steady_clock::now();
		u64 symbol_resolve_count_start = unit_symbol_resolve_count;
		u64 symbol_resolve_us_start = unit_symbol_resolve_total_us;
		std::vector<wasmtime::Extern> imports;
		imports.reserve(mod->imports.size());
		std::vector<std::pair<String, wasmtime::Global>> deferred_got;
		deferred_got.reserve(mod->got_memory_imports);
		wasmtime::GlobalType base_global_type(wasmtime::ValType::i32(), false);
		wasmtime::GlobalType got_global_type(wasmtime::ValType::i32(), true);
		for(const auto& import : mod->imports)
		{
			const String& name = import.name;
			if(import.kind == WasmUnitImportKind::Memory)
			{
				imports.push_back(*memory);
				continue;
			}
			if(import.kind == WasmUnitImportKind::Table)
			{
				imports.push_back(*table);
				continue;
			}
			if(import.kind == WasmUnitImportKind::StackPointer)
			{
				auto sp = core->get(cx, "__stack_pointer");
				if(!sp || !std::get_if<wasmtime::Global>(&*sp))
					return("core does not export __stack_pointer");
				imports.push_back(std::get<wasmtime::Global>(*sp));
				continue;
			}
			if(import.kind == WasmUnitImportKind::MemoryBase || import.kind == WasmUnitImportKind::TableBase)
			{
				int32_t value = import.kind == WasmUnitImportKind::MemoryBase ? (int32_t)memory_base : (int32_t)table_base;
				auto global = wasmtime::Global::create(cx, base_global_type, wasmtime::Val(value));
				if(!global)
					return("global create failed: " + String(global.err().message()));
				imports.push_back(global.ok());
				continue;
			}
			if(import.kind == WasmUnitImportKind::Function)
			{
				auto resolve_start = std::chrono::steady_clock::now();
				auto func = resolve_func(name);
				unit_symbol_resolve_count++;
				unit_symbol_resolve_total_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - resolve_start).count();
				if(!func)
					return(source_path + ": unresolved import env." + wasm_trace_demangle(name));
				imports.push_back(*func);
				continue;
			}
			if(import.kind == WasmUnitImportKind::GotMemory)
			{
				u32 address = 0;
				auto resolve_start = std::chrono::steady_clock::now();
				bool resolved = resolve_data(name, address);
				unit_symbol_resolve_count++;
				unit_symbol_resolve_total_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - resolve_start).count();
				if(resolved)
				{
					auto global = wasmtime::Global::create(cx, got_global_type, wasmtime::Val((int32_t)address));
					if(!global)
						return("global create failed: " + String(global.err().message()));
					imports.push_back(global.ok());
				}
				else
				{
					// provisional; self-resolved from the unit's own export
					// (plus its memory base) after instantiation
					auto global = wasmtime::Global::create(cx, got_global_type, wasmtime::Val((int32_t)0));
					if(!global)
						return("global create failed: " + String(global.err().message()));
					deferred_got.push_back({ name, global.ok() });
					imports.push_back(deferred_got.back().second);
				}
				continue;
			}
			if(import.kind == WasmUnitImportKind::GotFunction)
			{
				auto resolve_start = std::chrono::steady_clock::now();
				auto func = resolve_func(name);
				unit_symbol_resolve_count++;
				unit_symbol_resolve_total_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - resolve_start).count();
				if(!func)
					return(source_path + ": unresolved GOT.func." + wasm_trace_demangle(name));
				u32 slot = 0;
				error = place_funcref(*func, slot);
				if(error != "")
					return(error);
				auto global = wasmtime::Global::create(cx, got_global_type, wasmtime::Val((int32_t)slot));
				if(!global)
					return("global create failed: " + String(global.err().message()));
				imports.push_back(global.ok());
				continue;
			}
		}
		u64 import_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - import_start).count();
		unit_import_total_us += import_us;
		if(module_operation_index < unit_module_operations.size())
		{
			auto& operation = unit_module_operations[module_operation_index];
			operation.import_us = import_us;
			operation.symbol_resolve_count = unit_symbol_resolve_count - symbol_resolve_count_start;
			operation.symbol_resolve_us = unit_symbol_resolve_total_us - symbol_resolve_us_start;
		}

		auto instantiate_start = std::chrono::steady_clock::now();
		auto created = wasmtime::Instance::create(cx, module, imports);
		u64 instantiate_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - instantiate_start).count();
		unit_instantiate_total_us += instantiate_us;
		if(module_operation_index < unit_module_operations.size())
			unit_module_operations[module_operation_index].instantiate_us = instantiate_us;
		if(!created)
			return(source_path + ": instantiation failed: " + trap_text(created.err()));

		LoadedUnit unit;
		unit.mod = mod;
		unit.instance.emplace(created.ok());
		unit.memory_base = memory_base;
		units.push_back(std::move(unit));
		size_t unit_index = units.size() - 1;
		units_by_source[source_path] = unit_index;
		unit_index_out = unit_index;

		// deferred GOT: the unit's own data exports are module-relative —
		auto initialize_start = std::chrono::steady_clock::now();
		// add this unit's memory base (Phase 0 FINDINGS erratum)
		for(auto& [name, got] : deferred_got)
		{
			auto own = units[unit_index].instance->get(cx, std::string_view(name));
			if(!own || !std::get_if<wasmtime::Global>(&*own))
				return(source_path + ": GOT.mem." + name + " defined neither by core nor by any unit");
			u32 offset = (u32)std::get<wasmtime::Global>(*own).get(cx).i32();
			auto set = got.set(cx, wasmtime::Val((int32_t)(memory_base + offset)));
			if(!set)
				return("GOT patch failed: " + String(set.err().message()));
		}

		// init sequence, then bind this unit's context to the request
		if(auto relocs = unit_func(unit_index, "__wasm_apply_data_relocs"))
		{
			auto result = call_guest(*relocs, {});
			if(!result)
				return(trap_text(result.err()));
		}
		if(auto ctors = unit_func(unit_index, "__wasm_call_ctors"))
		{
			auto result = call_guest(*ctors, {});
			if(!result)
				return(trap_text(result.err()));
		}
		if(auto set_request = unit_func(unit_index, "__bearer_set_current_request"))
		{
			auto result = call_guest(*set_request, { wasmtime::Val(request_ptr) });
			if(!result)
				return(trap_text(result.err()));
		}
		if(worker.cfg.verbose)
			fprintf(stderr, "[wasm] loaded %s (mem_base=%u table_base=%u)\n",
				source_path.c_str(), memory_base, table_base);
		u64 initialize_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - initialize_start).count();
		unit_initialize_total_us += initialize_us;
		if(module_operation_index < unit_module_operations.size())
			unit_module_operations[module_operation_index].initialize_us = initialize_us;
		u64 materialize_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - materialize_start).count();
		if(kind == "entry")
		{
			entry_unit_load_count++;
			entry_unit_materialize_total_us += materialize_us;
		}
		else if(kind == "component")
		{
			dynamic_include_load_count++;
			dynamic_include_materialize_total_us += materialize_us;
		}
		if(module_operation_index < unit_module_operations.size())
			unit_module_operations[module_operation_index].materialize_us = materialize_us;
		// Exclude the rest of host-side loading as well. A genuine runaway loop
		// makes no loads, so it still trips the deadline.
		arm_guest_deadline(ctx());
		return("");
	}

	// ---- component target resolution (host side of the membrane) ----------

	static String normalize_component_path(String name)
	{
		// mirrors component_normalize_path in compiler.cpp / core.cpp
		if((name.length() >= 4 && name.substr(name.length() - 4) == ".uce") ||
			(name.length() >= 5 && name.substr(name.length() - 5) == ".capy"))
			return(name);
		return(name + ".uce");
	}

	static bool file_exists_host(const String& path)
	{
		struct stat st;
		return(stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode));
	}

	static bool dir_exists_host(const String& path)
	{
		struct stat st;
		return(stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
	}

	// Keep cwd host behavior local to this process but guard it with the same
	// write-root policy we use for file writes (plus a single parity fallback).
	String resolve_guest_cwd_set(const String& raw)
	{
		if(raw == "" || raw.find('\0') != String::npos)
			return("");

		String raw_target = raw;
		if(raw.rfind("/", 0) != 0)
		{
			String cwd = ::cwd_get();
			if(cwd == "")
				return("");
			raw_target = cwd + "/" + raw;
		}

		char resolved[PATH_MAX];
		if(!realpath(raw_target.c_str(), resolved))
			return("");
		String resolved_target(resolved);
		if(!dir_exists_host(resolved_target))
			return("");

		// Policy: allow only roots we already expose for writable filesystem access.
		std::vector<String> roots;
		roots.push_back(worker.cfg.site_root);
		for(auto& root : worker.cfg.write_roots)
			roots.push_back(root);
		for(auto& root : roots)
		{
			if(root == "")
				continue;
			char root_real[PATH_MAX];
			if(!realpath(root.c_str(), root_real))
				continue;
			String canonical_root(root_real);
			if(resolved_target == canonical_root)
				return(resolved_target);
			if(canonical_root != "/" && resolved_target.rfind(canonical_root + "/", 0) == 0)
				return(resolved_target);
		}

		// Parity/fallback: allow returning to the process start directory so
		// legacy behavior is not silently broken for existing native/cached flows.
		String start_directory = ::process_start_directory();
		if(start_directory != "" && resolved_target == start_directory)
			return(resolved_target);
		return("");
	}

	String resolve_source_path(const String& file_name, const String& current_unit)
	{
		std::vector<String> bases;
		if(file_name.rfind("/", 0) == 0)
			bases.push_back("");           // absolute target
		if(entry_dir != "")
			bases.push_back(entry_dir + "/");
		if(current_unit != "")
		{
			String current_dir = dir_of(current_unit);
			if(current_dir != "" && current_dir != entry_dir)
				bases.push_back(current_dir + "/");
		}
		String site_base = worker.cfg.site_root + "/";
		if(std::find(bases.begin(), bases.end(), site_base) == bases.end())
			bases.push_back(site_base);
		String normalized_file_name = normalize_component_path(file_name);

		for(auto& base : bases)
		{
			std::vector<String> candidates;
			candidates.push_back(base + file_name);
			if(normalized_file_name != file_name)
				candidates.push_back(base + normalized_file_name);
			if(file_name.rfind("components/", 0) != 0 && base != "")
			{
				candidates.push_back(base + "components/" + file_name);
				if(normalized_file_name != file_name)
					candidates.push_back(base + "components/" + normalized_file_name);
			}
			for(auto& candidate : candidates)
			{
				if(!file_exists_host(candidate))
					continue;
				char resolved[PATH_MAX];
				if(realpath(candidate.c_str(), resolved))
					return(String(resolved));
			}
		}
		return("");
	}

	// guest file access policy: only inside the site tree, resolved against
	// the entry unit's directory first (the native cwd convention), then the
	// site root; containment checked on the canonicalized path
	String resolve_guest_file(const String& raw, const String& current_unit = "", bool allow_dir = false)
	{
		if(raw == "" || raw.find('\0') != String::npos)
			return("");
		std::vector<String> candidates;
		if(raw.rfind("/", 0) == 0)
			candidates.push_back(raw);
		else
		{
			String current_dir = current_unit != "" ? dir_of(current_unit) : String("");
			if(current_dir != "")
				candidates.push_back(current_dir + "/" + raw);
			if(entry_dir != "" && entry_dir != current_dir)
				candidates.push_back(entry_dir + "/" + raw);
			candidates.push_back(worker.cfg.site_root + "/" + raw);
		}
		// readable roots = the site tree plus the writable scratch dirs (a page
		// can read back what it is allowed to write, e.g. /tmp), canonicalized.
		std::vector<String> read_roots;
		read_roots.push_back(worker.cfg.site_root);
		for(auto& root : worker.cfg.write_roots)
			read_roots.push_back(root);
		std::vector<String> root_prefixes;
		for(auto& root : read_roots)
		{
			char root_real[4096];
			if(root != "" && realpath(root.c_str(), root_real))
				root_prefixes.push_back(String(root_real));
		}
		for(auto& candidate : candidates)
		{
			char resolved[4096];
			if(!realpath(candidate.c_str(), resolved))
				continue;
			String path(resolved);
			bool allowed = false;
			for(auto& prefix : root_prefixes)
				if(path == prefix || path.rfind(prefix + "/", 0) == 0)
				{
					allowed = true;
					break;
				}
			if(!allowed)
				continue;
			if(file_exists_host(path) || (allow_dir && dir_exists_host(path)))
				return(path);
		}
		return("");
	}

	// write membrane policy: resolve the target (absolute, or relative to the
	// current unit / site root) and allow it only if its parent directory
	// canonicalizes under one of the configured write roots (site tree + the
	// runtime scratch dirs). The file itself need not exist yet.
	String resolve_guest_write(const String& raw, const String& current_unit)
	{
		if(raw == "" || raw.find('\0') != String::npos)
			return("");
		String target;
		if(raw.rfind("/", 0) == 0)
			target = raw;
		else
		{
			String current_dir = current_unit != "" ? dir_of(current_unit) : String("");
			target = (current_dir != "" ? current_dir : worker.cfg.site_root) + "/" + raw;
		}
		String parent = dir_of(target);
		String base = parent.size() < target.size() ? target.substr(parent.size() + 1) : String("");
		if(base == "" || base == "." || base == "..")
			return("");
		char parent_real[4096];
		if(!realpath(parent.c_str(), parent_real))
			return("");
		String resolved_parent(parent_real);
		for(auto& root : worker.cfg.write_roots)
		{
			char root_real[4096];
			if(root == "" || !realpath(root.c_str(), root_real))
				continue;
			String root_prefix(root_real);
			if(resolved_parent == root_prefix || resolved_parent.rfind(root_prefix + "/", 0) == 0)
				return(resolved_parent + "/" + base);
		}
		return("");
	}

	static String sanitize_symbol_suffix(const String& raw)
	{
		// mirrors ascii_safe_name in functionlib.cpp
		String result;
		for(auto c : raw)
			if(isalnum((unsigned char)c) || c == '_')
				result.push_back(c);
		return(result);
	}

	// __bearer_<base>[_<suffix>] for a handler spec like "component:CARD" / "render".
	static String handler_export_symbol(const String& handler)
	{
		String export_prefix = "export:";
		if(handler.rfind(export_prefix, 0) == 0)
			return(handler.substr(export_prefix.size()));
		auto colon = handler.find(":");
		String symbol = "__bearer_" + (colon == String::npos ? handler : handler.substr(0, colon));
		if(colon != String::npos)
			symbol += "_" + sanitize_symbol_suffix(handler.substr(colon + 1));
		return(symbol);
	}

	// hostcall body: bearer_host_component_resolve(unit, handler, current) → slot.
	// `handler` names the export ("render", "component:CARD", "cli",
	// "serve_http:named", "once") or is "exists" (probe only, loads nothing).
	int32_t component_resolve(const String& target, const String& handler, const String& current_unit,
		String& resolved_out, int32_t* once_slot_out = 0, bool* compile_timed_out = 0,
		String* compile_error_out = 0)
	{
		if(once_slot_out)
			*once_slot_out = 0;
		if(compile_timed_out)
			*compile_timed_out = false;
		if(compile_error_out)
			compile_error_out->clear();
		auto probe_start = std::chrono::steady_clock::now();
		auto record_probe = [&]() {
			component_resolve_count += 1;
			component_resolve_total_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - probe_start).count();
		};
		String file_name = target;
		if(file_name == "" && current_unit != "")
			file_name = current_unit;
		if(file_name == "")
		{
			component_path_total_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - probe_start).count();
			record_probe();
			return(0);
		}

		String resolve_key = current_unit + "\t" + file_name;
		String resolved;
		String error;
		size_t unit_index = 0;
		bool loaded_reuse = false;
		String method = context ? to_upper(trim(context->params["REQUEST_METHOD"])) : String("");
		bool cacheable_read = method == "GET" || method == "HEAD";
		bool read_request = cacheable_read || method == "OPTIONS";
		auto loaded = units_by_source.find(file_name);
		if(loaded == units_by_source.end())
		{
			auto known_path = component_loaded_paths.find(resolve_key);
			if(known_path != component_loaded_paths.end())
				loaded = units_by_source.find(known_path->second);
		}
		if(loaded != units_by_source.end())
		{
			resolved = loaded->first;
			unit_index = loaded->second;
			loaded_reuse = true;
			component_loaded_reuse_count++;
		}
		else
		{
			String cache_key = entry_dir + "\t" + resolve_key;
			auto cached = worker.component_resolutions.find(cache_key);
			auto now = std::chrono::steady_clock::now();
			if(cacheable_read && cached != worker.component_resolutions.end() &&
				std::chrono::duration_cast<std::chrono::seconds>(now - cached->second.checked_at).count() < 10)
				resolved = cached->second.resolved;
			else
			{
				resolved = resolve_source_path(file_name, current_unit);
				if(cacheable_read && resolved != "")
				{
					if(cached == worker.component_resolutions.end() && worker.component_resolutions.size() >= 4096)
					{
						auto oldest = worker.component_resolutions.begin();
						for(auto it = worker.component_resolutions.begin(); it != worker.component_resolutions.end(); ++it)
							if(it->second.checked_at < oldest->second.checked_at)
								oldest = it;
						worker.component_resolutions.erase(oldest);
					}
					worker.component_resolutions[cache_key] = { now, resolved };
				}
			}
		}
		component_path_total_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - probe_start).count();
		if(resolved == "")
		{
			record_probe();
			return(0);
		}
		resolved_out = resolved;
		if(handler == "exists")
		{
			record_probe();
			return(1);
		}

		if(!loaded_reuse)
		{
			auto artifact_start = std::chrono::steady_clock::now();
			bool artifact_exists = file_exists_host(worker.unit_wasm_path(resolved));
			bool stale_policy = compiler_request_can_serve_stale_artifact(context);
			if(stale_policy && read_request && !component_source_generation_checked)
			{
				component_source_generation = compiler_source_generation(context);
				component_source_generation_checked = true;
			}
			bool stale = false;
			if(artifact_exists)
			{
				auto now = std::chrono::steady_clock::now();
				auto cached_freshness = worker.component_freshness.find(resolved);
				// HTTP reads may serve a complete stale artifact, so bound their graph
				// stat work; CLI and mutations always check the current graph.
				bool generation_available = component_source_generation != "";
				bool check_freshness = !stale_policy || !read_request ||
					cached_freshness == worker.component_freshness.end() ||
					(generation_available ?
						cached_freshness->second.source_generation != component_source_generation :
						std::chrono::duration_cast<std::chrono::milliseconds>(
							now - cached_freshness->second.checked_at).count() >= 1000);
				if(check_freshness)
				{
					stale = compiler_unit_needs_recompile(context, resolved, 0, stale_policy && read_request, true);
					worker.component_freshness[resolved] = { now, stale, component_source_generation };
				}
				else
					stale = cached_freshness->second.stale;
			}
			bool can_serve_stale = stale && compiler_unit_can_serve_stale_artifact(context, resolved);
			if(stale && stale_policy)
			{
				compiler_prioritize_unit(context, resolved);
				if(!read_request)
				{
					stale_component_mutation_blocked = true;
					stale_component_mutation_status = context->params["GATEWAY_INTERFACE"] != "" ?
						"Status: 503 Service Unavailable" : "HTTP/1.1 503 Service Unavailable";
					component_artifact_total_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - artifact_start).count();
					record_probe();
					return(0);
				}
			}
			if(!artifact_exists || (stale && !can_serve_stale))
			{
				u64 remaining_ms = invocation_remaining_ms();
				bool timed_out = false;
				SharedUnit* compiled_unit = 0;
				if(remaining_ms == 0)
					timed_out = true;
				else
					compiled_unit = get_shared_unit_bounded(context, resolved, remaining_ms, &timed_out);
				if(compiled_unit && compile_error_out &&
					to_bool(context->server->config["SHOW_DYNAMIC_COMPILE_ERRORS"], true))
				{
					String detail = trim(first(compiled_unit->compiler_messages, compiled_unit->compile_error_status));
					if(detail != "")
					{
						*compile_error_out = "compile failed for " + resolved + ":\n" + detail;
						if(compile_error_out->size() > 4095)
							*compile_error_out = compile_error_out->substr(0, 4064) + "\n[compile error truncated]";
					}
				}
				if(timed_out)
				{
					if(compile_timed_out)
						*compile_timed_out = true;
					record_probe();
					return(0);
				}
			}
			component_artifact_total_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - artifact_start).count();

			auto load_start = std::chrono::steady_clock::now();
			error = load_unit(resolved, "component", unit_index);
			component_load_total_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - load_start).count();
			if(error != "")
			{
				fprintf(stderr, "[wasm] component load failed: %s\n", error.c_str());
				record_probe();
				return(0);
			}
			component_loaded_paths[resolve_key] = resolved;
		}
		auto link_start = std::chrono::steady_clock::now();
		auto record_link = [&]() {
			component_link_total_us += (u64)std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - link_start).count();
		};
		auto link_handler = [&](const String& requested_handler) -> int32_t {
			String symbol = handler_export_symbol(requested_handler);
			String slot_key = resolved + ":" + symbol;
			auto cached = handler_slots.find(slot_key);
			if(cached != handler_slots.end())
				return((int32_t)cached->second);
			auto handler_fn = unit_func(unit_index, symbol);
			if(!handler_fn)
			{
				// ONCE is optional per unit; a missing __bearer_once is not an error.
				if(requested_handler != "once")
					fprintf(stderr, "[wasm] %s does not export %s\n", resolved.c_str(), symbol.c_str());
				return(0);
			}
			u32 slot = 0;
			error = place_funcref(*handler_fn, slot);
			if(error != "")
			{
				fprintf(stderr, "[wasm] %s\n", error.c_str());
				return(0);
			}
			handler_slots[slot_key] = slot;
			return((int32_t)slot);
		};
		int32_t slot = link_handler(handler);
		bool runs_once = handler == "render" || handler.rfind("render:", 0) == 0 ||
			handler == "component" || handler.rfind("component:", 0) == 0;
		if(slot && once_slot_out && runs_once)
			*once_slot_out = link_handler("once");
		record_link();
		record_probe();
		return(slot);
	}

	String run_task_callback(u64 callback_id, u64 timeout_cap_ms)
	{
		InvocationScope invocation(*this, timeout_cap_ms, true);
		auto runner = core_func("bearer_wasm_task_run");
		if(!runner)
			return("core does not export bearer_wasm_task_run");
		auto result = call_guest(*runner, { wasmtime::Val((int64_t)callback_id) });
		if(!result)
			return(trap_text(result.err()));
		return("");
	}

	// ---- host imports for the core -----------------------------------------

	String make_host_import(wasmtime::Linker& linker, const String& mod, const String& name, const wasmtime::FuncType& func_type)
	{
		using namespace wasmtime;
		struct ActiveWorkspace
		{
			WasmWorker* worker;
			WasmWorkspace* operator->() const { return(worker->active_workspace); }
		};
		ActiveWorkspace self = { &worker };

		auto add = [&](auto&& callback) -> String {
			u64 profile_index = worker.core_host_import_count++;
			if(profile_index < WasmRequestProfile::HOSTCALL_OPERATION_MAX)
			{
				if(worker.hostcall_operation_names.size() <= profile_index)
					worker.hostcall_operation_names.push_back(name);
			}
			bool profile_enabled = name != "bearer_host_request_perf";
			bool profile_mysql = name == "bearer_host_mysql";
			bool profile_memcache = name == "bearer_host_memcache_command";
			auto profiled = [self, callback, profile_index, profile_enabled, profile_mysql, profile_memcache](Caller caller, Span<const Val> args, Span<Val> results) mutable -> Result<std::monostate, Trap> {
				auto started = std::chrono::steady_clock::now();
				if(self->invocation_expired(started))
					return(Trap(self->invocation_timeout_error()));
				f64 cpu_started = profile_enabled && self->worker.cfg.profile_hostcall_cpu ? wasm_thread_cpu_time() : 0;
				auto result = callback(caller, args, results);
				auto finished = std::chrono::steady_clock::now();
				self->arm_guest_deadline(caller.context());
				f64 cpu_finished = profile_enabled && self->worker.cfg.profile_hostcall_cpu ? wasm_thread_cpu_time() : 0;
				if(profile_enabled)
				{
					u64 elapsed = (u64)std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
					u64 cpu_elapsed = cpu_started > 0 && cpu_finished > cpu_started ?
						(u64)((cpu_finished - cpu_started) * 1000000.0) : 0;
					self->hostcall_count++;
					self->hostcall_total_us += elapsed;
					self->hostcall_cpu_total_us += cpu_elapsed;
					if(profile_index < WasmRequestProfile::HOSTCALL_OPERATION_MAX)
					{
						self->hostcall_operation_counts[profile_index]++;
						self->hostcall_operation_us[profile_index] += elapsed;
						self->hostcall_operation_cpu_us[profile_index] += cpu_elapsed;
					}
					if(profile_mysql)
					{
						self->mysql_hostcall_count++;
						self->mysql_hostcall_total_us += elapsed;
					}
					else if(profile_memcache)
					{
						self->memcache_hostcall_count++;
						self->memcache_hostcall_total_us += elapsed;
					}
				}
				if(self->invocation_expired(finished))
					return(Trap(self->invocation_timeout_error()));
				return(result);
			};
			auto defined = linker.func_new(mod, name, func_type, profiled);
			if(!defined)
				return("core host import failed: " + mod + "." + name + ": " + String(defined.err().message()));
			return("");
		};

		// Hostcall blocklist (BEARER_HOSTCALL_BLOCKLIST): a sysadmin-disabled hostcall
		// resolves to a trap stub instead of its real implementation, so a unit
		// invoking it fails at runtime into the configurable error page. The
		// decision is made once per worker when imports are defined — no per-call cost,
		// and zero cost when nothing is blocked. A small core set stays exempt so
		// the runtime itself cannot be bricked.
		if(mod == "env" && !worker.cfg.hostcall_blocklist.empty() && name.rfind("bearer_host_", 0) == 0)
		{
			static const std::set<String> non_blockable = { "component_resolve" };
			String bare = name.substr(9);
			if(worker.cfg.hostcall_blocklist.count(bare) && !non_blockable.count(bare))
			{
				std::string blocked(name);
				return(add([blocked](Caller, Span<const Val>, Span<Val>) -> Result<std::monostate, Trap> {
					return(Trap("BEARER_POLICY_BLOCKED:" + blocked));
				}));
			}
		}

		if(mod == "env" && name == "bearer_host_time")
			return(add([](Caller, Span<const Val>, Span<Val> results) -> Result<std::monostate, Trap> {
				results[0] = Val((int64_t)::time(0));
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_time_precise")
			return(add([](Caller, Span<const Val>, Span<Val> results) -> Result<std::monostate, Trap> {
				results[0] = Val(time_precise());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_request_perf")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String encoded;
				if(!self->hostcall_staged("request_perf", encoded))
				{
					DValue response;
					if(self->request_perf.active)
					{
						f64 cpu_now = wasm_thread_cpu_time();
						f64 now = time_precise();
						response["worker_pid"] = (f64)self->request_perf.worker_pid;
						response["parent_pid"] = (f64)self->request_perf.parent_pid;
						response["request_count"] = (f64)self->request_perf.request_count;
						response["ready_normalize_us"] = (f64)self->request_perf.ready_normalize_us;
						response["ready_mutation_check_us"] = (f64)self->request_perf.ready_mutation_check_us;
						response["ready_artifact_stat_us"] = (f64)self->request_perf.ready_artifact_stat_us;
						response["ready_freshness_us"] = (f64)self->request_perf.ready_freshness_us;
						response["ready_source_generation_us"] = (f64)self->request_perf.ready_source_generation_us;
						response["ready_freshness_full_check_us"] = (f64)self->request_perf.ready_freshness_full_check_us;
						response["ready_worker_us"] = (f64)self->request_perf.ready_worker_us;
						response["ready_check_count"] = (f64)self->request_perf.ready_check_count;
						response["ready_freshness_cache_hit_count"] = (f64)self->request_perf.ready_freshness_cache_hit_count;
						if(self->request_perf.time_start > 0 && self->request_perf.time_init > 0)
							response["accept_us"] = (f64)((self->request_perf.time_start - self->request_perf.time_init) * 1000000.0);
						if(self->request_perf.time_params > 0 && self->request_perf.time_init > 0)
							response["transport_params_us"] = (f64)((self->request_perf.time_params - self->request_perf.time_init) * 1000000.0);
						if(self->request_perf.time_input > 0 && self->request_perf.time_params > 0)
							response["transport_input_us"] = (f64)((self->request_perf.time_input - self->request_perf.time_params) * 1000000.0);
						if(self->request_perf.time_start > 0 && self->request_perf.time_input > 0)
							response["handler_queue_us"] = (f64)((self->request_perf.time_start - self->request_perf.time_input) * 1000000.0);
						if(self->request_perf.time_start > 0)
							response["running_us"] = (f64)((now - self->request_perf.time_start) * 1000000.0);
						if(self->request_perf.time_init > 0)
							response["total_us"] = (f64)((now - self->request_perf.time_init) * 1000000.0);
						u64 workspace_wall_us = self->request_perf.workspace_wall_start > 0 ? (u64)((now - self->request_perf.workspace_wall_start) * 1000000.0) : 0;
						u64 workspace_cpu_us = self->request_perf.workspace_cpu_start > 0 && cpu_now > 0 ? (u64)((cpu_now - self->request_perf.workspace_cpu_start) * 1000000.0) : 0;
						response["workspace_wall_us"] = (f64)workspace_wall_us;
						response["workspace_cpu_us"] = (f64)workspace_cpu_us;
						response["workspace_wait_us"] = (f64)(workspace_wall_us > workspace_cpu_us ? workspace_wall_us - workspace_cpu_us : 0);
						response["dispatch_us"] = (f64)self->dispatch_us;
						response["workspace_setup_us"] = (f64)self->workspace_setup_us;
						response["workspace_setup_cpu_us"] = (f64)self->workspace_setup_cpu_us;
						response["workspace_birth_us"] = (f64)self->workspace_birth_us;
						response["workspace_birth_cpu_us"] = (f64)self->workspace_birth_cpu_us;
						response["birth_policy_us"] = (f64)self->birth_policy_us;
						response["birth_import_us"] = (f64)self->birth_import_us;
						response["birth_instantiate_us"] = (f64)self->birth_instantiate_us;
						response["birth_exports_us"] = (f64)self->birth_exports_us;
						response["birth_initialize_us"] = (f64)self->birth_initialize_us;
						response["context_apply_us"] = (f64)self->context_apply_us;
						response["context_apply_cpu_us"] = (f64)self->context_apply_cpu_us;
						response["context_bytes"] = (f64)self->context_bytes;
						response["server_config_bytes"] = (f64)self->server_config_bytes;
						response["context_encode_us"] = (f64)self->context_encode_us;
						response["context_allocate_us"] = (f64)self->context_allocate_us;
						response["context_write_us"] = (f64)self->context_write_us;
						response["context_guest_apply_us"] = (f64)self->context_guest_apply_us;
						response["context_free_us"] = (f64)self->context_free_us;
						u64 phase_cpu_us = self->workspace_setup_cpu_us + self->workspace_birth_cpu_us + self->context_apply_cpu_us;
						response["execution_cpu_us"] = (f64)(workspace_cpu_us > phase_cpu_us ? workspace_cpu_us - phase_cpu_us : 0);
						response["hostcall_count"] = (f64)self->hostcall_count;
						response["hostcall_us"] = (f64)self->hostcall_total_us;
						response["hostcall_cpu_profiled"].set_bool(self->worker.cfg.profile_hostcall_cpu);
						response["hostcall_cpu_us"] = (f64)self->hostcall_cpu_total_us;
						struct rusage runtime_now = {};
						bool thread_runtime_profiled = self->request_perf.thread_runtime_profiled && getrusage(RUSAGE_THREAD, &runtime_now) == 0;
						response["thread_runtime_profiled"].set_bool(thread_runtime_profiled);
						if(thread_runtime_profiled)
						{
							auto timeval_us = [](const struct timeval& value) -> int64_t {
								return((int64_t)value.tv_sec * 1000000 + value.tv_usec);
							};
							int cpu_now = sched_getcpu();
							response["thread_user_cpu_us"] = (f64)std::max<int64_t>(0, timeval_us(runtime_now.ru_utime) - timeval_us(self->request_perf.thread_runtime_start.ru_utime));
							response["thread_system_cpu_us"] = (f64)std::max<int64_t>(0, timeval_us(runtime_now.ru_stime) - timeval_us(self->request_perf.thread_runtime_start.ru_stime));
							response["thread_voluntary_context_switches"] = (f64)std::max<int64_t>(0, runtime_now.ru_nvcsw - self->request_perf.thread_runtime_start.ru_nvcsw);
							response["thread_involuntary_context_switches"] = (f64)std::max<int64_t>(0, runtime_now.ru_nivcsw - self->request_perf.thread_runtime_start.ru_nivcsw);
							response["thread_minor_faults"] = (f64)std::max<int64_t>(0, runtime_now.ru_minflt - self->request_perf.thread_runtime_start.ru_minflt);
							response["thread_major_faults"] = (f64)std::max<int64_t>(0, runtime_now.ru_majflt - self->request_perf.thread_runtime_start.ru_majflt);
							response["thread_cpu_start"] = (f64)self->request_perf.thread_cpu_start;
							response["thread_cpu_end"] = (f64)cpu_now;
							response["thread_cpu_migrated"].set_bool(self->request_perf.thread_cpu_start >= 0 && cpu_now >= 0 && self->request_perf.thread_cpu_start != cpu_now);
						}
						std::vector<u64> invoked_hostcalls;
						for(u64 i = 0; i < self->hostcall_operation_slots; i++)
							if(self->hostcall_operation_counts[i] > 0)
								invoked_hostcalls.push_back(i);
						std::sort(invoked_hostcalls.begin(), invoked_hostcalls.end(), [&](u64 a, u64 b) {
							if(self->hostcall_operation_us[a] != self->hostcall_operation_us[b])
								return(self->hostcall_operation_us[a] > self->hostcall_operation_us[b]);
							return(self->worker.hostcall_operation_names[a] < self->worker.hostcall_operation_names[b]);
						});
						response["hostcall_operations_dropped"] = (f64)(invoked_hostcalls.size() > 32 ? invoked_hostcalls.size() - 32 : 0);
						response["hostcall_operations"].set_array();
						for(u64 i = 0; i < invoked_hostcalls.size() && i < 32; i++)
						{
							u64 operation_index = invoked_hostcalls[i];
							String& operation_name = self->worker.hostcall_operation_names[operation_index];
							DValue item;
							item["name"] = operation_name.rfind("bearer_host_", 0) == 0 ? operation_name.substr(9) : operation_name;
							item["count"] = (f64)self->hostcall_operation_counts[operation_index];
							item["us"] = (f64)self->hostcall_operation_us[operation_index];
							item["cpu_us"] = (f64)self->hostcall_operation_cpu_us[operation_index];
							response["hostcall_operations"].push(item);
						}
						response["mysql_hostcall_count"] = (f64)self->mysql_hostcall_count;
						response["mysql_hostcall_us"] = (f64)self->mysql_hostcall_total_us;
						response["mysql_operation_count"] = (f64)self->mysql_operation_count;
						response["mysql_operations_dropped"] = (f64)self->mysql_operations_dropped;
						response["mysql_connection_open_count"] = (f64)self->mysql_connection_open_count;
						response["mysql_connection_reuse_count"] = (f64)self->mysql_connection_reuse_count;
						response["mysql_request_pool_hit_count"] = (f64)self->mysql_request_pool_hit_count;
						response["mysql_operations"].set_array();
						for(auto& operation : self->mysql_operations)
						{
							DValue item;
							item["op"] = operation.op;
							if(operation.source != "")
								item["source"] = operation.source;
							item["us"] = (f64)operation.elapsed_us;
							response["mysql_operations"].push(item);
						}
						response["memcache_hostcall_count"] = (f64)self->memcache_hostcall_count;
						response["memcache_hostcall_us"] = (f64)self->memcache_hostcall_total_us;
						response["component_resolve_count"] = (f64)self->component_resolve_count;
						response["component_loaded_reuse_count"] = (f64)self->component_loaded_reuse_count;
						response["component_resolve_us"] = (f64)self->component_resolve_total_us;
						response["component_path_us"] = (f64)self->component_path_total_us;
						response["component_artifact_us"] = (f64)self->component_artifact_total_us;
						response["component_load_us"] = (f64)self->component_load_total_us;
						response["component_link_us"] = (f64)self->component_link_total_us;
						response["unit_load_count"] = (f64)self->unit_load_count;
						response["entry_unit_load_count"] = (f64)self->entry_unit_load_count;
						response["entry_unit_materialize_us"] = (f64)self->entry_unit_materialize_total_us;
						response["dynamic_include_load_count"] = (f64)self->dynamic_include_load_count;
						response["dynamic_include_materialize_us"] = (f64)self->dynamic_include_materialize_total_us;
						response["unit_module_us"] = (f64)self->unit_module_total_us;
						response["unit_module_cache_hit_count"] = (f64)self->unit_module_cache_hit_count;
						response["unit_module_cache_miss_count"] = (f64)self->unit_module_cache_miss_count;
						response["unit_module_serialized_cache_hit_count"] = (f64)self->unit_module_serialized_cache_hit_count;
						response["unit_module_compile_count"] = (f64)self->unit_module_compile_count;
						response["unit_module_lookup_us"] = (f64)self->unit_module_lookup_total_us;
						response["unit_module_read_us"] = (f64)self->unit_module_read_total_us;
						response["unit_module_read_bytes"] = (f64)self->unit_module_read_bytes;
						response["unit_module_read_count"] = (f64)self->unit_module_read_count;
						response["unit_module_parse_us"] = (f64)self->unit_module_parse_total_us;
						response["unit_module_compile_us"] = (f64)self->unit_module_compile_total_us;
						response["unit_module_classify_us"] = (f64)self->unit_module_classify_total_us;
						response["unit_module_operations_dropped"] = (f64)self->unit_module_operations_dropped;
						response["unit_module_operations"].set_array();
						for(auto& operation : self->unit_module_operations)
						{
							DValue item;
							item["unit"] = operation.unit;
							item["kind"] = operation.kind;
							item["source"] = operation.source;
							item["total_us"] = (f64)operation.total_us;
							item["materialize_us"] = (f64)operation.materialize_us;
							item["lookup_us"] = (f64)operation.lookup_us;
							item["read_us"] = (f64)operation.read_us;
							item["read_bytes"] = (f64)operation.read_bytes;
							item["read_count"] = (f64)operation.read_count;
							item["parse_us"] = (f64)operation.parse_us;
							item["build_us"] = (f64)operation.build_us;
							item["classify_us"] = (f64)operation.classify_us;
							item["allocate_us"] = (f64)operation.allocate_us;
							item["import_us"] = (f64)operation.import_us;
							item["symbol_resolve_count"] = (f64)operation.symbol_resolve_count;
							item["symbol_resolve_us"] = (f64)operation.symbol_resolve_us;
							item["instantiate_us"] = (f64)operation.instantiate_us;
							item["initialize_us"] = (f64)operation.initialize_us;
							response["unit_module_operations"].push(item);
						}
						response["unit_allocate_us"] = (f64)self->unit_allocate_total_us;
						response["unit_import_us"] = (f64)self->unit_import_total_us;
						response["unit_symbol_resolve_count"] = (f64)self->unit_symbol_resolve_count;
						response["unit_symbol_resolve_us"] = (f64)self->unit_symbol_resolve_total_us;
						response["unit_instantiate_us"] = (f64)self->unit_instantiate_total_us;
						response["unit_initialize_us"] = (f64)self->unit_initialize_total_us;
						f64 running_us = response["running_us"].to_f64();
						f64 accounted_us = (f64)(self->dispatch_us + self->workspace_setup_us + self->workspace_birth_us + self->context_apply_us + self->hostcall_total_us);
						response["guest_us"] = running_us > accounted_us ? running_us - accounted_us : 0.0;
					}
					encoded = brb_encode(response);
					if(args[2].i32() == 0)
						self->hostcall_stage("request_perf", encoded);
				}
				u32 cap = (u32)args[3].i32();
				int32_t buf = args[2].i32();
				if(buf != 0 && cap >= encoded.size())
					self->hostcall_write(buf, encoded);
				results[0] = Val((int32_t)encoded.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_env")
			return(add([self](Caller caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String key, value;
				if(self->hostcall_read(args[0].i32(), args[1].i32(), key) == "")
					if(const char* raw = getenv(key.c_str()))
						value = raw;
				u32 cap = (u32)args[3].i32();
				if(value.size() && cap >= value.size())
					self->hostcall_write(args[2].i32(), value);
				results[0] = Val((int32_t)value.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_random")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				u32 len = (u32)args[1].i32();
				String bytes(len, 0);
				FILE* urandom = fopen("/dev/urandom", "rb");
				if(urandom)
				{
					size_t got = fread(&bytes[0], 1, len, urandom);
					fclose(urandom);
					bytes.resize(got);
				}
				self->hostcall_write(args[0].i32(), bytes);
				results[0] = Val((int32_t)bytes.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_sha256")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String in; self->hostcall_read(args[0].i32(), args[1].i32(), in); String out=sha256_native(in); u32 cap=(u32)args[3].i32(); int32_t buf=args[2].i32(); if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_sha256_hex")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String in; self->hostcall_read(args[0].i32(), args[1].i32(), in); String out=sha256_hex_native(in); u32 cap=(u32)args[3].i32(); int32_t buf=args[2].i32(); if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_hmac_sha256")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String key,in; self->hostcall_read(args[0].i32(), args[1].i32(), key); self->hostcall_read(args[2].i32(), args[3].i32(), in); String out=hmac_sha256_native(key,in); u32 cap=(u32)args[5].i32(); int32_t buf=args[4].i32(); if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_hmac_sha256_hex")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String key,in; self->hostcall_read(args[0].i32(), args[1].i32(), key); self->hostcall_read(args[2].i32(), args[3].i32(), in); String out=hmac_sha256_hex_native(key,in); u32 cap=(u32)args[5].i32(); int32_t buf=args[4].i32(); if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_base64_encode")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String in; self->hostcall_read(args[0].i32(), args[1].i32(), in); String out=base64_encode(in); u32 cap=(u32)args[3].i32(); int32_t buf=args[2].i32(); if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_base64_decode")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String in; self->hostcall_read(args[0].i32(), args[1].i32(), in); bool ok=false; String out=base64_decode(in, ok); if(!ok) out=""; u32 cap=(u32)args[3].i32(); int32_t buf=args[2].i32(); if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_crypto_equal")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String a,b; self->hostcall_read(args[0].i32(), args[1].i32(), a); self->hostcall_read(args[2].i32(), args[3].i32(), b); results[0]=Val((int32_t)(crypto_equal_native(a,b)?1:0)); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_password_hash")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String password; self->hostcall_read(args[0].i32(), args[1].i32(), password); String out=password_hash_native(password); u32 cap=(u32)args[3].i32(); int32_t buf=args[2].i32(); if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_password_verify")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String password,encoded; self->hostcall_read(args[0].i32(), args[1].i32(), password); self->hostcall_read(args[2].i32(), args[3].i32(), encoded); bool valid=password_verify_native(password,encoded); results[0]=Val((int32_t)(valid?1:0)); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_password_needs_rehash")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String encoded; self->hostcall_read(args[0].i32(), args[1].i32(), encoded); results[0]=Val((int32_t)(password_needs_rehash_native(encoded)?1:0)); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_log")
			return(add([self](Caller, Span<const Val> args, Span<Val>) -> Result<std::monostate, Trap> {
				String text;
				self->hostcall_read(args[1].i32(), args[2].i32(), text);
				fprintf(stderr, "[guest log %d] %.*s\n", args[0].i32(), (int)text.size(), text.data());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_shell_exec")
			return(add([self](Caller caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String cmd;
				self->hostcall_read(args[0].i32(), args[1].i32(), cmd);
				u32 cap = (u32)args[3].i32();
				int32_t buf = args[2].i32();
				String out;
				String stage_key = "shell:" + cmd;
				if(!self->hostcall_staged(stage_key, out))
				{
					u64 remaining_ms = self->invocation_remaining_ms();
					u64 timeout_ms = std::max<u64>(1, self->bounded_hostcall_timeout_ms(5000));
					DValue execution = process_exec(cmd + " 2>&1", "", StringMap(), timeout_ms);
					if(execution["timed_out"].to_bool())
					{
						String kind = self->invocation_expired() || remaining_ms <= 5000 ?
							"BEARER_INVOCATION_TIMEOUT" : "BEARER_HOSTCALL_TIMEOUT";
						return(Trap(kind + ": shell_exec exceeded " + std::to_string(timeout_ms) + " ms"));
					}
					out = execution["stdout"].to_string();
					if(buf == 0)
						self->hostcall_stage(stage_key, out);
				}
				if(buf != 0 && cap >= out.size())
					self->hostcall_write(buf, out);
				results[0] = Val((int32_t)out.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_http_request")
			return(add([self](Caller caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String encoded; self->hostcall_read(args[0].i32(), args[1].i32(), encoded); u32 cap=(u32)args[3].i32(); int32_t buf=args[2].i32(); String out; String stage_key="http:"+encoded;
				if(!self->hostcall_staged(stage_key,out)) { DValue req,response; String err; if(brb_decode(encoded,req,&err)) { u64 requested=req.key("timeout_ms")?req.key("timeout_ms")->to_u64(5000):5000; if(requested==0)requested=5000; req["timeout_ms"]=(f64)std::max<u64>(1,self->bounded_hostcall_timeout_ms(requested)); response=bearer_http_request_value(req); } else response["error"]="http_request decode failed: "+err; out=brb_encode(response); if(buf==0) self->hostcall_stage(stage_key,out); }
				if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_http_request_async")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String encoded; self->hostcall_read(args[0].i32(), args[1].i32(), encoded); DValue req; String err; u64 id=0; if(brb_decode(encoded,req,&err)) id=bearer_http_spawn_spec(req); results[0]=Val((int64_t)id); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_shell_exec_dv")
			return(add([self](Caller caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String encoded; self->hostcall_read(args[0].i32(), args[1].i32(), encoded);
				u32 cap=(u32)args[3].i32(); int32_t buf=args[2].i32();
				String out; String stage_key="shell_dv:"+encoded;
				if(!self->hostcall_staged(stage_key,out)) { DValue spec, response; String err; if(brb_decode(encoded,spec,&err)) { u64 requested=spec.key("timeout_ms")?spec.key("timeout_ms")->to_u64(5000):5000; if(requested==0)requested=5000; spec["timeout_ms"]=(f64)std::max<u64>(1,self->bounded_hostcall_timeout_ms(requested)); response=bearer_shell_exec_spec(spec); } else response["error"]="shell_exec spec decode failed: "+err; out=brb_encode(response); if(buf==0) self->hostcall_stage(stage_key,out); }
				if(buf&&cap>=out.size()) self->hostcall_write(buf,out);
				results[0]=Val((int32_t)out.size()); return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_shell_spawn")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String encoded; self->hostcall_read(args[0].i32(), args[1].i32(), encoded); DValue spec; String err; u64 id=0; if(brb_decode(encoded,spec,&err)) id=bearer_shell_spawn_spec(spec); results[0]=Val((int64_t)id); return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_job_status")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { u64 job_id=(u64)args[0].i64(); String out,stage_key="job_status:"+std::to_string(job_id); u32 cap=(u32)args[2].i32(); int32_t buf=args[1].i32(); if(!self->hostcall_staged(stage_key,out)){out=brb_encode(bearer_job_status_value(job_id));if(buf==0)self->hostcall_stage(stage_key,out);} if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_job_result")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { u64 job_id=(u64)args[0].i64(); String out,stage_key="job_result:"+std::to_string(job_id); u32 cap=(u32)args[2].i32(); int32_t buf=args[1].i32(); if(!self->hostcall_staged(stage_key,out)){out=brb_encode(bearer_job_result_value(job_id, self->bounded_hostcall_timeout_ms(100)));if(buf==0)self->hostcall_stage(stage_key,out);} if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_job_await")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				u64 job_id=(u64)args[0].i64(), requested_timeout=(u64)args[1].i64();
				u32 cap=(u32)args[3].i32(); int32_t buf=args[2].i32();
				String out, stage_key="job_await:"+std::to_string(job_id)+":"+std::to_string(requested_timeout);
				if(!self->hostcall_staged(stage_key,out))
				{
					u64 timeout=self->bounded_hostcall_timeout_ms(std::min<u64>(requested_timeout, 30000));
					out=brb_encode(bearer_job_result_value(job_id, timeout));
					if(buf==0) self->hostcall_stage(stage_key,out);
				}
				if(buf&&cap>=out.size()) self->hostcall_write(buf,out);
				results[0]=Val((int32_t)out.size()); return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_job_cancel")
			return(add([](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { results[0]=Val((int32_t)(bearer_job_cancel_value((u64)args[0].i64())?1:0)); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_path_real")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path;
				self->hostcall_read(args[0].i32(), args[1].i32(), path);
				String resolved = ::path_real(path);
				u32 cap = (u32)args[3].i32();
				int32_t buf = args[2].i32();
				if(buf != 0 && cap >= resolved.size())
					self->hostcall_write(buf, resolved);
				results[0] = Val((int32_t)resolved.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_path_is_within")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path, root;
				self->hostcall_read(args[0].i32(), args[1].i32(), path);
				self->hostcall_read(args[2].i32(), args[3].i32(), root);
				results[0] = Val(::path_is_within(path, root) ? (int32_t)1 : (int32_t)0);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_cwd_get")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String cwd = ::cwd_get();
				u32 cap = (u32)args[1].i32();
				int32_t buf = args[0].i32();
				if(buf != 0 && cap >= cwd.size())
					self->hostcall_write(buf, cwd);
				results[0] = Val((int32_t)cwd.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_cwd_set")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path;
				self->hostcall_read(args[0].i32(), args[1].i32(), path);
				String resolved = self->resolve_guest_cwd_set(path);
				results[0] = Val(::chdir(resolved.c_str()) == 0 ? (int32_t)1 : (int32_t)0);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_process_start_directory")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String cwd = ::process_start_directory();
				u32 cap = (u32)args[1].i32();
				int32_t buf = args[0].i32();
				if(buf != 0 && cap >= cwd.size())
					self->hostcall_write(buf, cwd);
				results[0] = Val((int32_t)cwd.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_last_trap_trace")
			return(add([](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				(void)args;
				results[0] = Val((int32_t)0);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_exists")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path, current;
				self->hostcall_read(args[0].i32(), args[1].i32(), path);
				self->hostcall_read(args[2].i32(), args[3].i32(), current);
				String resolved = self->resolve_guest_file(path, current);
				if(self->worker.cfg.verbose)
					fprintf(stderr, "[wasm] file_exists(%s, current=%s) -> %s\n", path.c_str(), current.c_str(), resolved.c_str());
				results[0] = Val(resolved != "" ? (int32_t)1 : (int32_t)0);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_mkdir")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path, current;
				self->hostcall_read(args[0].i32(), args[1].i32(), path);
				self->hostcall_read(args[2].i32(), args[3].i32(), current);
				String resolved = self->resolve_guest_write(path, current);
				int ok = 0;
				if(resolved != "")
					ok = (::mkdir(resolved.c_str(), 0777) == 0 || errno == EEXIST) ? 1 : 0;
				results[0] = Val((int32_t)ok);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_mtime")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path, current;
				self->hostcall_read(args[0].i32(), args[1].i32(), path);
				self->hostcall_read(args[2].i32(), args[3].i32(), current);
				String resolved = self->resolve_guest_file(path, current);
				int64_t mtime = 0;
				struct stat st;
				if(resolved != "" && stat(resolved.c_str(), &st) == 0)
					mtime = (int64_t)st.st_mtime;
				results[0] = Val((int64_t)mtime);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_read")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path, current;
				self->hostcall_read(args[0].i32(), args[1].i32(), path);
				self->hostcall_read(args[2].i32(), args[3].i32(), current);
				String stage_key = "file_read:" + path + "\0" + current;
				String content;
				if(!self->hostcall_staged(stage_key, content))
				{
					String resolved = self->resolve_guest_file(path, current);
					content = resolved == "" ? String("") : ::file_get_contents(resolved);
					self->hostcall_stage(stage_key, content);
				}
				u32 cap = (u32)args[5].i32();
				int32_t buf = args[4].i32();
				// length-query convention: no copy unless the buffer fits
				if(buf != 0 && cap >= content.size())
					self->hostcall_write(buf, content);
				results[0] = Val((int32_t)content.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_list")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path, current;
				self->hostcall_read(args[0].i32(), args[1].i32(), path);
				self->hostcall_read(args[2].i32(), args[3].i32(), current);
				String resolved = self->resolve_guest_file(path, current, true /*allow_dir*/);
				String listing;
				if(resolved != "")
				{
					std::vector<String> names;
					if(DIR* d = opendir(resolved.c_str()))
					{
						while(struct dirent* e = readdir(d))
						{
							String n = e->d_name;
							if(n != "." && n != "..")
								names.push_back(n);
						}
						closedir(d);
					}
					// match the native ls -1 convention: bare names, sorted
					std::sort(names.begin(), names.end());
					listing = join(names, "\n");
				}
				u32 cap = (u32)args[5].i32();
				int32_t buf = args[4].i32();
				// length-query convention: no copy unless the buffer fits
				if(buf != 0 && cap >= listing.size())
					self->hostcall_write(buf, listing);
				results[0] = Val((int32_t)listing.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_write")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path, current, content;
				self->hostcall_read(args[0].i32(), args[1].i32(), path);
				self->hostcall_read(args[2].i32(), args[3].i32(), current);
				self->hostcall_read(args[4].i32(), args[5].i32(), content);
				bool append = args[6].i32() != 0;
				String resolved = self->resolve_guest_write(path, current);
				bool ok = false;
				if(resolved != "")
					ok = append ? file_append(resolved, content) : file_put_contents(resolved, content);
				else if(self->worker.cfg.verbose)
					fprintf(stderr, "[wasm] file_write denied: %s\n", path.c_str());
				results[0] = Val(ok ? (int32_t)1 : (int32_t)0);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_open")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path, current, mode;
				self->hostcall_read(args[0].i32(), args[1].i32(), path);
				self->hostcall_read(args[2].i32(), args[3].i32(), current);
				self->hostcall_read(args[4].i32(), args[5].i32(), mode);
				String resolved;
				int flags = O_RDONLY;
				int lock_type = LOCK_SH;
				bool writable = false;
				bool truncate_after_lock = false;
				if(mode == "r")
					resolved = self->resolve_guest_file(path, current);
				else if(mode == "w")
				{
					resolved = self->resolve_guest_write(path, current);
					flags = O_RDWR | O_CREAT;
					lock_type = LOCK_EX;
					writable = true;
					truncate_after_lock = true;
				}
				else if(mode == "a")
				{
					resolved = self->resolve_guest_write(path, current);
					flags = O_RDWR | O_CREAT | O_APPEND;
					lock_type = LOCK_EX;
					writable = true;
				}
				else if(mode == "r+")
				{
					resolved = self->resolve_guest_write(path, current);
					flags = O_RDWR;
					lock_type = LOCK_EX;
					writable = true;
				}
				uint64_t handle = 0;
				if(resolved != "")
				{
					int fd = wasm_open_locked_file(resolved, flags, lock_type, truncate_after_lock);
					if(fd >= 0)
					{
						if(mode == "a")
							lseek(fd, 0, SEEK_END);
						self->file_handles.push_back({fd, writable});
						handle = self->file_handles.size();
					}
				}
				results[0] = Val((int64_t)handle);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_handle_read")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				u64 handle = (u64)args[0].i64();
				u64 len = (u64)args[1].i64();
				u32 cap = (u32)args[3].i32();
				int32_t buf = args[2].i32();
				String out;
				String stage_key = "file_handle_read:" + std::to_string(handle) + ":" + std::to_string(len);
				if(!self->hostcall_staged(stage_key, out))
				{
					if(handle >= 1 && handle <= self->file_handles.size())
					{
						int fd = self->file_handles[(size_t)handle - 1].fd;
						if(fd >= 0 && len > 0)
						{
							out.resize((size_t)std::min<u64>(len, 16ull * 1024ull * 1024ull));
							ssize_t n = read(fd, &out[0], out.size());
							out.resize(n > 0 ? (size_t)n : 0);
						}
					}
					if(buf == 0) self->hostcall_stage(stage_key, out);
				}
				if(buf != 0 && cap >= out.size()) self->hostcall_write(buf, out);
				results[0] = Val((int32_t)out.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_handle_pread")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				u64 handle = (u64)args[0].i64();
				u64 offset = (u64)args[1].i64();
				u64 len = (u64)args[2].i64();
				u32 cap = (u32)args[4].i32();
				int32_t buf = args[3].i32();
				String out;
				String stage_key = "file_handle_pread:" + std::to_string(handle) + ":" + std::to_string(offset) + ":" + std::to_string(len);
				if(!self->hostcall_staged(stage_key, out))
				{
					if(handle >= 1 && handle <= self->file_handles.size())
					{
						int fd = self->file_handles[(size_t)handle - 1].fd;
						if(fd >= 0 && len > 0)
						{
							out.resize((size_t)std::min<u64>(len, 16ull * 1024ull * 1024ull));
							ssize_t n = pread(fd, &out[0], out.size(), (off_t)offset);
							out.resize(n > 0 ? (size_t)n : 0);
						}
					}
					if(buf == 0) self->hostcall_stage(stage_key, out);
				}
				if(buf != 0 && cap >= out.size()) self->hostcall_write(buf, out);
				results[0] = Val((int32_t)out.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_handle_write")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String data; self->hostcall_read(args[1].i32(), args[2].i32(), data);
				u64 handle = (u64)args[0].i64(); u64 written = 0;
				if(handle >= 1 && handle <= self->file_handles.size())
				{
					auto& h = self->file_handles[(size_t)handle - 1];
					if(h.fd >= 0 && h.writable) wasm_fd_write_all(h.fd, data.data(), data.size(), &written);
				}
				results[0] = Val((int64_t)written);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_handle_pwrite")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String data; self->hostcall_read(args[2].i32(), args[3].i32(), data);
				u64 handle = (u64)args[0].i64(); u64 offset = (u64)args[1].i64(); u64 written = 0;
				if(handle >= 1 && handle <= self->file_handles.size())
				{
					auto& h = self->file_handles[(size_t)handle - 1];
					if(h.fd >= 0 && h.writable)
					{
						while(written < data.size())
						{
							ssize_t n = pwrite(h.fd, data.data() + written, data.size() - written, (off_t)(offset + written));
							if(n < 0 && errno == EINTR) continue;
							if(n <= 0) break;
							written += (u64)n;
						}
					}
				}
				results[0] = Val((int64_t)written);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_handle_seek")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				u64 handle = (u64)args[0].i64(); s64 pos = -1;
				if(handle >= 1 && handle <= self->file_handles.size())
				{
					int fd = self->file_handles[(size_t)handle - 1].fd;
					if(fd >= 0) pos = (s64)lseek(fd, (off_t)args[1].i64(), args[2].i32());
				}
				results[0] = Val((int64_t)pos);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_handle_tell")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				u64 handle = (u64)args[0].i64(); s64 pos = -1;
				if(handle >= 1 && handle <= self->file_handles.size())
				{
					int fd = self->file_handles[(size_t)handle - 1].fd;
					if(fd >= 0) pos = (s64)lseek(fd, 0, SEEK_CUR);
				}
				results[0] = Val((int64_t)pos);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_handle_close")
			return(add([self](Caller, Span<const Val> args, Span<Val>) -> Result<std::monostate, Trap> {
				u64 handle = (u64)args[0].i64();
				if(handle >= 1 && handle <= self->file_handles.size())
				{
					auto& h = self->file_handles[(size_t)handle - 1];
					if(h.fd >= 0) { flock(h.fd, LOCK_UN); close(h.fd); h.fd = -1; }
				}
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_stat")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path, current; self->hostcall_read(args[0].i32(), args[1].i32(), path); self->hostcall_read(args[2].i32(), args[3].i32(), current);
				String resolved = self->resolve_guest_file(path, current, true); DValue r; struct stat st;
				r["exists"].set_bool(resolved != "" && lstat(resolved.c_str(), &st) == 0);
				if(r["exists"].to_bool()) { r["size"]=(f64)st.st_size; r["mtime"]=(f64)st.st_mtime; r["ctime"]=(f64)st.st_ctime; r["mode"]=(f64)(st.st_mode & 07777); r["is_dir"].set_bool(S_ISDIR(st.st_mode)); r["is_file"].set_bool(S_ISREG(st.st_mode)); r["is_symlink"].set_bool(S_ISLNK(st.st_mode)); }
				else { r["size"]=(f64)0; r["mtime"]=(f64)0; r["ctime"]=(f64)0; r["mode"]=(f64)0; r["is_dir"].set_bool(false); r["is_file"].set_bool(false); r["is_symlink"].set_bool(false); }
				String out = brb_encode(r); u32 cap=(u32)args[5].i32(); int32_t buf=args[4].i32(); if(buf && cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_dir_list")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String path, current; self->hostcall_read(args[0].i32(), args[1].i32(), path); self->hostcall_read(args[2].i32(), args[3].i32(), current);
				String resolved = self->resolve_guest_file(path, current, true); DValue list; list.set_array();
				if(resolved != "") { std::vector<String> names; if(DIR* d=opendir(resolved.c_str())) { while(struct dirent* e=readdir(d)) { String n=e->d_name; if(n!="."&&n!="..") names.push_back(n); } closedir(d); } std::sort(names.begin(), names.end()); for(auto& n:names) { String p=resolved+"/"+n; struct stat st; DValue item; item["name"]=n; if(lstat(p.c_str(), &st)==0) { item["size"]=(f64)st.st_size; item["mtime"]=(f64)st.st_mtime; item["type"]=S_ISDIR(st.st_mode)?"dir":S_ISLNK(st.st_mode)?"symlink":S_ISREG(st.st_mode)?"file":"other"; } list.push(item); } }
				String out=brb_encode(list); u32 cap=(u32)args[5].i32(); int32_t buf=args[4].i32(); if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_file_rename")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String from,to,current; self->hostcall_read(args[0].i32(),args[1].i32(),from); self->hostcall_read(args[2].i32(),args[3].i32(),to); self->hostcall_read(args[4].i32(),args[5].i32(),current); String rf=self->resolve_guest_write(from,current), rt=self->resolve_guest_write(to,current); results[0]=Val((int32_t)(rf!=""&&rt!=""&&rename(rf.c_str(),rt.c_str())==0)); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_file_copy")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String from,to,current; self->hostcall_read(args[0].i32(),args[1].i32(),from); self->hostcall_read(args[2].i32(),args[3].i32(),to); self->hostcall_read(args[4].i32(),args[5].i32(),current); String rf=self->resolve_guest_file(from,current), rt=self->resolve_guest_write(to,current); bool ok=false; if(rf!=""&&rt!="") { std::ifstream in(rf, std::ios::binary); std::ofstream out(rt, std::ios::binary|std::ios::trunc); out<<in.rdbuf(); struct stat st; if(in&&out) { ok=true; if(stat(rf.c_str(),&st)==0) chmod(rt.c_str(), st.st_mode & 07777); } } results[0]=Val((int32_t)ok); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_file_truncate")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String path,current; self->hostcall_read(args[0].i32(),args[1].i32(),path); self->hostcall_read(args[2].i32(),args[3].i32(),current); String r=self->resolve_guest_write(path,current); results[0]=Val((int32_t)(r!=""&&truncate(r.c_str(),(off_t)args[4].i64())==0)); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_dir_remove")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String path,current; self->hostcall_read(args[0].i32(),args[1].i32(),path); self->hostcall_read(args[2].i32(),args[3].i32(),current); String r=self->resolve_guest_write(path,current); bool rec=args[4].i32()!=0; bool ok=false; if(r!="") { if(rec) ok=std::filesystem::remove_all(r)>0; else ok=::rmdir(r.c_str())==0; } results[0]=Val((int32_t)ok); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_file_temp")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String prefix,current; self->hostcall_read(args[0].i32(),args[1].i32(),prefix); self->hostcall_read(args[2].i32(),args[3].i32(),current); u32 cap=(u32)args[5].i32(); int32_t buf=args[4].i32(); String out; String stage_key="file_temp:"+prefix+"\0"+current; if(!self->hostcall_staged(stage_key,out)) { if(prefix=="") prefix="/tmp/bearer-temp"; String templ=self->resolve_guest_write(prefix+"XXXXXX",current); if(templ!="") { std::vector<char> t(templ.begin(), templ.end()); t.push_back(0); int fd=mkstemp(t.data()); if(fd>=0) { close(fd); out=t.data(); } } if(buf==0) self->hostcall_stage(stage_key,out); } if(buf&&cap>=out.size()) self->hostcall_write(buf,out); results[0]=Val((int32_t)out.size()); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_file_chmod")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String path,current; self->hostcall_read(args[0].i32(),args[1].i32(),path); self->hostcall_read(args[2].i32(),args[3].i32(),current); String r=self->resolve_guest_write(path,current); results[0]=Val((int32_t)(r!=""&&chmod(r.c_str(),(mode_t)args[4].i32())==0)); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_file_symlink")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { String target,linkpath,current; self->hostcall_read(args[0].i32(),args[1].i32(),target); self->hostcall_read(args[2].i32(),args[3].i32(),linkpath); self->hostcall_read(args[4].i32(),args[5].i32(),current); String rt=self->resolve_guest_file(target,current), rl=self->resolve_guest_write(linkpath,current); results[0]=Val((int32_t)(rt!=""&&rl!=""&&symlink(rt.c_str(),rl.c_str())==0)); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_file_fsync")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> { u64 handle=(u64)args[0].i64(); bool ok=false; if(handle>=1&&handle<=self->file_handles.size()) { int fd=self->file_handles[(size_t)handle-1].fd; ok=fd>=0&&fsync(fd)==0; } results[0]=Val((int32_t)ok); return(std::monostate()); }));
		if(mod == "env" && name == "bearer_host_zip")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String encoded;
				self->hostcall_read(args[0].i32(), args[1].i32(), encoded);
				u32 cap = (u32)args[3].i32();
				int32_t buf = args[2].i32();
				String out;
				String stage_key = "zip:" + encoded;
				if(!self->hostcall_staged(stage_key, out))
				{
					DValue request, response;
					String decode_error;
					try
					{
						if(brb_decode(encoded, request, &decode_error))
						{
							String op = request["op"].to_string();
							if(op == "list")
							{
								String path = self->resolve_guest_file(request["path"].to_string());
								if(path == "") throw std::runtime_error("zip_list: path is outside wasm file policy");
								response["result"] = zip_list(path);
							}
							else if(op == "read")
							{
								String path = self->resolve_guest_file(request["path"].to_string());
								if(path == "") throw std::runtime_error("zip_read: path is outside wasm file policy");
								response["result"] = zip_read(path, request["entry"].to_string());
							}
							else if(op == "create")
							{
								String path = self->resolve_guest_write(request["path"].to_string(), "");
								if(path == "") throw std::runtime_error("zip_create: path is outside wasm file policy");
								DValue* entries = request.key("entries");
								response["ok"].set_bool(zip_create(path, entries ? *entries : DValue()));
							}
							else if(op == "extract")
							{
								String path = self->resolve_guest_file(request["path"].to_string());
								String destination = self->resolve_guest_write(request["destination"].to_string(), "");
								if(path == "" || destination == "") throw std::runtime_error("zip_extract: path is outside wasm file policy");
								response["ok"].set_bool(zip_extract(path, destination));
							}
							else if(op == "gz_compress")
								response["result"] = gz_compress(request["src"].to_string());
							else if(op == "gz_uncompress")
								response["result"] = gz_uncompress(request["src"].to_string());
						}
					}
					catch(const std::exception& e)
					{
						response["error"] = e.what();
					}
					out = brb_encode(response);
					if(buf == 0)
						self->hostcall_stage(stage_key, out);
				}
				if(buf != 0 && cap >= out.size())
					self->hostcall_write(buf, out);
				results[0] = Val((int32_t)out.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_units")
			return(add([self](Caller caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String encoded;
				self->hostcall_read(args[0].i32(), args[1].i32(), encoded);
				u32 cap = (u32)args[3].i32();
				int32_t buf = args[2].i32();
				String out;
				String stage_key = "units:" + encoded;
				if(!self->hostcall_staged(stage_key, out))
				{
					DValue request, response;
					String decode_error;
					try
					{
						if(brb_decode(encoded, request, &decode_error))
						{
							String op = request["op"].to_string();
							if(op == "info")
								response["result"] = unit_info(request["path"].to_string());
							else if(op == "list")
							{
								StringList paths = units_list();
								for(auto& path : paths)
								{
									DValue item;
									item = path;
									response["result"].push(item);
								}
							}
							else if(op == "compile")
							{
								u64 remaining_ms = self->invocation_remaining_ms();
								bool timed_out = false;
								String compile_error;
								bool ok = remaining_ms > 0 && unit_compile_bounded(context, request["path"].to_string(), remaining_ms, &timed_out, &compile_error);
								if(timed_out || remaining_ms == 0)
									return(Trap(self->invocation_timeout_error()));
								if(!ok && compile_error != "")
								{
									response["error"] = compile_error;
									printf("(!) unit_compile failed for %s: %s\n", request["path"].to_string().c_str(), compile_error.c_str());
								}
								response["ok"].set_bool(ok);
							}
							else if(op == "call")
							{
								DValue* param = request.key("param");
								ob_start();
								DValue* result = unit_call(request["file"].to_string(), request["function"].to_string(), param);
								response["output"] = ob_get_close();
								if(result)
									response["result"] = *result;
							}
						}
					}
					catch(const std::exception& e)
					{
						response["error"] = e.what();
					}
					out = brb_encode(response);
					if(buf == 0)
						self->hostcall_stage(stage_key, out);
				}
				if(buf != 0 && cap >= out.size())
					self->hostcall_write(buf, out);
				results[0] = Val((int32_t)out.size());
				return(std::monostate());
			}));
#ifdef BEARER_WASM_HOST_CONNECTORS
		if(mod == "env" && name == "bearer_host_sqlite")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				// {op,handle,path,query,params} in → result out. The native
				// connector runs host-side; connections live in the workspace
				// handle table (handle = 1-based index).
				String encoded;
				self->hostcall_read(args[0].i32(), args[1].i32(), encoded);
				u32 cap = (u32)args[3].i32();
				int32_t buf = args[2].i32();
				String out;
				String stage_key = "sqlite:" + encoded;
				// run the op once across the length-query + fetch pair
				if(!self->hostcall_staged(stage_key, out))
				{
					DValue request, response;
					String decode_error;
					if(brb_decode(encoded, request, &decode_error))
					{
						String op = request["op"].to_string();
						if(op == "connect")
						{
							SQLite* db = new SQLite();
							db->connect(request["path"].to_string());
							u64 handle = 0;
							if(db->connection)
							{
								self->sqlite_handles.push_back(db);
								handle = self->sqlite_handles.size();
							}
							response["handle"] = (f64)handle;
							response["error_code"] = (f64)db->error_code;
							response["statement_info"] = db->error();
							if(handle == 0)
								delete db;
						}
						else
						{
							u64 handle = request["handle"].to_u64();
							SQLite* db = (handle >= 1 && handle <= self->sqlite_handles.size())
								? self->sqlite_handles[(size_t)handle - 1] : 0;
							if(op == "query" && db)
							{
								StringMap params;
								DValue* p = request.key("params");
								if(p)
									p->each([&](const DValue& value, String key) { params[key] = value.to_string(); });
								response["result"] = db->query(request["query"].to_string(), params);
								response["insert_id"] = (f64)db->insert_id;
								response["affected"] = (f64)db->affected_rows;
								response["error_code"] = (f64)db->error_code;
								response["statement_info"] = db->error();
							}
							else if(op == "disconnect" && db)
							{
								delete db;
								self->sqlite_handles[(size_t)handle - 1] = 0;
							}
						}
					}
					out = brb_encode(response);
					if(buf == 0)
						self->hostcall_stage(stage_key, out);
				}
				if(buf != 0 && cap >= out.size())
					self->hostcall_write(buf, out);
				results[0] = Val((int32_t)out.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_memcache_command")
			return(add([self](Caller caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String command;
				self->hostcall_read(args[1].i32(), args[2].i32(), command);
				String key = "memcache:" + std::to_string((u64)args[0].i64()) + ":" + command;
				u32 cap = (u32)args[4].i32();
				int32_t buf = args[3].i32();
				String out;
				if(buf != 0 && self->staged_memcache_key == key)
				{
					out = self->staged_memcache_result;
					self->staged_memcache_key = "";
					self->staged_memcache_result = "";
				}
				else
				{
					u64 socket_fd = (u64)args[0].i64();
					wasm_socket_write_bounded(socket_fd, command + "\r\n", self->bounded_hostcall_timeout_ms(1000));
					out = wasm_socket_read_bounded(socket_fd, 1024 * 128, self->bounded_hostcall_timeout_ms(1000));
					if(buf == 0)
					{
						self->staged_memcache_key = key;
						self->staged_memcache_result = out;
					}
				}
				if(buf != 0 && cap >= out.size())
					self->hostcall_write(buf, out);
				results[0] = Val((int32_t)out.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_mysql")
			return(add([self](Caller caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String encoded;
				self->hostcall_read(args[0].i32(), args[1].i32(), encoded);
				u32 cap = (u32)args[3].i32();
				int32_t buf = args[2].i32();
				String out;
				String stage_key = "mysql:" + encoded;
				if(!self->hostcall_staged(stage_key, out))
				{
					DValue request, response;
					String decode_error;
					String op;
					String connection_source;
					auto operation_started = std::chrono::steady_clock::now();
					if(brb_decode(encoded, request, &decode_error))
					{
						op = request["op"].to_string();
						if(op == "connect")
						{
							String host = request["host"].to_string();
							String username = request["username"].to_string();
							String password = request["password"].to_string();
							String database = request["database"].to_string();
							MySQL* db = 0;
							for(auto* pooled : self->mysql_request_pool)
								if(pooled && pooled->connection && pooled->request_host == host && pooled->request_username == username && pooled->request_password == password && pooled->request_database == database)
								{
									db = pooled;
									connection_source = "request";
									break;
								}
							bool ok = db != 0;
							if(!db)
							{
								bool reused = false;
								bool persistent = false;
								db = self->worker.mysql_checkout(host, username, password, database, reused, persistent);
								connection_source = reused ? "worker" : "new";
								ok = db && db->connection;
								if(ok && db->connection)
								{
									self->mysql_request_pool.push_back(db);
									if(!persistent)
										self->mysql_request_owned.push_back(db);
								}
							}
							u64 handle = 0;
							if(ok && db->connection)
							{
								db->request_leases++;
								self->mysql_handles.push_back(db);
								handle = self->mysql_handles.size();
								if(connection_source == "new") self->mysql_connection_open_count++;
								else if(connection_source == "worker") self->mysql_connection_reuse_count++;
								else if(connection_source == "request") self->mysql_request_pool_hit_count++;
							}
							response["handle"] = (f64)handle;
							response["error_code"] = (f64)db->_preload_next_error_code;
							response["statement_info"] = db->error();
							if(handle == 0 && db)
								delete db;
						}
						else if(op == "escape")
						{
							String quote = request["quote_char"].to_string();
							response["result"] = mysql_escape(request["raw"].to_string(), quote.size() ? quote[0] : 0);
						}
						else
						{
							u64 handle = request["handle"].to_u64();
							MySQL* db = (handle >= 1 && handle <= self->mysql_handles.size())
								? self->mysql_handles[(size_t)handle - 1] : 0;
							if(op == "query" && db)
							{
								response["result"] = db->query(request["query"].to_string());
								response["insert_id"] = (f64)db->insert_id;
								response["affected"] = (f64)db->affected_rows;
								response["error_code"] = (f64)db->_preload_next_error_code;
								response["statement_info"] = db->error();
							}
							else if(op == "disconnect" && db)
							{
								if(db->request_leases > 0)
									db->request_leases--;
								self->mysql_handles[(size_t)handle - 1] = 0;
							}
						}
					}
					if(op != "")
					{
						WasmMySQLOperation operation;
						operation.op = op;
						operation.source = connection_source;
						operation.elapsed_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
							std::chrono::steady_clock::now() - operation_started).count();
						self->mysql_operation_count++;
						if(self->mysql_operations.size() < 64)
							self->mysql_operations.push_back(operation);
						else
							self->mysql_operations_dropped++;
					}
					out = brb_encode(response);
					if(buf == 0)
						self->hostcall_stage(stage_key, out);
				}
				if(buf != 0 && cap >= out.size())
					self->hostcall_write(buf, out);
				results[0] = Val((int32_t)out.size());
				return(std::monostate());
			}));
#endif
		if(mod == "env" && name == "bearer_host_file_unlink")
			return(add([self](Caller, Span<const Val> args, Span<Val>) -> Result<std::monostate, Trap> {
				String path, current;
				self->hostcall_read(args[0].i32(), args[1].i32(), path);
				self->hostcall_read(args[2].i32(), args[3].i32(), current);
				String resolved = self->resolve_guest_write(path, current);
				if(resolved != "")
					::unlink(resolved.c_str());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_socket_connect")
			return(add([self](Caller caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String host;
				self->hostcall_read(args[0].i32(), args[1].i32(), host);
				u64 fd = wasm_socket_connect_bounded(host, (u16)args[2].i32(), self->bounded_hostcall_timeout_ms(self->worker.cfg.invocation_timeout_ms));
				results[0] = Val((int64_t)fd);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_socket_close")
			return(add([](Caller, Span<const Val> args, Span<Val>) -> Result<std::monostate, Trap> {
				::socket_close((u64)args[0].i64());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_socket_write")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String data;
				self->hostcall_read(args[1].i32(), args[2].i32(), data);
				results[0] = Val(wasm_socket_write_bounded((u64)args[0].i64(), data, self->bounded_hostcall_timeout_ms(self->worker.cfg.invocation_timeout_ms)) ? (int32_t)1 : (int32_t)0);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_socket_read")
			return(add([self](Caller caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				u64 sockfd = (u64)args[0].i64();
				u32 max_length = (u32)args[1].i32();
				u32 requested_timeout = (u32)args[2].i32();
				u64 requested_ms = requested_timeout == 0 ? self->invocation_remaining_ms() : (u64)requested_timeout * 1000;
				u64 bounded_ms = self->bounded_hostcall_timeout_ms(requested_ms);
				int32_t buf = args[3].i32();
				u32 cap = (u32)args[4].i32();
				// The size and fetch calls share this key, while the remaining budget may change between them.
				String key = std::to_string(sockfd) + ":" + std::to_string(max_length) + ":" + std::to_string(requested_timeout);
				String out;
				if(buf != 0 && self->staged_socket_read_key == key)
				{
					out = self->staged_socket_read_result;
					self->staged_socket_read_key = "";
					self->staged_socket_read_result = "";
				}
				else
				{
					out = wasm_socket_read_bounded(sockfd, max_length, bounded_ms);
					if(buf == 0)
					{
						self->staged_socket_read_key = key;
						self->staged_socket_read_result = out;
					}
				}
				if(buf != 0 && cap >= out.size())
					self->hostcall_write(buf, out);
				results[0] = Val((int32_t)out.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_server_start_http")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String key, bind, file, function, current;
				self->hostcall_read(args[0].i32(), args[1].i32(), key);
				self->hostcall_read(args[2].i32(), args[3].i32(), bind);
				self->hostcall_read(args[4].i32(), args[5].i32(), file);
				self->hostcall_read(args[6].i32(), args[7].i32(), function);
				self->hostcall_read(args[8].i32(), args[9].i32(), current);
				String resolved = self->resolve_guest_file(file, current);
				pid_t pid = 0;
				try
				{
					if(resolved != "")
						pid = ::server_start_http(key, bind, resolved, function);
				}
				catch(const std::exception& e)
				{
					fprintf(stderr, "[wasm server] start failed for key '%s': %s\n", key.c_str(), e.what());
				}
				catch(...)
				{
					fprintf(stderr, "[wasm server] start failed for key '%s'\n", key.c_str());
				}
				results[0] = Val((int32_t)pid);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_server_stop")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String key;
				self->hostcall_read(args[0].i32(), args[1].i32(), key);
				results[0] = Val(::server_stop(key) ? (int32_t)1 : (int32_t)0);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_task_spawn")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String key;
				self->hostcall_read(args[0].i32(), args[1].i32(), key);
				u64 callback_id = (u64)args[2].i64();
				f64 interval = args[3].f64();
				u64 timeout = (u64)args[4].i64();
				bool repeat = args[5].i32() != 0;
				// task()/task_repeat() fork and invoke this lambda only in the child
				// before the hostcall stack unwinds, so `self` points to the child's
				// copy of this per-request workspace. The parent request can return and
				// destroy its workspace without invalidating the child copy.
				u64 task_timeout_ms = timeout > UINT64_MAX / 1000 ? UINT64_MAX : timeout * 1000;
				auto run_callback = [self, callback_id, task_timeout_ms]() {
					String error = self->run_task_callback(callback_id, task_timeout_ms);
					if(error != "")
						fprintf(stderr, "[wasm task] callback failed: %s\n", error.c_str());
				};
				pid_t pid = 0;
				try
				{
					if(!repeat || (interval > 0 && std::isfinite(interval)))
						pid = repeat
							? ::task_repeat(key, interval, run_callback, timeout)
							: ::task(key, run_callback, timeout);
				}
				catch(const std::exception& e)
				{
					fprintf(stderr, "[wasm task] spawn failed for key '%s': %s\n", key.c_str(), e.what());
				}
				catch(...)
				{
					fprintf(stderr, "[wasm task] spawn failed for key '%s'\n", key.c_str());
				}
				results[0] = Val((int32_t)pid);
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_task_pid")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String key;
				self->hostcall_read(args[0].i32(), args[1].i32(), key);
				results[0] = Val((int32_t)::task_pid(key));
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_task_kill")
			return(add([](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				results[0] = Val((int32_t)::task_kill((pid_t)args[0].i32(), args[1].i32()));
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_sleep_us")
			return(add([self](Caller caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				u64 usec = (u64)args[0].i64();
				u64 requested_ms = usec / 1000 + (usec % 1000 != 0);
				u64 bounded_ms = self->bounded_hostcall_timeout_ms(requested_ms);
				u64 bounded_usec = bounded_ms > UINT64_MAX / 1000 ? UINT64_MAX : bounded_ms * 1000;
				u64 sleep_usec = std::min(usec, bounded_usec);
				struct timespec requested = { (time_t)(sleep_usec / 1000000), (long)((sleep_usec % 1000000) * 1000) };
				struct timespec interrupted = { 0, 0 };
				u64 unslept_usec = usec - sleep_usec;
				if(sleep_usec > 0 && nanosleep(&requested, &interrupted) != 0 && errno == EINTR)
					unslept_usec += (u64)interrupted.tv_sec * 1000000 + (u64)interrupted.tv_nsec / 1000;
				u64 unslept_seconds = unslept_usec / 1000000 + (unslept_usec % 1000000 != 0);
				results[0] = Val((int32_t)std::min<u64>(UINT32_MAX, unslept_seconds));
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_regex")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				// {op,pattern,subject,flags,replacement} in (BRRB2) → result out.
				// PCRE2 lives host-side; this runs the native regex_*.
				String encoded;
				self->hostcall_read(args[0].i32(), args[1].i32(), encoded);
				String out = wasm_host_regex_execute(encoded);
				u32 cap = (u32)args[3].i32();
				int32_t buf = args[2].i32();
				if(buf != 0 && cap >= out.size())
					self->hostcall_write(buf, out);
				results[0] = Val((int32_t)out.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_regex_capy")
			return(add([self](Caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				int32_t buf = args[2].i32();
				u32 cap = (u32)args[3].i32();
				if(buf == 0)
				{
					String encoded;
					self->hostcall_read(args[0].i32(), args[1].i32(), encoded);
					try
					{
						self->capy_regex_result = wasm_host_regex_execute(encoded);
					}
					catch(const std::exception& error)
					{
						self->capy_regex_result.clear();
						return(Trap(error.what()));
					}
				}
				if(buf != 0 && cap >= self->capy_regex_result.size())
				{
					self->hostcall_write(buf, self->capy_regex_result);
					u32 size = (u32)self->capy_regex_result.size();
					self->capy_regex_result.clear();
					results[0] = Val((int32_t)size);
					return(std::monostate());
				}
				results[0] = Val((int32_t)self->capy_regex_result.size());
				return(std::monostate());
			}));
		if(mod == "env" && name == "bearer_host_component_resolve")
			return(add([self](Caller caller, Span<const Val> args, Span<Val> results) -> Result<std::monostate, Trap> {
				String target, handler, current, resolved, compile_error;
				self->hostcall_read(args[0].i32(), args[1].i32(), target);
				self->hostcall_read(args[2].i32(), args[3].i32(), handler);
				self->hostcall_read(args[4].i32(), args[5].i32(), current);
				int32_t once_slot = 0;
				bool compile_timed_out = false;
				int32_t slot = self->component_resolve(target, handler, current, resolved, &once_slot, &compile_timed_out, &compile_error);
				if(compile_timed_out)
					return(Trap(self->invocation_timeout_error()));
				if(slot == 0 && compile_error != "")
				{
					resolved = compile_error;
					slot = -1;
				}
				u32 cap = (u32)args[7].i32();
				if(cap > 0)
				{
					if(resolved.size() >= cap)
						resolved = resolved.substr(0, cap - 1);
					resolved.push_back('\0');
					self->hostcall_write(args[6].i32(), resolved);
				}
				String once_slot_bytes((const char*)&once_slot, sizeof(once_slot));
				self->hostcall_write(args[8].i32(), once_slot_bytes);
				results[0] = Val(slot);
				return(std::monostate());
			}));

		// anything else (wasi-libc residue): a named trap — tolerated as long
		// as it is never called
		String label = mod + "." + name;
		return(add([label](Caller, Span<const Val>, Span<Val>) -> Result<std::monostate, Trap> {
			return(wasmtime::Trap("unimplemented host import called: " + std::string(label)));
		}));
	}

	String hostcall_read(int32_t ptr, int32_t len, String& out)
	{
		return(guest_read((u32)ptr, (u32)len, out));
	}

	void hostcall_write(int32_t ptr, const String& data)
	{
		guest_write((u32)ptr, data);
	}
};

inline String wasm_worker_prepare(WasmWorker& worker)
{
	WasmWorkspace workspace(worker);
	return(workspace.birth());
}

// ---- public entry: one request through one workspace -----------------------

inline WasmResponse wasm_worker_serve(WasmWorker& worker, const Request& request, const String& entry_source_path,
	const String& handler = "render", u64 timeout_cap_ms = UINT64_MAX)
{
	WasmResponse response;
	f64 serve_started = time_precise();
	auto workspace_start = std::chrono::steady_clock::now();
	f64 cpu_started = wasm_thread_cpu_time();
	struct rusage thread_runtime_start = {};
	bool thread_runtime_profiled = worker.cfg.profile_thread_runtime && getrusage(RUSAGE_THREAD, &thread_runtime_start) == 0;
	int thread_cpu_start = thread_runtime_profiled ? sched_getcpu() : -1;
	WasmWorkspace workspace(worker);
	u64 workspace_setup_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - workspace_start).count();
	u64 workspace_setup_ms = workspace_setup_us / 1000 + (workspace_setup_us % 1000 != 0);
	u64 workspace_budget_ms = timeout_cap_ms;
	if(workspace_budget_ms != UINT64_MAX)
		workspace_budget_ms = workspace_budget_ms > workspace_setup_ms ? workspace_budget_ms - workspace_setup_ms : 0;
	WasmWorkspace::InvocationScope invocation(workspace, workspace_budget_ms, false, worker.cfg.invocation_timeout_ms);
	f64 setup_cpu_finished = wasm_thread_cpu_time();
	workspace.workspace_setup_us = workspace_setup_us;
	workspace.workspace_setup_cpu_us = cpu_started > 0 && setup_cpu_finished > cpu_started ?
		(u64)((setup_cpu_finished - cpu_started) * 1000000.0) : 0;
	workspace.set_perf_snapshot(my_pid, (u64)parent_pid, request.server ? request.server->request_count : 0,
		request.stats.time_init, request.stats.time_params, request.stats.time_input, request.stats.time_start,
		request.stats.wasm_ready_normalize_us, request.stats.wasm_ready_mutation_check_us,
		request.stats.wasm_ready_artifact_stat_us, request.stats.wasm_ready_freshness_us,
		request.stats.wasm_ready_source_generation_us, request.stats.wasm_ready_freshness_full_check_us,
		request.stats.wasm_ready_worker_us, request.stats.wasm_ready_check_count,
		request.stats.wasm_ready_freshness_cache_hit_count,
		serve_started, cpu_started);
	workspace.request_perf.thread_runtime_start = thread_runtime_start;
	workspace.request_perf.thread_cpu_start = thread_cpu_start;
	workspace.request_perf.thread_runtime_profiled = thread_runtime_profiled;
	if(request.stats.time_start > 0 && serve_started > request.stats.time_start)
		workspace.dispatch_us = (u64)((serve_started - request.stats.time_start) * 1000000.0);
	auto birth_start = std::chrono::steady_clock::now();
	f64 birth_cpu_start = wasm_thread_cpu_time();
	String error = workspace.birth();
	f64 birth_cpu_finished = wasm_thread_cpu_time();
	workspace.workspace_birth_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - birth_start).count();
	workspace.workspace_birth_cpu_us = birth_cpu_start > 0 && birth_cpu_finished > birth_cpu_start ?
		(u64)((birth_cpu_finished - birth_cpu_start) * 1000000.0) : 0;
	if(error == "")
	{
		auto context_start = std::chrono::steady_clock::now();
		f64 context_cpu_start = wasm_thread_cpu_time();
		error = workspace.apply_context(request, entry_source_path, handler);
		f64 context_cpu_finished = wasm_thread_cpu_time();
		workspace.context_apply_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - context_start).count();
		workspace.context_apply_cpu_us = context_cpu_start > 0 && context_cpu_finished > context_cpu_start ?
			(u64)((context_cpu_finished - context_cpu_start) * 1000000.0) : 0;
	}
	if(error == "")
	{
		auto invoke_start = std::chrono::steady_clock::now();
		error = workspace.invoke_entry(entry_source_path, handler, &response.handler_present);
		workspace.entry_invoke_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - invoke_start).count();
	}
	if(error == "")
	{
		auto collect_start = std::chrono::steady_clock::now();
		error = workspace.collect(response);
		workspace.output_collect_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - collect_start).count();
	}
	workspace.workspace_complete_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - workspace_start).count();
	static_cast<WasmRequestProfile&>(response) = workspace;
	if(error != "")
	{
		response.ok = false;
		response.error = error;
		return(response);
	}
	response.ok = true;
	return(response);
}
