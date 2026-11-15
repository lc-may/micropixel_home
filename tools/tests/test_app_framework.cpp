// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>

#include "host/controller/app_manager.hpp"
#include "runtime/audio/pcm_notification.hpp"
#include "runtime/component_model.hpp"

namespace {
using namespace micropixel;
struct Runner final : runtime::SessionRunner {
    runtime::SessionObservation state{};
    bool visible{}, stopped{}, auto_ready{true};
    unsigned starts{}, resumes{}, suspends{};
    bool BeginStart(uint32_t, runtime::InstanceKey owner) override {
        state = {.owner = owner, .ready = auto_ready, .frame_ready = auto_ready};
        ++starts;
        stopped = false;
        return true;
    }
    void RequestStop() override { stopped = true; }
    void RequestSuspend() override {
        state.suspended = true;
        ++suspends;
    }
    void RequestResume() override {
        state.suspended = false;
        ++resumes;
    }
    void SetVisible(bool value) override { visible = value; }
    runtime::SessionObservation Observe() override { return state; }
};
void TestEnsureWork() {
    Runner runner;
    std::array<runtime::SessionRunner*, 1> slots{&runner};
    host::AppManager manager(slots, 99);
    assert(manager.Register(0, {.background = false, .activity = true, .messages = true}));
    runner.auto_ready = false;
    assert(manager.EnsureWork(0) == host::ManagerResult::kAccepted);
    manager.Process(0);
    assert(runner.starts == 1 && manager.Snapshot().foreground < 0);
    runner.state.ready = true;
    runner.state.work_pending = true;
    runner.state.frame_ready = false;
    manager.Process(1);
    assert(manager.Snapshot().apps[0].state == host::AppState::kRunning && manager.Snapshot().foreground < 0);
    runner.state.work_pending = false;
    manager.Process(2);
    assert(runner.state.suspended);
    assert(manager.EnsureWork(0) == host::ManagerResult::kAccepted);
    manager.Process(3);
    assert(runner.starts == 1);
    assert(manager.Stop(0) == host::ManagerResult::kAccepted);
    manager.Process(4);
    runner.state.exited = true;
    manager.Process(5);
    assert(manager.EnsureWork(0) == host::ManagerResult::kBusy);
    assert(manager.EnsureWork(0, true) == host::ManagerResult::kAccepted);
    manager.Process(6);
    assert(runner.starts == 2 && !manager.Snapshot().apps[0].user_stopped);
    assert(manager.Stop(0) == host::ManagerResult::kAccepted);
    manager.Process(7);
    runner.state.exited = true;
    manager.Process(8);
    assert(manager.Deactivate(0));
    assert(manager.Activate(0) == host::ManagerResult::kInvalidApp &&
           manager.EnsureWork(0) == host::ManagerResult::kInvalidApp);
    assert(manager.Register(0, {.messages = true}) && manager.Activate(0) == host::ManagerResult::kAccepted);
}
void TestOwner() {
    runtime::OwnerScope a({1, 0, 1}), b({1, 1, 1});
    assert(a.Acquire(runtime::ResourceKind::kStream, 1));
    assert(!a.Acquire(runtime::ResourceKind::kStream, 1));
    assert(b.Acquire(runtime::ResourceKind::kStream, 1));
    a.Close();
    a.Release(runtime::ResourceKind::kStream);
    assert(a.Count(runtime::ResourceKind::kStream) == 0);
    assert(b.Count(runtime::ResourceKind::kStream) == 1);
    assert(!a.accepts({1, 0, 2}) && !a.accepts({1, 0, 1}));
    assert(b.accepts({1, 1, 1}));
}
void TestManager() {
    Runner a, b;
    runtime::SessionRunner* runners[]{&a, &b};
    host::AppManager manager(runners, 17);
    assert(manager.Register(0, {true}));
    assert(manager.Register(1, {false}));
    assert(manager.Register(2, {}));
    assert(manager.Activate(0) == host::ManagerResult::kAccepted);
    manager.Process();
    auto original = manager.Snapshot().apps[0].instance;
    assert(a.visible && manager.Snapshot().foreground == 0);
    b.auto_ready = false;
    assert(manager.Activate(1) == host::ManagerResult::kAccepted);
    manager.Process();
    assert(a.visible && !b.visible && manager.Snapshot().foreground == 0);
    b.state.ready = b.state.frame_ready = true;
    manager.Process();
    assert(!a.visible && b.visible && manager.Snapshot().foreground == 1);
    assert(manager.Activate(2) == host::ManagerResult::kNoCapacity);
    assert(manager.ShowHome() == host::ManagerResult::kAccepted);
    manager.Process();
    assert(manager.Snapshot().foreground == -1 && !b.visible && b.state.suspended);
    assert(manager.Activate(0) == host::ManagerResult::kAccepted);
    manager.Process();
    assert(a.starts == 1 && manager.Snapshot().apps[0].instance == original);
    assert(manager.Stop(1) == host::ManagerResult::kAccepted);
    manager.Process();
    assert(b.stopped && manager.Snapshot().apps[1].state == host::AppState::kStopping && a.visible);
    auto stale = b.state.owner;
    b.state.exited = true;
    manager.Process();
    assert(manager.Snapshot().apps[1].state == host::AppState::kStopped);
    assert(manager.Activate(2) == host::ManagerResult::kAccepted);
    b.auto_ready = true;
    manager.Process();
    auto current = b.state.owner;
    assert(current != stale);
    b.state.owner = stale;
    b.state.exited = true;
    manager.Process();
    assert(manager.Snapshot().apps[2].state != host::AppState::kStopped);
    b.state.owner = current;
    b.state.exited = false;
    assert(manager.Activate(0) == host::ManagerResult::kAccepted);
    assert(manager.ShowHome() == host::ManagerResult::kAccepted);
    manager.Process();
    assert(manager.Snapshot().foreground == -1);
    for (unsigned i = 0; i < 32; ++i) assert(manager.ShowHome() == host::ManagerResult::kAccepted);
    assert(manager.ShowHome() == host::ManagerResult::kBusy);
    manager.Process();
}
void TestDeadline() {
    Runner a, b;
    b.auto_ready = false;
    runtime::SessionRunner* runners[]{&a, &b};
    host::AppManager manager(runners, 18);
    assert(manager.Register(0, {true}) && manager.Register(1, {}));
    assert(manager.Activate(0) == host::ManagerResult::kAccepted);
    manager.Process(100);
    assert(manager.Activate(1) == host::ManagerResult::kAccepted);
    manager.Process(200);
    manager.Process(10200);
    assert(b.stopped && a.visible);
    assert(manager.Snapshot().apps[1].last_result == host::ManagerResult::kStartFailed);
    assert(manager.Activate(1) == host::ManagerResult::kBusy);
}
void TestBackgroundLease() {
    Runner runner;
    runtime::SessionRunner* runners[]{&runner};
    host::AppManager manager(runners, 19);
    assert(manager.Register(0, {true}));
    assert(manager.Activate(0) == host::ManagerResult::kAccepted);
    manager.Process();
    runner.state.background_eligible = false;
    assert(manager.ShowHome() == host::ManagerResult::kAccepted);
    manager.Process();
    assert(runner.state.suspended);
    runner.state.background_eligible = true;
    manager.Process();
    assert(!runner.state.suspended);
    assert(manager.Snapshot().foreground == -1 && runner.starts == 1);
}
void TestComponents() {
    runtime::ComponentModel model;
    model.declared = 7;
    model.registered = true;
    model.want_activity = model.want_service = true;
    auto next = [&](uint32_t command) {
        micropixel_component_event_payload_t event{};
        assert(model.Next(event));
        assert(event.command == command);
        auto bad = micropixel_component_ack_t{sizeof(micropixel_component_ack_t), 0, event.token, event.transition + 1,
                                              event.command};
        assert(!model.Ack(bad));
        bad.transition = event.transition;
        assert(model.Ack(bad));
        assert(!model.Ack(bad));
        return event.token;
    };
    const auto activity = next(MICROPIXEL_COMPONENT_CREATE);
    const auto service = next(MICROPIXEL_COMPONENT_START);
    model.visible = true;
    assert(next(MICROPIXEL_COMPONENT_RESUME) == activity);
    model.visible = false;
    assert(next(MICROPIXEL_COMPONENT_STOP) == activity);
    assert(model.service.token == service && model.service.state == 2);
    model.want_activity = false;
    assert(next(MICROPIXEL_COMPONENT_DESTROY) == activity);
    assert(!model.Alive(activity));
    model.want_activity = true;
    auto recreated = next(MICROPIXEL_COMPONENT_CREATE);
    assert(recreated != activity);
    model.want_service = false;
    assert(next(MICROPIXEL_COMPONENT_STOP) == service);
    assert(model.activity.token == recreated && model.activity.state == 1 && !model.Alive(service));
    micropixel_component_event_payload_t event{};
    assert(!model.Next(event));
}
}  // namespace
int main() {
    std::atomic<bool> armed{true};
    unsigned notifications = 0;
    assert(micropixel::runtime::PublishPcmLowWater(armed, [&] {
        ++notifications;
        armed.store(true);
        return true;
    }));
    assert(armed.load());  // The woken writer rearmed before publication returned.
    assert(micropixel::runtime::PublishPcmLowWater(armed, [&] {
        ++notifications;
        return true;
    }));
    assert(!armed.load() && notifications == 2);
    assert(!micropixel::runtime::PublishPcmLowWater(armed, [&] {
        ++notifications;
        return true;
    }));
    armed = true;
    assert(!micropixel::runtime::PublishPcmLowWater(armed, [] { return false; }));
    assert(armed.load());
    TestEnsureWork();
    TestOwner();
    TestManager();
    TestDeadline();
    TestBackgroundLease();
    TestComponents();
    std::puts(
        "PASS owner quotas, generations, manager handoff, capacity, stale notifications, deadlines and component "
        "transitions");
}
