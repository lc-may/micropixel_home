// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <string_view>

#include "host/controller/job_scheduler.hpp"
struct sqlite3;
namespace micropixel::pc {
class ScheduleStore final {
   public:
    ScheduleStore();
    ~ScheduleStore();
    bool SaveJob(std::string_view app, uint32_t id, const micropixel_job_plan_t*);
    void LoadJobs(std::string_view app, uint32_t index, host::JobScheduler&);
    bool SetStopped(std::string_view app, bool);
    bool Stopped(std::string_view app);
    bool Delete(std::string_view app);
    bool SaveWake(std::string_view app, std::string_view caller, uint32_t types);
    void LoadWake(std::string_view app, uint32_t index, host::AppHub&);

   private:
    sqlite3* db_{};
};
}  // namespace micropixel::pc
