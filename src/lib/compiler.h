#pragma once

#if defined(__UCE_WASM_UNIT__)
#define UCE_UNIT_EXPORT __attribute__((visibility("default")))
#else
#define UCE_UNIT_EXPORT
#endif

#define RENDER(X) extern "C" UCE_UNIT_EXPORT void __uce_render(X)
#define COMPONENT(X) extern "C" UCE_UNIT_EXPORT void __uce_component(X)
#define ONCE(X) extern "C" UCE_UNIT_EXPORT void __uce_once(X)
#define INIT(X) extern "C" UCE_UNIT_EXPORT void __uce_init(X)
#define WS(X) extern "C" UCE_UNIT_EXPORT void __uce_websocket(X)
#define CLI(X) extern "C" UCE_UNIT_EXPORT void __uce_cli(X)
#define SERVE_HTTP(X) extern "C" UCE_UNIT_EXPORT void __uce_serve_http(X)
#define EXPORT extern "C" UCE_UNIT_EXPORT

String preprocess_shared_unit(Request* context, SharedUnit* su);
String compiler_generated_cpp_path(Request* context, String source_file);
String compiler_generated_cpp_path(SharedUnit* su);
#ifndef __UCE_WASM_UNIT__
String compiler_unit_bin_directory(Request* context);
String compiler_unit_wasm_path(Request* context, String source_file);
#endif
void setup_unit_paths(Request* context, SharedUnit* su, String file_name);
void compile_shared_unit(Request* context, SharedUnit* su);
SharedUnit* get_shared_unit(Request* context, String file_name);
#ifndef __UCE_WASM_UNIT__
SharedUnit* get_shared_unit_for_preprocess(Request* context, String file_name);
SharedUnit* get_shared_unit_bounded(Request* context, String file_name, u64 timeout_ms, bool* timed_out);
bool unit_compile_bounded(Request* context, String path, u64 timeout_ms, bool* timed_out, String* error = 0);
#endif
String compiler_error_page_unit(Request* context, String config_key);
bool compiler_unit_compile_pending(Request* context, String file_name);
bool compiler_unit_compile_in_progress(Request* context, String file_name);
bool compiler_request_can_serve_stale_artifact(Request* context);
bool compiler_preserve_last_known_good(Request* context, String file_name);
bool compiler_unit_can_serve_stale_artifact(Request* context, String file_name);
void compiler_prioritize_unit(Request* context, String file_name);
StringList compiler_take_priority_units(Request* context);
String compiler_source_generation(Request* context);
void compiler_mark_source_generation(Request* context);
String compiler_site_directory(Request* context);
StringList compiler_scan_site_units(Request* context);
StringList compiler_list_known_units(Request* context);
void compiler_set_known_units(Request* context, StringList files);
void compiler_track_known_unit(Request* context, String file_name);
void compiler_untrack_known_unit(Request* context, String file_name);
bool compiler_unit_needs_recompile(Request* context, String file_name, bool* source_missing = 0, bool allow_recent_source_stat = false, bool path_is_normalized = false, bool retry_current_failure = false);
DValue unit_info(String path = "");
StringList units_list();
bool unit_compile(String path = "");

#ifndef __UCE_WASM_UNIT__
SharedUnit* unit_load(String file_name);
#endif
void unit_render(String file_name);
void unit_render(String file_name, Request& context);
DValue* unit_call(String file_name, String function_name, DValue* call_param = 0);
String component_resolve(String name);
bool component_exists(String name);
void component_render(String name);
void component_render(String name, Request& context);
void component_render(String name, DValue props);
void component_render(String name, DValue props, Request& context);
String component(String name);
String component(String name, Request& context);
String component(String name, DValue props);
String component(String name, DValue props, Request& context);
