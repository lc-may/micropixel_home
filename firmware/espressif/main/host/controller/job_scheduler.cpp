// SPDX-License-Identifier: Apache-2.0
#include "host/controller/job_scheduler.hpp"

#include <cstring>
namespace micropixel::host {
namespace {
bool Valid(const micropixel_job_plan_t& p) {
    return p.size == sizeof(p) && p.flags == 1 && p.id > 0 && p.id <= 2 &&
           (!p.interval_seconds || (p.interval_seconds >= 60 && p.interval_seconds <= 86400)) &&
           p.deadline_ms >= 1000 && p.deadline_ms <= 60000 && p.next_utc >= 1577836800ULL &&
           p.next_utc <= 4102444800ULL;
}
}  // namespace
int32_t JobScheduler::Register(uint32_t app, const micropixel_job_plan_t& plan, uint64_t utc) {
    if (app >= 64 || !Valid(plan) || utc < 1577836800ULL) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    uint32_t at = kCapacity;
    for (uint32_t i = 0; i < kCapacity; ++i)
        if (records_[i].app == app && records_[i].plan.id == plan.id) {
            at = i;
            break;
        }
    if (at == kCapacity)
        for (uint32_t i = 0; i < kCapacity; ++i)
            if (records_[i].app == UINT32_MAX) {
                at = i;
                break;
            }
    if (at == kCapacity) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    if (records_[at].app == app && records_[at].plan.interval_seconds == plan.interval_seconds &&
        records_[at].plan.deadline_ms == plan.deadline_ms && plan.interval_seconds)
        return 0;
    if (persistence_.commit && !persistence_.commit(persistence_.context, app, plan.id, &plan))
        return MICROPIXEL_STATUS_INTERNAL;
    records_[at] = {app, plan};
    return 0;
}
int32_t JobScheduler::Remove(uint32_t app, uint32_t id) {
    for (uint32_t i = 0; i < kCapacity; ++i)
        if (records_[i].app == app && records_[i].plan.id == id) {
            if (persistence_.commit && !persistence_.commit(persistence_.context, app, id, nullptr))
                return MICROPIXEL_STATUS_INTERNAL;
            records_[i] = {};
            work_[i] = {};
            return 0;
        }
    return MICROPIXEL_STATUS_NOT_FOUND;
}
bool JobScheduler::Restore(const Record& r) {
    auto checked = r.plan;
    checked.flags = 1;
    if (r.app >= 64 || r.plan.flags > 1 || !Valid(checked)) return false;
    for (const auto& item : records_)
        if (item.app == r.app && item.plan.id == r.plan.id) return false;
    for (auto& item : records_)
        if (item.app == UINT32_MAX) {
            item = r;
            return true;
        }
    return false;
}
void JobScheduler::Process(uint64_t now, uint64_t utc) {
    for (uint32_t i = 0; i < kCapacity; ++i) {
        auto& record = records_[i];
        auto& work = work_[i];
        if (work.ticket && now >= work.deadline) {
            if (!work.delivered) {
                work = {};
                continue;
            }
            work.cancelled = true;
        }
        if (record.app == UINT32_MAX || work.ticket || !record.plan.flags || utc < 1577836800ULL ||
            utc < record.plan.next_utc)
            continue;
        auto next = record.plan;
        if (next.interval_seconds)
            next.next_utc = utc + next.interval_seconds;
        else
            next.flags = 0;
        if (persistence_.commit && !persistence_.commit(persistence_.context, record.app, next.id, &next)) continue;
        record.plan = next;
        if (sequence_ == UINT32_MAX) continue;
        work.ticket = ++sequence_;
        work.deadline = now + next.deadline_ms;
    }
}
bool JobScheduler::Pending(uint32_t app) const {
    for (uint32_t i = 0; i < kCapacity; ++i)
        if (records_[i].app == app && work_[i].ticket) return true;
    return false;
}
bool JobScheduler::Deliver(uint32_t app, AppCaller receiver, device::AppEventSink sink) {
    if (!sink.push || !receiver.component) return false;
    bool sent = false;
    for (uint32_t i = 0; i < kCapacity; ++i) {
        auto& w = work_[i];
        if (records_[i].app != app || !w.ticket) continue;
        if (w.delivered && (w.receiver.instance != receiver.instance || w.receiver.component != receiver.component))
            continue;
        if (w.delivered && (!w.cancelled || w.cancel_delivered)) continue;
        micropixel_event_t event{};
        event.size = sizeof(event);
        event.service_id = MICROPIXEL_SERVICE_JOB;
        event.event_id = MICROPIXEL_JOB_EVENT_WORK;
        event.source = receiver.component;
        micropixel_job_event_t payload{w.ticket, records_[i].plan.id, w.cancelled ? 2U : 1U,
                                       records_[i].plan.deadline_ms};
        std::memcpy(event.payload, &payload, sizeof(payload));
        if (sink.push(sink.context, event)) {
            w.receiver = receiver;
            w.delivered = true;
            w.cancel_delivered = w.cancelled;
            sent = true;
        }
    }
    return sent;
}
bool JobScheduler::Owns(AppCaller caller, uint32_t ticket) const {
    for (const auto& w : work_)
        if (w.ticket == ticket && w.delivered && w.receiver.app == caller.app &&
            w.receiver.instance == caller.instance && w.receiver.component == caller.component)
            return !w.cancelled;
    return false;
}
int32_t JobScheduler::Complete(AppCaller caller, uint32_t ticket) {
    for (auto& w : work_)
        if (w.ticket == ticket && w.delivered && w.receiver.app == caller.app &&
            w.receiver.instance == caller.instance && w.receiver.component == caller.component) {
            w = {};
            return 0;
        }
    return MICROPIXEL_STATUS_STALE_STATE;
}
uint32_t JobScheduler::Cancelled(uint32_t app) const {
    for (uint32_t i = 0; i < kCapacity; ++i)
        if (records_[i].app == app && work_[i].ticket && work_[i].cancelled) return work_[i].ticket;
    return 0;
}
std::array<uint32_t, 2> JobScheduler::Retire(AppCaller caller) {
    std::array<uint32_t, 2> tickets{};
    uint32_t count = 0;
    for (auto& work : work_)
        if (work.ticket && work.delivered && work.receiver.app == caller.app &&
            work.receiver.instance == caller.instance && work.receiver.component == caller.component) {
            if (count < tickets.size()) tickets[count++] = work.ticket;
            work = {};
        }
    return tickets;
}
void JobScheduler::Stop(uint32_t app) {
    for (uint32_t i = 0; i < kCapacity; ++i)
        if (records_[i].app == app) work_[i] = {};
}
}  // namespace micropixel::host
