// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

#include "app_sessions.hpp"
#include "esp_lv_adapter.h"
namespace pc = micropixel::pc;
namespace host = micropixel::host;
namespace device = micropixel::device;
namespace graphics = micropixel::platform::lvgl;
struct Random final : device::Random {
    int32_t GetU32(uint32_t& n) override {
        n = ++next;
        return 0;
    }
    std::atomic<uint32_t> next{};
};
struct Battery final : device::Battery {
    device::BatterySnapshot Snapshot() override { return {}; }
    void SetStateChangeSink(device::BatteryStateChangeSink, void*) override {}
};
template <class Predicate>
void Until(pc::AppSessions& sessions, Predicate predicate, unsigned seconds = 8) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!predicate() && std::chrono::steady_clock::now() < end) {
        sessions.Process();
        esp_lv_adapter_lock(-1);
        lv_timer_handler();
        esp_lv_adapter_unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(predicate());
}
