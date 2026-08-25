// W1 smoke driver for the production BEARER core.wasm.

#include <wasm.h>
#include "abi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while(0)
#define CHECK(cond, ...) do { if(!(cond)) FAIL(__VA_ARGS__); } while(0)

static std::vector<uint8_t> read_file(const char* path)
{
	FILE* f = fopen(path, "rb");
	CHECK(f, "cannot open %s", path);
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> data((size_t)n);
	CHECK(fread(data.data(), 1, data.size(), f) == data.size(), "short read on %s", path);
	fclose(f);
	return(data);
}

static std::string wasm_name(const wasm_name_t* name)
{
	std::string s(name->data, name->size);
	while(!s.empty() && s.back() == '\0') s.pop_back();
	return(s);
}

static wasm_store_t* g_store_for_traps = nullptr;
static int g_module_reset_count = 0;

static wasm_trap_t* module_trap(const char* message)
{
	wasm_byte_vec_t bytes;
	wasm_byte_vec_new(&bytes, strlen(message) + 1, message);
	wasm_trap_t* trap = wasm_trap_new(g_store_for_traps, &bytes);
	wasm_byte_vec_delete(&bytes);
	return(trap);
}

static void set_i32(wasm_val_t* result, int32_t value)
{
	result->kind = WASM_I32;
	result->of.i32 = value;
}

static void set_i64(wasm_val_t* result, int64_t value)
{
	result->kind = WASM_I64;
	result->of.i64 = value;
}

static void set_f64(wasm_val_t* result, double value)
{
	result->kind = WASM_F64;
	result->of.f64 = value;
}

static wasm_trap_t* host_time(void*, const wasm_val_vec_t*, wasm_val_vec_t* results)
{
	set_i64(&results->data[0], 1700000000);
	return(nullptr);
}

static wasm_trap_t* host_time_precise(void*, const wasm_val_vec_t*, wasm_val_vec_t* results)
{
	set_f64(&results->data[0], 1700000000.25);
	return(nullptr);
}

static wasm_trap_t* host_env(void*, const wasm_val_vec_t*, wasm_val_vec_t* results)
{
	set_i32(&results->data[0], 0);
	return(nullptr);
}

static wasm_memory_t* g_memory = nullptr;

static wasm_trap_t* host_random(void*, const wasm_val_vec_t* args, wasm_val_vec_t* results)
{
	uint32_t ptr = args->data[0].of.i32;
	uint32_t len = args->data[1].of.i32;
	uint8_t* mem = (uint8_t*)wasm_memory_data(g_memory);
	size_t mem_size = wasm_memory_data_size(g_memory);
	if((size_t)ptr + len > mem_size)
	{
		set_i32(&results->data[0], 0);
		return(nullptr);
	}
	for(uint32_t i = 0; i < len; ++i)
		mem[ptr + i] = (uint8_t)(0x5au ^ (i * 29u));
	set_i32(&results->data[0], len);
	return(nullptr);
}

static wasm_trap_t* host_log(void*, const wasm_val_vec_t*, wasm_val_vec_t*)
{
	return(nullptr);
}

static wasm_trap_t* host_module_reset(void*, const wasm_val_vec_t*, wasm_val_vec_t*)
{
	g_module_reset_count++;
	return(nullptr);
}

static wasm_trap_t* host_module_staged_size(void*, const wasm_val_vec_t* args, wasm_val_vec_t* results)
{
	if(args->data[0].of.i32 == 0)
		return(module_trap("BEARER_MODULE_CALL: invalid, stale, or foreign module capability"));
	if(args->data[3].of.i32 == 0 || args->data[4].of.i32 < 5)
		return(module_trap("BEARER_MODULE_CALL: malformed BRRB input"));
	set_i32(&results->data[0], -1);
	return(nullptr);
}

static wasm_trap_t* host_module_resolve(void*, const wasm_val_vec_t*, wasm_val_vec_t*)
{
	return(module_trap("BEARER_MODULE_CALL: module does not export requested name"));
}

static wasm_trap_t* stub_callback(void* env, const wasm_val_vec_t*, wasm_val_vec_t*)
{
	std::string label = (const char*)env;
	std::string msg = "unexpected import called: " + label;
	wasm_byte_vec_t message;
	wasm_byte_vec_new(&message, msg.size(), msg.data());
	wasm_trap_t* trap = wasm_trap_new(g_store_for_traps, &message);
	wasm_byte_vec_delete(&message);
	return(trap);
}

