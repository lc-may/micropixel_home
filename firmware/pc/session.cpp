// SPDX-License-Identifier: Apache-2.0
#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>

#include "work/background_executor.hpp"

namespace micropixel::pc {
Session::Session(PortableApp& app, device::DeviceServices& devices)
    : app_(app),
      devices_(devices),
      launch_arguments_(app.launch_arguments),
      component_flags_(app.requirements.component_flags) {}
Session::~Session() { Stop(); }
void Session::Start() {
    controller_ = std::thread([this] { Control(); });
    worker_ = std::thread([this] { Run(); });
}
void Session::RequestStop() {
    stop_requested_.store(true);
    control_wake_.notify_all();
}
void Session::RequestSuspend() {
    want_suspended_.store(true);
    control_wake_.notify_all();
}
void Session::RequestResume(bool ensure_activity) {
    if (ensure_activity) ensure_activity_.store(true);
    want_suspended_.store(false);
    control_wake_.notify_all();
}
void Session::Control() {
    bool stopping = false, forced = false, applied_visible = false;
    std::chrono::steady_clock::time_point stop_time{};
    std::unique_lock lock(mutex_);
    while (!Done()) {
        if (context_) {
            if (component_flags_) {
                const bool visible = visible_.load();
                if (visible != applied_visible) {
                    context_->components().SetVisible(visible);
                    applied_visible = visible;
                }
                if (ensure_activity_.exchange(false)) context_->components().EnsureActivity();
                if (ensure_service_.exchange(false)) context_->components().EnsureService();
                const auto info = context_->components().Info();
                components_registered_.store((info.flags & MICROPIXEL_COMPONENT_FLAG_REGISTERED) != 0);
                activity_state_.store(info.activity_state);
                activity_token_.store(info.activity_token);
                service_token_.store(info.service_token);
                service_state_.store(info.service_state);
                resource_count_.store(info.resource_count);
            }
            if (stop_requested_.load()) {
                if (!stopping) {
                    (void)context_->RequestStop();
                    stopping = true;
                    stop_time = std::chrono::steady_clock::now();
                }
                if (!forced && std::chrono::steady_clock::now() - stop_time > std::chrono::milliseconds(500)) {
                    if (instance_) wasm_runtime_terminate(instance_);
                    if (child_pid_ > 0) kill(child_pid_, SIGTERM);
                    if (!app_.rv32) context_->ForceStop();
                    forced = true;
                }
            } else if (want_suspended_.load() != suspended_.load()) {
                if (want_suspended_.load()) {
                    if (context_->Suspend(pdMS_TO_TICKS(100))) suspended_.store(true);
                } else if (context_->Resume())
                    suspended_.store(false);
            }
        }
        control_wake_.wait_for(lock, std::chrono::milliseconds(5));
    }
}
void Session::Stop() {
    if (!worker_.joinable()) return;
    RequestStop();
    worker_.join();
    control_wake_.notify_all();
    if (controller_.joinable()) controller_.join();
    JoinTasks(this);
}
void Session::WriteGuestLog(const char* id, uint32_t level, const uint8_t* bytes, size_t length, uint64_t) {
    std::fprintf(stderr, "[Guest %s level=%u] %.*s\n", id, level, static_cast<int>(length),
                 reinterpret_cast<const char*>(bytes));
}
void Session::Run() {
    BindTaskOwner(this);
    if (app_.rv32) {
        RunRv32();
        done_.store(true, std::memory_order_release);
        return;
    }
    if (!wasm_runtime_init_thread_env()) {
        done_ = true;
        return;
    }
    char error[256]{};
    // WAMR's fast interpreter rewrites the input module during loading. Keep
    // the descriptor pristine so the same app can be opened again from Hall.
    auto module_bytes = app_.wasm;
    auto module = wasm_runtime_load(module_bytes.data(), module_bytes.size(), error, sizeof(error));
    if (!module) {
        std::fprintf(stderr, "Wasm load failed: %s\n", error);
        done_ = true;
        wasm_runtime_destroy_thread_env();
        return;
    }
    wasm_module_inst_t instance = nullptr;
    wasm_exec_env_t exec = nullptr;
    {
        work::BackgroundExecutor background;
        auto arguments = launch_arguments_;
        arguments.size = sizeof(arguments);
        auto owned_context = std::make_unique<runtime::GuestContext>(app_.package, devices_, background, "en",
                                                                     arguments, this, component_flags_);
        auto& context = *owned_context;
        InstantiationArgs config{};
        config.default_stack_size = 64U * 1024;
        config.max_memory_pages = 256;
        // A smaller module ceiling is already a sufficient bound. WAMR rejects
        // attempts to override it with a larger value.
        for (int32_t i = 0; i < wasm_runtime_get_export_count(module); ++i) {
            wasm_export_t item{};
            wasm_runtime_get_export_type(module, i, &item);
            if (item.kind == WASM_IMPORT_EXPORT_KIND_MEMORY)
                config.max_memory_pages =
                    std::min(config.max_memory_pages, wasm_memory_type_get_max_page_count(item.u.memory_type));
        }
        instance = wasm_runtime_instantiate_ex(module, &config, error, sizeof(error));
        if (!instance || !context.valid()) {
            std::fprintf(stderr, "Guest initialization failed: %s\n", error);
        } else {
            wasm_runtime_set_custom_data(instance, &context);
            context.BindGuestMemory({.context = instance,
                                     .resolve =
                                         [](void* ctx, uint32_t offset, uint32_t length, uint8_t** out) {
                                             auto inst = static_cast<wasm_module_inst_t>(ctx);
                                             if (!out || !length) return false;
                                             // Match AppSession: validation must return INVALID_MEMORY,
                                             // rather than leave a pending WAMR bounds exception.
                                             auto memory = wasm_runtime_get_memory(inst, 0);
                                             if (!memory) return false;
                                             const uint64_t size = uint64_t(wasm_memory_get_cur_page_count(memory)) *
                                                                   wasm_memory_get_bytes_per_page(memory);
                                             if (offset >= size || length > size - offset) return false;
                                             *out =
                                                 static_cast<uint8_t*>(wasm_runtime_addr_app_to_native(inst, offset));
                                             return *out != nullptr;
                                         },
                                     .stable_base = false});
            exec = wasm_runtime_create_exec_env(instance, 64U * 1024);
            auto start = wasm_runtime_lookup_function(instance, "__micropixel_start");
            {
                std::lock_guard lock(mutex_);
                context_ = &context;
                instance_ = instance;
                ready_.store(true, std::memory_order_release);
                control_wake_.notify_all();
                if (stop_requested_) (void)context.RequestStop();
            }
            uint32_t result = 0;
            const bool ok = exec && start && wasm_runtime_call_wasm(exec, start, 0, &result);
            if (!ok)
                std::fprintf(stderr, "Guest failed: %s %s\n", wasm_runtime_get_exception(instance),
                             context.LastPanic());
            result_ = ok ? static_cast<int>(result) : 1;
            if (ok) std::fprintf(stderr, "Guest completed: result=%d\n", result_);
            {
                std::lock_guard lock(mutex_);
                context_ = nullptr;
                instance_ = nullptr;
            }
            // Release every service that may still borrow Guest linear memory
            // before destroying the module instance.
            context.ForceStop();
            wasm_runtime_set_custom_data(instance, nullptr);
            // Services copy PCM data and Host-owned surfaces; stable_base=false
            // prevents any asynchronous borrowing of Guest direct buffers.
        }
    }
    if (exec) wasm_runtime_destroy_exec_env(exec);
    if (instance) wasm_runtime_deinstantiate(instance);
    wasm_runtime_unload(module);
    wasm_runtime_destroy_thread_env();
    done_.store(true, std::memory_order_release);
}
int32_t PcInput::GetInfo(micropixel_input_info_t& info) {
    info = {.size = sizeof(info),
            .max_touch_points = 1,
            .capabilities = MICROPIXEL_INPUT_CAP_PRESSURE | MICROPIXEL_INPUT_CAP_KEY_EVENTS | MICROPIXEL_INPUT_CAP_TEXT,
            .logical_width = width_,
            .logical_height = height_,
            .reserved0 = 0};
    return MICROPIXEL_STATUS_OK;
}
void PcInput::BindTouchSink(device::TouchSink sink, void* ctx) {
    std::lock_guard lock(mutex_);
    touch_ = sink;
    touch_context_ = ctx;
}
void PcInput::UnbindTouchSink(void* ctx) {
    std::lock_guard lock(mutex_);
    if (touch_context_ == ctx) {
        touch_ = nullptr;
        touch_context_ = nullptr;
    }
}
void PcInput::BindKeySink(device::KeySink sink, void* ctx) {
    std::lock_guard lock(mutex_);
    key_ = sink;
    key_context_ = ctx;
}
void PcInput::UnbindKeySink(void* ctx) {
    std::lock_guard lock(mutex_);
    if (key_context_ == ctx) {
        key_ = nullptr;
        key_context_ = nullptr;
    }
}
bool PcInput::InjectTouch(const device::TouchSample& sample) {
    std::lock_guard lock(mutex_);
    if (!focused_) return false;
    last_touch_ = sample;
    touching_ = sample.phase == device::TouchPhase::kDown || sample.phase == device::TouchPhase::kMove;
    return touch_ && touch_(touch_context_, sample);
}
bool PcInput::InjectKey(const device::KeySample& sample) {
    std::lock_guard lock(mutex_);
    if (!focused_) return false;
    const auto index = static_cast<size_t>(sample.code);
    if (index < down_keys_.size())
        down_keys_[index] = sample.phase == device::KeyPhase::kDown || sample.phase == device::KeyPhase::kRepeat;
    return key_ && key_(key_context_, sample);
}
void PcInput::SetFocused(bool value) {
    std::lock_guard lock(mutex_);
    if (focused_.exchange(value) == value) return;
    text_length_ = 0;
    if (value) return;
    if (touching_ && touch_) {
        last_touch_.phase = device::TouchPhase::kCancel;
        (void)touch_(touch_context_, last_touch_);
    }
    touching_ = false;
    for (size_t i = 0; i < down_keys_.size(); ++i) {
        if (down_keys_[i] && key_)
            (void)key_(key_context_, {static_cast<uint64_t>(esp_timer_get_time()), static_cast<device::KeyCode>(i),
                                      device::KeyPhase::kCancel, 0});
        down_keys_[i] = false;
    }
}
}  // namespace micropixel::pc

