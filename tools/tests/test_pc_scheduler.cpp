// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "app_sessions.hpp"
#include "esp_lv_adapter.h"
#include "runtime/abi/abi_bridge.h"
#include "schedule_store.hpp"
#include "screen_font.hpp"
namespace pc = micropixel::pc;
namespace host = micropixel::host;
namespace device = micropixel::device;
namespace graphics = micropixel::platform::lvgl;
struct Random final : device::Random {
    int32_t GetU32(uint32_t& n) override {
        n = ++next;
        return 0;
    }
    std::atomic<uint32_t> next{};
};
struct Battery final : device::Battery {
    device::BatterySnapshot Snapshot() override { return {}; }
    void SetStateChangeSink(device::BatteryStateChangeSink, void*) override {}
};
template <class Predicate>
void Until(pc::AppSessions& sessions, Predicate predicate, unsigned seconds = 8) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!predicate() && std::chrono::steady_clock::now() < end) {
        sessions.Process();
        esp_lv_adapter_lock(-1);
        lv_timer_handler();
        esp_lv_adapter_unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(predicate());
}
void Args(pc::PortableApp& app, const char* origin) {
    auto& a = app.launch_arguments;
    a = {};
    a.size = sizeof(a);
    const char* values[] = {"--weather-origin", origin, "--weather-budget-ms", "1000"};
    for (auto* s : values) {
        a.offsets[a.count++] = a.bytes_length;
        auto n = std::strlen(s) + 1;
        assert(a.bytes_length + n < sizeof(a.bytes));
        std::memcpy(a.bytes + a.bytes_length, s, n);
        a.bytes_length += n;
    }
}
int main(int argc, char** argv) {
    assert(argc == 5);
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
        pc::ScreenFont screen_font;
        auto& fonts = screen_font.guest_fonts;
        std::vector<std::unique_ptr<pc::PortableApp>> apps;
        for (int i = 1; i <= 2; ++i) {
            auto a = std::make_unique<pc::PortableApp>();
            std::string error;
            assert(a->Load(argv[i], error));
            apps.push_back(std::move(a));
        }
        Args(*apps[0], argv[3]);
        {
            pc::AppSessions sessions(apps, display, info, fonts, audio, random, registry, battery, true);
            assert(sessions.Activate(1));
            Until(sessions, [&] { return sessions.Snapshot().foreground == 1; });
            auto peer = sessions.Snapshot().apps[1].instance;
            assert(sessions.EnsureWork(0));
            Until(sessions, [&] { return sessions.Find(0) && sessions.Find(0)->ComponentInfo().service_state == 2; });
            host::WidgetSnapshot weather{};
            auto snapshot = [&] {
                for (unsigned i = 0; i < 8; ++i)
                    if (sessions.Widget(i, weather) && weather.app == 0) return true;
                return false;
            };
            Until(sessions, [&] {
                return snapshot() && weather.data.updated_utc && !(weather.data.flags & MICROPIXEL_WIDGET_ERROR);
            });
            assert(sessions.Find(0)->ComponentInfo().activity_state == 0 && sessions.Snapshot().foreground == 1);
            auto first = weather.instance;
            auto temperature = weather.data.value;
            auto updated = weather.data.updated_utc;
            Until(sessions, [&] { return sessions.Find(0)->Suspended(); });
            auto refresh = [&](uint64_t sequence) {
                assert(snapshot());
                uint32_t ticket = 0;
                assert(sessions.WidgetAction(weather, MICROPIXEL_ACTION_REFRESH, 0, sequence, ticket) == 0);
                Until(sessions, [&] { return sessions.WidgetResult(ticket).complete; });
                return sessions.WidgetResult(ticket).result;
            };
            assert(refresh(1) != 0);
            assert(snapshot() && weather.data.value == temperature && weather.data.updated_utc == updated &&
                   (weather.data.flags & MICROPIXEL_WIDGET_ERROR));
            auto started = std::chrono::steady_clock::now();
            assert(refresh(2) != 0);
            assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
            assert(snapshot() && weather.data.value == temperature);
            assert(sessions.Snapshot().foreground == 1 && sessions.Snapshot().apps[1].instance == peer);
            assert(sessions.Activate(0));
            Until(sessions, [&] { return sessions.Snapshot().foreground == 0; });
            assert(sessions.Snapshot().apps[0].instance == first);
            sessions.ShowHome();
            Until(sessions, [&] { return sessions.Find(0)->ComponentInfo().activity_state == 3; });
            sessions.Stop(0);
            Until(sessions, [&] { return sessions.Snapshot().apps[0].state == host::AppState::kStopped; });
            assert(sessions.Snapshot().apps[0].user_stopped && !sessions.EnsureWork(0));
            assert(snapshot() && !weather.online);
            Args(*apps[0], argv[3]);
            assert(refresh(3) == 0);
            assert(snapshot() && weather.online && weather.instance != first && weather.data.value != temperature);
            assert(sessions.Find(0)->ComponentInfo().activity_state == 0 && sessions.Snapshot().foreground < 0);
            {
                pc::ScheduleStore store;
                auto restored = std::make_unique<host::JobScheduler>();
                store.LoadJobs(apps[0]->app_id, 0, *restored);
                unsigned count = 0;
                for (auto& r : restored->Records()) count += r.app == 0;
                assert(count == 2 && !store.Stopped(apps[0]->app_id));
            }
            assert(sessions.ClearSchedules(0));
            {
                pc::ScheduleStore store;
                auto restored = std::make_unique<host::JobScheduler>();
                store.LoadJobs(apps[0]->app_id, 0, *restored);
                for (auto& r : restored->Records()) assert(r.app == UINT32_MAX);
            }
            sessions.Shutdown();
        }
        std::vector<std::unique_ptr<pc::PortableApp>> cold;
        for (unsigned i = 0; i < 2; ++i) {
            auto app = std::make_unique<pc::PortableApp>();
            std::string error;
            assert(app->Load(argv[4], error));
            app->app_id = i ? "micropixel.cold.sender" : "micropixel.cold.receiver";
            std::memset(app->package.app_id, 0, sizeof(app->package.app_id));
            std::memcpy(app->package.app_id, app->app_id.data(), app->app_id.size());
            auto& req = app->requirements;
            req.schema_version = 2;
            req.component_flags = MICROPIXEL_COMPONENT_FLAG_SERVICE | MICROPIXEL_COMPONENT_FLAG_BACKGROUND;
            req.background_kinds = 2;
            req.widget_count = 0;
            req.services[12] = 65536;
            req.services[14] = 65536;
            req.services[15] = i ? 0 : 65536;
            if (i) {
                auto& args = app->launch_arguments;
                args.size = sizeof(args);
                args.count = 1;
                args.bytes_length = 9;
                std::memcpy(args.bytes, "--sender", 9);
            }
            cold.push_back(std::move(app));
        }
        {
            pc::AppSessions sessions(cold, display, info, fonts, audio, random, registry, battery, true);
            assert(sessions.EnsureWork(0));
            Until(sessions, [&] { return sessions.Find(0) && sessions.Find(0)->ComponentInfo().service_state == 2; });
            auto old = sessions.Snapshot().apps[0].instance;
            sessions.Stop(0, false);
            Until(sessions, [&] { return sessions.Snapshot().apps[0].state == host::AppState::kStopped; });
            assert(sessions.EnsureWork(1));
            Until(sessions, [&] {
                return sessions.Snapshot().apps[1].state == host::AppState::kStopped &&
                       sessions.Snapshot().apps[0].instance.valid() && sessions.Snapshot().apps[0].instance != old;
            });
            assert(sessions.Snapshot().apps[1].result == 0 && sessions.Snapshot().foreground < 0);
            sessions.Shutdown();
        }
        std::puts(
            "PASS actual Wasm Jobs: cold Service without Activity, suspended/hot work, HTTP failure/deadline cache "
            "retention, user stop, explicit cold widget refresh, durable plans and cleanup");
    }
    pc::JoinTasks();
    lv_display_delete(display);
    lv_deinit();
    wasm_runtime_destroy();
}