struct Instance
{
	wasm_module_t* module = nullptr;
	wasm_instance_t* instance = nullptr;
	wasm_extern_vec_t exports = WASM_EMPTY_VEC;
	std::map<std::string, wasm_extern_t*> by_name;

	void index_exports()
	{
		wasm_exporttype_vec_t types = WASM_EMPTY_VEC;
		wasm_module_exports(module, &types);
		wasm_instance_exports(instance, &exports);
		CHECK(types.size == exports.size, "export count mismatch");
		for(size_t i = 0; i < types.size; ++i)
			by_name[wasm_name(wasm_exporttype_name(types.data[i]))] = exports.data[i];
		wasm_exporttype_vec_delete(&types);
	}

	wasm_func_t* func(const char* name)
	{
		auto it = by_name.find(name);
		return(it == by_name.end() ? nullptr : wasm_extern_as_func(it->second));
	}

	wasm_memory_t* memory()
	{
		auto it = by_name.find("memory");
		return(it == by_name.end() ? nullptr : wasm_extern_as_memory(it->second));
	}
};

static void report_trap(wasm_trap_t* trap, const char* what)
{
	if(!trap) return;
	wasm_message_t msg;
	wasm_trap_message(trap, &msg);
	FAIL("trap during %s: %.*s", what, (int)msg.size, msg.data);
}

static int32_t call_i32(Instance& inst, const char* name, std::vector<int32_t> argv = {})
{
	wasm_func_t* f = inst.func(name);
	CHECK(f, "missing function %s", name);
	std::vector<wasm_val_t> args_buf(argv.size());
	for(size_t i = 0; i < argv.size(); ++i) args_buf[i] = WASM_I32_VAL(argv[i]);
	wasm_val_t result_buf[1] = { WASM_INIT_VAL };
	wasm_val_vec_t args = { argv.size(), args_buf.data() };
	wasm_val_vec_t results = { 1, result_buf };
	wasm_val_vec_t no_results = WASM_EMPTY_VEC;
	wasm_trap_t* trap = wasm_func_call(f, &args, wasm_func_result_arity(f) ? &results : &no_results);
	report_trap(trap, name);
	return(wasm_func_result_arity(f) ? result_buf[0].of.i32 : 0);
}

static void expect_i32_trap(Instance& inst, const char* name, std::vector<int32_t> argv, const char* expected)
{
	wasm_func_t* f = inst.func(name);
	CHECK(f, "missing function %s", name);
	std::vector<wasm_val_t> args_buf(argv.size());
	for(size_t i = 0; i < argv.size(); ++i) args_buf[i] = WASM_I32_VAL(argv[i]);
	wasm_val_t result = WASM_INIT_VAL;
	wasm_val_vec_t args = { argv.size(), args_buf.data() };
	wasm_val_vec_t results = { 1, &result };
	wasm_trap_t* trap = wasm_func_call(f, &args, &results);
	CHECK(trap, "%s unexpectedly succeeded", name);
	wasm_message_t message;
	wasm_trap_message(trap, &message);
	std::string text(message.data, message.size);
	wasm_byte_vec_delete(&message);
	wasm_trap_delete(trap);
	CHECK(text.find(expected) != std::string::npos, "%s trap mismatch: %s", name, text.c_str());
}

static void write_bytes(wasm_memory_t* memory, uint32_t ptr, const std::string& data)
{
	uint8_t* mem = (uint8_t*)wasm_memory_data(memory);
	size_t mem_size = wasm_memory_data_size(memory);
	CHECK((size_t)ptr + data.size() <= mem_size, "write outside memory");
	memcpy(mem + ptr, data.data(), data.size());
}

static std::string read_bytes(wasm_memory_t* memory, uint32_t ptr, uint32_t len)
{
	uint8_t* mem = (uint8_t*)wasm_memory_data(memory);
	size_t mem_size = wasm_memory_data_size(memory);
	CHECK((size_t)ptr + len <= mem_size, "read outside memory");
	return(std::string((const char*)mem + ptr, len));
}

static std::string read_cstr(wasm_memory_t* memory, uint32_t ptr, uint32_t cap = 4096)
{
	uint8_t* mem = (uint8_t*)wasm_memory_data(memory);
	size_t mem_size = wasm_memory_data_size(memory);
	CHECK(ptr < mem_size, "cstr starts outside memory");
	std::string out;
	for(uint32_t i = 0; i < cap && (size_t)ptr + i < mem_size; ++i)
	{
		if(mem[ptr + i] == 0)
			return(out);
		out.push_back((char)mem[ptr + i]);
	}
	FAIL("unterminated cstr");
}

