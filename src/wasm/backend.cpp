// W4 — FastCGI backend glue for the W3 wasm workspace runtime.
//
// Included into the native server TU (src/linux_fastcgi.cpp) after uce_lib.cpp,
// so it shares String/DValue/config and the UCEB2 codec. Provides a
// Always-on wasm backend: every unit request is served through a per-request
// wasm workspace. The legacy native dlopen execution path has been removed.

#include "../lib/wasm_trace.h"
// The native server TU has the native connectors (sqlite/mysql) compiled in, so
// the worker's host-side connector hostcalls are available here.
#define UCE_WASM_HOST_CONNECTORS 1
#include "worker.cpp"

#include <atomic>
#include <sys/stat.h>
#include <thread>
// Worker flushes ws_* dispatch batches to the broker via the FastCGI client.
#include "../lib/fcgi_forward.h"

// per forked worker process: one engine + compiled-core cache, one epoch ticker
static WasmWorker* g_wasm_worker = 0;
static std::thread* g_wasm_epoch_ticker = 0;
static std::atomic<bool> g_wasm_epoch_running(false);
static String g_wasm_init_error;
static bool g_wasm_init_attempted = false;

struct WasmEntryArtifactIdentity
{
	dev_t device = 0;
	ino_t inode = 0;
	mode_t mode = 0;
	off_t size = 0;
	timespec modified = {};
	timespec changed = {};
};

struct WasmEntryFreshnessState
{
	std::chrono::steady_clock::time_point checked_at;
	String source_generation;
	WasmEntryArtifactIdentity wasm;
	WasmEntryArtifactIdentity metadata;
	WasmEntryArtifactIdentity setup_template;
};

static std::mutex g_wasm_entry_freshness_mutex;
static std::map<String, WasmEntryFreshnessState> g_wasm_entry_freshness;
static constexpr u64 WASM_ENTRY_FRESHNESS_CACHE_MAX = 4096;
static constexpr auto WASM_ENTRY_FRESHNESS_TTL = std::chrono::seconds(10);

static WasmEntryArtifactIdentity wasm_entry_artifact_identity(const struct stat& info)
{
	return(WasmEntryArtifactIdentity{ info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_mtim, info.st_ctim });
}

static bool wasm_entry_artifact_identity_matches(const WasmEntryArtifactIdentity& expected, const struct stat& actual)
{
	return(
		expected.device == actual.st_dev && expected.inode == actual.st_ino && expected.mode == actual.st_mode && expected.size == actual.st_size &&
		expected.modified.tv_sec == actual.st_mtim.tv_sec && expected.modified.tv_nsec == actual.st_mtim.tv_nsec &&
		expected.changed.tv_sec == actual.st_ctim.tv_sec && expected.changed.tv_nsec == actual.st_ctim.tv_nsec
	);
}

static bool wasm_entry_cache_allowed(Request* context)
{
	if(!compiler_request_can_serve_stale_artifact(context))
		return(false);
	String method = to_upper(trim(context->params["REQUEST_METHOD"]));
	return(method == "GET" || method == "HEAD" || method == "OPTIONS");
}

static void wasm_entry_freshness_forget(const String& entry_unit)
{
	std::lock_guard<std::mutex> lock(g_wasm_entry_freshness_mutex);
	g_wasm_entry_freshness.erase(entry_unit);
}

bool wasm_backend_configured(Request* context)
{
	return(context && context->server);
}

