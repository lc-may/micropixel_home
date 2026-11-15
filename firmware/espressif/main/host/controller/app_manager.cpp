// SPDX-License-Identifier: Apache-2.0
#include "host/controller/app_manager.hpp"

#include <algorithm>

namespace micropixel::host {
AppManager::AppManager(std::span<runtime::SessionRunner*> runners, uint64_t boot) : boot_(boot) {
    slot_count_ = std::min<uint32_t>(runners.size(), slots_.size());
    for (uint32_t i = 0; i < slot_count_; ++i) slots_[i].runner = runners[i];
}
bool AppManager::Register(uint32_t app, AppPolicy policy) {
    if (app >= kMaxApps || snapshot_.apps[app].state != AppState::kStopped) return false;
    if (app > snapshot_.count) return false;
    snapshot_.count = std::max(snapshot_.count, app + 1);
    policies_[app] = policy;
    return true;
}
ManagerResult AppManager::Enqueue(Command command) {
    if (size_ == commands_.size()) return ManagerResult::kBusy;
    commands_[(head_ + size_++) % commands_.size()] = command;
    return ManagerResult::kAccepted;
}
ManagerResult AppManager::Activate(uint32_t app) {
    if (app >= snapshot_.count) return ManagerResult::kInvalidApp;
    if (snapshot_.apps[app].state == AppState::kStopping) return ManagerResult::kBusy;
    if (size_ == commands_.size()) return ManagerResult::kBusy;
    // A full table must not destroy the old foreground to make room.
    if (!Find(app)) {
        bool free = false;
        for (uint32_t i = 0; i < slot_count_; ++i) free |= slots_[i].app < 0;
        if (!free) return ManagerResult::kNoCapacity;
    }
    return Enqueue({Kind::kActivate, app, ++intent_, false});
}
ManagerResult AppManager::Stop(uint32_t app, bool user) {
    if (app >= snapshot_.count) return ManagerResult::kInvalidApp;
    return Enqueue({Kind::kStop, app, 0, user});
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
    if (command.intent != intent_) return;
    if (app.state == AppState::kStopping) {
        app.last_result = ManagerResult::kBusy;
        return;
    }
    if (!slot) {
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
            app.state = AppState::kStarting;
            app.result = 0;
            slot->deadline_ms = now_ms_ + 10000;
            break;
        }
    }
    if (!slot) {
        app.last_result = ManagerResult::kNoCapacity;
        return;
    }
    app.last_result = ManagerResult::kAccepted;
    app.user_stopped = false;
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
        const bool prepared = observation.ready && (!policies_[slot.app].activity || observation.frame_ready);
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
        if (observation.ready && snapshot_.foreground != slot.app && snapshot_.pending != slot.app &&
            !policies_[slot.app].background && !observation.suspended)
            slot.runner->RequestSuspend();
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
}
}  // namespace micropixel::host
