#include "compiler.h"
#include "compiler-parser.h"
#include "hash.h"
#include "../wasm/abi.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const u64 UCE_UNIT_ABI_VERSION = UCE_COMPILER_UNIT_ABI_VERSION;

struct SharedUnitFilesystemState
{
	bool source_exists = false;
	bool metadata_exists = false;
	bool compile_output_exists = false;
	bool metadata_parsed = false;
	bool abi_compatible = false;
	bool input_signature_matches = false;
	time_t source_time = 0;
	time_t compiled_time = 0;
	time_t metadata_time = 0;
	time_t compile_output_time = 0;
	time_t setup_template_time = 0;
	time_t compiler_abi_time = 0;
	time_t required_time = 0;
	u64 metadata_abi_version = 0;
	u64 runtime_abi_version = UCE_UNIT_ABI_VERSION;
	u64 metadata_wasm_core_abi_version = 0;
	u64 runtime_wasm_core_abi_version = UCE_WASM_CORE_ABI_VERSION;
	String metadata_content;
	String compile_output_content;
	String metadata_build_token;
	String metadata_input_signature;
	String metadata_source_path;
	String current_input_signature;
};

struct SharedUnitCompileCheck
{
	bool source_missing = false;
	bool needs_compile = false;
};

struct CompilerDeadline
{
	using Clock = std::chrono::steady_clock;
	Clock::time_point expires_at;
	bool timed_out = false;
	bool operational_failure = false;
	String operational_error;
	bool dependency_failure = false;
	String dependency_error;

	explicit CompilerDeadline(u64 timeout_ms) : expires_at(Clock::now() + std::chrono::milliseconds(timeout_ms)) {}

	u64 remaining_ms() const
	{
		auto now = Clock::now();
		if(now >= expires_at)
			return(0);
		return((u64)std::chrono::duration_cast<std::chrono::milliseconds>(expires_at - now).count());
	}

	bool expire_if_needed()
	{
		if(remaining_ms() > 0)
			return(false);
		timed_out = true;
		return(true);
	}
};

static std::atomic<u64> compiler_invocation_stage_counter(0);
static thread_local CompilerDeadline* compiler_active_deadline = 0;

class CompilerDeadlineScope
{
	CompilerDeadline* previous;
public:
	explicit CompilerDeadlineScope(CompilerDeadline* deadline) : previous(compiler_active_deadline)
	{
		compiler_active_deadline = deadline;
	}
	~CompilerDeadlineScope() { compiler_active_deadline = previous; }
};

struct UnitSourceSignatureEntry
{
	std::chrono::steady_clock::time_point checked_at;
	u64 modified_ns = 0;
	u64 changed_ns = 0;
	u64 size = 0;
	bool readable = false;
	String content_hash;
	StringList loaded_paths;
};

const u64 UCE_UNIT_SOURCE_SIGNATURE_CACHE_MAX = 4096;
std::mutex unit_source_signature_cache_mutex;
std::map<String, UnitSourceSignatureEntry> unit_source_signature_cache;

bool compiler_recent_unit_source_entry(String file_name, UnitSourceSignatureEntry& entry)
{
	auto checked_at = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(unit_source_signature_cache_mutex);
	auto cached = unit_source_signature_cache.find(file_name);
	if(cached == unit_source_signature_cache.end() ||
		std::chrono::duration_cast<std::chrono::milliseconds>(checked_at - cached->second.checked_at).count() >= 1000)
		return(false);
	entry = cached->second;
	return(true);
}

bool compiler_jit_compile_on_request_enabled(Request* context)
{
	if(!context || !context->server)
		return(true);
	return(to_bool(context->server->config["JIT_COMPILE_ON_REQUEST"], true));
}

bool compiler_is_u64_string(String value)
{
	value = trim(value);
	if(value == "")
		return(false);
	for(auto c : value)
	{
		if(c < '0' || c > '9')
			return(false);
	}
	return(true);
}

StringMap compiler_parse_unit_metadata(String content)
{
	StringMap metadata;
	content = trim(content);
	if(content == "")
		return(metadata);

	auto lines = split(content, "\n");
	for(auto& raw_line : lines)
	{
		auto line = trim(raw_line);
		if(line == "")
			continue;
		auto split_pos = line.find("=");
		if(split_pos == String::npos)
			continue;
		auto key = trim(line.substr(0, split_pos));
		auto value = trim(line.substr(split_pos + 1));
		if(key != "")
			metadata[key] = value;
	}
	return(metadata);
}

StringList compiler_unit_load_paths(String file_name, String content)
{
	StringList paths;
	u64 line_start = 0;
	while(line_start < content.length())
	{
		u64 line_end = content.find("\n", line_start);
		if(line_end == String::npos)
			break;
		String line = content.substr(line_start, line_end - line_start);
		if(str_starts_with(line, "#load "))
		{
			u64 first_quote = line.find('"', 6);
			u64 second_quote = first_quote == String::npos ? String::npos : line.find('"', first_quote + 1);
			if(first_quote != String::npos && second_quote != String::npos)
			{
				String loaded = line.substr(first_quote + 1, second_quote - first_quote - 1);
				if(loaded != "")
					paths.push_back(loaded[0] == '/' ? loaded : expand_path(loaded, dirname(file_name)));
			}
		}
		line_start = line_end + 1;
	}
	return(paths);
}

bool compiler_source_readable(String file_name, const struct stat& info, int* read_error = 0)
{
	int error = 0;
	if((info.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) == 0)
		error = EACCES;
	else if(access(file_name.c_str(), R_OK) != 0)
		error = errno;
	if(read_error)
		*read_error = error;
	return(error == 0);
}

String compiler_source_path_real(String file_name)
{
	int fd = open(file_name.c_str(), O_PATH | O_CLOEXEC);
	if(fd < 0)
		return(path_real(file_name));
	String proc_path = "/proc/self/fd/" + std::to_string(fd);
	char resolved[PATH_MAX];
	ssize_t length = readlink(proc_path.c_str(), resolved, sizeof(resolved));
	close(fd);
	if(length <= 0 || length >= (ssize_t)sizeof(resolved))
		return(path_real(file_name));
	String result(resolved, (size_t)length);
	if(str_ends_with(result, " (deleted)"))
		return(path_real(file_name));
	return(result);
}

UnitSourceSignatureEntry compiler_unit_source_entry(String file_name, bool allow_recent_stat)
{
	auto checked_at = std::chrono::steady_clock::now();
	if(allow_recent_stat)
	{
		UnitSourceSignatureEntry cached;
		if(compiler_recent_unit_source_entry(file_name, cached))
			return(cached);
	}
	struct stat info;
	UnitSourceSignatureEntry entry;
	if(stat(file_name.c_str(), &info) != 0)
		return(entry);
	entry.checked_at = checked_at;
	entry.modified_ns = (u64)info.st_mtim.tv_sec * 1000000000ull + (u64)info.st_mtim.tv_nsec;
	entry.changed_ns = (u64)info.st_ctim.tv_sec * 1000000000ull + (u64)info.st_ctim.tv_nsec;
	entry.size = (u64)info.st_size;
	entry.readable = compiler_source_readable(file_name, info);
	{
		std::lock_guard<std::mutex> lock(unit_source_signature_cache_mutex);
		auto cached = unit_source_signature_cache.find(file_name);
		if(cached != unit_source_signature_cache.end() && cached->second.modified_ns == entry.modified_ns && cached->second.changed_ns == entry.changed_ns && cached->second.size == entry.size && cached->second.readable == entry.readable)
		{
			cached->second.checked_at = checked_at;
			return(cached->second);
		}
	}
	String content = file_get_contents(file_name);
	entry.content_hash = gen_sha1(content);
	entry.loaded_paths = compiler_unit_load_paths(file_name, content);
	{
		std::lock_guard<std::mutex> lock(unit_source_signature_cache_mutex);
		if(unit_source_signature_cache.size() >= UCE_UNIT_SOURCE_SIGNATURE_CACHE_MAX)
			unit_source_signature_cache.clear();
		unit_source_signature_cache[file_name] = entry;
	}
	return(entry);
}

void compiler_append_unit_source_signature(String file_name, std::set<String>& visited, String& signature, bool allow_recent_stat)
{
	// The signature already deduplicates canonical files. Skip repeated exact
	// load paths before resolving them again; distinct aliases are still
	// canonicalized so symlink retargets remain visible on the next check.
	if(visited.find(file_name) != visited.end())
		return;
	String normalized = file_name;
	UnitSourceSignatureEntry entry;
	bool recent = allow_recent_stat && compiler_recent_unit_source_entry(normalized, entry);
	if(!recent)
	{
		normalized = compiler_source_path_real(file_name);
		if(normalized == "")
			normalized = file_name;
	}
	if(visited.find(normalized) != visited.end())
	{
		visited.insert(file_name);
		return;
	}
	visited.insert(file_name);
	visited.insert(normalized);

	if(!recent)
		entry = compiler_unit_source_entry(normalized, allow_recent_stat);
	signature += normalized + ":" + entry.content_hash + (entry.readable ? String("") : String(":unreadable")) + "\n";
	for(String loaded : entry.loaded_paths)
		compiler_append_unit_source_signature(loaded, visited, signature, allow_recent_stat);
}