static uint32_t read_u32(wasm_memory_t* memory, uint32_t ptr)
{
	uint8_t* mem = (uint8_t*)wasm_memory_data(memory);
	size_t mem_size = wasm_memory_data_size(memory);
	CHECK((size_t)ptr + 4 <= mem_size, "u32 read outside memory");
	return((uint32_t)mem[ptr] | ((uint32_t)mem[ptr + 1] << 8) | ((uint32_t)mem[ptr + 2] << 16) | ((uint32_t)mem[ptr + 3] << 24));
}

static void append_varint(std::string& out, size_t value)
{
	do
	{
		uint8_t byte = value & 0x7f;
		value >>= 7;
		out.push_back((char)(byte | (value ? 0x80 : 0)));
	}
	while(value);
}

static std::string brrb_node(char type, const std::string& scalar = "", const std::vector<std::pair<std::string, std::string>>& children = {}, bool list = false)
{
	std::string out;
	out.push_back(list ? 1 : 0);
	out.push_back(type);
	append_varint(out, scalar.size());
	out += scalar;
	append_varint(out, children.size());
	for(const auto& child : children)
	{
		append_varint(out, child.first.size());
		out += child.first;
		out += child.second;
	}
	return(out);
}

static std::string brrb_document(const std::string& node)
{
	return(std::string("BRRB\x02", 5) + node);
}

