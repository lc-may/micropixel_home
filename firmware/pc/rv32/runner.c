// SPDX-License-Identifier: Apache-2.0
#include "abi/micropixel_abi.h"
#include "platform_api_vmcore.h"
#include "protocol.h"
#include "wasm_export.h"

static uint32_t sequence;
static struct mp_rv_frame call;
static uint64_t rpc(wasm_exec_env_t env, uint32_t kind) {
    call.magic = MP_RV_MAGIC;
    call.version = MP_RV_VERSION;
    call.kind = kind;
    call.sequence = ++sequence;
    call.memory_base = call.memory_size = 0;
    if (env) {
        wasm_memory_inst_t memory = wasm_runtime_get_memory(wasm_runtime_get_module_inst(env), 0);
        if (memory) {
            call.memory_base = (uint32_t)(uintptr_t)wasm_memory_get_base_address(memory);
            call.memory_size = wasm_memory_get_cur_page_count(memory) * wasm_memory_get_bytes_per_page(memory);
        }
    }
    __asm__ volatile("fence rw,rw" ::: "memory");
    mp_rv_uart_write(&call, sizeof(call));
    struct mp_rv_frame reply;
    mp_rv_uart_read(&reply, sizeof(reply));
    if (reply.magic != MP_RV_MAGIC || reply.version != MP_RV_VERSION || reply.kind != MP_RV_REPLY ||
        reply.sequence != call.sequence || reply.payload_size || reply.reserved)
        mp_rv_exit(1);
    return ((uint64_t)reply.result_hi << 32) | reply.result_lo;
}
static uint32_t address(const void* p) { return (uint32_t)(uintptr_t)p; }
static uint32_t abi(wasm_exec_env_t env) { return rpc(env, MP_RV_ABI); }
static int32_t log_write(wasm_exec_env_t env, uint32_t level, const void* bytes, uint32_t length) {
    call.args[0] = level;
    call.args[1] = address(bytes);
    call.args[2] = length;
    return rpc(env, MP_RV_LOG);
}
static int32_t event_wait(wasm_exec_env_t env, void* out, uint32_t capacity, uint64_t timeout) {
    call.args[0] = address(out);
    call.args[1] = capacity;
    call.args[2] = timeout;
    call.args[3] = timeout >> 32;
    return rpc(env, MP_RV_EVENT);
}
static uint64_t clock_now(wasm_exec_env_t env) { return rpc(env, MP_RV_CLOCK); }
static int32_t service_open(wasm_exec_env_t env, uint32_t id, uint32_t version, void* out, uint32_t capacity) {
    call.args[0] = id;
    call.args[1] = version;
    call.args[2] = address(out);
    call.args[3] = capacity;
    return rpc(env, MP_RV_OPEN);
}
static int32_t service_call(wasm_exec_env_t env, uint32_t handle, uint32_t method, const void* request, uint32_t size,
                            void* response, uint32_t capacity, uint32_t* response_size) {
    call.args[0] = handle;
    call.args[1] = method;
    call.args[2] = address(request);
    call.args[3] = size;
    call.args[4] = address(response);
    call.args[5] = capacity;
    call.args[6] = address(response_size);
    return rpc(env, MP_RV_CALL);
}
static int32_t service_submit(wasm_exec_env_t env, uint32_t handle, uint32_t channel, const void* bytes,
                              uint32_t size) {
    call.args[0] = handle;
    call.args[1] = channel;
    call.args[2] = address(bytes);
    call.args[3] = size;
    return rpc(env, MP_RV_SUBMIT);
}
static NativeSymbol symbols[] = {{"abi_version", abi, "()i", NULL},
                                 {"log_write", log_write, "(i*~)i", NULL},
                                 {"event_wait", event_wait, "(*~I)i", NULL},
                                 {"clock_now", clock_now, "()I", NULL},
                                 {"service_open", service_open, "(ii*~)i", NULL},
                                 {"service_call", service_call, "(ii*~*~*)i", NULL},
                                 {"service_submit", service_submit, "(ii*~)i", NULL}};
int main(void) {
    if (rpc(NULL, MP_RV_HELLO) != 0) return 1;
    const struct mp_rv_boot* boot = (const struct mp_rv_boot*)MP_RV_BOOT_ADDRESS;
    if (boot->magic != MP_RV_MAGIC || boot->version != MP_RV_VERSION || !boot->aot_size ||
        boot->aot_size > MP_RV_AOT_LIMIT) {
        os_printf("Invalid RV32 boot descriptor\n");
        return 1;
    }
    if (!wasm_runtime_init() ||
        !wasm_runtime_register_natives("micropixel", symbols, sizeof(symbols) / sizeof(symbols[0])))
        return 1;
    char error[256];
    wasm_module_t module = wasm_runtime_load((uint8_t*)MP_RV_AOT_ADDRESS, boot->aot_size, error, sizeof(error));
    if (!module) {
        os_printf("AOT load failed: %s\n", error);
        return 1;
    }
    InstantiationArgs config = {0};
    config.default_stack_size = 64 * 1024;
    config.max_memory_pages = MP_RV_MEMORY_LIMIT / 65536;
    for (int32_t i = 0; i < wasm_runtime_get_export_count(module); ++i) {
        wasm_export_t item;
        wasm_runtime_get_export_type(module, i, &item);
        if (item.kind == WASM_IMPORT_EXPORT_KIND_MEMORY) {
            uint32_t max = wasm_memory_type_get_max_page_count(item.u.memory_type);
            if (max < config.max_memory_pages) config.max_memory_pages = max;
        }
    }
    wasm_module_inst_t instance = wasm_runtime_instantiate_ex(module, &config, error, sizeof(error));
    if (!instance) {
        os_printf("AOT instantiate failed: %s\n", error);
        return 1;
    }
    wasm_exec_env_t exec = wasm_runtime_create_exec_env(instance, 64 * 1024);
    wasm_function_inst_t start = wasm_runtime_lookup_function(instance, "__micropixel_start");
    uint32_t result = 0;
    os_printf("RV32 AOT ready: ILP32F, WAMR AOT v6\n");
    bool ok = exec && start && wasm_runtime_call_wasm(exec, start, 0, &result);
    if (!ok) {
        os_printf("AOT execution failed: %s\n", wasm_runtime_get_exception(instance));
        result = 1;
    }
    if (exec) wasm_runtime_destroy_exec_env(exec);
    wasm_runtime_deinstantiate(instance);
    wasm_runtime_unload(module);
    wasm_runtime_destroy();
    return result;
}
