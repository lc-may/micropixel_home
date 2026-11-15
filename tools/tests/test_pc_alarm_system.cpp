// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstring>
#include <ctime>

#include "pc_app_test_support.hpp"
#include "runtime/abi/abi_bridge.h"
#include "schedule_store.hpp"
#include "screen_font.hpp"
int main(int argc, char** argv) {
    assert(argc == 3);
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
        auto& args = app->launch_arguments;
        args.size = sizeof(args);
        args.count = 1;
        args.bytes_length = 13;
        std::memcpy(args.bytes, "--alarm-test", 13);
        apps.push_back(std::move(app));
        {
            pc::ScheduleStore store;
            host::AlarmScheduler::Record record{};
            record.app = 0;
            record.plan.size = sizeof(record.plan);
            record.plan.id = 1;
            record.plan.flags = 1;
            record.plan.once_utc = std::time(nullptr) + 3;
            std::strcpy(record.plan.label, "Persistent Host alarm");
            assert(store.SaveAlarm(apps[0]->app_id, 1, &record) && store.SetStopped(apps[0]->app_id, true));
        }
        pc::MediaPlayer player(audio);
        player.Play(argv[2]);
        {
            pc::AppSessions sessions(apps, display, info, fonts.guest_fonts, audio, random, registry, battery, true);
            assert(sessions.Snapshot().apps[0].user_stopped && !sessions.Find(0));
            host::WidgetSnapshot widget{};
            assert(sessions.Widget(0, widget) && widget.data.template_id == MICROPIXEL_WIDGET_CLOCK);
            std::string before = widget.data.title;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                sessions.Process();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            assert(sessions.Widget(0, widget) && before != widget.data.title);
            auto intent = player.PlaybackIntent();
            Until(sessions, [&] { return sessions.Alarm().ringing; });
            assert(audio.AlarmFocus() && !sessions.Find(0));
            assert(player.PlaybackIntent().version == intent.version && !player.Paused());
            player.Pause(true);
            auto paused = player.PlaybackIntent();
            sessions.StopAlarm();
            sessions.Process();
            assert(!audio.AlarmFocus() && player.Paused() && player.PlaybackIntent().version == paused.version);
            player.Pause(false);
            assert(sessions.Activate(0));
            Until(sessions, [&] {
                return sessions.Snapshot().foreground == 0 && sessions.Find(0)->ComponentInfo().activity_state == 2;
            });
            sessions.Stop(0);
            Until(sessions, [&] { return sessions.Snapshot().apps[0].state == host::AppState::kStopped; });
            assert(sessions.Snapshot().apps[0].user_stopped);
            sessions.Shutdown();
        }
        {
            pc::AppSessions sessions(apps, display, info, fonts.guest_fonts, audio, random, registry, battery, true);
            assert(sessions.Snapshot().apps[0].user_stopped && !sessions.Find(0));
            Until(sessions, [&] { return sessions.Alarm().ringing; });
            assert(!sessions.Find(0) && audio.AlarmFocus());
            auto old = player.PlaybackIntent();
            player.Stop();
            player.Play(argv[2]);
            auto changed = player.PlaybackIntent();
            assert(changed.generation != old.generation && changed.version != old.version);
            sessions.StopAlarm();
            sessions.Process();
            assert(!player.Paused() && !player.Finished() && player.PlaybackIntent().version == changed.version);
            assert(sessions.Remove(0, true));
            Until(sessions, [&] { return !sessions.Removing(0); });
            assert(sessions.RemovalResult(0) == 0 && !apps[0]->available && !sessions.Activate(0));
            {
                pc::ScheduleStore store;
                host::AlarmScheduler restored;
                store.LoadAlarms(apps[0]->app_id, 0, restored);
                for (auto& record : restored.Records()) assert(record.app == UINT32_MAX);
            }
            sessions.Shutdown();
        }
        player.Stop();
        std::puts(
            "PASS stopped Clock Host time/alarm, actual durable restart, audio intent unchanged by duck/release, pause "
            "and owner generation switch, alarm cleanup");
    }
    pc::JoinTasks();
    lv_display_delete(display);
    lv_deinit();
    wasm_runtime_destroy();
}