namespace micropixel::pc {
int32_t PcInput::EnableText(bool enabled) {
    std::lock_guard lock(mutex_);
    text_enabled_ = enabled;
    text_length_ = 0;
    return MICROPIXEL_STATUS_OK;
}
void PcInput::InjectText(std::string_view text) {
    std::lock_guard lock(mutex_);
    if (!focused_ || !text_enabled_ || text.empty() || text.size() > 31 ||
        text.size() > text_bytes_.size() - text_length_)
        return;
    std::memcpy(text_bytes_.data() + text_length_, text.data(), text.size());
    text_length_ += text.size();
}
int32_t PcInput::ReadText(micropixel_text_response_t& text) {
    std::lock_guard lock(mutex_);
    text = {};
    text.size = sizeof(text);
    if (!text_enabled_) return MICROPIXEL_STATUS_CLOSED;
    uint32_t n = std::min<uint32_t>(32, text_length_);
    if (n < text_length_)
        while (n && (uint8_t(text_bytes_[n]) & 0xc0) == 0x80) --n;
    text.length = n;
    std::memcpy(text.bytes, text_bytes_.data(), n);
    text_length_ -= n;
    std::memmove(text_bytes_.data(), text_bytes_.data() + n, text_length_);
    return MICROPIXEL_STATUS_OK;
}
}  // namespace micropixel::pc