// Lazily bring up the per-process worker on first use inside a forked child
// (the engine must not be inherited across fork). Returns "" on success.
static String wasm_backend_ensure_started(Request* context)
{
	if(g_wasm_init_attempted)
		return(g_wasm_init_error);
	g_wasm_init_attempted = true;

	StringMap& cfg = context->server->config;
	WasmWorkerConfig wc;
	wc.core_wasm_path = first(cfg["WASM_CORE_PATH"],
		path_join(cfg["COMPILER_SYS_PATH"], "bin/wasm/core.wasm"));
	wc.site_root = path_join(cfg["COMPILER_SYS_PATH"], cfg["SITE_DIRECTORY"]);
	wc.cache_root = compiler_unit_bin_directory(context);
	// write membrane allowlist: the site tree plus the runtime scratch dirs
	// pages legitimately write to (matches native reachable write targets).
	wc.write_roots = { wc.site_root, "/tmp" };
	for(const char* key : { "BIN_DIRECTORY", "SESSION_PATH", "TMP_UPLOAD_PATH" })
		if(cfg[key] != "")
			wc.write_roots.push_back(cfg[key]);
	wc.memory_limit = (int64_t)to_u64(cfg["WASM_MEMORY_LIMIT_BYTES"], 512ull * 1024 * 1024);
	wc.epoch_deadline_ticks = to_u64(first(cfg["WASM_EPOCH_DEADLINE_TICKS"], "200"), 0);
	wc.epoch_period_ms = to_u64(first(cfg["WASM_EPOCH_PERIOD_MS"], "50"), 0);
	wc.invocation_timeout_ms = to_u64(first(cfg["WASM_INVOCATION_TIMEOUT_MS"], "30000"), 0);
	if(wc.epoch_deadline_ticks == 0 || wc.epoch_deadline_ticks > (u64)INT64_MAX)
	{
		g_wasm_init_error = "WASM_EPOCH_DEADLINE_TICKS must be a positive integer no greater than INT64_MAX";
		return(g_wasm_init_error);
	}
	if(wc.epoch_period_ms == 0 || wc.epoch_period_ms > 1000)
	{
		g_wasm_init_error = "WASM_EPOCH_PERIOD_MS must be a positive integer no greater than 1000";
		return(g_wasm_init_error);
	}
	if(wc.invocation_timeout_ms == 0 || wc.invocation_timeout_ms > 86400000)
	{
		g_wasm_init_error = "WASM_INVOCATION_TIMEOUT_MS must be a positive integer no greater than 86400000";
		return(g_wasm_init_error);
	}
	wc.mysql_persistent_pool_size = std::min<u64>(to_u64(cfg["MYSQL_PERSISTENT_POOL_SIZE"], 8), 64);
	wc.profile_hostcall_cpu = to_bool(cfg["WASM_PROFILE_HOSTCALL_CPU"], false);
	wc.profile_thread_runtime = to_bool(cfg["WASM_PROFILE_THREAD_RUNTIME"], false);
	wc.verbose = to_bool(cfg["WASM_BACKEND_VERBOSE"], false);
	// UCE_HOSTCALL_BLOCKLIST: comma-separated uce_host_* names (with or without
	// the "uce_host_" prefix) the sysadmin disables; each blocked call traps into
	// the error page (see make_host_import). Parsed once here, per worker process.
	for(String entry : split(cfg["UCE_HOSTCALL_BLOCKLIST"], ","))
	{
		entry = trim(entry);
		if(entry.rfind("uce_host_", 0) == 0)
			entry = entry.substr(9);
		if(entry != "")
			wc.hostcall_blocklist.insert(entry);
	}

	g_wasm_worker = new WasmWorker(wc);
	// Server configuration is finalized before render workers start. Encode it
	// once; each fresh guest decodes the cached flat map into its own Server.
	g_wasm_worker->server_config_context = cfg;
	g_wasm_worker->server_config_encoded = ucb_encode(g_wasm_worker->server_config_context);
	g_wasm_init_error = g_wasm_worker->init();
	if(g_wasm_init_error == "")
		g_wasm_init_error = wasm_worker_prepare(*g_wasm_worker);
	if(g_wasm_init_error != "")
	{
		delete g_wasm_worker;
		g_wasm_worker = 0;
		return(g_wasm_init_error);
	}

	g_wasm_epoch_running.store(true);
	WasmWorker* worker = g_wasm_worker;
	u64 period_ms = wc.epoch_period_ms;
	// The ticker inherits this block so it cannot run the process-wide child reaper.
	WasmSigchldBlock sigchld;
	g_wasm_epoch_ticker = new std::thread([worker, period_ms] {
		while(g_wasm_epoch_running.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
			worker->engine.increment_epoch();
		}
	});
	sigchld.restore();
	return("");
}

String wasm_backend_start(Request& request)
{
	return(wasm_backend_ensure_started(&request));
}

bool wasm_serialized_module_needs_refresh(const String& wasm_path)
{
	return(WasmWorker::serialized_module_needs_refresh(wasm_path));
}

String wasm_serialize_module_artifact(const String& wasm_path)
{
	return(WasmWorker::serialize_module_artifact(wasm_path));
}

