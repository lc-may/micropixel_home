// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstring>
#include <ctime>

#include "pc_app_test_support.hpp"
#include "runtime/abi/abi_bridge.h"
#include "schedule_store.hpp"
#include "screen_font.hpp"
namespace {
void Click(pc::AppSessions& s, int x, int y) {
    assert(s.Input().InjectTouch({.id = 1, .x = x, .y = y, .phase = device::TouchPhase::kDown}));
    assert(s.Input().InjectTouch({.id = 1, .x = x, .y = y, .phase = device::TouchPhase::kUp}));
}
void Confirm(pc::AppSessions& s) {
    assert(s.Input().InjectKey({.code = device::KeyCode::kConfirm, .phase = device::KeyPhase::kDown}));
    assert(s.Input().InjectKey({.code = device::KeyCode::kConfirm, .phase = device::KeyPhase::kUp}));
}
}  // namespace
int main(int argc, char** argv) {
    assert(argc == 2);
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    assert(wasm_runtime_init() && micropixel_register_native_apis());
    lv_init();
    auto* display = lv_sdl_window_create(720, 720);
    assert(display);
    {
        pc::SdlAudio audio;
        assert(audio.Initialize());
        Random random;
        Battery battery;
        device::DeviceRegistry registry;
        assert(registry.InitializeStorage() && registry.RegisterDisplay("display") && registry.RegisterTouch("touch") &&
               registry.RegisterAudioOutput("audio"));
        device::DisplayInfo info{.width_pixels = 720, .height_pixels = 720};
        pc::ScreenFont fonts;
        std::vector<std::unique_ptr<pc::PortableApp>> apps;
        auto app = std::make_unique<pc::PortableApp>();
        std::string error;
        assert(app->Load(argv[1], error));
        apps.push_back(std::move(app));
        uint64_t due = 0;
        {
            pc::AppSessions sessions(apps, display, info, fonts.guest_fonts, audio, random, registry, battery, true);
            assert(sessions.Activate(0));
            Until(
                sessions,
                [&] {
                    return sessions.Snapshot().foreground == 0 && sessions.Find(0)->ComponentInfo().activity_state == 2;
                },
                30);
            Click(sessions, 530, 657);
            Click(sessions, 360, 665);
            Click(sessions, 360, 150);
            Until(sessions, [&] { return sessions.Input().TextEnabled(); }, 30);
            sessions.Input().InjectText("项目会议");
            Confirm(sessions);
            Until(sessions, [&] { return !sessions.Input().TextEnabled(); }, 30);
            Click(sessions, 470, 665);
            Until(sessions, [&] { return sessions.Planner().pending_notes == 1; }, 30);
            auto summary = sessions.Planner();
            assert(std::strcmp(summary.note.data(), "项目会议") == 0 && summary.note_utc > sessions.Clock().utc);
            due = summary.note_utc;
            sessions.Stop(0);
            Until(sessions, [&] { return !sessions.Find(0); }, 30);
            assert(sessions.Planner().note_utc == due);
            sessions.Shutdown();
        }
        {
            pc::ScheduleStore store;
            assert(store.SaveTime({480, true, static_cast<int64_t>(due) - std::time(nullptr) - 2}));
        }
        {
            pc::AppSessions sessions(apps, display, info, fonts.guest_fonts, audio, random, registry, battery, true);
            assert(!sessions.Find(0) && sessions.Snapshot().apps[0].user_stopped);
            Until(sessions, [&] { return sessions.Alarm().ringing; }, 8);
            assert(!sessions.Find(0) && std::strcmp(sessions.Alarm().label.data(), "项目会议") == 0 &&
                   audio.AlarmFocus());
            sessions.StopAlarm();
            sessions.Process();
            assert(sessions.Activate(0));
            Until(
                sessions,
                [&] {
                    return sessions.Snapshot().foreground == 0 && sessions.Find(0)->ComponentInfo().activity_state == 2;
                },
                30);
            Click(sessions, 530, 657);
            Click(sessions, 66, 135);
            // Wait for the actual owner-scoped record to disappear before stopping the Guest.
            assert(sessions.Planner().note_records == 1);
            Click(sessions, 654, 135);
            Until(sessions, [&] { return sessions.Planner().note_records == 0; }, 30);
            assert(!sessions.Planner().pending_notes);
            sessions.Shutdown();
        }
        std::puts(
            "PASS Notes UI: UTF-8 input, future reminder, persistent stopped-app alarm after Host restart, completion "
            "and deletion");
    }
    pc::JoinTasks();
    lv_display_delete(display);
    lv_deinit();
    wasm_runtime_destroy();
}