String compiler_unit_source_signature(String file_name, bool allow_recent_stat = false)
{
	std::set<String> visited;
	String signature = "uce-load-graph-v1\n";
	compiler_append_unit_source_signature(file_name, visited, signature, allow_recent_stat);
	return(gen_sha1(signature));
}

String compiler_unit_input_signature(Request* context, SharedUnit* su, bool allow_recent_source_stat = false, bool source_exists_known = false)
{
	if(!context || !su || (!source_exists_known && !file_exists(su->file_name)))
		return("");

	String setup_template = context->server->config["COMPILER_SYS_PATH"] + "/" + context->server->config["SETUP_TEMPLATE"];
	return(
		compiler_unit_source_signature(su->file_name, allow_recent_source_stat) + ":" +
		gen_sha1(file_get_contents(setup_template)) + ":" +
		std::to_string(UCE_UNIT_ABI_VERSION) + ":" +
		std::to_string(UCE_WASM_CORE_ABI_VERSION)
	);
}

String compiler_unit_build_token()
{
	return(
		std::to_string(getpid()) + ":" +
		std::to_string((u64)(time_precise() * 1000000.0))
	);
}

String compiler_unit_metadata_text(Request* context, SharedUnit* su, String input_signature = "")
{
	if(input_signature == "")
		input_signature = compiler_unit_input_signature(context, su);
	return(
		"format=uce-unit-metadata-v1\n"
		"unit_abi_version=" + std::to_string(UCE_UNIT_ABI_VERSION) + "\n"
		"wasm_core_abi_version=" + std::to_string(UCE_WASM_CORE_ABI_VERSION) + "\n"
		"source_path=" + su->file_name + "\n"
		"input_signature=" + input_signature + "\n"
		"build_token=" + compiler_unit_build_token() + "\n"
	);
}

String compiler_cached_wasm_path(String wasm_path)
{
	if(wasm_path.size() >= 5 && wasm_path.rfind(".wasm", wasm_path.size() - 5) == wasm_path.size() - 5)
		return(wasm_path.substr(0, wasm_path.size() - 5) + ".cwasm");
	return(wasm_path + ".cwasm");
}

String compiler_source_map_path(String wasm_path)
{
	return(wasm_path + ".source-map");
}

