# SPDX-License-Identifier: Apache-2.0
"""Verify the Orion alarm file across scheduler lifetimes and failed saves."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class AlarmStoreTests(unittest.TestCase):
    def test_restore_update_delete_and_ring_commit(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "bflb_core.h").write_text("#define ATTR_NOINIT_PSRAM_SECTION\n")
            (root / "lfs.h").write_text("#define LFS_ERR_NOENT -2\n#define LFS_ERR_IO -5\n")
            source = root / "alarm.cpp"
            source.write_text(
                r'''
#include "host/controller/alarm_store.hpp"
#include "platform/storage/file_store.h"
#include "lfs.h"

#include <cassert>
#include <cstring>
#include <vector>

using namespace micropixel;
using namespace bouffalo;

std::vector<uint8_t> disk;
bool fail_write;

int mpx_app_data_read(const char* id, const char* key, void* data, size_t capacity, size_t* size) {
    assert(std::strcmp(id, "micropixel.clock-alarm") == 0);
    assert(std::strcmp(key, "@alarms.v1") == 0);
    if (disk.empty()) return LFS_ERR_NOENT;
    assert(disk.size() <= capacity);
    std::memcpy(data, disk.data(), disk.size());
    *size = disk.size();
    return 0;
}
int mpx_app_data_write(const char* id, const char* key, const void* data, size_t size) {
    assert(std::strcmp(id, "micropixel.clock-alarm") == 0);
    assert(std::strcmp(key, "@alarms.v1") == 0);
    if (fail_write) return LFS_ERR_IO;
    auto* first = static_cast<const uint8_t*>(data);
    disk.assign(first, first + size);
    return 0;
}

struct TestHost {
    host::AlarmScheduler alarms;
    host::SystemClock clock;
    uint32_t app;

    explicit TestHost(uint32_t index) : app(index) {
        RestoreClockAlarms(alarms, clock, app);
        alarms.SetPersistence({this, [](void* context, const host::AlarmScheduler::Record* record, uint32_t owner,
                                         uint32_t id) {
                                   const auto& self = *static_cast<TestHost*>(context);
                                   return owner != self.app || SaveClockAlarms(self.alarms, self.clock, owner, id, record);
                               }});
    }
};

int main() {
    constexpr uint64_t now = 1795596000;
    TestHost first(1);
    assert(first.clock.Restore({60, true, 8}));
    const micropixel_alarm_plan_t weekly{
        .size = sizeof(weekly), .flags = MICROPIXEL_ALARM_ENABLED | MICROPIXEL_ALARM_WEEKLY,
        .id = 1, .once_utc = 0, .minute_of_day = 7 * 60 + 15, .weekdays = 0x7f, .reserved0 = 0, .label = "Morning"};
    const micropixel_alarm_plan_t once{
        .size = sizeof(once), .flags = MICROPIXEL_ALARM_ENABLED, .id = 3,
        .once_utc = now + 5, .minute_of_day = 0, .weekdays = 0, .reserved0 = 0, .label = "Meeting"};
    auto later = once;
    later.id = 4;
    later.once_utc = now + 3600;
    assert(first.alarms.Register(first.app, weekly, now, true) == 0);
    assert(first.alarms.Register(first.app, once, now, true) == 0);
    assert(first.alarms.Register(first.app, later, now, true) == 0);
    assert(first.alarms.Register(0, weekly, now, true) == 0);

    const auto saved = disk;
    auto moved = once;
    moved.once_utc += 30;
    fail_write = true;
    assert(first.alarms.Register(first.app, moved, now, true) == MICROPIXEL_STATUS_INTERNAL);
    assert(first.alarms.Remove(first.app, weekly.id) == MICROPIXEL_STATUS_INTERNAL);
    assert(first.alarms.Get(first.app, once.id)->plan.once_utc == once.once_utc && disk == saved);
    fail_write = false;

    // The on-disk clock app identity does not depend on its catalog index.
    TestHost rebooted(5);
    const auto& settings = rebooted.clock.Configuration();
    assert(settings.offset_minutes == 60 && settings.correction == 8 && settings.enabled);
    assert(rebooted.alarms.Get(rebooted.app, 1)->plan.minute_of_day == weekly.minute_of_day);
    assert(rebooted.alarms.Get(rebooted.app, 3)->plan.once_utc == once.once_utc);
    assert(rebooted.alarms.Get(rebooted.app, 4)->plan.once_utc == later.once_utc);
    assert(rebooted.alarms.Get(0, 1) == nullptr);
    rebooted.alarms.Process(0, now, 60, true);
    assert(rebooted.alarms.NextOccurrence(rebooted.app, once.id) == once.once_utc);
    rebooted.alarms.Process(5000, now + 5, 60, true);
    assert(rebooted.alarms.Ringing());
    assert(rebooted.alarms.Remove(rebooted.app, later.id) == 0);
    assert(rebooted.clock.Restore({0, true, 0}));
    assert(SaveClockAlarms(rebooted.alarms, rebooted.clock, rebooted.app));

    TestHost after_ring(1);
    const auto* fired = after_ring.alarms.Get(after_ring.app, once.id);
    assert(fired && fired->last_occurrence == once.once_utc && fired->plan.flags == 0);
    assert(after_ring.alarms.Get(after_ring.app, later.id) == nullptr);
    assert(after_ring.alarms.Get(after_ring.app, weekly.id));
    assert(after_ring.clock.Configuration().offset_minutes == 0);
    after_ring.alarms.Process(0, now + 3, 0, true);
    after_ring.alarms.Process(2000, now + 5, 0, true);
    assert(!after_ring.alarms.Ringing());

    disk.resize(1);
    TestHost corrupt(1);
    assert(corrupt.alarms.Get(corrupt.app, weekly.id) == nullptr);
    assert(corrupt.clock.Configuration().offset_minutes == 480);
}
''',
                encoding="utf-8",
            )
            host = ROOT.parent / "common"
            binary = root / "alarm"
            subprocess.run(
                [
                    compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root), "-I", str(ROOT), "-I", str(host),
                    "-I", str(ROOT.parents[1] / "guest"), str(source), str(ROOT / "host/controller/alarm_store.cpp"),
                    str(host / "host/controller/alarm_scheduler.cpp"), str(host / "device/text.cpp"), "-o", str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)
