// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "runtime/session_runner.hpp"

namespace micropixel::host {
enum class AppState : uint8_t { kStopped, kStarting, kRunning, kSuspended, kStopping };
enum class ManagerResult : uint8_t { kAccepted, kInvalidApp, kBusy, kNoCapacity, kStartFailed };
struct AppPolicy final {
    bool background{};
    bool activity{true};
    bool messages{};
    bool available{true};
    uint32_t start_timeout_ms{10000};
    uint64_t memory_bytes{};
    uint32_t subprocesses{};
};
struct AppLimits final {
    uint64_t memory_bytes{UINT64_MAX};
    uint32_t subprocesses{4};
};
struct AppSnapshot final {
    AppState state{AppState::kStopped};
    runtime::InstanceKey instance{};
    int32_t result{};
    bool user_stopped{};
    ManagerResult last_result{};
};
struct ManagerSnapshot final {
    std::array<AppSnapshot, 64> apps{};
    uint32_t count{};
    int32_t foreground{-1};
    int32_t pending{-1};
    uint64_t revision{};
    uint64_t reserved_bytes{};
    uint32_t subprocesses{};
};

// Single writer: UI/controller consumes ingress and polls runner mailboxes.
// Neither a Guest nor an ISR calls this object directly.
class AppManager final {
   public:
    static constexpr uint32_t kMaxApps = 64;
    static constexpr uint32_t kMaxInstances = 4;
    AppManager(std::span<runtime::SessionRunner*> runners, uint64_t boot, AppLimits limits = {});
    [[nodiscard]] bool Register(uint32_t app, AppPolicy policy);
    bool Deactivate(uint32_t app);
    [[nodiscard]] ManagerResult Activate(uint32_t app);
    [[nodiscard]] ManagerResult EnsureWork(uint32_t app, bool explicit_user = false);
    void RestoreUserStopped(uint32_t app, bool stopped);
    [[nodiscard]] ManagerResult Stop(uint32_t app, bool user = true);
    [[nodiscard]] ManagerResult ShowHome();
    void Process(uint64_t now_ms = 0);
    [[nodiscard]] const ManagerSnapshot& Snapshot() const { return snapshot_; }

   private:
    enum class Kind : uint8_t { kActivate, kStop, kHome, kWork };
    struct Command {
        Kind kind{};
        uint32_t app{};
        uint64_t intent{};
        bool user{};
    };
    struct Slot {
        runtime::SessionRunner* runner{};
        int32_t app{-1};
        uint32_t epoch{};
        uint64_t deadline_ms{};
        bool background_start{};
    };
    ManagerResult Enqueue(Command command);
    Slot* Find(uint32_t app);
    bool Capacity(uint32_t app) const;
    void HideForeground();
    void Execute(const Command& command);
    std::array<Slot, kMaxInstances> slots_{};
    std::array<AppPolicy, kMaxApps> policies_{};
    std::array<Command, 32> commands_{};
    uint32_t head_{}, size_{}, slot_count_{};
    uint64_t boot_{}, intent_{}, now_ms_{};
    ManagerSnapshot snapshot_{};
    AppLimits limits_{};
};
}  // namespace micropixel::host