static bool wasm_artifact_exists(Request* context, const String& entry_unit)
{
	if(entry_unit == "")
		return(false);
	String wasm_path = compiler_unit_wasm_path(context, entry_unit);
	struct stat wasm_st;
	f64 phase_started = time_precise();
	context->stats.wasm_ready_check_count++;
	if(stat(wasm_path.c_str(), &wasm_st) != 0 || !S_ISREG(wasm_st.st_mode))
	{
		context->stats.wasm_ready_artifact_stat_us += (u64)((time_precise() - phase_started) * 1000000.0);
		return(false);
	}
	context->stats.wasm_ready_artifact_stat_us += (u64)((time_precise() - phase_started) * 1000000.0);
	f64 freshness_started = time_precise();
	bool cache_allowed = wasm_entry_cache_allowed(context);
	String metadata_path = compiler_unit_bin_directory(context) + entry_unit + ".meta.txt";
	String setup_template_path = path_join(context->server->config["COMPILER_SYS_PATH"], context->server->config["SETUP_TEMPLATE"]);
	String source_generation;
	struct stat metadata_st;
	struct stat setup_template_st;
	bool metadata_exists = false;
	bool setup_template_exists = false;
	auto now = std::chrono::steady_clock::now();
	if(cache_allowed)
	{
		phase_started = time_precise();
		source_generation = compiler_source_generation(context);
		context->stats.wasm_ready_source_generation_us += (u64)((time_precise() - phase_started) * 1000000.0);
		metadata_exists = stat(metadata_path.c_str(), &metadata_st) == 0 && S_ISREG(metadata_st.st_mode);
		setup_template_exists = stat(setup_template_path.c_str(), &setup_template_st) == 0 && S_ISREG(setup_template_st.st_mode);
		if(source_generation != "" && metadata_exists && setup_template_exists)
		{
			std::lock_guard<std::mutex> lock(g_wasm_entry_freshness_mutex);
			auto cached = g_wasm_entry_freshness.find(entry_unit);
			if(cached != g_wasm_entry_freshness.end() && now - cached->second.checked_at < WASM_ENTRY_FRESHNESS_TTL &&
				cached->second.source_generation == source_generation &&
				wasm_entry_artifact_identity_matches(cached->second.wasm, wasm_st) &&
				wasm_entry_artifact_identity_matches(cached->second.metadata, metadata_st) &&
				wasm_entry_artifact_identity_matches(cached->second.setup_template, setup_template_st))
			{
				context->stats.wasm_ready_freshness_cache_hit_count++;
				context->stats.wasm_ready_freshness_us += (u64)((time_precise() - freshness_started) * 1000000.0);
				return(true);
			}
			if(cached != g_wasm_entry_freshness.end())
				g_wasm_entry_freshness.erase(cached);
		}
	}
	// Require the artifact to satisfy the full compiler freshness check. Source
	// mtime alone misses runtime/unit ABI changes, setup-template changes, and
	// metadata mismatches, which can leave stale wasm with old imports.
	bool source_missing = false;
	phase_started = time_precise();
	if(compiler_unit_needs_recompile(context, entry_unit, &source_missing, false, true))
	{
		u64 full_freshness_us = (u64)((time_precise() - phase_started) * 1000000.0);
		context->stats.wasm_ready_freshness_us += (u64)((time_precise() - freshness_started) * 1000000.0);
		context->stats.wasm_ready_freshness_full_check_us += full_freshness_us;
		wasm_entry_freshness_forget(entry_unit);
		compiler_prioritize_unit(context, entry_unit);
		return(compiler_unit_can_serve_stale_artifact(context, entry_unit));
	}
	u64 full_freshness_us = (u64)((time_precise() - phase_started) * 1000000.0);
	context->stats.wasm_ready_freshness_full_check_us += full_freshness_us;
	if(source_missing)
	{
		context->stats.wasm_ready_freshness_us += (u64)((time_precise() - freshness_started) * 1000000.0);
		wasm_entry_freshness_forget(entry_unit);
		return(false);
	}
	if(cache_allowed && source_generation != "")
	{
		struct stat final_wasm_st;
		struct stat final_metadata_st;
		struct stat final_setup_template_st;
		phase_started = time_precise();
		String final_generation = compiler_source_generation(context);
		context->stats.wasm_ready_source_generation_us += (u64)((time_precise() - phase_started) * 1000000.0);
		if(final_generation == source_generation && stat(wasm_path.c_str(), &final_wasm_st) == 0 && S_ISREG(final_wasm_st.st_mode) &&
			stat(metadata_path.c_str(), &final_metadata_st) == 0 && S_ISREG(final_metadata_st.st_mode) &&
			stat(setup_template_path.c_str(), &final_setup_template_st) == 0 && S_ISREG(final_setup_template_st.st_mode))
		{
			std::lock_guard<std::mutex> lock(g_wasm_entry_freshness_mutex);
			if(g_wasm_entry_freshness.size() >= WASM_ENTRY_FRESHNESS_CACHE_MAX)
				g_wasm_entry_freshness.clear();
			g_wasm_entry_freshness[entry_unit] = {
				std::chrono::steady_clock::now(), final_generation,
				wasm_entry_artifact_identity(final_wasm_st), wasm_entry_artifact_identity(final_metadata_st),
				wasm_entry_artifact_identity(final_setup_template_st)
			};
		}
	}
	context->stats.wasm_ready_freshness_us += (u64)((time_precise() - freshness_started) * 1000000.0);
	return(true);
}

