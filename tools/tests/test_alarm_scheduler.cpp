// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

#include "host/controller/alarm_scheduler.hpp"
#include "host/controller/system_clock.hpp"
using namespace micropixel::host;
struct Store {
    bool fail{};
    unsigned commits{};
    static bool Commit(void* raw, const AlarmScheduler::Record*, uint32_t, uint32_t) {
        auto& s = *static_cast<Store*>(raw);
        ++s.commits;
        return !s.fail;
    }
};
micropixel_alarm_plan_t Once(uint32_t id, uint64_t utc) {
    micropixel_alarm_plan_t p{};
    p.size = sizeof(p);
    p.id = id;
    p.flags = 1;
    p.once_utc = utc;
    std::strcpy(p.label, "Wake up");
    return p;
}
int main() {
    constexpr uint64_t utc = 1800000000;
    auto owned = std::make_unique<AlarmScheduler>();
    auto& alarms = *owned;
    Store store;
    alarms.SetPersistence({&store, Store::Commit});
    alarms.Process(0, utc, 480, true);
    auto plan = Once(1, utc + 2);
    store.fail = true;
    assert(alarms.Register(0, plan, utc, true) == MICROPIXEL_STATUS_INTERNAL && !alarms.Get(0, 1));
    store.fail = false;
    assert(alarms.Register(0, plan, utc, false) == MICROPIXEL_STATUS_WOULD_BLOCK);
    auto bad = plan;
    bad.id = 9;
    assert(alarms.Register(0, bad, utc, true) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    bad = plan;
    bad.label[0] = char(0xff);
    assert(alarms.Register(0, bad, utc, true) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    assert(alarms.Register(0, plan, utc, true) == 0 && alarms.Register(1, plan, utc, true) == 0);
    alarms.Process(1000, utc + 1, 480, true);
    assert(!alarms.Ringing());
    alarms.Process(2000, utc + 2, 480, true);
    assert(alarms.Ringing() && alarms.RingCount() == 2);
    assert(!(alarms.Get(0, 1)->plan.flags & 1));
    auto committed = *alarms.Get(0, 1);
    assert(committed.last_occurrence == utc + 2);
    alarms.StopRinging(0);
    assert(alarms.Ringing() && alarms.RingCount() == 1);
    assert(alarms.Remove(1, 1) == 0 && !alarms.Ringing());
    auto restored = std::make_unique<AlarmScheduler>();
    assert(restored->Restore(committed));
    restored->Process(0, utc + 10, 480, true);
    assert(!restored->Ringing());
    plan = Once(2, utc + 20);
    assert(alarms.Register(0, plan, utc + 2, true) == 0);
    alarms.Process(3000, utc + 50, 480, true);
    assert(!alarms.Ringing());  // forward jump skips overdue one-shot
    alarms.Process(4000, utc + 51, 480, false);
    assert(!alarms.Ringing());
    alarms.Process(5000, utc + 52, 480, true);
    assert(!alarms.Ringing());
    uint64_t due = ((utc + 52 + 480 * 60) / 60 + 1) * 60 - 480 * 60;
    plan = {};
    plan.size = sizeof(plan);
    plan.id = 3;
    plan.flags = 3;
    plan.minute_of_day = ((due + 480 * 60) % 86400) / 60;
    plan.weekdays = 127;
    std::strcpy(plan.label, "Daily");
    assert(alarms.Register(0, plan, utc + 52, true) == 0);
    alarms.Process(5000 + (due - utc - 52) * 1000, due, 480, true);
    assert(alarms.Ringing());
    auto last = alarms.Get(0, 3)->last_occurrence;
    alarms.StopRinging();
    alarms.Process(100000, due - 60, 480, true);
    assert(!alarms.Ringing());
    alarms.Process(160000, due, 480, true);
    assert(!alarms.Ringing() && alarms.Get(0, 3)->last_occurrence == last);
    alarms.Process(161000, due + 1, 0, true);
    assert(!alarms.Ringing());  // zone change only recomputes future
    auto next = Once(4, due + 3);
    assert(alarms.Register(0, next, due + 1, true) == 0);
    alarms.Process(162000, due + 2, 0, true);
    store.fail = true;
    alarms.Process(163000, due + 3, 0, true);
    assert(!alarms.Ringing());
    store.fail = false;
    alarms.Process(164000, due + 4, 0, true);
    assert(alarms.Ringing());
    alarms.StopRinging();
    // Guest close has no operation on the Host model; uninstall only forgets that owner.
    auto other = Once(1, due + 6);
    assert(alarms.Register(2, other, due + 4, true) == 0);
    assert(alarms.Register(3, other, due + 4, true) == 0);
    alarms.Process(166000, due + 6, 0, true);
    assert(alarms.RingCount() == 2);
    alarms.Forget(2);
    assert(alarms.Ringing() && alarms.RingCount() == 1);
    alarms.Forget(3);
    assert(!alarms.Ringing());
    {
        auto weekly = std::make_unique<AlarmScheduler>();
        weekly->Process(0, utc, 480, true);
        auto day = (utc + 480 * 60) / 86400;
        auto monday_mask = 1U << ((day + 1 + 3) % 7);
        auto next_day = (day + 1) * 86400 - 480 * 60;
        auto plan = Once(1, 0);
        plan.flags = 3;
        plan.weekdays = monday_mask;
        plan.minute_of_day = 0;
        assert(weekly->Register(0, plan, utc, true) == 0);
        weekly->Process((next_day - utc) * 1000, next_day, 480, true);
        assert(weekly->Ringing());
        weekly->StopRinging();
        weekly->Process((next_day + 7 * 86400 - utc) * 1000, next_day + 7 * 86400, 480, true);
        assert(weekly->Ringing());
    }
    auto full = std::make_unique<AlarmScheduler>();
    for (unsigned app = 0; app < 4; ++app)
        for (unsigned id = 1; id <= 8; ++id) assert(full->Register(app, Once(id, utc + 100), utc, true) == 0);
    assert(full->Register(4, Once(1, utc + 100), utc, true) == MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    SystemClock clock;
    assert(clock.Now(utc).valid && clock.Now(utc).offset_minutes == 480);
    assert(!clock.Now(0).valid);
    assert(!clock.Restore({841, true, 0}));
    assert(clock.Restore({0, false, 0}) && !clock.Now(utc).valid);
    assert(clock.Restore({-720, true, 60}) && clock.Now(utc).utc == utc + 60);
    std::puts(
        "PASS durable alarms, once/weekly/coalescing, restart, no catch-up, backward jump dedup, zone changes, invalid "
        "time, owner cleanup and capacity");
}
