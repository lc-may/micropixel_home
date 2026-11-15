// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <string_view>

#include "device/contracts/application.hpp"
#include "runtime/instance.hpp"

namespace micropixel::host {
struct AppCaller {
    uint32_t app{};
    runtime::InstanceKey instance{};
    uint32_t component{};
};
struct WidgetSnapshot {
    micropixel_widget_snapshot_t data{};
    uint32_t app{}, revision{}, component{};
    runtime::InstanceKey instance{};
    uint64_t published_ms{};
    bool online{}, expired{};
};
// Bounded data model. The composition root serializes calls; no Guest callback
// executes here. The event sink only enqueues an immutable 48-byte notification.
class AppHub final {
   public:
    static constexpr uint32_t kMaxApps = 64, kMaxWidgets = 8, kMaxMessages = 32;
    explicit AppHub(uint64_t boot) : boot_(boot) {}
    bool Register(uint32_t app, std::string_view id, uint8_t widgets);
    void Bind(uint32_t app, runtime::InstanceKey instance);
    void SetSink(const AppCaller&, device::AppEventSink);
    void Components(const AppCaller&, const micropixel_component_info_t&);
    void Unbind(const AppCaller&);
    void Quiesce(const AppCaller&);
    int32_t Call(const AppCaller&, uint32_t service, uint32_t method, const uint8_t*, uint32_t, uint8_t*, uint32_t,
                 uint32_t&, uint64_t now_ms);
    void Process(uint64_t now_ms);
    bool Pending(uint32_t app, runtime::InstanceKey instance) const;
    bool Widget(uint32_t index, uint64_t now_ms, WidgetSnapshot& out) const;
    int32_t Action(const WidgetSnapshot& expected, uint16_t type, uint32_t value, uint64_t sequence, uint64_t now_ms,
                   uint32_t& ticket);
    micropixel_message_result_t Result(const AppCaller&, uint32_t ticket) const;
    bool ReadyService(uint32_t app, AppCaller&, device::AppEventSink&, bool running = true) const;
    int32_t SendHome(uint32_t app, uint16_t type, uint32_t value, uint64_t sequence, uint64_t now_ms, uint32_t& ticket);
    void ClearWake(uint32_t app) {
        if (app < 64) apps_[app].cold_rules = {};
    }
    bool AllowWake(uint32_t app, std::string_view caller, uint32_t types);
    int32_t WakeTarget(const AppCaller&, const micropixel_message_send_t&) const;
    bool AliveCaller(const AppCaller& caller) const {
        return Valid(caller) && (!caller.component || ComponentAlive(caller));
    }
    AppCaller Home() const { return {kMaxApps, {boot_, UINT32_MAX, 1}, 0}; }

   private:
    struct Rule {
        char caller[68]{};
        uint32_t types{}, component{};
    };
    struct App {
        char id[68]{};
        uint8_t widget_limit{};
        runtime::InstanceKey instance{};
        device::AppEventSink events{};
        micropixel_component_info_t components{};
        std::array<Rule, 4> rules{};
        std::array<Rule, 4> cold_rules{};
        uint64_t watermark{};
        bool accepting{};
    };
    struct Published {
        WidgetSnapshot snapshot{};
        bool used{};
    };
    struct Message {
        AppCaller source{}, target{};
        micropixel_message_send_t request{};
        uint64_t deadline{};
        uint32_t ticket{};
        int32_t result{};
        bool complete{};
    };
    bool Valid(const AppCaller&) const;
    bool ComponentAlive(const AppCaller&) const;
    Message* Find(uint32_t ticket);
    const Message* Find(uint32_t ticket) const;
    int32_t Publish(const AppCaller&, const micropixel_widget_snapshot_t&, uint64_t, uint32_t&);
    int32_t Send(const AppCaller&, const micropixel_message_send_t&, uint64_t, uint32_t&);
    std::array<App, kMaxApps> apps_{};
    std::array<Published, kMaxWidgets> widgets_{};
    std::array<Message, kMaxMessages> messages_{};
    uint64_t boot_{}, home_watermark_{};
    uint32_t next_ticket_{}, next_revision_{};
};
}  // namespace micropixel::host
