// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>

#include "abi/micropixel_abi.h"
namespace micropixel::host {
class AlarmScheduler final {
   public:
    struct Record {
        uint32_t app{UINT32_MAX};
        micropixel_alarm_plan_t plan{};
        uint64_t last_occurrence{};
    };
    struct Persistence {
        void* context{};
        bool (*commit)(void*, const Record*, uint32_t app, uint32_t id){};
    };
    static constexpr uint32_t kCapacity = 32;
    void SetPersistence(Persistence value) { persistence_ = value; }
    int32_t Register(uint32_t app, const micropixel_alarm_plan_t&, uint64_t utc, bool valid);
    int32_t Remove(uint32_t app, uint32_t id);
    bool Restore(const Record&);
    const Record* Get(uint32_t app, uint32_t id) const;
    uint64_t NextOccurrence(uint32_t app, uint32_t id) const;
    const std::array<Record, kCapacity>& Records() const { return records_; }
    // A discontinuity recalculates future occurrences without catching up.
    void Process(uint64_t now_ms, uint64_t utc, int32_t offset_minutes, bool valid);
    bool Ringing() const { return ringing_; }
    uint32_t RingCount() const { return ring_count_; }
    const char* RingLabel() const { return ring_label_.data(); }
    uint64_t RingRevision() const { return ring_revision_; }
    void StopRinging(uint32_t app = UINT32_MAX);
    void Forget(uint32_t app);

   private:
    uint64_t Next(const Record&, uint64_t utc, int32_t offset) const;
    uint64_t Key(const Record&, uint64_t occurrence, int32_t offset) const;
    void Recompute(uint64_t utc, int32_t offset);
    void RefreshActive();
    std::array<Record, kCapacity> records_{};
    std::array<uint64_t, kCapacity> next_{};
    std::array<bool, kCapacity> active_{};
    Persistence persistence_{};
    std::array<char, 64> ring_label_{};
    uint64_t last_ms_{}, last_utc_{}, ring_until_{}, ring_revision_{};
    int32_t offset_{};
    uint32_t ring_count_{};
    bool initialized_{}, time_valid_{}, ringing_{};
};
}  // namespace micropixel::host
