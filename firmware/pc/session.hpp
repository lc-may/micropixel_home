// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "portable_app.hpp"
#include "runtime/guest_context.hpp"
#include "wasm_export.h"

namespace micropixel::pc {
class Session final : public runtime::GuestLogSink {
   public:
    Session(PortableApp& app, device::DeviceServices& devices);
    ~Session();
    void Start();
    void Stop();
    void RequestStop();
    void RequestSuspend();
    void RequestResume(bool ensure_activity = true);
    void RequestWork() {
        ensure_service_.store(true);
        RequestResume(false);
    }
    bool Ready() const {
        return ready_.load(std::memory_order_acquire) && (!component_flags_ || components_registered_.load());
    }
    bool Suspended() const { return suspended_.load(std::memory_order_acquire); }
    bool ActivityAvailable() const { return !component_flags_ || activity_state_.load() != 0; }
    void SetVisible(bool value) {
        visible_.store(value);
        control_wake_.notify_all();
    }
    bool Done() const { return done_.load(std::memory_order_acquire); }
    int Result() const { return result_; }
    micropixel_component_info_t ComponentInfo() const {
        return {sizeof(micropixel_component_info_t),
                component_flags_,
                activity_token_.load(),
                activity_state_.load(),
                service_token_.load(),
                service_state_.load(),
                resource_count_.load()};
    }
    void WriteGuestLog(const char*, uint32_t, const uint8_t*, size_t, uint64_t) override;

   private:
    void Run();
    void RunRv32();
    void Control();
    PortableApp& app_;
    device::DeviceServices& devices_;
    micropixel_system_launch_arguments_response_t launch_arguments_{};
    std::thread worker_;
    std::thread controller_;
    std::condition_variable control_wake_;
    std::mutex mutex_;
    runtime::GuestContext* context_{};
    wasm_module_inst_t instance_{};
    int child_pid_{};
    std::atomic<bool> done_{};
    std::atomic<bool> stop_requested_{};
    std::atomic<bool> ready_{};
    std::atomic<bool> suspended_{};
    std::atomic<bool> want_suspended_{};
    std::atomic<bool> visible_{};
    std::atomic<bool> ensure_activity_{};
    std::atomic<bool> ensure_service_{};
    std::atomic<bool> components_registered_{};
    std::atomic<uint32_t> activity_state_{};
    std::atomic<uint32_t> activity_token_{}, service_token_{}, service_state_{}, resource_count_{};
    const uint16_t component_flags_{};
    int result_{1};
};
class PcInput final : public device::Input {
   public:
    PcInput(uint32_t width, uint32_t height) : width_(width), height_(height) {}
    int32_t GetInfo(micropixel_input_info_t&) override;
    int32_t EnableText(bool) override;
    int32_t ReadText(micropixel_text_response_t&) override;
    void InjectText(std::string_view);
    void SetFocused(bool focused);
    bool TextEnabled() const { return text_enabled_.load() && focused_.load(); }
    void BindTouchSink(device::TouchSink, void*) override;
    void UnbindTouchSink(void*) override;
    void BindKeySink(device::KeySink, void*) override;
    void UnbindKeySink(void*) override;
    bool InjectTouch(const device::TouchSample&) override;
    bool InjectKey(const device::KeySample&) override;

   private:
    std::mutex mutex_;
    uint32_t width_, height_;
    std::atomic<bool> text_enabled_{};
    std::atomic<bool> focused_{true};
    std::array<bool, 12> down_keys_{};
    device::TouchSample last_touch_{};
    bool touching_{};
    std::array<char, 256> text_bytes_{};
    uint32_t text_length_{};
    device::TouchSink touch_{};
    void* touch_context_{};
    device::KeySink key_{};
    void* key_context_{};
};
}  // namespace micropixel::pc
