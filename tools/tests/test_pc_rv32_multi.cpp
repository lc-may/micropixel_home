// SPDX-License-Identifier: Apache-2.0
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "backend_limits.hpp"
#include "pc_app_test_support.hpp"
#include "runtime/abi/abi_bridge.h"
namespace {
std::string Ram(int pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/cmdline");
    std::string arg;
    while (std::getline(file, arg, '\0'))
        if (auto at = arg.find("mem-path="); at != std::string::npos) return arg.substr(at + 9);
    assert(false);
    return {};
}
void Alias(pc::PortableApp& app, unsigned index) {
    app.app_id = "test.rv32." + std::to_string(index);
    std::memset(app.package.app_id, 0, sizeof(app.package.app_id));
    std::memcpy(app.package.app_id, app.app_id.c_str(), app.app_id.size());
}
void Click(pc::AppSessions& sessions, int x, int y) {
    assert(sessions.Input().InjectTouch({.id = 1, .x = x, .y = y, .phase = device::TouchPhase::kDown}));
    assert(sessions.Input().InjectTouch({.id = 1, .x = x, .y = y, .phase = device::TouchPhase::kUp}));
}
}  // namespace
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
        graphics::SystemFontSet fonts;
        for (unsigned i = 0; i < fonts.fonts.size(); ++i)
            fonts.fonts[i] = graphics::BuiltinLatinFont(static_cast<graphics::SystemFontRole>(i));
        std::vector<std::unique_ptr<pc::PortableApp>> apps;
        for (unsigned i = 0; i < 5; ++i) {
            auto app = std::make_unique<pc::PortableApp>();
            std::string error;
            assert(app->Load(argv[i == 2 ? 2 : 1], error));
            Alias(*app, i);
            apps.push_back(std::move(app));
        }
        pc::AppSessions sessions(apps, display, info, fonts, audio, random, registry, battery, true);
        auto foreground = [&](uint32_t app) {
            assert(sessions.Activate(app));
            Until(
                sessions,
                [&] {
                    return sessions.Snapshot().foreground == static_cast<int>(app) &&
                           sessions.Find(app)->ComponentInfo().activity_state == 2;
                },
                30);
        };
        auto live = [&](uint32_t app) {
            return sessions.Find(app) && sessions.Find(app)->ComponentInfo().service_state == 2 &&
                   sessions.Find(app)->ComponentInfo().resource_count == 4;
        };
        foreground(0);
        foreground(1);
        Until(sessions, [&] { return live(0) && live(1); }, 30);
        auto key0 = sessions.Snapshot().apps[0].instance, key1 = sessions.Snapshot().apps[1].instance;
        int pid0 = sessions.Find(0)->ChildPid(), pid1 = sessions.Find(1)->ChildPid();
        assert(pid0 > 0 && pid1 > 0 && pid0 != pid1);
        auto ram0 = Ram(pid0), ram1 = Ram(pid1);
        assert(ram0 != ram1 && std::filesystem::file_size(ram0) == MP_RV_RAM_SIZE &&
               std::filesystem::file_size(ram1) == MP_RV_RAM_SIZE);
        auto peer_frames = audio.RenderedFrames(key1.slot + 1);
        Until(sessions, [&] { return audio.RenderedFrames(key1.slot + 1) > peer_frames + 4800; });
        foreground(0);
        Click(sessions, 100, 550);
        Until(
            sessions,
            [&] {
                return sessions.Find(0)->ComponentInfo().activity_state == 0 &&
                       sessions.Find(0)->ComponentInfo().resource_count == 2;
            },
            30);
        assert(sessions.Find(1)->ChildPid() == pid1 && live(1));
        foreground(0);
        assert(sessions.Snapshot().apps[0].instance == key0);
        foreground(2);
        assert(sessions.Find(2)->ChildPid() == 0);
        assert(live(0) && live(1));
        foreground(3);
        assert(sessions.Snapshot().subprocesses == 3 && sessions.Snapshot().reserved_bytes <= pc::kSessionMemoryBudget);
        assert(!sessions.Activate(4) && sessions.Snapshot().foreground == 3);
        auto before_stop = audio.RenderedFrames(key1.slot + 1);
        sessions.Stop(0);
        Until(sessions, [&] { return !sessions.Find(0); }, 30);
        assert(kill(pid0, 0) == -1 && errno == ESRCH && !std::filesystem::exists(ram0));
        assert(kill(pid1, 0) == 0 && std::filesystem::exists(ram1));
        Until(sessions, [&] { return audio.RenderedFrames(key1.slot + 1) > before_stop + 4800; });
        foreground(0);
        assert(sessions.Snapshot().apps[0].instance != key0 && sessions.Find(0)->ChildPid() != pid1);
        assert(sessions.Find(1)->ChildPid() == pid1 && sessions.Snapshot().apps[1].instance == key1);
        for (unsigned cycle = 0; cycle < 8; ++cycle) {
            sessions.Stop(0);
            Until(sessions, [&] { return !sessions.Find(0); }, 30);
            auto previous = sessions.Snapshot().apps[1].instance;
            foreground(0);
            Until(sessions, [&] { return live(0); }, 30);
            assert(sessions.Find(1)->ChildPid() == pid1 && sessions.Snapshot().apps[1].instance == previous && live(1));
        }
        // An emulator process disappearing only retires its own slot and resources.
        auto before_failure = audio.RenderedFrames(key1.slot + 1);
        auto failed = sessions.Find(0)->ChildPid();
        kill(failed, SIGKILL);
        Until(sessions, [&] { return !sessions.Find(0); }, 30);
        assert(sessions.Snapshot().apps[0].result != 0 && sessions.Find(1)->ChildPid() == pid1 && live(1));
        Until(sessions, [&] { return audio.RenderedFrames(key1.slot + 1) > before_failure + 4800; });
        for (unsigned i = 0; i < apps.size(); ++i) sessions.Stop(i);
        Until(
            sessions,
            [&] {
                for (unsigned i = 0; i < apps.size(); ++i)
                    if (sessions.Find(i)) return false;
                return true;
            },
            30);
        assert(!sessions.Snapshot().subprocesses && !sessions.Snapshot().reserved_bytes &&
               !std::filesystem::exists(ram1));
        std::puts(
            "PASS RV32 multi-instance: four mixed slots, private QEMU/RAM, independent PCM/input/components, process "
            "drain, stale generations and isolated child failure");
    }
    pc::JoinTasks();
    lv_display_delete(display);
    lv_deinit();
    wasm_runtime_destroy();
}
