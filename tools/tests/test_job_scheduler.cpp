// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

#include "host/controller/job_scheduler.hpp"
using namespace micropixel;
struct Durable {
    bool fail{};
    unsigned commits{};
    static bool Commit(void* raw, uint32_t, uint32_t, const micropixel_job_plan_t*) {
        auto& d = *static_cast<Durable*>(raw);
        ++d.commits;
        return !d.fail;
    }
};
struct Inbox {
    micropixel_job_event_t last{};
    unsigned count{};
    static bool Push(void* raw, const micropixel_event_t& e) {
        auto& self = *static_cast<Inbox*>(raw);
        assert(e.service_id == 27 && e.event_id == 1 && e.source == 6);
        std::memcpy(&self.last, e.payload, sizeof(self.last));
        ++self.count;
        return true;
    }
};
int main() {
    auto scheduler = std::make_unique<host::JobScheduler>();
    auto& jobs = *scheduler;
    Durable store;
    Inbox inbox;
    constexpr uint64_t utc = 1800000000;
    jobs.SetPersistence({&store, Durable::Commit});
    micropixel_job_plan_t plan{sizeof(plan), 1, 1, 900, 1000, utc};
    store.fail = true;
    assert(jobs.Register(0, plan, utc) == MICROPIXEL_STATUS_INTERNAL);
    jobs.Process(0, utc);
    assert(!jobs.Pending(0));
    store.fail = false;
    assert(jobs.Register(0, plan, utc) == 0);
    auto bad = plan;
    bad.id = 3;
    assert(jobs.Register(0, bad, utc) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    jobs.Process(5, utc);
    assert(jobs.Pending(0));
    assert(jobs.Records()[0].plan.next_utc == utc + 900);
    host::AppCaller caller{0, {123, 0, 1}, 6};
    assert(jobs.Deliver(0, caller, {&inbox, Inbox::Push}));
    auto ticket = inbox.last.ticket;
    assert(inbox.last.reason == 1 && jobs.Owns(caller, ticket));
    assert(!jobs.Deliver(0, caller, {&inbox, Inbox::Push}));
    auto other = caller;
    other.instance.epoch++;
    assert(!jobs.Owns(other, ticket) && jobs.Complete(other, ticket) == MICROPIXEL_STATUS_STALE_STATE);
    plan.id = 2;
    plan.interval_seconds = 0;
    assert(jobs.Register(0, plan, utc) == 0);
    jobs.Process(500, utc);
    assert(jobs.Deliver(0, caller, {&inbox, Inbox::Push}));
    auto second = inbox.last.ticket;
    assert(second != ticket && jobs.Complete(caller, ticket) == 0 && jobs.Pending(0));
    assert(jobs.Complete(caller, ticket) == MICROPIXEL_STATUS_STALE_STATE);
    jobs.Process(1501, utc);
    assert(jobs.Cancelled(0) == second && !jobs.Owns(caller, second));
    assert(jobs.Deliver(0, caller, {&inbox, Inbox::Push}) && inbox.last.reason == 2);
    assert(jobs.Complete(caller, second) == 0 && !jobs.Pending(0));
    auto restored = std::make_unique<host::JobScheduler>();
    for (const auto& r : jobs.Records())
        if (r.app != UINT32_MAX) assert(restored->Restore(r));
    restored->Process(900000, utc + 899);
    assert(!restored->Pending(0));
    restored->Process(900001, utc + 900);
    assert(restored->Pending(0));
    restored->Stop(0);
    assert(!restored->Pending(0));
    assert(restored->Remove(0, 1) == 0);
    restored->Process(2000000, utc + 3000);
    assert(!restored->Pending(0));
    std::puts(
        "PASS durable Jobs, one-shot/periodic, independent tickets, owner generations, monotonic deadline and "
        "future-plan restore");
}
