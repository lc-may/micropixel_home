// SPDX-License-Identifier: Apache-2.0
#include "guest_runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "bflb_mtimer.h"
#include "mm.h"
#include "platform_api_vmcore.h"
#include "runtime/abi/timer_endpoint.hpp"
#include "runtime/components.hpp"
#include "wasm_export.h"
#if defined(CONFIG_XZ)
#include "platform/storage/embedded_xz.hpp"
#endif

namespace micropixel::bouffalo {
namespace {

using runtime::EventWaitResult;

#if defined(MICROPIXEL_ORION_HOST)
constexpr uint32_t kWasmStackBytes = 16U * 1024U;

uint32_t LinearMemoryLimit(wasm_module_t module, uint32_t growth_pages) {
    const auto exports = wasm_runtime_get_export_count(module);
    for (int32_t i = 0; i < exports; ++i) {
        wasm_export_t item{};
        wasm_runtime_get_export_type(module, i, &item);
        if (item.kind != WASM_IMPORT_EXPORT_KIND_MEMORY || !item.u.memory_type) continue;
        const auto initial = wasm_memory_type_get_init_page_count(item.u.memory_type);
        const auto maximum = wasm_memory_type_get_max_page_count(item.u.memory_type);
        if (initial > maximum) return 0;
        const auto limit = initial + std::min(growth_pages, maximum - initial);
        return limit;
    }
    return 0;
}
#else
constexpr uint32_t kWasmStackBytes = 8192U;
#endif
#if WASM_ENABLE_INSTRUCTION_METERING
constexpr int kWasmInstructionBudget = 100'000'000;
#endif

GuestContext& Context(wasm_exec_env_t env) {
    return *static_cast<GuestContext*>(wasm_runtime_get_custom_data(wasm_runtime_get_module_inst(env)));
}

uint32_t AbiVersion(wasm_exec_env_t) { return MICROPIXEL_ABI_VERSION; }

uint64_t ClockNow(wasm_exec_env_t env) {
    return static_cast<uint64_t>(bflb_mtimer_get_time_us() - Context(env).clock_origin_us);
}

int32_t LogWrite(wasm_exec_env_t, uint32_t level, const char* bytes, uint32_t size) {
    if ((bytes == nullptr && size != 0U) || size > MICROPIXEL_ABI_MAX_LOG_BYTES || level > MICROPIXEL_LOG_ERROR) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (size != 0U) std::printf("Guest: %.*s\n", static_cast<int>(size), bytes);
    return MICROPIXEL_STATUS_OK;
}

int32_t EventWait(wasm_exec_env_t env, uint8_t* out, uint32_t capacity, uint64_t timeout) {
    if (out == nullptr || capacity < sizeof(micropixel_event_t)) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    micropixel_event_t event{};
    auto& context = Context(env);
    const auto start = bflb_mtimer_get_time_us();
    EventWaitResult result;
    for (;;) {
        if (context.components && !context.stopping.load(std::memory_order_acquire) &&
            context.components->TakeEvent(event)) {
            result = EventWaitResult::kReceived;
            break;
        }
        const auto elapsed = bflb_mtimer_get_time_us() - start;
        const uint64_t remaining = timeout == UINT64_MAX ? timeout : elapsed < timeout ? timeout - elapsed : 0U;
        result = context.timers ? context.timers->Wait(context.events, event, remaining)
                                : context.events.Wait(event, remaining);
        if (result != EventWaitResult::kReceived || event.service_id != MICROPIXEL_SERVICE_LIFECYCLE ||
            event.event_id != 0)
            break;
    }
#if WASM_ENABLE_INSTRUCTION_METERING
    // Refill after yielding to the scheduler for at least one tick. Polling
    // with zero timeout and already-queued events do not renew the budget.
    if (timeout != 0 && bflb_mtimer_get_time_us() - start >= 1'000'000 / configTICK_RATE_HZ) {
        context.instruction_budget_epoch = !context.instruction_budget_epoch;
        wasm_runtime_set_instruction_count_limit(env, kWasmInstructionBudget + context.instruction_budget_epoch);
    }
#endif
    switch (result) {
        case EventWaitResult::kReceived:
            std::memcpy(out, &event, sizeof(event));
            return MICROPIXEL_STATUS_OK;
        case EventWaitResult::kTimeout:
            return MICROPIXEL_STATUS_TIMEOUT;
        case EventWaitResult::kClosed:
            return MICROPIXEL_STATUS_CLOSED;
    }
    return MICROPIXEL_STATUS_INTERNAL;
}

int32_t ServiceOpen(wasm_exec_env_t env, uint32_t service, uint32_t version, uint8_t* out, uint32_t capacity) {
    if (out == nullptr || capacity < sizeof(micropixel_service_info_t)) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    micropixel_service_info_t info{};
    const int32_t status = Context(env).services.Open(service, version, info, capacity);
    if (status == MICROPIXEL_STATUS_OK) std::memcpy(out, &info, sizeof(info));
    return status;
}

int32_t ServiceCall(wasm_exec_env_t env, uint32_t handle, uint32_t method, const uint8_t* request, uint32_t length,
                    uint8_t* response, uint32_t capacity, uint8_t* size_out) {
    if (size_out == nullptr ||
        !wasm_runtime_validate_native_addr(wasm_runtime_get_module_inst(env), size_out, sizeof(uint32_t))) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    uint32_t size{};
    auto& context = Context(env);
    const auto service = context.services.ServiceId(handle);
    if (context.components && service == MICROPIXEL_SERVICE_AUDIO && method == MICROPIXEL_AUDIO_METHOD_STOP_ALL &&
        length == 0 && context.components->StopAudioScoped()) {
        std::memcpy(size_out, &size, sizeof(size));
        return MICROPIXEL_STATUS_OK;
    }
    auto status = context.components ? context.components->BeforeCall(service, method) : MICROPIXEL_STATUS_OK;
    if (status == MICROPIXEL_STATUS_OK) {
        status = context.services.Call(handle, method, request, length, response, capacity, size);
        if (context.components && service != MICROPIXEL_SERVICE_LIFECYCLE &&
            (status == MICROPIXEL_STATUS_OK || status == MICROPIXEL_STATUS_NOT_FOUND ||
             status == MICROPIXEL_STATUS_STALE_STATE || status == MICROPIXEL_STATUS_CLOSED))
            context.components->AfterCall(handle, service, method, request, length, response, size, status);
    }
    std::memcpy(size_out, &size, sizeof(size));
    return status;
}

int32_t ServiceSubmit(wasm_exec_env_t env, uint32_t handle, uint32_t channel, const uint8_t* bytes, uint32_t length) {
    auto& context = Context(env);
    if (context.components && context.services.ServiceId(handle) == MICROPIXEL_SERVICE_GRAPHICS &&
        !context.components->CanDraw())
        return MICROPIXEL_STATUS_PERMISSION_DENIED;
    return context.services.Submit(handle, channel, bytes, length);
}

NativeSymbol symbols[] = {
    {"abi_version", reinterpret_cast<void*>(AbiVersion), "()i", nullptr},
    {"log_write", reinterpret_cast<void*>(LogWrite), "(i*~)i", nullptr},
    {"clock_now", reinterpret_cast<void*>(ClockNow), "()I", nullptr},
    {"event_wait", reinterpret_cast<void*>(EventWait), "(*~I)i", nullptr},
    {"service_open", reinterpret_cast<void*>(ServiceOpen), "(ii*~)i", nullptr},
    {"service_call", reinterpret_cast<void*>(ServiceCall), "(ii*~*~*)i", nullptr},
    {"service_submit", reinterpret_cast<void*>(ServiceSubmit), "(ii*~)i", nullptr},
};

}  // namespace

const uint8_t* GuestMemory::Resolve(uint32_t offset, uint32_t length) const {
    const auto instance = static_cast<wasm_module_inst_t>(module);
    if (!instance || !offset || !length || !wasm_runtime_validate_app_addr(instance, offset, length)) return nullptr;
    return static_cast<const uint8_t*>(wasm_runtime_addr_app_to_native(instance, offset));
}

bool InitializeGuestRuntime() {
    RuntimeInitArgs args{};
    args.mem_alloc_type = Alloc_With_Allocator;
    args.mem_alloc_option.allocator.malloc_func = reinterpret_cast<void*>(os_malloc);
    args.mem_alloc_option.allocator.realloc_func = reinterpret_cast<void*>(os_realloc);
    args.mem_alloc_option.allocator.free_func = reinterpret_cast<void*>(os_free);
    args.native_module_name = "micropixel";
    args.native_symbols = symbols;
    args.n_native_symbols = sizeof(symbols) / sizeof(symbols[0]);
    if (!wasm_runtime_full_init(&args)) return false;
#if defined(MICROPIXEL_ORION_HOST)
    wasm_runtime_set_linear_memory_reserve_max(true);
    wasm_runtime_set_enlarge_mem_error_callback(
        [](uint32_t pages, uint64_t size, uint32_t, enlarge_memory_error_reason_t reason, wasm_module_inst_t,
           wasm_exec_env_t, void*) {
            std::printf("MPX GROW failed linear=%lu additional_pages=%lu reason=%s\n", static_cast<unsigned long>(size),
                        static_cast<unsigned long>(pages), reason == MAX_SIZE_REACHED ? "limit" : "allocation");
        },
        nullptr);
#endif
    return true;
}

int32_t RunGuest(GuestContext& context, std::span<const uint8_t> binary, uint32_t unpacked_size,
                 const char* launch_name, [[maybe_unused]] uint32_t linear_growth_pages) {
    const auto start_us = bflb_mtimer_get_time_us();
    // The loader may rewrite the input, and some sections remain backed by it.
    const uint32_t size = unpacked_size ? unpacked_size : binary.size();
    auto* image = static_cast<uint8_t*>(os_malloc(size));
    if (image == nullptr) return -1;
    if (unpacked_size) {
#if defined(CONFIG_XZ)
        const bool valid = DecodeEmbeddedXz(binary, {image, size});
#else
        const bool valid = false;
#endif
        if (!valid) {
            std::puts("MicroPixel Guest image decompression failed");
            os_free(image);
            return -1;
        }
    } else {
        std::memcpy(image, binary.data(), size);
    }
    return RunGuestImage(context, {image, size}, launch_name, linear_growth_pages, start_us);
}

#if WASM_ENABLE_AOT && defined(MICROPIXEL_ORION_HOST)
bool ValidateGuestAot(std::span<uint8_t> image) {
    constexpr uint8_t magic[]{0, 'a', 'o', 't', 6, 0, 0, 0};
    if (image.size() < sizeof(magic) || std::memcmp(image.data(), magic, sizeof(magic)) != 0) return false;
    char error[128]{};
    const LoadArgs args{.wasm_binary_freeable = true};
    const auto module = wasm_runtime_load_ex(image.data(), image.size(), &args, error, sizeof(error));
    if (!module) {
        std::printf("MPX STORE AOT rejected: %s\n", error);
        return false;
    }
    bool valid = false;
    const auto initial_pages = LinearMemoryLimit(module, 0);
    if (initial_pages != 0 && initial_pages <= 64U) {
        for (int32_t i = 0; i < wasm_runtime_get_export_count(module); ++i) {
            wasm_export_t item{};
            wasm_runtime_get_export_type(module, i, &item);
            if (item.kind == WASM_IMPORT_EXPORT_KIND_FUNC && item.name &&
                std::strcmp(item.name, "__micropixel_start") == 0 && item.u.func_type &&
                wasm_func_type_get_param_count(item.u.func_type) == 0 &&
                wasm_func_type_get_result_count(item.u.func_type) == 1 &&
                wasm_func_type_get_result_valkind(item.u.func_type, 0) == WASM_I32) {
                valid = true;
                break;
            }
        }
    }
    wasm_runtime_unload(module);
    return valid;
}
#endif

int32_t RunGuestImage(GuestContext& context, std::span<uint8_t> binary, const char* launch_name,
                      [[maybe_unused]] uint32_t linear_growth_pages, uint64_t start_us) {
    auto* image = binary.data();
    const uint32_t size = binary.size();
    const auto image_us = bflb_mtimer_get_time_us();
    char error[128]{};
    constexpr const char* backend = WASM_ENABLE_AOT ? "aot" : "wasm";
#if WASM_ENABLE_AOT && defined(MICROPIXEL_ORION_HOST)
    // Keep relocated code in PSRAM and release the temporary decompressed file.
    const LoadArgs load_args{.wasm_binary_freeable = true};
    wasm_module_t module = wasm_runtime_load_ex(image, size, &load_args, error, sizeof(error));
    if (module && wasm_runtime_is_underlying_binary_freeable(module)) {
        os_free(image);
        image = nullptr;
    }
#else
    wasm_module_t module = wasm_runtime_load(image, size, error, sizeof(error));
#endif
    const auto loaded_us = bflb_mtimer_get_time_us();
#if defined(MICROPIXEL_ORION_HOST)
    const uint32_t maximum = module ? LinearMemoryLimit(module, linear_growth_pages) : 0;
    if (module && !maximum) std::snprintf(error, sizeof(error), "missing Guest linear memory budget");
#else
    constexpr uint32_t maximum = 64U;
#endif
    const InstantiationArgs args{
        .default_stack_size = kWasmStackBytes,
        .max_memory_pages = maximum,
    };
    wasm_module_inst_t instance =
        module && maximum ? wasm_runtime_instantiate_ex(module, &args, error, sizeof(error)) : nullptr;
    wasm_exec_env_t exec = instance ? wasm_runtime_create_exec_env(instance, kWasmStackBytes) : nullptr;
    wasm_function_inst_t entry = instance ? wasm_runtime_lookup_function(instance, "__micropixel_start") : nullptr;
    uint32_t result[1]{};
    int32_t status = -1;
    if (exec != nullptr && entry != nullptr) {
        if (launch_name) {
            const auto memory = wasm_runtime_get_default_memory(instance);
            const auto page_bytes = memory ? wasm_memory_get_bytes_per_page(memory) : 0;
            const auto pages = memory ? wasm_memory_get_cur_page_count(memory) : 0;
            const auto max_pages = memory ? wasm_memory_get_max_page_count(memory) : 0;
            const auto base = memory ? wasm_memory_get_base_address(memory) : nullptr;
            const auto reserved = base ? kmalloc_size(base) : 0;
            std::printf(
                "MPX LOAD %s image=%lu load=%lu instantiate=%lu bytes=%lu backend=%s linear=%lu limit=%lu "
                "reserved=%lu\n",
                launch_name, static_cast<unsigned long>(image_us - start_us),
                static_cast<unsigned long>(loaded_us - image_us),
                static_cast<unsigned long>(bflb_mtimer_get_time_us() - loaded_us), static_cast<unsigned long>(size),
                backend, static_cast<unsigned long>(pages * page_bytes),
                static_cast<unsigned long>(max_pages * page_bytes), static_cast<unsigned long>(reserved));
        }
        wasm_runtime_set_custom_data(instance, &context);
        if (context.memory) context.memory->module = instance;
#if WASM_ENABLE_INSTRUCTION_METERING
        context.instruction_budget_epoch = false;
        wasm_runtime_set_instruction_count_limit(exec, kWasmInstructionBudget);
#endif
        if (wasm_runtime_call_wasm(exec, entry, 0, result)) {
            status = static_cast<int32_t>(result[0]);
        } else if (const char* exception = wasm_runtime_get_exception(instance)) {
            std::printf("MicroPixel Guest exception: %s\n", exception);
        }
    } else {
        std::printf("MicroPixel Guest setup failed: %s\n",
                    error[0] == '\0' ? "missing execution environment or entry" : error);
    }

    if (context.memory) context.memory->module = nullptr;
    if (exec != nullptr) wasm_runtime_destroy_exec_env(exec);
    if (instance != nullptr) wasm_runtime_deinstantiate(instance);
    if (module != nullptr) wasm_runtime_unload(module);
    os_free(image);
    return status;
}

void ShutdownGuestRuntime() { wasm_runtime_destroy(); }

}  // namespace micropixel::bouffalo