int main(int argc, char** argv)
{
	const char* core_path = argc > 1 ? argv[1] : "/tmp/bearer/wasm-w1/core.wasm";
	wasm_engine_t* engine = wasm_engine_new();
	CHECK(engine, "engine");
	wasm_store_t* store = wasm_store_new(engine);
	CHECK(store, "store");
	g_store_for_traps = store;

	std::vector<uint8_t> bytes = read_file(core_path);
	wasm_byte_vec_t bv;
	wasm_byte_vec_new(&bv, bytes.size(), (const char*)bytes.data());
	Instance core;
	core.module = wasm_module_new(store, &bv);
	wasm_byte_vec_delete(&bv);
	CHECK(core.module, "module load");

	wasm_importtype_vec_t imports = WASM_EMPTY_VEC;
	wasm_module_imports(core.module, &imports);
	std::vector<wasm_extern_t*> import_externs(imports.size);
	for(size_t i = 0; i < imports.size; ++i)
	{
		std::string mod = wasm_name(wasm_importtype_module(imports.data[i]));
		std::string name = wasm_name(wasm_importtype_name(imports.data[i]));
		const wasm_externtype_t* et = wasm_importtype_type(imports.data[i]);
		CHECK(wasm_externtype_kind(et) == WASM_EXTERN_FUNC, "unexpected non-func import %s.%s", mod.c_str(), name.c_str());
		const wasm_functype_t* ft = wasm_externtype_as_functype_const(et);
		wasm_func_t* fn = nullptr;
		if(mod == "env" && name == "bearer_host_time") fn = wasm_func_new_with_env(store, ft, host_time, nullptr, nullptr);
		else if(mod == "env" && name == "bearer_host_time_precise") fn = wasm_func_new_with_env(store, ft, host_time_precise, nullptr, nullptr);
		else if(mod == "env" && name == "bearer_host_env") fn = wasm_func_new_with_env(store, ft, host_env, nullptr, nullptr);
		else if(mod == "env" && name == "bearer_host_random") fn = wasm_func_new_with_env(store, ft, host_random, nullptr, nullptr);
		else if(mod == "env" && name == "bearer_host_log") fn = wasm_func_new_with_env(store, ft, host_log, nullptr, nullptr);
		else if(mod == "env" && name == "bearer_host_module_reset") fn = wasm_func_new_with_env(store, ft, host_module_reset, nullptr, nullptr);
		else if(mod == "env" && name == "bearer_host_module_staged_size") fn = wasm_func_new_with_env(store, ft, host_module_staged_size, nullptr, nullptr);
		else if(mod == "env" && name == "bearer_host_module_resolve") fn = wasm_func_new_with_env(store, ft, host_module_resolve, nullptr, nullptr);
		else if(mod == "env")
		{
			char* label = strdup((mod + "." + name).c_str());
			fn = wasm_func_new_with_env(store, ft, stub_callback, label, nullptr);
		}
		else if(mod == "wasi_snapshot_preview1")
		{
			char* label = strdup((mod + "." + name).c_str());
			fn = wasm_func_new_with_env(store, ft, stub_callback, label, nullptr);
		}
		else
			FAIL("unexpected core import %s.%s", mod.c_str(), name.c_str());
		CHECK(fn, "import function %s.%s", mod.c_str(), name.c_str());
		import_externs[i] = wasm_func_as_extern(fn);
	}
	wasm_extern_vec_t iv = { import_externs.size(), import_externs.data() };
	wasm_trap_t* trap = nullptr;
	core.instance = wasm_instance_new(store, core.module, &iv, &trap);
	report_trap(trap, "core instantiation");
	CHECK(core.instance, "core instantiate");
	core.index_exports();
	CHECK(core.memory(), "core exports memory");
	g_memory = core.memory();
	if(core.func("_initialize")) call_i32(core, "_initialize");

	CHECK(call_i32(core, "bearer_wasm_core_init") == 0, "core init failed");
	call_i32(core, "bearer_wasm_core_reset_request");
	CHECK(g_module_reset_count == 1, "request reset did not clear module capability state");
	CHECK(call_i32(core, "bearer_wasm_core_abi_version") == BEARER_WASM_CORE_ABI_VERSION, "unexpected ABI version");
	CHECK(core.func("bearer_unit_load"), "core does not export bearer_unit_load");
	CHECK(core.func("bearer_module_call_brrb"), "core does not export bearer_module_call_brrb");
	CHECK(core.func("bearer_dv_none_brrb"), "core does not export bearer_dv_none_brrb");
	CHECK(core.func("bearer_dv_read_brrb"), "core does not export bearer_dv_read_brrb");
	CHECK(core.func("bearer_dv_is_none_brrb"), "core does not export bearer_dv_is_none_brrb");
	CHECK(core.func("bearer_dv_require_brrb"), "core does not export bearer_dv_require_brrb");
	CHECK(core.func("bearer_dv_set_path_brrb"), "core does not export bearer_dv_set_path_brrb");
	CHECK(core.func("bearer_dv_callable_extract_brrb"), "core does not export bearer_dv_callable_extract_brrb");
	CHECK(core.func("bearer_dv_callable_at_brrb"), "core does not export bearer_dv_callable_at_brrb");

	wasm_memory_t* memory = core.memory();
	auto callable_wire = [&](uint32_t pointer, uint32_t type) {
		std::string scalar(8, '\0');
		for(unsigned byte = 0; byte < 4; ++byte)
		{
			scalar[byte] = (char)(pointer >> (byte * 8));
			scalar[byte + 4] = (char)(type >> (byte * 8));
		}
		return(brrb_document(brrb_node('C', scalar)));
	};
	auto expect_forged_callable = [&](uint32_t pointer, const char* label) {
		std::string wire = callable_wire(pointer, 7);
		int32_t wire_ptr = call_i32(core, "bearer_alloc", { (int32_t)wire.size() });
		write_bytes(memory, wire_ptr, wire);
		CHECK(call_i32(core, "bearer_dv_callable_extract_brrb", { wire_ptr, (int32_t)wire.size(), 7 }) == 0, "%s callable extraction accepted", label);
		CHECK(call_i32(core, "bearer_dv_callable_at_brrb", { wire_ptr, (int32_t)wire.size(), 0 }) == 0, "%s callable enumeration accepted", label);
	};
	expect_forged_callable(0, "null");
	expect_forged_callable(1, "unaligned");
	expect_forged_callable((uint32_t)(wasm_memory_data_size(memory) - 4), "out-of-bounds");
	int32_t wrong_type_header = call_i32(core, "bearer_alloc", { 24 });
	std::string header(24, '\0');
	auto store_u32 = [&](size_t offset, uint32_t value) {
		for(unsigned byte = 0; byte < 4; ++byte)
			header[offset + byte] = (char)(value >> (byte * 8));
	};
	store_u32(0, 1); store_u32(4, 1); store_u32(8, 0x40000000); store_u32(12, 24); store_u32(20, 8);
	write_bytes(memory, wrong_type_header, header);
	expect_forged_callable((uint32_t)wrong_type_header, "wrong-type");
	call_i32(core, "bearer_free", { wrong_type_header });
	std::string malformed_callable = brrb_document(brrb_node('C'));
	int32_t malformed_callable_ptr = call_i32(core, "bearer_alloc", { (int32_t)malformed_callable.size() });
	write_bytes(memory, malformed_callable_ptr, malformed_callable);
	CHECK(call_i32(core, "bearer_dv_callable_extract_brrb", { malformed_callable_ptr, (int32_t)malformed_callable.size(), 7 }) == 0, "malformed callable extraction accepted");
	CHECK(call_i32(core, "bearer_dv_callable_at_brrb", { malformed_callable_ptr, (int32_t)malformed_callable.size(), 0 }) == 0, "malformed callable enumeration accepted");
	int32_t root = call_i32(core, "bearer_dv_root");
	CHECK(root != 0, "bearer_dv_root returned null");
	std::string key = "message";
	std::string value = "hello from W1 core";
	int32_t key_ptr = call_i32(core, "bearer_alloc", { (int32_t)key.size() });
	int32_t value_ptr = call_i32(core, "bearer_alloc", { (int32_t)value.size() });
	write_bytes(memory, key_ptr, key);
	write_bytes(memory, value_ptr, value);
	int32_t child = call_i32(core, "bearer_dv_get", { root, key_ptr, (int32_t)key.size() });
	CHECK(child != 0, "bearer_dv_get returned null");
	call_i32(core, "bearer_dv_set_value", { child, value_ptr, (int32_t)value.size() });
	CHECK(call_i32(core, "bearer_dv_find", { root, key_ptr, (int32_t)key.size() }) == child, "bearer_dv_find mismatch");
	CHECK(call_i32(core, "bearer_dv_count", { root }) == 1, "root count mismatch");
	CHECK(call_i32(core, "bearer_dv_is_list", { root }) == 0, "root unexpectedly list-shaped");
	int32_t value_len_ptr = call_i32(core, "bearer_alloc", { 4 });
	int32_t value_result_ptr = call_i32(core, "bearer_dv_value", { child, value_len_ptr });
	uint32_t value_result_len = read_u32(memory, value_len_ptr);
	CHECK(read_bytes(memory, value_result_ptr, value_result_len) == value, "bearer_dv_value mismatch");
	int32_t encoded_len = call_i32(core, "bearer_dv_encode", { root, 0, 0 });
	CHECK(encoded_len > 5, "encoded length too small");
	int32_t encoded_ptr = call_i32(core, "bearer_alloc", { encoded_len });
	CHECK(call_i32(core, "bearer_dv_encode", { root, encoded_ptr, encoded_len }) == encoded_len, "encode length mismatch");
	std::string encoded = read_bytes(memory, encoded_ptr, encoded_len);
	CHECK(encoded.size() >= 5 && encoded.compare(0, 4, "BRRB") == 0 && (unsigned char)encoded[4] == 2, "BRRB2 header missing");
	int32_t decoded = call_i32(core, "bearer_dv_decode", { encoded_ptr, encoded_len });
	CHECK(decoded != 0, "bearer_dv_decode failed");
	CHECK(call_i32(core, "bearer_dv_count", { decoded }) == 1, "decoded root count mismatch");
	int32_t last_error_ptr = call_i32(core, "bearer_dv_last_error");
	CHECK(last_error_ptr != 0, "bearer_dv_last_error returned null");
	CHECK(read_cstr(memory, last_error_ptr) == "", "last error not clear after successful decode");
	std::string bad = "bad";
	int32_t bad_ptr = call_i32(core, "bearer_alloc", { (int32_t)bad.size() });
	write_bytes(memory, bad_ptr, bad);
	CHECK(call_i32(core, "bearer_dv_decode", { bad_ptr, (int32_t)bad.size() }) == 0, "bad BRRB2 decode unexpectedly succeeded");
	CHECK(read_cstr(memory, call_i32(core, "bearer_dv_last_error")) != "", "bad BRRB2 decode did not set error");
	std::string export_name = "missing";
	int32_t export_name_ptr = call_i32(core, "bearer_alloc", { (int32_t)export_name.size() });
	write_bytes(memory, export_name_ptr, export_name);
	expect_i32_trap(core, "bearer_module_call_brrb", { 0, export_name_ptr, (int32_t)export_name.size(), bad_ptr, (int32_t)bad.size(), 0, 0 }, "invalid, stale, or foreign");
	expect_i32_trap(core, "bearer_module_call_brrb", { 7, export_name_ptr, (int32_t)export_name.size(), bad_ptr, (int32_t)bad.size(), 0, 0 }, "malformed BRRB");
	expect_i32_trap(core, "bearer_module_call_brrb", { 7, export_name_ptr, (int32_t)export_name.size(), encoded_ptr, encoded_len, 0, 0 }, "does not export");

	CHECK(call_i32(core, "bearer_dv_merge_brrb", { bad_ptr, (int32_t)bad.size(), encoded_ptr, encoded_len, 0, 0 }) == -1,
		"malformed merge input did not fail");
	int32_t merge_len = call_i32(core, "bearer_dv_merge_brrb", { encoded_ptr, encoded_len, encoded_ptr, encoded_len, 0, 0 });
	CHECK(merge_len > 0, "valid merge sizing failed after malformed input");
	int32_t merge_ptr = call_i32(core, "bearer_alloc", { merge_len });
	CHECK(call_i32(core, "bearer_dv_merge_brrb", { encoded_ptr, encoded_len, encoded_ptr, encoded_len, merge_ptr, merge_len }) == merge_len,
		"valid merge copy failed after malformed input");
	CHECK(call_i32(core, "bearer_dv_decode", { merge_ptr, merge_len }) != 0, "merged BRRB2 did not decode");

	merge_len = call_i32(core, "bearer_dv_merge_brrb", { encoded_ptr, encoded_len, encoded_ptr, encoded_len, 0, 0 });
	CHECK(merge_len > 0, "merge staging failed before malformed reset check");
	CHECK(call_i32(core, "bearer_dv_merge_brrb", { bad_ptr, (int32_t)bad.size(), encoded_ptr, encoded_len, 0, 0 }) == -1,
		"malformed merge did not replace staged state");
	CHECK(call_i32(core, "bearer_dv_merge_brrb", { encoded_ptr, encoded_len, encoded_ptr, encoded_len, merge_ptr, merge_len }) == 0,
		"malformed merge leaked prior staged bytes");

	merge_len = call_i32(core, "bearer_dv_merge_brrb", { encoded_ptr, encoded_len, encoded_ptr, encoded_len, 0, 0 });
	CHECK(merge_len > 0, "merge staging failed before request reset check");
	call_i32(core, "bearer_wasm_core_reset_request");
	CHECK(g_module_reset_count == 2, "second request reset did not clear module capability state");
	CHECK(call_i32(core, "bearer_dv_merge_brrb", { encoded_ptr, encoded_len, encoded_ptr, encoded_len, merge_ptr, merge_len }) == 0,
		"request reset leaked staged merge bytes");
	merge_len = call_i32(core, "bearer_dv_merge_brrb", { encoded_ptr, encoded_len, encoded_ptr, encoded_len, 0, 0 });
	CHECK(merge_len > 0, "valid merge did not recover after request reset");
	CHECK(call_i32(core, "bearer_dv_merge_brrb", { encoded_ptr, encoded_len, encoded_ptr, encoded_len, merge_ptr, merge_len }) == merge_len,
		"valid merge copy did not recover after request reset");

	auto put_brrb = [&](const std::string& bytes) {
		int32_t ptr = call_i32(core, "bearer_alloc", { (int32_t)bytes.size() });
		write_bytes(memory, ptr, bytes);
		return(ptr);
	};
	auto copied_brrb = [&](const char* name, std::vector<int32_t> args) {
		args.insert(args.end(), { 0, 0 });
		int32_t len = call_i32(core, name, args);
		CHECK(len >= 0, "%s sizing failed", name);
		int32_t ptr = call_i32(core, "bearer_alloc", { len });
		args[args.size() - 2] = ptr;
		args.back() = len;
		CHECK(call_i32(core, name, args) == len, "%s copy failed", name);
		return(read_bytes(memory, ptr, len));
	};
	std::string none = copied_brrb("bearer_dv_none_brrb", {});
	CHECK(none == brrb_document(brrb_node('N')), "none BRRB is not deterministic N");
	int32_t none_ptr = put_brrb(none);
	CHECK(call_i32(core, "bearer_dv_is_none_brrb", { none_ptr, (int32_t)none.size() }) == 1, "none presence test failed");
	CHECK(call_i32(core, "bearer_dv_is_none_brrb", { encoded_ptr, encoded_len }) == 0, "map is none");
	CHECK(call_i32(core, "bearer_dv_is_none_brrb", { bad_ptr, (int32_t)bad.size() }) == -1, "malformed none test did not fail");
	CHECK(call_i32(core, "bearer_dv_extract_s32", { none_ptr, (int32_t)none.size(), 37 }) == 37, "none ignored extraction fallback");

	std::string original = brrb_document(brrb_node('M'));
	std::string path = brrb_document(brrb_node('M', "", {{ "0", brrb_node('S', "profile") }, { "1", brrb_node('S', "name") }}, true));
	std::string replacement = brrb_document(brrb_node('S', "copied"));
	std::string profile_selector = brrb_document(brrb_node('S', "profile"));
	std::string negative_selector = brrb_document(brrb_node('F', "-1"));
	std::string one_selector = brrb_document(brrb_node('F', "1"));
	int32_t original_ptr = put_brrb(original);
	int32_t path_ptr = put_brrb(path);
	int32_t replacement_ptr = put_brrb(replacement);
	int32_t extract_len = call_i32(core, "bearer_dv_extract_string", { replacement_ptr, (int32_t)replacement.size(), 0, 0, 0, 0 });
	CHECK(extract_len == 6, "string extraction staging failed");
	call_i32(core, "bearer_wasm_core_reset_request");
	int32_t extract_ptr = call_i32(core, "bearer_alloc", { extract_len });
	CHECK(call_i32(core, "bearer_dv_extract_string", { replacement_ptr, (int32_t)replacement.size(), 0, 0, extract_ptr, extract_len }) == 0,
		"request reset leaked staged string extraction bytes");
	CHECK(call_i32(core, "bearer_dv_extract_string", { replacement_ptr, (int32_t)replacement.size(), 0, 0, 0, 0 }) == extract_len,
		"string extraction did not recover after request reset");
	CHECK(call_i32(core, "bearer_dv_extract_string", { replacement_ptr, (int32_t)replacement.size(), 0, 0, extract_ptr, extract_len }) == extract_len &&
		read_bytes(memory, extract_ptr, extract_len) == "copied", "string extraction copy failed after request reset");
	int32_t profile_selector_ptr = put_brrb(profile_selector);
	int32_t negative_selector_ptr = put_brrb(negative_selector);
	int32_t one_selector_ptr = put_brrb(one_selector);
	std::string updated = copied_brrb("bearer_dv_set_path_brrb", { original_ptr, (int32_t)original.size(), path_ptr, (int32_t)path.size(), replacement_ptr, (int32_t)replacement.size() });
	int32_t updated_ptr = put_brrb(updated);
	std::string profile = copied_brrb("bearer_dv_read_brrb", { updated_ptr, (int32_t)updated.size(), 0, put_brrb("profile"), 7, 0 });
	int32_t profile_ptr = put_brrb(profile);
	CHECK(copied_brrb("bearer_dv_read_brrb", { profile_ptr, (int32_t)profile.size(), 0, put_brrb("name"), 4, 0 }) == replacement, "path update did not write replacement");
	CHECK(copied_brrb("bearer_dv_read_brrb", { original_ptr, (int32_t)original.size(), 0, put_brrb("profile"), 7, 0 }) == none, "path update mutated source value");
	CHECK(call_i32(core, "bearer_dv_read_brrb", { bad_ptr, (int32_t)bad.size(), 0, 0, 0, 0, 0, 0 }) == -2, "malformed safe read did not fail");
	CHECK(copied_brrb("bearer_dv_read_brrb", { replacement_ptr, (int32_t)replacement.size(), 0, put_brrb("x"), 1, 0 }) == none, "scalar safe read is not none");
	CHECK(copied_brrb("bearer_dv_read_brrb", { updated_ptr, (int32_t)updated.size(), 1, 0, 0, -1 }) == none, "negative safe index is not none");
	CHECK(copied_brrb("bearer_dv_require_brrb", { updated_ptr, (int32_t)updated.size(), profile_selector_ptr, (int32_t)profile_selector.size() }) == profile,
		"strict read did not return the present child");
	CHECK(call_i32(core, "bearer_dv_require_brrb", { bad_ptr, (int32_t)bad.size(), profile_selector_ptr, (int32_t)profile_selector.size(), 0, 0 }) == -1, "malformed strict read did not fail");
	CHECK(call_i32(core, "bearer_dv_require_brrb", { replacement_ptr, (int32_t)replacement.size(), profile_selector_ptr, (int32_t)profile_selector.size(), 0, 0 }) == -1, "scalar strict read did not fail");

	std::string list = brrb_document(brrb_node('M', "", {{ "0", brrb_node('S', "old") }}, true));
	std::string index_zero = brrb_document(brrb_node('M', "", {{ "0", brrb_node('F', "0") }}, true));
	int32_t list_ptr = put_brrb(list);
	int32_t index_zero_ptr = put_brrb(index_zero);
	std::string list_updated = copied_brrb("bearer_dv_set_path_brrb", { list_ptr, (int32_t)list.size(), index_zero_ptr, (int32_t)index_zero.size(), replacement_ptr, (int32_t)replacement.size() });
	CHECK(copied_brrb("bearer_dv_read_brrb", { put_brrb(list_updated), (int32_t)list_updated.size(), 1, 0, 0, 0 }) == replacement, "list update failed");
	for(const std::string& invalid : { brrb_node('B', "true"), brrb_node('N'), brrb_node('M'), brrb_node('F', "1.5"), brrb_node('F', "2147483648") })
	{
		std::string invalid_path = brrb_document(brrb_node('M', "", {{ "0", invalid }}, true));
		CHECK(call_i32(core, "bearer_dv_set_path_brrb", { original_ptr, (int32_t)original.size(), put_brrb(invalid_path), (int32_t)invalid_path.size(), replacement_ptr, (int32_t)replacement.size(), 0, 0 }) == -1,
			"invalid path segment was accepted");
	}
	std::string out_of_range = brrb_document(brrb_node('M', "", {{ "0", brrb_node('F', "1") }}, true));
	CHECK(call_i32(core, "bearer_dv_set_path_brrb", { list_ptr, (int32_t)list.size(), put_brrb(out_of_range), (int32_t)out_of_range.size(), replacement_ptr, (int32_t)replacement.size(), 0, 0 }) == -1,
		"sparse list write was accepted");
	std::vector<std::pair<std::string, std::string>> deep_segments;
	for(int i = 0; i < 60; ++i) deep_segments.push_back({ std::to_string(i), brrb_node('S', "k") });
	std::string deep_path = brrb_document(brrb_node('M', "", deep_segments, true));
	std::string deep = copied_brrb("bearer_dv_set_path_brrb", { original_ptr, (int32_t)original.size(), put_brrb(deep_path), (int32_t)deep_path.size(), replacement_ptr, (int32_t)replacement.size() });
	CHECK(call_i32(core, "bearer_dv_decode", { put_brrb(deep), (int32_t)deep.size() }) != 0, "bounded deep path did not decode");
	CHECK(call_i32(core, "bearer_dv_require_brrb", { list_ptr, (int32_t)list.size(), negative_selector_ptr, (int32_t)negative_selector.size(), 0, 0 }) == -1,
		"negative strict index did not fail");
	CHECK(call_i32(core, "bearer_dv_require_brrb", { list_ptr, (int32_t)list.size(), one_selector_ptr, (int32_t)one_selector.size(), 0, 0 }) == -1,
		"out-of-range strict index did not fail");
	CHECK(call_i32(core, "bearer_dv_set_path_brrb", { bad_ptr, (int32_t)bad.size(), path_ptr, (int32_t)path.size(), replacement_ptr, (int32_t)replacement.size(), 0, 0 }) == -1,
		"malformed path root did not fail");
	CHECK(call_i32(core, "bearer_dv_set_path_brrb", { original_ptr, (int32_t)original.size(), bad_ptr, (int32_t)bad.size(), replacement_ptr, (int32_t)replacement.size(), 0, 0 }) == -1,
		"malformed path selector did not fail");
	CHECK(call_i32(core, "bearer_dv_set_path_brrb", { original_ptr, (int32_t)original.size(), path_ptr, (int32_t)path.size(), bad_ptr, (int32_t)bad.size(), 0, 0 }) == -1,
		"malformed path replacement did not fail");

	std::string out = "W1 output";
	int32_t out_ptr = call_i32(core, "bearer_alloc", { (int32_t)out.size() });
	write_bytes(memory, out_ptr, out);
	call_i32(core, "bearer_print_bytes", { out_ptr, (int32_t)out.size() });
	call_i32(core, "bearer_wasm_finish_output");
	int32_t output_len = call_i32(core, "bearer_wasm_output_size");
	int32_t output_ptr = call_i32(core, "bearer_wasm_output_data");
	CHECK(read_bytes(memory, output_ptr, output_len) == out, "output plumbing mismatch");

	printf("W1 core.wasm smoke: abi=%d encoded=%d output=%d\n", BEARER_WASM_CORE_ABI_VERSION, encoded_len, output_len);
	printf("W1 EXIT CRITERION: PASS\n");
	return(0);
}
