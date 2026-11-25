// SPDX-License-Identifier: Apache-2.0
#include "alarm_store.hpp"

#include <array>
#include <cstdio>

#include "bflb_core.h"
#include "file_store.h"
#include "lfs.h"

namespace micropixel::bouffalo {
namespace {

constexpr char kAlarmFile[] = "clock.alarms.v1";
constexpr uint32_t kAlarmSlots = 8;

struct StoredAlarm final {
    micropixel_alarm_plan_t plan;
    uint64_t last_occurrence;
};

struct StoredAlarms final {
    int32_t offset_minutes;
    uint32_t clock_enabled;
    int64_t correction;
    std::array<StoredAlarm, kAlarmSlots> entries;
};

static_assert(sizeof(StoredAlarm) == 96);
static_assert(sizeof(StoredAlarms) == 784);

// Startup restores this before Guest tasks run. All later saves hold the AlarmHost mutex.
ATTR_NOINIT_PSRAM_SECTION StoredAlarms file;

}  // namespace

void RestoreClockAlarms(host::AlarmScheduler& alarms, host::SystemClock& clock, uint32_t app) {
    if (!mpx_files_mount()) {
        std::puts("MicroPixel alarm filesystem unavailable");
        return;
    }
    size_t bytes = 0;
    const int result = mpx_file_read(kAlarmFile, &file, sizeof(file), &bytes);
    if (result == LFS_ERR_NOENT) return;
    if (result || bytes != sizeof(file) || file.clock_enabled > 1 ||
        !clock.Restore({file.offset_minutes, file.clock_enabled != 0, file.correction})) {
        std::puts("MicroPixel invalid alarm file");
        return;
    }
    unsigned restored = 0;
    for (uint32_t id = 1; id <= file.entries.size(); ++id) {
        const auto& entry = file.entries[id - 1];
        if (entry.plan.id == id && alarms.Restore({app, entry.plan, entry.last_occurrence})) ++restored;
    }
    std::printf("MicroPixel restored %u alarm records\n", restored);
}

bool SaveClockAlarms(const host::AlarmScheduler& alarms, const host::SystemClock& clock, uint32_t app,
                     uint32_t updated_id, const host::AlarmScheduler::Record* replacement) {
    const auto& settings = clock.Configuration();
    file.offset_minutes = settings.offset_minutes;
    file.clock_enabled = settings.enabled;
    file.correction = settings.correction;
    for (uint32_t id = 1; id <= file.entries.size(); ++id) {
        const auto* record = id == updated_id ? replacement : alarms.Get(app, id);
        auto& entry = file.entries[id - 1];
        entry = record ? StoredAlarm{record->plan, record->last_occurrence} : StoredAlarm{};
    }
    return mpx_file_write(kAlarmFile, &file, sizeof(file)) == 0;
}

}  // namespace micropixel::bouffalo
