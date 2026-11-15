// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>

#include "host/controller/app_hub.hpp"

namespace micropixel::host {
// Serialized by the composition root. Plans are durable; work tickets are boot-local.
class JobScheduler final {
   public:
    struct Record {
        uint32_t app{UINT32_MAX};
        micropixel_job_plan_t plan{};
    };
    struct Persistence {
        void* context{};
        bool (*commit)(void*, uint32_t app, uint32_t id, const micropixel_job_plan_t*){};
    };
    static constexpr uint32_t kCapacity = 32;
    void SetPersistence(Persistence value) { persistence_ = value; }
    int32_t Register(uint32_t app, const micropixel_job_plan_t&, uint64_t utc);
    int32_t Remove(uint32_t app, uint32_t id);
    bool Restore(const Record&);
    const std::array<Record, kCapacity>& Records() const { return records_; }
    void Process(uint64_t now_ms, uint64_t utc);
    bool Pending(uint32_t app) const;
    bool Deliver(uint32_t app, AppCaller receiver, device::AppEventSink);
    int32_t Complete(AppCaller, uint32_t ticket);
    bool Owns(AppCaller, uint32_t ticket) const;
    uint32_t Cancelled(uint32_t app) const;
    void Stop(uint32_t app);

   private:
    struct Work {
        uint32_t ticket{};
        uint64_t deadline{};
        AppCaller receiver{};
        bool delivered{}, cancelled{}, cancel_delivered{};
    };
    std::array<Record, kCapacity> records_{};
    std::array<Work, kCapacity> work_{};
    Persistence persistence_{};
    uint32_t sequence_{};
};
}  // namespace micropixel::host
