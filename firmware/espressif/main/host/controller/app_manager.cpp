// SPDX-License-Identifier: Apache-2.0
#include "host/controller/app_manager.hpp"

#include <algorithm>

namespace micropixel::host {
AppManager::AppManager(std::span<runtime::SessionRunner*> runners, uint64_t boot, AppLimits limits)
    : boot_(boot), limits_(limits) {
    slot_count_ = std::min<uint32_t>(runners.size(), slots_.size());
    for (uint32_t i = 0; i < slot_count_; ++i) slots_[i].runner = runners[i];
}
bool AppManager::Register(uint32_t app, AppPolicy policy) {
    if (!policy.start_timeout_ms || policy.start_timeout_ms > 60000 || policy.subprocesses > kMaxInstances)
        return false;
    if (app >= kMaxApps || snapshot_.apps[app].state != AppState::kStopped) return false;
    if (app > snapshot_.count) return false;
    snapshot_.count = std::max(snapshot_.count, app + 1);
    policies_[app] = policy;
    return true;
}
bool AppManager::Deactivate(uint32_t app) {
    if (app >= snapshot_.count || snapshot_.apps[app].state != AppState::kStopped) return false;
    policies_[app].available = false;
    return true;
}
ManagerResult AppManager::Enqueue(Command command) {
    if (size_ == commands_.size()) return ManagerResult::kBusy;
    commands_[(head_ + size_++) % commands_.size()] = command;
    return ManagerResult::kAccepted;
}
ManagerResult AppManager::Activate(uint32_t app) {
    if (app >= snapshot_.count || !policies_[app].available) return ManagerResult::kInvalidApp;
    if (snapshot_.apps[app].state == AppState::kStopping) return ManagerResult::kBusy;
    if (size_ == commands_.size()) return ManagerResult::kBusy;
    // A full table must not destroy the old foreground to make room.
    if (!Find(app)) {
        if (!Capacity(app)) return ManagerResult::kNoCapacity;
    }
    return Enqueue({Kind::kActivate, app, ++intent_, false});
}
ManagerResult AppManager::Stop(uint32_t app, bool user) {
    if (app >= snapshot_.count || !policies_[app].available) return ManagerResult::kInvalidApp;
    return Enqueue({Kind::kStop, app, 0, user});
}
ManagerResult AppManager::EnsureWork(uint32_t app, bool user) {
    if (app >= snapshot_.count || !policies_[app].available || !policies_[app].messages)
        return ManagerResult::kInvalidApp;
    if (snapshot_.apps[app].user_stopped && !user) return ManagerResult::kBusy;
    if (snapshot_.apps[app].state == AppState::kStopping) return ManagerResult::kBusy;
    if (!Find(app)) {
        if (!Capacity(app)) return ManagerResult::kNoCapacity;
    }
    return Enqueue({Kind::kWork, app, 0, user});
}
void AppManager::RestoreUserStopped(uint32_t app, bool stopped) {
    if (app < snapshot_.count) snapshot_.apps[app].user_stopped = stopped;
}
ManagerResult AppManager::ShowHome() {
    if (size_ == commands_.size()) return ManagerResult::kBusy;
    return Enqueue({Kind::kHome, 0, ++intent_, false});
}
AppManager::Slot* AppManager::Find(uint32_t app) {
    for (uint32_t i = 0; i < slot_count_; ++i)
        if (slots_[i].app == static_cast<int32_t>(app)) return &slots_[i];
    return nullptr;
}
bool AppManager::Capacity(uint32_t app) const {
    auto memory = limits_.memory_bytes;
    auto processes = limits_.subprocesses;
    bool free = false;
    for (uint32_t i = 0; i < slot_count_; ++i) {
        const auto& slot = slots_[i];
        if (slot.app < 0) {
            free = true;
            continue;
        }
        const auto& cost = policies_[slot.app];
        if (cost.memory_bytes > memory || cost.subprocesses > processes) return false;
        memory -= cost.memory_bytes;
        processes -= cost.subprocesses;
    }
    return free && policies_[app].memory_bytes <= memory && policies_[app].subprocesses <= processes;
}
void AppManager::HideForeground() {
    if (snapshot_.foreground < 0) return;
    const auto app = static_cast<uint32_t>(snapshot_.foreground);
    if (auto* slot = Find(app)) {
        slot->runner->SetVisible(false);
        if (!policies_[app].background) slot->runner->RequestSuspend();
    }
    snapshot_.foreground = -1;
}
void AppManager::Execute(const Command& command) {
    if (command.kind == Kind::kHome) {
        if (command.intent != intent_) return;
        snapshot_.pending = -1;
        HideForeground();
        return;
    }
    if (!policies_[command.app].available) return;
    auto& app = snapshot_.apps[command.app];
    auto* slot = Find(command.app);
    if (command.kind == Kind::kStop) {
        app.user_stopped |= command.user;
        if (snapshot_.pending == static_cast<int32_t>(command.app)) snapshot_.pending = -1;
        if (snapshot_.foreground == static_cast<int32_t>(command.app)) HideForeground();
        if (slot && app.state != AppState::kStopping) {
            app.state = AppState::kStopping;
            slot->runner->RequestStop();
        }
        return;
    }
    const bool background = command.kind == Kind::kWork;
    if (!background && command.intent != intent_) return;
    if (background && app.user_stopped && !command.user) return;
    if (app.state == AppState::kStopping) {
        app.last_result = ManagerResult::kBusy;
        return;
    }
    if (!slot) {
        if (!Capacity(command.app)) {
            app.last_result = ManagerResult::kNoCapacity;
            return;
        }
        for (uint32_t i = 0; i < slot_count_; ++i) {
            if (slots_[i].app >= 0) continue;
            slot = &slots_[i];
            if (++slot->epoch == 0) ++slot->epoch;
            app.instance = {boot_, i, slot->epoch};
            if (!slot->runner->BeginStart(command.app, app.instance)) {
                app.instance = {};
                app.last_result = ManagerResult::kStartFailed;
                app.result = 1;
                return;
            }
            slot->app = command.app;
            slot->background_start = background;
            app.state = AppState::kStarting;
            app.result = 0;
            slot->deadline_ms = now_ms_ + policies_[command.app].start_timeout_ms;
            break;
        }
    }
    if (!slot) {
        app.last_result = ManagerResult::kNoCapacity;
        return;
    }
    app.last_result = ManagerResult::kAccepted;
    if (!background || command.user) app.user_stopped = false;
    if (background) {
        slot->runner->RequestWork();
        return;
    }
    slot->background_start = false;
    snapshot_.pending = policies_[command.app].activity ? static_cast<int32_t>(command.app) : -1;
    slot->runner->RequestResume();
}
void AppManager::Process(uint64_t now_ms) {
    now_ms_ = now_ms;
    while (size_) {
        const auto command = commands_[head_];
        head_ = (head_ + 1) % commands_.size();
        --size_;
        Execute(command);
        ++snapshot_.revision;
    }
    for (uint32_t i = 0; i < slot_count_; ++i) {
        auto& slot = slots_[i];
        if (slot.app < 0) continue;
        auto& app = snapshot_.apps[slot.app];
        const auto observation = slot.runner->Observe();
        if (observation.owner != app.instance) continue;
        if (observation.exited) {
            if (snapshot_.foreground == slot.app) HideForeground();
            if (snapshot_.pending == slot.app) snapshot_.pending = -1;
            app.state = AppState::kStopped;
            app.result = app.last_result == ManagerResult::kStartFailed ? 1 : observation.result;
            app.instance = {};
            slot.app = -1;
            ++snapshot_.revision;
            continue;
        }
        if (app.state == AppState::kStopping) continue;
        const bool prepared =
            observation.ready && (slot.background_start || !policies_[slot.app].activity || observation.frame_ready);
        if (app.state == AppState::kStarting && !prepared && now_ms_ >= slot.deadline_ms) {
            app.last_result = ManagerResult::kStartFailed;
            app.state = AppState::kStopping;
            if (snapshot_.pending == slot.app) snapshot_.pending = -1;
            slot.runner->RequestStop();
            ++snapshot_.revision;
            continue;
        }
        if (prepared) {
            const auto state = observation.suspended ? AppState::kSuspended : AppState::kRunning;
            if (app.state != state) {
                app.state = state;
                ++snapshot_.revision;
            }
        }
        if (snapshot_.foreground == slot.app && !observation.activity_available) HideForeground();
        if (observation.ready && snapshot_.foreground != slot.app && snapshot_.pending != slot.app) {
            const bool background = (policies_[slot.app].background && observation.background_eligible) ||
                                    (policies_[slot.app].messages && observation.work_pending);
            // Publish desired state even while a runner is between safe points.
            // An observed state can lag an in-flight suspend/resume request.
            if (!background)
                slot.runner->RequestSuspend();
            else
                slot.runner->RequestBackgroundResume();
        }
        if (snapshot_.pending != slot.app || !observation.ready || !observation.frame_ready || observation.suspended)
            continue;
        if (snapshot_.foreground != slot.app) {
            HideForeground();
            snapshot_.foreground = slot.app;
            slot.runner->SetVisible(true);
        }
        snapshot_.pending = -1;
        ++snapshot_.revision;
    }
    snapshot_.reserved_bytes = 0;
    snapshot_.subprocesses = 0;
    for (uint32_t i = 0; i < slot_count_; ++i)
        if (slots_[i].app >= 0) {
            const auto& cost = policies_[slots_[i].app];
            snapshot_.reserved_bytes += cost.memory_bytes;
            snapshot_.subprocesses += cost.subprocesses;
        }
}
}  // namespace micropixel::host
