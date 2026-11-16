// SPDX-License-Identifier: Apache-2.0
#include "host/controller/alarm_scheduler.hpp"

#include <algorithm>
#include <cstring>

#include "device/text.hpp"
namespace micropixel::host {
namespace {
bool Valid(const micropixel_alarm_plan_t& p) {
    auto length = strnlen(p.label, sizeof(p.label));
    for (size_t i = 0; i < length; ++i)
        if (static_cast<unsigned char>(p.label[i]) < 32 || p.label[i] == 127) return false;
    return p.size == sizeof(p) && p.id > 0 && p.id <= 8 && p.flags <= 3 && !p.reserved0 && length < sizeof(p.label) &&
           device::IsValidUtf8(reinterpret_cast<const uint8_t*>(p.label), length) &&
           ((p.flags & MICROPIXEL_ALARM_WEEKLY)
                ? (p.minute_of_day < 1440 && p.weekdays > 0 && p.weekdays < 128 && !p.once_utc)
                : (!p.weekdays && !p.minute_of_day && p.once_utc >= 1577836800ULL && p.once_utc <= 4102444800ULL));
}
}  // namespace
int32_t AlarmScheduler::Register(uint32_t app, const micropixel_alarm_plan_t& plan, uint64_t utc, bool valid) {
    if (app >= 64 || !Valid(plan)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if ((plan.flags & 1) && (!valid || (!(plan.flags & 2) && plan.once_utc <= utc)))
        return MICROPIXEL_STATUS_WOULD_BLOCK;
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
    Record candidate{app, plan, 0};
    const auto& old = records_[at];
    if (old.app == app && old.plan.once_utc == plan.once_utc && old.plan.minute_of_day == plan.minute_of_day &&
        old.plan.weekdays == plan.weekdays)
        candidate.last_occurrence = old.last_occurrence;
    if (persistence_.commit && !persistence_.commit(persistence_.context, &candidate, app, plan.id))
        return MICROPIXEL_STATUS_INTERNAL;
    records_[at] = candidate;
    next_[at] = valid ? Next(candidate, utc, offset_) : 0;
    if (!(plan.flags & 1)) {
        active_[at] = false;
        RefreshActive();
    }
    return 0;
}
int32_t AlarmScheduler::Remove(uint32_t app, uint32_t id) {
    for (uint32_t i = 0; i < kCapacity; ++i)
        if (records_[i].app == app && records_[i].plan.id == id) {
            if (persistence_.commit && !persistence_.commit(persistence_.context, nullptr, app, id))
                return MICROPIXEL_STATUS_INTERNAL;
            records_[i] = {};
            next_[i] = 0;
            active_[i] = false;
            RefreshActive();
            return 0;
        }
    return MICROPIXEL_STATUS_NOT_FOUND;
}
bool AlarmScheduler::Restore(const Record& r) {
    if (r.app >= 64 || !Valid(r.plan) || Get(r.app, r.plan.id)) return false;
    for (auto& row : records_)
        if (row.app == UINT32_MAX) {
            row = r;
            return true;
        }
    return false;
}
const AlarmScheduler::Record* AlarmScheduler::Get(uint32_t app, uint32_t id) const {
    for (const auto& r : records_)
        if (r.app == app && r.plan.id == id) return &r;
    return nullptr;
}
uint64_t AlarmScheduler::Key(const Record& r, uint64_t occurrence, int32_t offset) const {
    return (r.plan.flags & 2)
               ? static_cast<uint64_t>((static_cast<int64_t>(occurrence) + int64_t(offset) * 60) / 86400) * 1440 +
                     r.plan.minute_of_day
               : occurrence;
}
uint64_t AlarmScheduler::Next(const Record& r, uint64_t utc, int32_t offset) const {
    if (!(r.plan.flags & 1)) return 0;
    if (!(r.plan.flags & 2)) return r.plan.once_utc > utc && r.plan.once_utc > r.last_occurrence ? r.plan.once_utc : 0;
    auto day = (static_cast<int64_t>(utc) + int64_t(offset) * 60) / 86400;
    for (int i = 0; i < 8; ++i) {
        auto d = day + i;
        unsigned weekday = (d + 3) % 7;
        if (!(r.plan.weekdays & (1U << weekday))) continue;
        auto when = d * 86400 + r.plan.minute_of_day * 60 - int64_t(offset) * 60;
        if (when > static_cast<int64_t>(utc) &&
            static_cast<uint64_t>(d * 1440 + r.plan.minute_of_day) > r.last_occurrence)
            return when;
    }
    return 0;
}
void AlarmScheduler::Recompute(uint64_t utc, int32_t offset) {
    for (uint32_t i = 0; i < kCapacity; ++i) next_[i] = Next(records_[i], utc, offset);
}
void AlarmScheduler::Process(uint64_t now, uint64_t utc, int32_t offset, bool valid) {
    valid = valid && utc >= 1577836800ULL && utc <= 4102444800ULL && offset >= -720 && offset <= 840;
    const auto elapsed = initialized_ && now >= last_ms_ ? (now - last_ms_) / 1000 : 0;
    const auto delta = initialized_ ? static_cast<int64_t>(utc) - static_cast<int64_t>(last_utc_) : 0;
    const bool jump = !initialized_ || !time_valid_ || offset != offset_ || now < last_ms_ || delta < 0 ||
                      delta > static_cast<int64_t>(elapsed) + 2 || delta + 2 < static_cast<int64_t>(elapsed);
    if (!valid) {
        if (ringing_) StopRinging();
        next_ = {};
    } else if (jump)
        Recompute(utc, offset);
    else
        for (uint32_t i = 0; i < kCapacity; ++i) {
            auto& record = records_[i];
            const auto occurrence = next_[i];
            if (!occurrence || utc < occurrence) continue;
            if (utc - occurrence > 2) {
                next_[i] = Next(record, utc, offset);
                continue;
            }
            auto committed = record;
            committed.last_occurrence = Key(record, occurrence, offset);
            if (!(committed.plan.flags & 2)) committed.plan.flags &= ~1U;
            if (persistence_.commit &&
                !persistence_.commit(persistence_.context, &committed, record.app, record.plan.id))
                continue;
            record = committed;
            next_[i] = Next(record, utc, offset);
            active_[i] = true;
            std::memcpy(ring_label_.data(), record.plan.label, sizeof(record.plan.label));
            ringing_ = true;
            ring_until_ = now + 60000;
            ++ring_revision_;
        }
    last_ms_ = now;
    last_utc_ = utc;
    offset_ = offset;
    time_valid_ = valid;
    initialized_ = true;
    ring_count_ = std::count(active_.begin(), active_.end(), true);
    if (ringing_ && now >= ring_until_) StopRinging();
}
uint64_t AlarmScheduler::NextOccurrence(uint32_t app, uint32_t id) const {
    for (uint32_t i = 0; i < records_.size(); ++i)
        if (records_[i].app == app && records_[i].plan.id == id) return next_[i];
    return 0;
}
void AlarmScheduler::StopRinging(uint32_t app) {
    for (uint32_t i = 0; i < kCapacity; ++i)
        if (app == UINT32_MAX || records_[i].app == app) active_[i] = false;
    RefreshActive();
}
void AlarmScheduler::RefreshActive() {
    ring_count_ = std::count(active_.begin(), active_.end(), true);
    for (uint32_t i = 0; i < kCapacity; ++i)
        if (active_[i]) {
            std::memcpy(ring_label_.data(), records_[i].plan.label, sizeof(records_[i].plan.label));
            break;
        }
    ++ring_revision_;
    if (!ring_count_ && ringing_) {
        ringing_ = false;
        ++ring_revision_;
    }
}
void AlarmScheduler::Forget(uint32_t app) {
    for (uint32_t i = 0; i < kCapacity; ++i)
        if (records_[i].app == app) {
            records_[i] = {};
            next_[i] = 0;
            active_[i] = false;
        }
    StopRinging(app);
}
}  // namespace micropixel::host
