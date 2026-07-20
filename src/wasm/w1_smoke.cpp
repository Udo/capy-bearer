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
	CHECK(argv.size() <= 4, "too many args for %s", name);
	wasm_val_t args_buf[4];
	for(size_t i = 0; i < argv.size(); ++i) args_buf[i] = WASM_I32_VAL(argv[i]);
	wasm_val_t result_buf[1] = { WASM_INIT_VAL };
	wasm_val_vec_t args = { argv.size(), args_buf };
	wasm_val_vec_t results = { 1, result_buf };
	wasm_val_vec_t no_results = WASM_EMPTY_VEC;
	wasm_trap_t* trap = wasm_func_call(f, &args, wasm_func_result_arity(f) ? &results : &no_results);
	report_trap(trap, name);
	return(wasm_func_result_arity(f) ? result_buf[0].of.i32 : 0);
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
	CHECK(call_i32(core, "bearer_wasm_core_abi_version") == BEARER_WASM_CORE_ABI_VERSION, "unexpected ABI version");

	wasm_memory_t* memory = core.memory();
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
