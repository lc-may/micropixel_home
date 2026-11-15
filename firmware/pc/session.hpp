// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
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
    bool Done() const { return done_.load(std::memory_order_acquire); }
    int Result() const { return result_; }
    void WriteGuestLog(const char*, uint32_t, const uint8_t*, size_t, uint64_t) override;

   private:
    void Run();
    void RunRv32();
    PortableApp& app_;
    device::DeviceServices& devices_;
    micropixel_system_launch_arguments_response_t launch_arguments_{};
    std::thread worker_;
    std::mutex mutex_;
    runtime::GuestContext* context_{};
    wasm_module_inst_t instance_{};
    int child_pid_{};
    std::atomic<bool> done_{};
    std::atomic<bool> stop_requested_{};
    int result_{1};
};
class PcInput final : public device::Input {
   public:
    PcInput(uint32_t width, uint32_t height) : width_(width), height_(height) {}
    int32_t GetInfo(micropixel_input_info_t&) override;
    int32_t EnableText(bool) override;
    int32_t ReadText(micropixel_text_response_t&) override;
    void InjectText(std::string_view);
    bool TextEnabled() const { return text_enabled_.load(); }
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
    std::array<char, 256> text_bytes_{};
    uint32_t text_length_{};
    device::TouchSink touch_{};
    void* touch_context_{};
    device::KeySink key_{};
    void* key_context_{};
};
}  // namespace micropixel::pc