static bool compiler_publish_staged_artifacts(SharedUnit* su, String staged_pre_name, String staged_api_name,
	String staged_wasm_name, String staged_map_name, String staged_meta_name, CompilerDeadline* deadline, String& error)
{
	struct Artifact
	{
		String staged;
		String canonical;
		String previous;
		bool remove_only = false;
		bool existed = false;
		bool published = false;
	};
	std::vector<Artifact> artifacts = {
		{ staged_pre_name, compiler_generated_cpp_path(su), staged_pre_name + ".previous" },
		{ staged_api_name, su->api_file_name, staged_api_name + ".previous" },
		{ staged_map_name, compiler_source_map_path(su->wasm_name), staged_map_name + ".previous" },
		{ "", compiler_cached_wasm_path(su->wasm_name), staged_wasm_name + ".cached.previous", true },
		{ staged_wasm_name, su->wasm_name, staged_wasm_name + ".previous" },
		{ "", su->compile_output_file_name, staged_wasm_name + ".compile.previous", true },
		{ "", su->wasm_check_file_name, staged_wasm_name + ".check.previous", true },
		{ staged_meta_name, su->meta_file_name, staged_meta_name + ".previous" }
	};
	auto rollback = [&]() {
		for(auto it = artifacts.rbegin(); it != artifacts.rend(); ++it)
		{
			if(it->published)
			{
				if(it->existed)
				{
					if(rename(it->previous.c_str(), it->canonical.c_str()) != 0)
						error += "\ncould not restore previous bounded compile artifact " + it->canonical + ": " + String(std::strerror(errno));
				}
				else
					file_unlink(it->canonical);
			}
			file_unlink(it->previous);
		}
	};
	auto copy_previous = [&](const Artifact& artifact, int link_error) {
		struct stat path_info;
		if(lstat(artifact.canonical.c_str(), &path_info) != 0 || !S_ISREG(path_info.st_mode))
		{
			error = "refusing non-regular bounded compile artifact " + artifact.canonical;
			return(false);
		}
		int input = open(artifact.canonical.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if(input < 0)
		{
			error = "could not read previous bounded compile artifact " + artifact.canonical +
				" after hard-link failure " + String(std::strerror(link_error)) + ": " + std::strerror(errno);
			return(false);
		}
		struct stat source_info;
		if(fstat(input, &source_info) != 0)
		{
			error = "could not inspect previous bounded compile artifact " + artifact.canonical + ": " + std::strerror(errno);
			close(input);
			return(false);
		}
		if(source_info.st_dev != path_info.st_dev || source_info.st_ino != path_info.st_ino || !S_ISREG(source_info.st_mode))
		{
			error = "bounded compile artifact changed while preserving " + artifact.canonical;
			close(input);
			return(false);
		}
		int output = open(artifact.previous.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, source_info.st_mode & 07777);
		if(output < 0)
		{
			error = "could not create bounded compile rollback copy " + artifact.previous + ": " + std::strerror(errno);
			close(input);
			return(false);
		}
		bool copied = true;
		char buffer[65536];
		while(copied)
		{
			if(deadline && deadline->expire_if_needed())
			{
				error = "bounded compile deadline expired while copying previous artifact " + artifact.canonical;
				copied = false;
				break;
			}
			ssize_t got = read(input, buffer, sizeof(buffer));
			if(got == 0)
				break;
			if(got < 0)
			{
				if(errno == EINTR)
					continue;
				error = "could not read previous bounded compile artifact " + artifact.canonical + ": " + std::strerror(errno);
				copied = false;
				break;
			}
			ssize_t written = 0;
			while(written < got)
			{
				if(deadline && deadline->expire_if_needed())
				{
					error = "bounded compile deadline expired while copying previous artifact " + artifact.canonical;
					copied = false;
					break;
				}
				ssize_t amount = write(output, buffer + written, got - written);
				if(amount < 0 && errno == EINTR)
					continue;
				if(amount <= 0)
				{
					error = "could not copy previous bounded compile artifact " + artifact.canonical + ": " + std::strerror(errno);
					copied = false;
					break;
				}
				written += amount;
			}
		}
		if(copied && fchmod(output, source_info.st_mode & 07777) != 0)
		{
			error = "could not preserve mode for bounded compile artifact " + artifact.canonical + ": " + std::strerror(errno);
			copied = false;
		}
		if(close(output) != 0 && copied)
		{
			error = "could not close bounded compile rollback copy " + artifact.previous + ": " + std::strerror(errno);
			copied = false;
		}
		close(input);
		if(!copied)
			file_unlink(artifact.previous);
		return(copied);
	};
	for(auto& artifact : artifacts)
	{
		if(deadline && deadline->expire_if_needed())
		{
			error = "bounded compile deadline expired while preserving prior artifacts";
			for(auto& cleanup : artifacts)
				file_unlink(cleanup.previous);
			return(false);
		}
		file_unlink(artifact.previous);
		artifact.existed = file_exists(artifact.canonical);
		if(artifact.existed && link(artifact.canonical.c_str(), artifact.previous.c_str()) != 0)
		{
			int link_error = errno;
			if(link_error != EPERM && link_error != EACCES && link_error != EMLINK && link_error != EXDEV)
			{
				error = "could not preserve previous bounded compile artifact " + artifact.canonical + ": " + std::strerror(link_error);
				for(auto& cleanup : artifacts)
					file_unlink(cleanup.previous);
				return(false);
			}
			if(!copy_previous(artifact, link_error))
			{
				for(auto& cleanup : artifacts)
					file_unlink(cleanup.previous);
				return(false);
			}
		}
	}
	for(auto& artifact : artifacts)
	{
		if(deadline && deadline->expire_if_needed())
		{
			error = "bounded compile deadline expired during artifact publication";
			rollback();
			return(false);
		}
		if(artifact.remove_only)
			file_unlink(artifact.canonical);
		else if(rename(artifact.staged.c_str(), artifact.canonical.c_str()) != 0)
		{
			error = "could not publish bounded compile artifact " + artifact.canonical + ": " + std::strerror(errno);
			rollback();
			return(false);
		}
		artifact.published = true;
	}
	if(deadline && deadline->expire_if_needed())
	{
		error = "bounded compile deadline expired during artifact publication";
		rollback();
		return(false);
	}
	for(auto& artifact : artifacts)
		file_unlink(artifact.previous);
	return(true);
}

void compiler_unlink_unit_wasm_artifacts(SharedUnit* su)
{
	file_unlink(su->wasm_name);
	file_unlink(compiler_cached_wasm_path(su->wasm_name));
	file_unlink(compiler_source_map_path(su->wasm_name));
}

String compiler_wasm_compile_script(Request* context)
{
	String script = "scripts/compile_wasm_unit";
	if(context && context->server)
		script = first(context->server->config["WASM_COMPILE_SCRIPT"], script);
	if(script != "" && script[0] != '/' && context && context->server)
		script = path_join(context->server->config["COMPILER_SYS_PATH"], script);
	return(script);
}

SharedUnitCompileCheck shared_unit_compile_check(const SharedUnitFilesystemState& state)
{
	SharedUnitCompileCheck result;
	result.source_missing = !state.source_exists;
	result.needs_compile =
		!state.source_exists ||
		state.compiled_time == 0 ||
		state.compiled_time < state.required_time ||
		!state.metadata_exists ||
		!state.metadata_parsed ||
		!state.abi_compatible ||
		!state.input_signature_matches;
	return(result);
}

bool compiler_failure_retry_deferred(Request* context, SharedUnit* su, const SharedUnitFilesystemState& state)
{
	if(!context || !su)
		return(false);
	if(!state.compile_output_exists || state.compile_output_time == 0)
		return(false);
	if(state.source_time == 0)
		return(false);
	if(!state.metadata_exists || !state.metadata_parsed || !state.abi_compatible || !state.input_signature_matches)
		return(false);
	if(state.metadata_source_path == "" || state.metadata_source_path != su->file_name)
		return(false);
	auto required_failure_inputs_time = std::max({
		state.source_time,
		state.setup_template_time,
		state.compiler_abi_time
	});
	return(state.compile_output_time >= required_failure_inputs_time);
}

String compiler_failure_output_for_state(SharedUnit* su, const SharedUnitFilesystemState& state)
{
	if(!state.compile_output_exists)
		return("");
	auto content = trim(state.compile_output_content);
	if(content != "")
		return(content);
	if(su)
		return(trim(su->compile_error_status));
	return("");
}

void compiler_restore_persisted_failure(SharedUnit* su, const SharedUnitFilesystemState& state, String status = "compile_error")
{
	if(!su)
		return;
	auto message = compiler_failure_output_for_state(su, state);
	if(message == "")
		message = "recent compilation failed";
	su->compiler_messages = message;
	su->compile_status = status;
	su->compile_error_status = message;
	su->last_error = (state.compile_output_time != 0 ? state.compile_output_time : time());
}

time_t compiler_runtime_abi_time(Request* context)
{
	(void)context;
	// Do not force every unit stale on every server/core rebuild. The stable unit
	// ABI is tracked explicitly in compiler_unit_input_signature() via
	// UCE_UNIT_ABI_VERSION; bump that when generated-unit/runtime ABI changes.
	return(0);
}

String compiler_registry_file_name(Request* context)
{
	return(compiler_unit_bin_directory(context) + "/known-uce-files.txt");
}

String compiler_registry_lock_file_name(Request* context)
{
	return(compiler_registry_file_name(context) + ".lock");
}

String compiler_priority_file_name(Request* context)
{
	return(compiler_unit_bin_directory(context) + "/proactive-priority.txt");
}

String compiler_source_generation_file_name(Request* context)
{
	return(compiler_unit_bin_directory(context) + "/source-generation.txt");
}

int compiler_open_lock_file(String file_name, String purpose, bool nonblocking = false)
{
	(void)purpose;
	auto lock_dir = dirname(file_name);
	if(lock_dir != "")
		mkdir(lock_dir);
	int fdlock = open(file_name.c_str(), O_RDWR | O_CREAT, 0666);
	if(fdlock == -1 && (errno == EACCES || errno == EPERM))
		fdlock = open(file_name.c_str(), O_RDONLY | O_CLOEXEC);
	if(fdlock == -1)
	{
		printf("(!) Could not open lock file %s\n", file_name.c_str());
		return(fdlock);
	}
	fcntl(fdlock, F_SETFD, FD_CLOEXEC);
	if(flock(fdlock, LOCK_EX | (nonblocking ? LOCK_NB : 0)) != 0)
	{
		if(nonblocking && (errno == EWOULDBLOCK || errno == EAGAIN))
		{
			close(fdlock);
			return(-2);
		}
		close(fdlock);
		printf("(!) Could not lock file %s\n", file_name.c_str());
		return(-1);
	}
	return(fdlock);
}

int compiler_open_lock_file_bounded(String file_name, String purpose, CompilerDeadline* deadline)
{
	if(!deadline)
		return(compiler_open_lock_file(file_name, purpose));
	auto lock_dir = dirname(file_name);
	if(lock_dir != "")
		mkdir(lock_dir);
	int fdlock = open(file_name.c_str(), O_RDWR | O_CREAT, 0666);
	if(fdlock == -1 && (errno == EACCES || errno == EPERM))
		fdlock = open(file_name.c_str(), O_RDONLY | O_CLOEXEC);
	if(fdlock == -1)
		return(-1);
	fcntl(fdlock, F_SETFD, FD_CLOEXEC);
	while(true)
	{
		if(flock(fdlock, LOCK_EX | LOCK_NB) == 0)
			return(fdlock);
		if(errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
		{
			close(fdlock);
			return(-1);
		}
		if(deadline->expire_if_needed())
		{
			close(fdlock);
			return(-2);
		}
		usleep(1000);
	}
}

void compiler_close_lock_file(int fdlock)
{
	if(fdlock == -1)
		return;
	flock(fdlock, LOCK_UN);
	close(fdlock);
}

static void compiler_publish_source_generation(String file_name)
{
	String staged_file_name = file_name + ".stage-" + std::to_string((u64)getpid());
	file_unlink(staged_file_name);
	if(file_put_contents(staged_file_name, std::to_string(getpid()) + ":" + std::to_string((u64)(time_precise() * 1000000.0)) + "\n") &&
		rename(staged_file_name.c_str(), file_name.c_str()) != 0)
		printf("(!) Could not publish %s: %s\n", file_name.c_str(), std::strerror(errno));
	file_unlink(staged_file_name);
}

static void compiler_mark_source_generation_nonblocking(Request* context)
{
	if(!context || !context->server)
		return;
	String file_name = compiler_source_generation_file_name(context);
	int fdlock = compiler_open_lock_file(file_name + ".lock", "source-generation", true);
	if(fdlock < 0)
		return;
	compiler_publish_source_generation(file_name);
	compiler_close_lock_file(fdlock);
}

String compiler_normalize_unit_path(Request* context, String file_name)
{
	file_name = trim(file_name);
	if(file_name == "")
		return("");
	if(file_name[0] != '/')
		file_name = expand_path(file_name, context->server->config["COMPILER_SYS_PATH"]);
	String canonical = path_real(file_name);
	if(canonical != "")
		return(canonical);
	return(file_name);
}

bool compiler_is_known_unit_file(String file_name)
{
	return(file_name.length() >= 4 && file_name.substr(file_name.length() - 4) == ".uce");
}

StringList compiler_normalize_unit_list(Request* context, StringList files)
{
	StringList normalized;
	for(auto& file_name : files)
	{
		auto normalized_name = compiler_normalize_unit_path(context, trim(file_name));
		if(normalized_name != "" && compiler_is_known_unit_file(normalized_name))
			normalized.push_back(normalized_name);
	}
	std::sort(normalized.begin(), normalized.end());
	normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
	return(normalized);
}

StringList compiler_read_known_units_unlocked(Request* context)
{
	auto content = trim(file_get_contents(compiler_registry_file_name(context)));
	if(content == "")
		return(StringList());
	return(compiler_normalize_unit_list(context, split(content, "\n")));
}

void compiler_write_known_units_unlocked(Request* context, StringList files)
{
	files = compiler_normalize_unit_list(context, files);
	if(files.size() == 0)
	{
		file_put_contents(compiler_registry_file_name(context), "");
		return;
	}
	file_put_contents(compiler_registry_file_name(context), join(files, "\n") + "\n");
}

template <typename TCallback>
auto compiler_with_registry_lock(Request* context, TCallback callback) -> decltype(callback())
{
	auto lock_file_name = compiler_registry_lock_file_name(context);
	int fdlock = compiler_open_lock_file(lock_file_name, "compiler-registry");
	if(fdlock == -1)
		throw std::runtime_error("could not open compiler registry lock: " + lock_file_name);
	try
	{
		auto result = callback();
		compiler_close_lock_file(fdlock);
		return(result);
	}
	catch(...)
	{
		compiler_close_lock_file(fdlock);
		throw;
	}
}

bool compiler_has_known_unit_cached(Request* context, String file_name)
{
	if(!context)
		return(false);
	return(compiler_with_registry_lock(context, [&]() {
		auto files = compiler_read_known_units_unlocked(context);
		return(std::find(files.begin(), files.end(), file_name) != files.end());
	}));
}

SharedUnitFilesystemState inspect_shared_unit_filesystem(Request* context, SharedUnit* su, bool allow_recent_source_stat = false)
{
	SharedUnitFilesystemState state;
	struct stat source_stat;
	if(stat(su->file_name.c_str(), &source_stat) == 0)
	{
		state.source_exists = true;
		state.source_time = source_stat.st_mtime;
	}
	state.setup_template_time = file_mtime(
		context->server->config["COMPILER_SYS_PATH"] + "/" +
		context->server->config["SETUP_TEMPLATE"]
	);
	state.compiler_abi_time = compiler_runtime_abi_time(context);
	state.metadata_time = file_mtime(su->meta_file_name);
	state.compile_output_time = file_mtime(su->compile_output_file_name);
	state.current_input_signature = compiler_unit_input_signature(context, su, allow_recent_source_stat, state.source_exists);
	state.metadata_exists = (state.metadata_time != 0);
	state.compile_output_exists = (state.compile_output_time != 0);
	if(state.metadata_exists)
	{
		state.metadata_content = file_get_contents(su->meta_file_name);
		auto metadata = compiler_parse_unit_metadata(state.metadata_content);
		auto abi_it = metadata.find("unit_abi_version");
		if(abi_it != metadata.end() && compiler_is_u64_string(abi_it->second))
		{
			state.metadata_abi_version = (u64)atoll(abi_it->second.c_str());
			state.metadata_parsed = true;
		}
		auto input_it = metadata.find("input_signature");
		if(input_it != metadata.end())
			state.metadata_input_signature = input_it->second;
		auto core_abi_it = metadata.find("wasm_core_abi_version");
		if(core_abi_it != metadata.end() && compiler_is_u64_string(core_abi_it->second))
			state.metadata_wasm_core_abi_version = (u64)atoll(core_abi_it->second.c_str());
		auto source_it = metadata.find("source_path");
		if(source_it != metadata.end())
			state.metadata_source_path = source_it->second;
		auto build_it = metadata.find("build_token");
		if(build_it != metadata.end())
			state.metadata_build_token = build_it->second;
	}
	if(state.compile_output_exists)
		state.compile_output_content = file_get_contents(su->compile_output_file_name);
	state.abi_compatible = (
		state.metadata_parsed &&
		state.metadata_abi_version == state.runtime_abi_version &&
		state.metadata_wasm_core_abi_version == state.runtime_wasm_core_abi_version
	);
	state.input_signature_matches = (
		state.metadata_parsed &&
		state.metadata_input_signature != "" &&
		state.current_input_signature != "" &&
		state.metadata_input_signature == state.current_input_signature
	);
	// Source and setup-template freshness is content-addressed by the input
	// signature. Keep the executable timestamp as the independent codegen
	// boundary, but do not rebuild byte-identical sources after a touch/restore.
	state.required_time = state.compiler_abi_time;
	// Native .so execution has been removed; compile freshness is the wasm
	// side-module freshness.
	state.compiled_time = file_mtime(su->wasm_name);
	return(state);
}

void compiler_record_observed_filesystem_state(SharedUnit* su, const SharedUnitFilesystemState& state)
{
	if(!su)
		return;
	su->observed_compiled_time = state.compiled_time;
	su->observed_metadata_content = state.metadata_content;
}

bool shared_unit_cache_is_stale(Request* context, SharedUnit* su)
{
	if(!su)
		return(true);

	auto state = inspect_shared_unit_filesystem(context, su);
	if(shared_unit_compile_check(state).needs_compile)
		return(true);
	if(state.compiled_time != su->observed_compiled_time)
		return(true);
	if(state.metadata_content != su->observed_metadata_content)
		return(true);
	return(false);
}

void release_shared_unit_cache_entry(Request* context, String file_name)
{
	auto it = context->server->units.find(file_name);
	if(it == context->server->units.end())
		return;
	delete it->second;
	context->server->units.erase(it);
}

SharedUnit* compiler_cached_unit(Request* context, String file_name)
{
	auto it = context->server->units.find(file_name);
	if(it == context->server->units.end())
		return(0);
	return(it->second);
}

bool compiler_cached_unit_is_reusable(Request* context, SharedUnit* su, bool force_recompile)
{
	return(
		!force_recompile &&
		su &&
		!shared_unit_cache_is_stale(context, su)
	);
}

SharedUnit* compiler_reusable_cached_unit(Request* context, String file_name, bool force_recompile)
{
	auto su = compiler_cached_unit(context, file_name);
	if(compiler_cached_unit_is_reusable(context, su, force_recompile))
		return(su);
	return(0);
}

void compiler_release_cached_unit_if_needed(Request* context, String file_name, bool force_recompile)
{
	auto su = compiler_cached_unit(context, file_name);
	if(!su)
		return;
	if(force_recompile || shared_unit_cache_is_stale(context, su))
		release_shared_unit_cache_entry(context, file_name);
}

String compiler_current_unit_path(Request* context)
{
	if(!context)
		return("");
	return(first(
		context->resources.current_unit_file,
		context->params["SCRIPT_FILENAME"]
	));
}

String compiler_resolve_unit_path(Request* context, String file_name, String current_path = "")
{
	if(!context)
		return("");

	file_name = trim(file_name);
	if(file_name == "")
		file_name = compiler_current_unit_path(context);

	if(file_name == "")
		return("");

	if(current_path == "")
	{
		auto current_unit_file = compiler_current_unit_path(context);
		if(current_unit_file != "")
			current_path = dirname(current_unit_file);
		else
			current_path = cwd_get();
	}

	if(file_name[0] != '/')
		file_name = expand_path(file_name, current_path);

	return(compiler_normalize_unit_path(context, file_name));
}

f64 compiler_average(f64 total, u64 count)
{
	if(count == 0)
		return(0);
	return(total / (f64)count);
}

void compiler_record_compile_result(SharedUnit* su, f64 duration, bool success, String status, String error_status = "")
{
	su->compile_count += 1;
	su->last_compile_duration = duration;
	su->total_compile_duration += duration;
	if(su->best_compile_duration == 0 || duration < su->best_compile_duration)
		su->best_compile_duration = duration;
	if(duration > su->worst_compile_duration)
		su->worst_compile_duration = duration;

	if(success)
	{
		su->compile_success_count += 1;
		su->compile_status = status;
		su->compile_error_status = "";
	}
	else
	{
		su->compile_failure_count += 1;
		su->compile_status = status;
		su->compile_error_status = error_status;
		su->last_error = time();
	}
}

String compiler_error_status(SharedUnit* su)
{
	if(!su)
		return("");
	if(trim(su->compile_error_status) != "")
		return(su->compile_error_status);
	return(su->runtime_error_status);
}

String compiler_status_from_filesystem(const SharedUnitFilesystemState& state, SharedUnit* su = 0)
{
	auto compile_check = shared_unit_compile_check(state);
	if(!state.source_exists)
		return("missing_source");
	if(state.compiled_time == 0 || !state.metadata_exists)
		return("not_compiled");
	if(compile_check.needs_compile)
		return("stale");
	return("compiled");
}

StringList compiler_unit_exports(SharedUnit* su)
{
	StringList exports;
	if(su && su->api_declarations.size() > 0)
		exports = su->api_declarations;
	for(auto it = exports.begin(); it != exports.end();)
	{
		*it = trim(*it);
		if(*it == "")
		{
			it = exports.erase(it);
			continue;
		}
		++it;
	}
	return(exports);
}

String compiler_unit_exports_text(SharedUnit* su)
{
	if(!su || su->api_file_name == "")
		return("");
	return(trim(file_get_contents(su->api_file_name)));
}

void compiler_tree_push_string(DValue& tree, String value)
{
	DValue item;
	item = value;
	tree.push(item);
}

void compiler_tree_push_strings(DValue& tree, StringList values)
{
	tree.set_array();
	for(auto& value : values)
		compiler_tree_push_string(tree, value);
}

void compiler_tree_set_bool(DValue& tree, String key, bool value)
{
	tree[key].set_bool(value);
}

}

String preprocess_shared_unit(Request* context, SharedUnit* su)
{
	String content = file_get_contents(su->file_name);
	return(compiler_preprocess_source(context, su, content));
}

String compiler_generated_cpp_path(Request* context, String source_file)
{
	if(!context || !context->server || source_file == "")
		return("");
	return(compiler_unit_bin_directory(context) + dirname(source_file) + "/" + basename(source_file) + ".cpp");
}

String compiler_unit_bin_directory(Request* context)
{
	if(!context || !context->server)
		return("");
	return(path_join(
		context->server->config["BIN_DIRECTORY"],
		"units-c" + std::to_string(UCE_UNIT_ABI_VERSION) +
		"-w" + std::to_string(UCE_WASM_CORE_ABI_VERSION)
	));
}

String compiler_unit_wasm_path(Request* context, String source_file)
{
	return(compiler_unit_bin_directory(context) + source_file + ".wasm");
}

String compiler_generated_cpp_path(SharedUnit* su)
{
	if(!su)
		return("");
	return(su->pre_path + "/" + su->pre_file_name);
}

void setup_unit_paths(Request* context, SharedUnit* su, String file_name)
{
	su->file_name = file_name;

	if(su->src_path.length() > 0) // we did this already
		return;

	su->src_path = dirname(file_name);
	su->bin_path = compiler_unit_bin_directory(context) + su->src_path;
	su->pre_path = compiler_unit_bin_directory(context) + su->src_path;

	su->src_file_name = basename(file_name);
	su->wasm_file_name = su->src_file_name + ".wasm";
	su->pre_file_name = su->src_file_name + ".cpp";

	su->wasm_name = su->bin_path + "/" + su->wasm_file_name;
	su->wasm_check_file_name = su->bin_path + "/" + su->src_file_name + ".wasm-check.txt";
	su->api_file_name = su->bin_path + "/" + su->src_file_name + ".exports.txt";
	su->meta_file_name = su->bin_path + "/" + su->src_file_name + ".meta.txt";
	su->compile_output_file_name = su->bin_path + "/" + su->src_file_name + ".compile.txt";
	//su->setup_file_name = su->bin_path + "/" + su->src_file_name + ".setup.h";
}

/*String compile_setup_file(Request* context, SharedUnit* su)
{
	String result =
		String("#ifndef UCE_LIB_INCLUDED\n") +
		"#define UCE_LIB_INCLUDED\n" +
		"#include \"uce_lib.h\" \n"+
		file_get_contents(
			context->server->config["COMPILER_SYS_PATH"] + "/" + context->server->config["SETUP_TEMPLATE"]) +
		"#endif \n";
	return(result);
}*/

s64 compiler_first_error_line_for_path(String messages, String path)
{
	if(path == "")
		return(-1);
	String needle = path + ":";
	auto pos = messages.find(needle);
	if(pos == String::npos)
		return(-1);
	pos += needle.length();
	String digits;
	while(pos < messages.length() && isdigit((unsigned char)messages[pos]))
	{
		digits.append(1, messages[pos]);
		pos += 1;
	}
	if(digits == "")
		return(-1);
	return(int_val(digits));
}

String compiler_source_excerpt(String file_name, s64 line_number, u64 radius = 3)
{
	if(file_name == "" || line_number <= 0 || !file_exists(file_name))
		return("");
	auto lines = split(file_get_contents(file_name), "\n");
	if(lines.size() == 0)
		return("");
	s64 start = line_number - (s64)radius;
	if(start < 1)
		start = 1;
	s64 end = line_number + (s64)radius;
	if(end > (s64)lines.size())
		end = lines.size();
	String result;
	for(s64 i = start; i <= end; i++)
	{
		String marker = (i == line_number ? "> " : "  ");
		result += marker + std::to_string((u64)i) + " | " + lines[i - 1] + "\n";
	}
	return(result);
}

String compiler_format_compile_failure(Request* context, SharedUnit* su, String raw_messages)
{
	bool expected_cli_failure =
		context &&
		context->resources.is_cli &&
		first(context->get["__uce_expected_compile_failure"], context->post["__uce_expected_compile_failure"]) == "1";
	String generated_file = compiler_generated_cpp_path(su);
	String result = expected_cli_failure ? "UCE expected compile error\n" : "UCE compile error\n";
	result += "Source: " + su->file_name + "\n";
	result += "Generated C++: " + generated_file + "\n";
	result += "Compile output: " + su->compile_output_file_name + "\n";

	s64 source_line = compiler_first_error_line_for_path(raw_messages, su->file_name);
	String excerpt = compiler_source_excerpt(su->file_name, source_line);
	if(excerpt != "")
		result += "\nSource excerpt:\n" + excerpt;
	else
	{
		s64 generated_line = compiler_first_error_line_for_path(raw_messages, generated_file);
		String generated_excerpt = compiler_source_excerpt(generated_file, generated_line);
		if(generated_excerpt != "")
			result += "\nGenerated C++ excerpt:\n" + generated_excerpt;
	}

	result += "\nCompiler output:\n" + trim(raw_messages) + "\n";
	return(result);
}

String compiler_format_source_read_failure(Request* context, SharedUnit* su, String raw_messages)
{
	bool expected_cli_failure =
		context &&
		context->resources.is_cli &&
		first(context->get["__uce_expected_source_read_failure"], context->post["__uce_expected_source_read_failure"]) == "1";
	String result = expected_cli_failure ? "UCE expected source read failure\n" : "UCE source read failure\n";
	result += "Source: " + su->file_name + "\n";
	result += "Compile output: " + su->compile_output_file_name + "\n";
	result += "\nMessage:\n" + trim(raw_messages) + "\n";
	return(result);
}

void compile_shared_unit_bounded(Request* context, SharedUnit* su, CompilerDeadline* deadline)
{
	f64 comp_start = time_precise();
	bool preserve_last_known_good = compiler_preserve_last_known_good(context, su->file_name);
	bool stage_artifacts = deadline || preserve_last_known_good;

	if(!file_exists(su->file_name))
	{
		su->compiler_messages = "source file not found (" + su->file_name + ")";
		file_put_contents(su->compile_output_file_name, su->compiler_messages + "\n");
		if(!deadline)
			compiler_untrack_known_unit(context, su->file_name);
		compiler_record_compile_result(su, time_precise() - comp_start, false, "missing_source", su->compiler_messages);
		if(deadline)
			compiler_mark_source_generation_nonblocking(context);
		else
			compiler_mark_source_generation(context);
		return;
	}
	struct stat source_info;
	int read_error = 0;
	bool source_readable = stat(su->file_name.c_str(), &source_info) == 0 && compiler_source_readable(su->file_name, source_info, &read_error);
	if(!source_readable && read_error == 0)
		read_error = errno == 0 ? ENOENT : errno;
	if(!source_readable)
	{
		su->compiler_messages = "source file is not readable (" + su->file_name + "): " + std::strerror(read_error);
		file_put_contents(su->compile_output_file_name, su->compiler_messages + "\n");
		file_put_contents(su->wasm_check_file_name, su->compiler_messages + "\n");
		file_put_contents(su->meta_file_name, compiler_unit_metadata_text(context, su));
		if(!preserve_last_known_good)
			compiler_unlink_unit_wasm_artifacts(su);
		compiler_record_compile_result(su, time_precise() - comp_start, false, "compile_error", su->compiler_messages);
		printf("%s \n", compiler_format_source_read_failure(context, su, su->compiler_messages).c_str());
		if(deadline)
			compiler_mark_source_generation_nonblocking(context);
		else
			compiler_mark_source_generation(context);
		return;
	}

	mkdir(su->pre_path);
	String compiled_input_signature;
	String staged_wasm_file_name;
	String staged_wasm_name;
	String staged_map_name;
	String staged_pre_file_name;
	String staged_pre_name;
	String staged_api_name;
	String staged_meta_name;
	bool publication_failed = false;
	if(stage_artifacts)
	{
		u64 stage_id = compiler_invocation_stage_counter.fetch_add(1, std::memory_order_relaxed) + 1;
		String stage_suffix = ".invocation-" + std::to_string((u64)getpid()) + "-" + std::to_string(stage_id);
		staged_wasm_file_name = su->src_file_name + stage_suffix + ".wasm";
		staged_wasm_name = su->bin_path + "/" + staged_wasm_file_name;
		staged_map_name = compiler_source_map_path(staged_wasm_name);
		staged_pre_file_name = su->src_file_name + stage_suffix + ".cpp";
		staged_pre_name = su->pre_path + "/" + staged_pre_file_name;
		staged_api_name = su->bin_path + "/" + su->src_file_name + stage_suffix + ".exports.txt";
		staged_meta_name = su->bin_path + "/" + su->src_file_name + stage_suffix + ".meta.txt";
		file_unlink(staged_wasm_name);
		file_unlink(staged_map_name);
		file_unlink(staged_pre_name);
		file_unlink(staged_api_name);
		file_unlink(staged_meta_name);
	}
	for(u64 attempt = 0; attempt < 2; attempt++)
	{
		if(deadline && deadline->expire_if_needed())
			break;
		su->api_declarations.clear();
		compiled_input_signature = compiler_unit_input_signature(context, su);
		String generated_source;
		{
			CompilerDeadlineScope deadline_scope(deadline);
			generated_source = preprocess_shared_unit(context, su);
		}
		if(deadline && deadline->dependency_failure)
		{
			su->compiler_messages = first(deadline->dependency_error, "transitive dependency compilation failed");
			break;
		}
		if(deadline && (deadline->timed_out || deadline->operational_failure))
			break;
		if(!file_put_contents(stage_artifacts ? staged_pre_name : su->pre_path + "/" + su->pre_file_name, generated_source) ||
			!file_put_contents(stage_artifacts ? staged_api_name : su->api_file_name, join(su->api_declarations, "\n")))
		{
			su->compiler_messages = "could not write generated bounded compile inputs";
			break;
		}

		String compile_command = shell_escape(compiler_wasm_compile_script(context))+" "+
			shell_escape(su->src_path)+" "+
			shell_escape(su->bin_path)+" "+
			shell_escape(su->file_name)+" "+
			shell_escape(stage_artifacts ? staged_pre_file_name : su->pre_file_name)+" "+
			shell_escape(stage_artifacts ? staged_wasm_file_name : su->wasm_file_name)+" "+
			shell_escape(compiler_unit_bin_directory(context));
		if(deadline)
		{
			u64 remaining_ms = deadline->remaining_ms();
			if(remaining_ms == 0)
			{
				deadline->timed_out = true;
				break;
			}
			DValue execution = process_exec(compile_command + " 2>&1", "", StringMap(), remaining_ms, 1024 * 1024);
			if(execution["timed_out"].to_bool())
			{
				deadline->timed_out = true;
				break;
			}
			su->compiler_messages = trim(execution["stdout"].to_string() + execution["stderr"].to_string());
			if(execution["output_truncated"].to_bool())
				su->compiler_messages += (su->compiler_messages == "" ? "" : "\n") + String("compiler output truncated at 1048576 bytes");
			if(execution["exit_code"].to_s64(-1) != 0 && su->compiler_messages == "")
				su->compiler_messages = "wasm compile script exited with status " + std::to_string(execution["exit_code"].to_s64(-1));
		}
		else
			su->compiler_messages = trim(shell_exec(compile_command));

		String compiled_wasm_name = stage_artifacts ? staged_wasm_name : su->wasm_name;
		if(su->compiler_messages.length() == 0 && !file_exists(compiled_wasm_name))
			su->compiler_messages = "wasm compile script completed without creating " + compiled_wasm_name;
		if(su->compiler_messages.length() > 0)
			break;
		String current_input_signature = compiler_unit_input_signature(context, su);
		if(current_input_signature == compiled_input_signature)
		{
			if(stage_artifacts)
			{
				if(deadline && deadline->expire_if_needed())
					break;
				String source_map = file_get_contents(staged_map_name);
				if(source_map == "")
					su->compiler_messages = "bounded wasm compile did not create a source map";
				else
			{
					if(!file_put_contents(staged_map_name, replace(source_map, staged_pre_name, compiler_generated_cpp_path(su))) ||
						!file_put_contents(staged_meta_name, compiler_unit_metadata_text(context, su, compiled_input_signature)))
					{
						su->compiler_messages = "could not stage bounded compile metadata";
						publication_failed = true;
					}
					else
						publication_failed = !compiler_publish_staged_artifacts(su, staged_pre_name, staged_api_name,
							staged_wasm_name, staged_map_name, staged_meta_name, deadline, su->compiler_messages);
				}
			}
			break;
		}
		if(stage_artifacts)
		{
			file_unlink(staged_wasm_name);
			file_unlink(staged_map_name);
			file_unlink(staged_pre_name);
			file_unlink(staged_api_name);
			file_unlink(staged_meta_name);
		}
		else
			compiler_unlink_unit_wasm_artifacts(su);
		if(attempt == 1)
			su->compiler_messages = "source changed during wasm compile; retry required";
	}
	if(deadline && deadline->timed_out)
	{
		file_unlink(staged_wasm_name);
		file_unlink(staged_map_name);
		file_unlink(staged_pre_name);
		file_unlink(staged_api_name);
		file_unlink(staged_meta_name);
		su->compiler_messages = "UCE_INVOCATION_TIMEOUT: unit compilation exceeded the invocation deadline";
		compiler_record_compile_result(su, time_precise() - comp_start, false, "compile_timeout", su->compiler_messages);
		return;
	}
	if(deadline && deadline->operational_failure)
	{
		file_unlink(staged_wasm_name);
		file_unlink(staged_map_name);
		file_unlink(staged_pre_name);
		file_unlink(staged_api_name);
		file_unlink(staged_meta_name);
		su->compiler_messages = first(deadline->operational_error, "transitive dependency compilation failed");
		compiler_record_compile_result(su, time_precise() - comp_start, false, "dependency_error", su->compiler_messages);
		return;
	}
	if(publication_failed)
	{
		file_unlink(staged_wasm_name);
		file_unlink(staged_map_name);
		file_unlink(staged_pre_name);
		file_unlink(staged_api_name);
		file_unlink(staged_meta_name);
		compiler_record_compile_result(su, time_precise() - comp_start, false, "publish_error", su->compiler_messages);
		if(deadline)
		{
			deadline->operational_failure = true;
			deadline->operational_error = su->compiler_messages;
		}
		return;
	}
	if(stage_artifacts)
	{
		file_unlink(staged_wasm_name);
		file_unlink(staged_map_name);
		file_unlink(staged_pre_name);
		file_unlink(staged_api_name);
		file_unlink(staged_meta_name);
	}

	if(su->compiler_messages.length() > 0)
	{
		String raw_messages = su->compiler_messages;
		file_put_contents(su->compile_output_file_name, raw_messages + "\n");
		file_put_contents(su->wasm_check_file_name, raw_messages + "\n");
		file_put_contents(su->meta_file_name, compiler_unit_metadata_text(context, su, compiled_input_signature));
		if(!publication_failed && !preserve_last_known_good)
			compiler_unlink_unit_wasm_artifacts(su);
		compiler_record_compile_result(su, time_precise() - comp_start, false, "compile_error", raw_messages);
		printf("%s \n", compiler_format_compile_failure(context, su, raw_messages).c_str());
	}
	else
	{
		su->last_compiled = file_mtime(su->wasm_name);
		if(!deadline)
		{
			file_unlink(compiler_cached_wasm_path(su->wasm_name));
			file_put_contents(su->meta_file_name, compiler_unit_metadata_text(context, su, compiled_input_signature));
		}
		file_unlink(su->compile_output_file_name);
		file_unlink(su->wasm_check_file_name);
		compiler_record_compile_result(
			su,
			time_precise() - comp_start,
			file_exists(su->wasm_name),
			(file_exists(su->wasm_name) ? "compiled" : "compile_error"),
			compiler_error_status(su)
		);
		printf("(i) compiled wasm unit %s in %f s\n",
			(su->pre_path + "/" + su->pre_file_name).c_str(),
			time_precise() - comp_start);
	}
	if(deadline)
		compiler_mark_source_generation_nonblocking(context);
	else
		compiler_mark_source_generation(context);
}

void compile_shared_unit(Request* context, SharedUnit* su)
{
	compile_shared_unit_bounded(context, su, 0);
}

SharedUnit* compiler_get_shared_unit_internal(Request* context, String file_name, bool force_recompile, bool retry_current_failure = false, CompilerDeadline* deadline = 0)
{
	file_name = compiler_normalize_unit_path(context, file_name);
	bool bypass_cached_result = force_recompile || retry_current_failure;

	auto cached = compiler_reusable_cached_unit(context, file_name, bypass_cached_result);
	if(cached)
		return(cached);

	compiler_release_cached_unit_if_needed(context, file_name, bypass_cached_result);

	SharedUnit* su = new SharedUnit();
	setup_unit_paths(context, su, file_name);

	bool can_serve_stale = !bypass_cached_result && compiler_unit_can_serve_stale_artifact(context, file_name);
	int fdlock = deadline ? compiler_open_lock_file_bounded(su->wasm_name + ".lock", "shared-unit:" + file_name, deadline) :
		compiler_open_lock_file(su->wasm_name + ".lock", "shared-unit:" + file_name, can_serve_stale);
	if(deadline && fdlock == -2)
	{
		delete su;
		return(0);
	}
	if(fdlock == -2 && file_exists(su->wasm_name))
	{
		auto state = inspect_shared_unit_filesystem(context, su);
		su->api_declarations = split(file_get_contents(su->api_file_name), "\n");
		su->last_compiled = state.compiled_time;
		su->compile_status = "rebuilding";
		compiler_record_observed_filesystem_state(su, state);
		context->server->units[file_name] = su;
		return(su);
	}
	if(fdlock == -2)
		fdlock = compiler_open_lock_file(su->wasm_name + ".lock", "shared-unit:" + file_name);
	if(fdlock == -1)
	{
		su->compiler_messages = "could not open compile lock";
		su->compile_status = "lock_error";
		su->compile_error_status = su->compiler_messages;
		su->last_error = time();
		context->server->units[file_name] = su;
		return(su);
	}

	cached = compiler_reusable_cached_unit(context, file_name, bypass_cached_result);
	if(cached)
	{
		compiler_close_lock_file(fdlock);
		delete su;
		return(cached);
	}

	compiler_release_cached_unit_if_needed(context, file_name, bypass_cached_result);

	auto state = inspect_shared_unit_filesystem(context, su);
	auto compile_check = shared_unit_compile_check(state);
	bool retry_deferred = compiler_failure_retry_deferred(context, su, state);
	bool jit_enabled = compiler_jit_compile_on_request_enabled(context);
	bool do_recompile = force_recompile || compile_check.needs_compile;
	if(do_recompile)
	{
		if(!force_recompile && !retry_current_failure && retry_deferred)
			compiler_restore_persisted_failure(su, state);
		else if(!force_recompile && !retry_current_failure && !jit_enabled)
		{
			compiler_restore_persisted_failure(su, state, "jit_compile_disabled");
			if(trim(su->compiler_messages) == "")
			{
				su->compiler_messages = "JIT compilation on request is disabled";
				su->compile_error_status = su->compiler_messages;
			}
		}
		else
			compile_shared_unit_bounded(context, su, deadline);
	}
	else
	{
		su->api_declarations = split(file_get_contents(su->api_file_name), "\n");
		su->last_compiled = state.compiled_time;
		su->compile_status = compiler_status_from_filesystem(state, su);
		su->compile_error_status = "";
	}

	if(deadline && (deadline->timed_out || deadline->operational_failure))
	{
		compiler_close_lock_file(fdlock);
		delete su;
		return(0);
	}

	auto observed_state = inspect_shared_unit_filesystem(context, su);
	compiler_record_observed_filesystem_state(su, observed_state);

	compiler_close_lock_file(fdlock);

	context->server->units[file_name] = su;
	return(su);
}

SharedUnit* get_shared_unit(Request* context, String file_name)
{
	return(compiler_get_shared_unit_internal(context, file_name, false));
}

SharedUnit* get_shared_unit_for_preprocess(Request* context, String file_name)
{
	if(!compiler_active_deadline)
		return(get_shared_unit(context, file_name));
	auto result = compiler_get_shared_unit_internal(context, file_name, false, false, compiler_active_deadline);
	if(result && trim(result->compiler_messages) != "")
	{
		compiler_active_deadline->dependency_failure = true;
		compiler_active_deadline->dependency_error = "transitive dependency compilation failed: " + file_name + "\n" + trim(result->compiler_messages);
		return(0);
	}
	if(!result)
		return(0);
	if(!file_exists(compiler_generated_cpp_path(result)))
	{
		compiler_active_deadline->dependency_failure = true;
		compiler_active_deadline->dependency_error = "transitive dependency generated source unavailable: " + file_name;
		return(0);
	}
	return(result);
}

SharedUnit* get_shared_unit_bounded(Request* context, String file_name, u64 timeout_ms, bool* timed_out)
{
	CompilerDeadline deadline(timeout_ms);
	auto result = compiler_get_shared_unit_internal(context, file_name, false, false, &deadline);
	if(timed_out)
		*timed_out = deadline.timed_out;
	return(result);
}

String compiler_source_generation(Request* context)
{
	if(!context || !context->server)
		return("");
	String file_name = compiler_source_generation_file_name(context);
	if(!file_exists(file_name))
		return("");
	return(trim(file_get_contents(file_name)));
}

void compiler_mark_source_generation(Request* context)
{
	if(!context || !context->server)
		return;
	String file_name = compiler_source_generation_file_name(context);
	int fdlock = compiler_open_lock_file(file_name + ".lock", "source-generation");
	if(fdlock < 0)
		return;
	compiler_publish_source_generation(file_name);
	compiler_close_lock_file(fdlock);
}

String compiler_error_page_unit(Request* context, String config_key)
{
	if(!context || !context->server)
		return("");
	String configured = trim(first(
		context->server->config[config_key],
		context->server->config[to_upper(config_key)]
	));
	if(configured == "")
		return("");
	// Resolve relative to where the server was started, not the volatile
	// per-unit working directory (which is also stale right after a fault).
	String resolved = compiler_resolve_unit_path(context, configured, process_start_directory());
	if(resolved == "" || !file_exists(resolved))
		return("");
	return(resolved);
}

bool compiler_unit_compile_pending(Request* context, String file_name)
{
	String normalized = compiler_normalize_unit_path(context, file_name);
	if(normalized == "" || !file_exists(normalized))
		return(false);
	if(compiler_reusable_cached_unit(context, normalized, false))
		return(false);

	SharedUnit probe;
	setup_unit_paths(context, &probe, normalized);
	auto state = inspect_shared_unit_filesystem(context, &probe);
	if(!shared_unit_compile_check(state).needs_compile)
		return(false);
	// A persisted, still-current failure is a compiler error, not a build in
	// progress: let the load path surface it.
	if(compiler_failure_retry_deferred(context, &probe, state))
		return(false);
	return(true);
}

bool compiler_unit_compile_in_progress(Request* context, String file_name)
{
	SharedUnit su;
	setup_unit_paths(context, &su, compiler_normalize_unit_path(context, file_name));
	int fdlock = compiler_open_lock_file(su.wasm_name + ".lock", "compile-probe", true);
	if(fdlock == -2)
		return(true);
	compiler_close_lock_file(fdlock);
	return(false);
}

bool compiler_request_can_serve_stale_artifact(Request* context)
{
	return(context && context->server && !context->resources.is_cli &&
		context->params["UCE_WS"] != "1" &&
		to_bool(context->server->config["SERVE_LAST_KNOWN_GOOD"], false) &&
		to_bool(context->server->config["PROACTIVE_COMPILE_ENABLED"], true) &&
		float_val(context->server->config["PROACTIVE_COMPILE_CHECK_INTERVAL"]) > 0);
}

bool compiler_preserve_last_known_good(Request* context, String file_name)
{
	return(
		context && context->server &&
		to_bool(context->server->config["SERVE_LAST_KNOWN_GOOD"], false) &&
		compiler_unit_can_serve_stale_artifact(context, file_name)
	);
}

bool compiler_unit_can_serve_stale_artifact(Request* context, String file_name)
{
	if(!compiler_request_can_serve_stale_artifact(context))
		return(false);
	file_name = compiler_normalize_unit_path(context, file_name);
	if(file_name == "")
		return(false);
	SharedUnit su;
	setup_unit_paths(context, &su, file_name);
	auto state = inspect_shared_unit_filesystem(context, &su, true);
	return(
		state.source_exists &&
		state.compiled_time != 0 &&
		state.metadata_exists &&
		state.metadata_parsed &&
		state.abi_compatible &&
		state.metadata_source_path == file_name
	);
}

void compiler_prioritize_unit(Request* context, String file_name)
{
	if(!compiler_request_can_serve_stale_artifact(context))
		return;
	file_name = compiler_normalize_unit_path(context, file_name);
	if(file_name == "" || file_name.find('\n') != String::npos || file_name.find('\r') != String::npos)
		return;

	String queue_file = compiler_priority_file_name(context);
	int fd = compiler_open_lock_file(queue_file + ".lock", "proactive-priority");
	if(fd < 0)
		return;
	int queue_fd = open(queue_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
	if(queue_fd >= 0)
	{
		String line = file_name + "\n";
		ssize_t offset = 0;
		while(offset < (ssize_t)line.size())
		{
			ssize_t written = write(queue_fd, line.data() + offset, line.size() - offset);
			if(written <= 0)
				break;
			offset += written;
		}
		close(queue_fd);
	}
	compiler_close_lock_file(fd);
}

StringList compiler_take_priority_units(Request* context)
{
	StringList result;
	if(!context || !context->server)
		return(result);
	String queue_file = compiler_priority_file_name(context);
	int fd = compiler_open_lock_file(queue_file + ".lock", "proactive-priority");
	if(fd < 0)
		return(result);
	if(file_exists(queue_file))
	{
		for(auto file_name : split(file_get_contents(queue_file), "\n"))
		{
			file_name = trim(file_name);
			if(file_name != "" && std::find(result.begin(), result.end(), file_name) == result.end())
				result.push_back(file_name);
		}
		file_put_contents(queue_file, "");
	}
	compiler_close_lock_file(fd);
	return(result);
}

void unit_render(String file_name)
{
	unit_render(file_name, *context);
}

void unit_render(String file_name, Request& request)
{
	request.set_status(500, "Native Unit Render Removed");
	print("native unit_render() is unavailable; units must render through the wasm backend: " + file_name);
}

String component_resolve(String name)
{
	(void)name;
	return("");
}

bool component_exists(String name)
{
	return(component_resolve(name) != "");
}

String component_error_banner(String message)
{
	return("<div class=\"banner\">" + html_escape(message) + "</div>");
}

void component_render(String name)
{
	DValue props;
	component_render(name, props, *context);
}

void component_render(String name, Request& context)
{
	DValue props;
	component_render(name, props, context);
}

void component_render(String name, DValue props)
{
	component_render(name, props, *context);
}

void component_render(String name, DValue props, Request& context)
{
	(void)props; (void)context;
	print(component_error_banner("native component_render() is unavailable; components must render through the wasm backend: " + trim(name)));
}

String component(String name)
{
	DValue props;
	return(component(name, props, *context));
}

String component(String name, Request& context)
{
	DValue props;
	return(component(name, props, context));
}

String component(String name, DValue props)
{
	return(component(name, props, *context));
}

String component(String name, DValue props, Request& context)
{
	ob_start();
	component_render(name, props, context);
	return(ob_get_close());
}

SharedUnit* unit_load(String file_name)
{
	String resolved = compiler_resolve_unit_path(context, file_name);
	return(resolved == "" ? 0 : get_shared_unit(context, resolved));
}

DValue* unit_call(String file_name, String function_name, DValue* call_param)
{
	(void)call_param;
	print("Error: native unit_call() is unavailable; units must call through the wasm workspace: ", file_name, "::", function_name);
	return(0);
}

String compiler_site_directory(Request* context)
{
	String site_directory = first(
		context->server->config["PRECOMPILE_FILES_IN"],
		context->server->config["SITE_DIRECTORY"],
		"site"
	);
	return(compiler_normalize_unit_path(context, site_directory));
}

StringList compiler_scan_site_units(Request* context)
{
	StringList files;
	auto site_directory = compiler_site_directory(context);
	if(site_directory == "" || !file_exists(site_directory))
		return(files);

	std::error_code walk_error;
	auto options = std::filesystem::directory_options::skip_permission_denied;
	for(auto it = std::filesystem::recursive_directory_iterator(site_directory, options, walk_error);
		it != std::filesystem::recursive_directory_iterator();
		it.increment(walk_error))
	{
		if(walk_error)
		{
			printf("(!) proactive scan warning in %s: %s\n", site_directory.c_str(), walk_error.message().c_str());
			walk_error.clear();
			continue;
		}
		std::error_code entry_error;
		if(!it->is_regular_file(entry_error) || entry_error)
			continue;
		auto path = it->path().string();
		if(path.length() >= 4 && path.substr(path.length() - 4) == ".uce")
			files.push_back(path);
	}
	return(compiler_normalize_unit_list(context, files));
}

StringList compiler_list_known_units(Request* context)
{
	return(compiler_with_registry_lock(context, [&]() { return(compiler_read_known_units_unlocked(context)); }));
}

void compiler_set_known_units(Request* context, StringList files)
{
	files = compiler_normalize_unit_list(context, files);
	compiler_with_registry_lock(context, [&]() {
		compiler_write_known_units_unlocked(context, files);
		return(0);
	});
}

void compiler_track_known_unit(Request* context, String file_name)
{
	file_name = compiler_normalize_unit_path(context, file_name);
	if(file_name == "" || !compiler_is_known_unit_file(file_name))
		return;
	compiler_with_registry_lock(context, [&]() {
		auto files = compiler_read_known_units_unlocked(context);
		if(std::find(files.begin(), files.end(), file_name) != files.end())
			return(0);
		files.push_back(file_name);
		compiler_write_known_units_unlocked(context, files);
		return(0);
	});
}

static bool compiler_track_known_unit_bounded(Request* context, String file_name, CompilerDeadline* deadline)
{
	file_name = compiler_normalize_unit_path(context, file_name);
	if(file_name == "" || !compiler_is_known_unit_file(file_name))
		return(true);
	String lock_file_name = compiler_registry_lock_file_name(context);
	int fdlock = compiler_open_lock_file_bounded(lock_file_name, "compiler-registry", deadline);
	if(fdlock < 0)
		return(false);
	auto files = compiler_read_known_units_unlocked(context);
	if(std::find(files.begin(), files.end(), file_name) == files.end())
	{
		files.push_back(file_name);
		compiler_write_known_units_unlocked(context, files);
	}
	compiler_close_lock_file(fdlock);
	return(true);
}

void compiler_untrack_known_unit(Request* context, String file_name)
{
	file_name = compiler_normalize_unit_path(context, file_name);
	if(file_name == "")
		return;
	compiler_with_registry_lock(context, [&]() {
		auto files = compiler_read_known_units_unlocked(context);
		files.erase(std::remove(files.begin(), files.end(), file_name), files.end());
		compiler_write_known_units_unlocked(context, files);
		return(0);
	});
}

bool compiler_unit_needs_recompile(Request* context, String file_name, bool* source_missing, bool allow_recent_source_stat, bool path_is_normalized, bool retry_current_failure)
{
	if(!path_is_normalized)
		file_name = compiler_normalize_unit_path(context, file_name);
	SharedUnit su;
	setup_unit_paths(context, &su, file_name);
	auto state = inspect_shared_unit_filesystem(context, &su, allow_recent_source_stat);
	auto compile_check = shared_unit_compile_check(state);
	if(source_missing)
		*source_missing = compile_check.source_missing;
	if(compile_check.source_missing)
		return(false);
	if(!retry_current_failure && compiler_failure_retry_deferred(context, &su, state))
		return(false);
	return(compile_check.needs_compile);
}

DValue unit_info(String path)
{
	DValue info;
	if(!context)
		return(info);
	String resolved_path = compiler_resolve_unit_path(context, path);
	if(resolved_path == "")
		return(info);

	SharedUnit* su = 0;
	auto it = context->server->units.find(resolved_path);
	if(it != context->server->units.end())
		su = it->second;
	else
	{
		auto known_units = compiler_list_known_units(context);
		if(std::find(known_units.begin(), known_units.end(), resolved_path) == known_units.end() && !file_exists(resolved_path))
			return(info);
	}

	SharedUnit temp_unit;
	if(!su)
	{
		su = &temp_unit;
		setup_unit_paths(context, su, resolved_path);
	}

	auto fs_state = inspect_shared_unit_filesystem(context, su);
	if(su->compile_status == "unknown" && compiler_failure_retry_deferred(context, su, fs_state))
		compiler_restore_persisted_failure(su, fs_state);
	auto exports_text = compiler_unit_exports_text(su);
	auto exports = compiler_unit_exports(su);
	if(exports.size() == 0 && exports_text != "")
		exports = split(exports_text, "\n");

	info["path"] = resolved_path;
	info["file_name"] = su->file_name;
	info["src_path"] = su->src_path;
	info["bin_path"] = su->bin_path;
	info["pre_path"] = su->pre_path;
	info["src_file_name"] = su->src_file_name;
	info["pre_file_name"] = su->pre_file_name;
	info["wasm_file_name"] = su->wasm_file_name;
	info["wasm_name"] = su->wasm_name;
	info["wasm_exists"].set_bool(file_exists(su->wasm_name));
	info["api_file_name"] = su->api_file_name;
	info["meta_file_name"] = su->meta_file_name;
	info["compile_output_file_name"] = su->compile_output_file_name;
	info["compile_status"] = (su->compile_status != "unknown" ? su->compile_status : compiler_status_from_filesystem(fs_state, su));
	info["compile_error_status"] = su->compile_error_status;
	info["runtime_error_status"] = su->runtime_error_status;
	info["error_status"] = compiler_error_status(su);
	info["compiler_messages"] = su->compiler_messages;
	info["last_compiled"] = (f64)su->last_compiled;
	info["last_rendered"] = (f64)su->last_rendered;
	info["last_error"] = (f64)su->last_error;
	info["request_count"] = (f64)su->request_count;
	info["invoke_count"] = (f64)su->invoke_count;
	info["runtime_error_count"] = (f64)su->runtime_error_count;
	info["compile_count"] = (f64)su->compile_count;
	info["compile_success_count"] = (f64)su->compile_success_count;
	info["compile_failure_count"] = (f64)su->compile_failure_count;
	info["last_compile_time"] = su->last_compile_duration;
	info["average_compile_time"] = compiler_average(su->total_compile_duration, su->compile_count);
	info["best_compile_time"] = su->best_compile_duration;
	info["worst_compile_time"] = su->worst_compile_duration;
	info["last_render_time"] = su->last_render_duration;
	info["average_render_time"] = compiler_average(su->total_render_duration, su->invoke_count);
	info["best_render_time"] = su->best_render_duration;
	info["worst_render_time"] = su->worst_render_duration;
	info["source_mtime"] = (f64)fs_state.source_time;
	info["compiled_mtime"] = (f64)fs_state.compiled_time;
	info["metadata_mtime"] = (f64)fs_state.metadata_time;
	info["compile_output_mtime"] = (f64)fs_state.compile_output_time;
	info["setup_template_mtime"] = (f64)fs_state.setup_template_time;
	info["required_mtime"] = (f64)fs_state.required_time;
	info["runtime_abi_version"] = (f64)fs_state.runtime_abi_version;
	info["metadata_abi_version"] = (f64)fs_state.metadata_abi_version;
	info["runtime_wasm_core_abi_version"] = (f64)fs_state.runtime_wasm_core_abi_version;
	info["metadata_wasm_core_abi_version"] = (f64)fs_state.metadata_wasm_core_abi_version;
	info["current_input_signature"] = fs_state.current_input_signature;
	info["metadata_input_signature"] = fs_state.metadata_input_signature;
	info["metadata_build_token"] = fs_state.metadata_build_token;
	compiler_tree_set_bool(info, "known", compiler_has_known_unit_cached(context, resolved_path));
	compiler_tree_set_bool(info, "current_unit", resolved_path == compiler_current_unit_path(context));
	compiler_tree_set_bool(info, "wasm_available", file_exists(su->wasm_name));
	compiler_tree_set_bool(info, "source_exists", fs_state.source_exists);
	compiler_tree_set_bool(info, "compiled_exists", fs_state.compiled_time != 0);
	compiler_tree_set_bool(info, "metadata_exists", fs_state.metadata_exists);
	compiler_tree_set_bool(info, "metadata_parsed", fs_state.metadata_parsed);
	compiler_tree_set_bool(info, "abi_compatible", fs_state.abi_compatible);
	compiler_tree_set_bool(info, "input_signature_matches", fs_state.input_signature_matches);
	compiler_tree_set_bool(info, "stale", shared_unit_compile_check(fs_state).needs_compile && !compiler_failure_retry_deferred(context, su, fs_state));
	compiler_tree_set_bool(info, "retry_deferred", compiler_failure_retry_deferred(context, su, fs_state));
	compiler_tree_set_bool(info, "cache_stale", shared_unit_cache_is_stale(context, su));
	compiler_tree_set_bool(info, "has_render", false);
	compiler_tree_set_bool(info, "has_component", false);
	compiler_tree_set_bool(info, "has_websocket", false);
	compiler_tree_set_bool(info, "has_setup", false);
	compiler_tree_set_bool(info, "has_error", trim(compiler_error_status(su)) != "");
	compiler_tree_push_strings(info["exports"], exports);
	info["exports_text"] = exports_text;
	return(info);
}

StringList units_list()
{
	if(!context)
		return(StringList());
	auto known_units = compiler_list_known_units(context);
	for(auto& it : context->server->units)
		known_units.push_back(it.first);
	return(compiler_normalize_unit_list(context, known_units));
}

bool unit_compile(String path)
{
	if(!context)
		return(false);
	String resolved_path = compiler_resolve_unit_path(context, path);
	if(resolved_path == "")
		return(false);
	compiler_track_known_unit(context, resolved_path);
	auto su = compiler_get_shared_unit_internal(context, resolved_path, true);
	return(su && trim(su->compiler_messages) == "" && file_exists(su->wasm_name));
}

bool unit_compile_bounded(Request* request, String path, u64 timeout_ms, bool* timed_out, String* error)
{
	if(timed_out)
		*timed_out = false;
	if(error)
		error->clear();
	if(!request || timeout_ms == 0)
	{
		if(timed_out)
			*timed_out = timeout_ms == 0;
		return(false);
	}
	CompilerDeadline deadline(timeout_ms);
	String resolved_path = compiler_resolve_unit_path(request, path);
	if(resolved_path == "")
		return(false);
	if(!compiler_track_known_unit_bounded(request, resolved_path, &deadline))
	{
		if(timed_out)
			*timed_out = deadline.timed_out;
		return(false);
	}
	auto su = compiler_get_shared_unit_internal(request, resolved_path, true, false, &deadline);
	if(timed_out)
		*timed_out = deadline.timed_out;
	if(error)
		*error = first(deadline.operational_error, su ? trim(su->compiler_messages) : "");
	return(su && trim(su->compiler_messages) == "" && file_exists(su->wasm_name));
}