// True if this request can be served by the wasm backend now. Every unit runs
// on wasm; cold/stale artifacts return false so dispatch can compile on demand
// and then recheck.
bool wasm_backend_should_handle(Request& request, const String& entry_unit)
{
	if(!wasm_backend_configured(&request))
		return(false);
	if(!wasm_artifact_exists(&request, entry_unit))
		return(false);
	f64 phase_started = time_precise();
	String start_error = wasm_backend_ensure_started(&request);
	request.stats.wasm_ready_worker_us += (u64)((time_precise() - phase_started) * 1000000.0);
	if(start_error != "")
		return(false);
	return(true);
}

u64 wasm_backend_invocation_timeout_ms(Request& request)
{
	if(!request.server)
		return(0);
	u64 timeout_ms = to_u64(first(request.server->config["WASM_INVOCATION_TIMEOUT_MS"], "30000"), 0);
	return(timeout_ms > 0 && timeout_ms <= 86400000 ? timeout_ms : 0);
}

// Serve a request through a wasm workspace using the unit handler selected by
// `kind` (page render / cli / serve_http, with an optional named serve_http
// handler). Populates the native Request (status/headers/cookies/session/body)
// so the existing transport writes the response unchanged. Returns "" on
// success, or a collapsed error/trace string for the caller to route into the
// configured error page.
String wasm_backend_serve(Request& request, const String& entry_unit, const String& handler, u64 timeout_cap_ms)
{
	if(timeout_cap_ms == 0)
		return("UCE_INVOCATION_TIMEOUT: wasm invocation exceeded " + std::to_string(wasm_backend_invocation_timeout_ms(request)) + " ms");
	WasmResponse response = wasm_worker_serve(*g_wasm_worker, request, entry_unit, handler, timeout_cap_ms);
	request.stats.wasm_dispatch_us = response.dispatch_us;
	request.stats.wasm_workspace_complete_us = response.workspace_complete_us;
	request.stats.wasm_entry_invoke_us = response.entry_invoke_us;
	request.stats.wasm_output_collect_us = response.output_collect_us;
	if(!response.ok)
		return(response.error == "" ? String("wasm workspace failed") : response.error);

	// Any handler may have called ws_send/ws_close (not just WS handlers). Flush the
	// websocket batch whenever either command frames or an updated per-connection
	// state are present. This is the only path WS data takes out; the workspace
	// owns no connections.
	DValue* cmds = response.meta.key("ws_commands");
	DValue* cstate = response.meta.key("ws_connection_state");
	if(cmds || cstate)
	{
		DValue batch;
		if(cmds)
			batch["commands"] = *cmds;
		if(cstate)
		{
			batch["connection_id"] = request.resources.websocket_connection_id;
			batch["connection_state"] = *cstate;
		}
		StringMap dispatch_params;
		dispatch_params["UCE_WS_DISPATCH"] = "1";
		String broker_socket = first(request.server->config["WS_BROKER_SOCKET_PATH"],
			"/run/uce/ws-broker.sock");
		fcgi_forward_request(broker_socket, dispatch_params, ucb_encode(batch), 5);
	}

	// A cli/serve_http unit that does not export the requested handler is a 404.
	// For page render a missing handler is simply an empty body.
	if(!response.handler_present && handler != "render")
	{
		request.set_status(404, handler == "cli" ? "CLI Entry Point Not Found" : "Handler Not Found");
		return("");
	}

	// Diagnostic timing headers are opt-in: they leak workspace internals and
	// belong to the W5 benchmark harness, not public responses.
	if(to_bool(request.server->config["WASM_BACKEND_VERBOSE"], false))
	{
		request.header["X-UCE-Backend"] = "wasm";
		request.header["X-UCE-Wasm-Workspace-Setup-Us"] = std::to_string(response.workspace_setup_us);
		request.header["X-UCE-Wasm-Workspace-Birth-Us"] = std::to_string(response.workspace_birth_us);
		request.header["X-UCE-Wasm-Birth-Policy-Us"] = std::to_string(response.birth_policy_us);
		request.header["X-UCE-Wasm-Birth-Import-Us"] = std::to_string(response.birth_import_us);
		request.header["X-UCE-Wasm-Birth-Instantiate-Us"] = std::to_string(response.birth_instantiate_us);
		request.header["X-UCE-Wasm-Birth-Exports-Us"] = std::to_string(response.birth_exports_us);
		request.header["X-UCE-Wasm-Birth-Initialize-Us"] = std::to_string(response.birth_initialize_us);
		request.header["X-UCE-Wasm-Context-Apply-Us"] = std::to_string(response.context_apply_us);
		request.header["X-UCE-Wasm-Context-Bytes"] = std::to_string(response.context_bytes);
		request.header["X-UCE-Wasm-Server-Config-Bytes"] = std::to_string(response.server_config_bytes);
		request.header["X-UCE-Wasm-Context-Encode-Us"] = std::to_string(response.context_encode_us);
		request.header["X-UCE-Wasm-Context-Allocate-Us"] = std::to_string(response.context_allocate_us);
		request.header["X-UCE-Wasm-Context-Write-Us"] = std::to_string(response.context_write_us);
		request.header["X-UCE-Wasm-Context-Guest-Apply-Us"] = std::to_string(response.context_guest_apply_us);
		request.header["X-UCE-Wasm-Context-Free-Us"] = std::to_string(response.context_free_us);
		request.header["X-UCE-Wasm-Workspace-Complete-Us"] = std::to_string(response.workspace_complete_us);
		request.header["X-UCE-Wasm-Entry-Invoke-Us"] = std::to_string(response.entry_invoke_us);
		request.header["X-UCE-Wasm-Entry-Load-Us"] = std::to_string(response.entry_load_us);
		request.header["X-UCE-Wasm-Entry-Presence-Us"] = std::to_string(response.entry_presence_us);
		request.header["X-UCE-Wasm-Entry-Link-Us"] = std::to_string(response.entry_link_us);
		request.header["X-UCE-Wasm-Entry-Dispatch-Us"] = std::to_string(response.entry_dispatch_us);
		request.header["X-UCE-Wasm-Output-Collect-Us"] = std::to_string(response.output_collect_us);
		request.header["X-UCE-Wasm-Component-Resolve-Count"] = std::to_string(response.component_resolve_count);
		request.header["X-UCE-Wasm-Component-Loaded-Reuse-Count"] = std::to_string(response.component_loaded_reuse_count);
		request.header["X-UCE-Wasm-Component-Resolve-Total-Us"] = std::to_string(response.component_resolve_total_us);
		request.header["X-UCE-Wasm-Component-Resolve-Avg-Us"] = std::to_string(
			response.component_resolve_count ? response.component_resolve_total_us / response.component_resolve_count : 0);
	}

	// status line: keep the native default unless the unit set one
	String status = response.meta["status"].to_string();
	if(status != "")
		request.response_code = status;
	// merge headers over the native defaults (so Content-Type survives unless
	// the unit overrode it); replace cookies/session with the unit's view
	if(response.meta.key("headers"))
		response.meta["headers"].each([&](const DValue& value, String name) {
			request.header[name] = value.to_string();
		});
	if(response.meta.key("cookies"))
		response.meta["cookies"].each([&](const DValue& value, String) {
			request.set_cookies.push_back(value.to_string());
		});
	if(response.meta.key("session"))
	{
		request.session.clear();
		response.meta["session"].each([&](const DValue& value, String name) {
			request.session[name] = value.to_string();
		});
	}
	if(response.meta.key("session_id"))
		request.session_id = response.meta["session_id"].to_string();
	if(response.meta.key("session_name"))
		request.session_name = response.meta["session_name"].to_string();
	if(response.meta.key("session_loaded_hash"))
		request.session_loaded_hash = response.meta["session_loaded_hash"].to_string();

	// body into the request's primary output stream (ob_stack[0]); the
	// transport's assemble_output_buffer concatenates the stack
	if(request.ob)
		request.ob->write(response.body.data(), response.body.size());
	return("");
}

// Stop the ticker before the worker process exits (best-effort; forked workers
// are usually killed, but a clean ager-out path should join the thread).
void wasm_backend_shutdown()
{
	if(g_wasm_epoch_ticker)
	{
		g_wasm_epoch_running.store(false);
		if(g_wasm_epoch_ticker->joinable())
			g_wasm_epoch_ticker->join();
		delete g_wasm_epoch_ticker;
		g_wasm_epoch_ticker = 0;
	}
}
