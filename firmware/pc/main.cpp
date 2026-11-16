// SPDX-License-Identifier: Apache-2.0
#include <SDL2/SDL.h>
#include <cJSON.h>
#include <png.h>
#include <sys/random.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "alarm_overlay.hpp"
#include "app_sessions.hpp"
#include "audio.hpp"
#include "device/device_registry.hpp"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "home_app.hpp"
#include "market.hpp"
#include "media.hpp"
#include "network.hpp"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "remote.hpp"
#include "runtime/abi/abi_bridge.h"
#include "runtime/resources/bitmap_decoder.hpp"
#include "screen_font.hpp"
#include "session.hpp"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "store.hpp"

namespace {
namespace pc = micropixel::pc;
namespace device = micropixel::device;
namespace graphics = micropixel::platform::lvgl;
struct Random final : device::Random {
    int32_t GetU32(uint32_t& out) override {
        return getrandom(&out, sizeof(out), 0) == sizeof(out) ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
    }
};
struct Battery final : device::Battery {
    device::BatterySnapshot Snapshot() override {
        return {.external_power_connected = true, .external_power_available = true};
    }
    void SetStateChangeSink(device::BatteryStateChangeSink, void*) override {}
};
struct Events {
    std::mutex mutex;
    std::array<SDL_Event, 256> queue{};
    size_t count{};
    std::atomic<bool> quit{};
    bool overflow{};
};
int Filter(void* ctx, SDL_Event* event) {
    auto& events = *static_cast<Events*>(ctx);
    if (event->type == SDL_QUIT || (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_CLOSE)) {
        events.quit = true;
        return 0;
    }
    if (event->type == SDL_TEXTINPUT || event->type == SDL_KEYDOWN || event->type == SDL_KEYUP ||
        event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP || event->type == SDL_MOUSEMOTION) {
        std::lock_guard lock(events.mutex);
        if (events.count < events.queue.size())
            events.queue[events.count++] = *event;
        else
            events.overflow = true;
    }
    return 1;
}
std::optional<device::KeyCode> Key(SDL_Keycode code) {
    switch (code) {
        case SDLK_UP:
        case SDLK_w:
            return device::KeyCode::kUp;
        case SDLK_DOWN:
        case SDLK_s:
            return device::KeyCode::kDown;
        case SDLK_LEFT:
        case SDLK_a:
            return device::KeyCode::kLeft;
        case SDLK_RIGHT:
        case SDLK_d:
            return device::KeyCode::kRight;
        case SDLK_RETURN:
        case SDLK_SPACE:
            return device::KeyCode::kConfirm;
        case SDLK_BACKSPACE:
            return device::KeyCode::kBack;
        case SDLK_z:
            return device::KeyCode::kSouth;
        case SDLK_x:
            return device::KeyCode::kEast;
        default:
            return {};
    }
}
struct Options {
    int width = 720, height = 720;
    double seconds = 0;
    bool headless{}, exit_when_done{}, autostart{}, market{}, music{}, home{}, multi_app{}, profile_explicit{};
    std::string music_query = "纯音乐";
    bool music_query_explicit{};
    uint64_t music_song{};
    std::string market_app;
    std::string install_app, remove_app;
    bool uninstall{}, rv32{};
    std::vector<std::string> guest_arguments;
    std::string screenshot;
    std::string play, http, mqtt, topic = "micropixel/pc/test", message = "MicroPixel PC", output;
    bool exit_when_audio_done{};
    std::vector<std::filesystem::path> paths;
};
bool Parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            std::puts(
                "Usage: micropixel-pc [--headless] [--seconds N] [--width N --height N]\n"
                "       [--screenshot file.png] [--autostart] [--exit-on-app-complete]\n"
                "       [--play local.wav|https://... --exit-on-audio-complete]\n"
                "       [--home | --market | --market-app APP_ID] [--multi-app | --single-app] [--rv32]\n"
                "       [--install-app APP_ID] (download and run official RV32 Bundle)\n       [--uninstall-app APP_ID "
                "| --clear-app-data APP_ID]\n"
                "       [--music | --music-login | --music-query TEXT | --music-song SONG_ID] [--guest-arg TEXT ...]\n"
                "       [--http URL --output file | --mqtt URL --topic TOPIC --message TEXT]\n"
                "       [app.pc.json | app.wasm | apps-directory ...]\n"
                "Mouse: touch; arrows/WASD: direction; Enter: confirm; Home/F1: Home (keep app running); Esc: close "
                "app");
            return false;
        } else if (arg == "--headless")
            options.headless = true;
        else if (arg == "--multi-app") {
            options.multi_app = true;
            options.profile_explicit = true;
        } else if (arg == "--single-app") {
            options.multi_app = false;
            options.profile_explicit = true;
        } else if (arg == "--market")
            options.market = true;
        else if (arg == "--rv32")
            options.rv32 = true;
        else if (arg == "--home")
            options.home = true;
        else if (arg == "--music-login") {
            options.music = true;
            options.guest_arguments.emplace_back("--login");
        } else if (arg == "--music")
            options.music = true;
        else if (arg == "--exit-on-app-complete")
            options.exit_when_done = true;
        else if (arg == "--autostart")
            options.autostart = true;
        else if (arg == "--exit-on-audio-complete")
            options.exit_when_audio_done = true;
        else if ((arg == "--width" || arg == "--height" || arg == "--seconds" || arg == "--screenshot" ||
                  arg == "--play" || arg == "--http" || arg == "--mqtt" || arg == "--topic" || arg == "--message" ||
                  arg == "--output" || arg == "--market-app" || arg == "--install-app" || arg == "--guest-arg" ||
                  arg == "--music-query" || arg == "--music-song" || arg == "--uninstall-app" ||
                  arg == "--clear-app-data") &&
                 i + 1 < argc) {
            const char* value = argv[++i];
            if (arg == "--uninstall-app" || arg == "--clear-app-data") {
                options.remove_app = value;
                options.uninstall = arg == "--uninstall-app";
            } else if (arg == "--width")
                options.width = std::atoi(value);
            if (arg == "--height") options.height = std::atoi(value);
            if (arg == "--seconds") options.seconds = std::atof(value);
            if (arg == "--screenshot") options.screenshot = value;
            if (arg == "--play") options.play = value;
            if (arg == "--http") options.http = value;
            if (arg == "--mqtt") options.mqtt = value;
            if (arg == "--topic") options.topic = value;
            if (arg == "--message") options.message = value;
            if (arg == "--output") options.output = value;
            if (arg == "--market-app") {
                options.market = true;
                options.market_app = value;
            }
            if (arg == "--install-app") options.install_app = value;
            if (arg == "--guest-arg") options.guest_arguments.emplace_back(value);
            if (arg == "--music-query") {
                options.music = true;
                options.music_query_explicit = true;
                options.music_query = value;
            }
            if (arg == "--music-song") {
                options.music = true;
                char* end = nullptr;
                options.music_song = std::strtoull(value, &end, 10);
                if (!options.music_song || !end || *end || options.music_song > 9007199254740991ULL) {
                    std::fputs("Invalid song ID\n", stderr);
                    std::exit(2);
                }
            }
        } else if (!arg.empty() && arg[0] != '-')
            options.paths.emplace_back(arg);
        else {
            std::fprintf(stderr, "Unknown or incomplete argument: %s\n", arg.c_str());
            std::exit(2);
        }
    }
    if (options.width < 240 || options.width > 1920 || options.height < 240 || options.height > 1920 ||
        options.seconds < 0) {
        std::fputs("Invalid display size or timeout\n", stderr);
        std::exit(2);
    }
    if (options.market && (options.autostart || options.exit_when_done)) {
        std::fputs("--market cannot be combined with automatic Guest launch\n", stderr);
        std::exit(2);
    }
    if (options.music && (options.market || options.autostart || !options.install_app.empty())) {
        std::fputs("--music cannot be combined with market or automatic Guest launch\n", stderr);
        std::exit(2);
    }
    if (options.home && (options.music || options.market || options.autostart || options.exit_when_done ||
                         !options.install_app.empty())) {
        std::fputs("--home cannot be combined with automatic app launch or market\n", stderr);
        std::exit(2);
    }
    if (options.home && !options.profile_explicit) options.multi_app = true;
    return true;
}
// System fonts stay local; no downloaded font is passed to the TTF parser.
void Screenshot(const std::string& path) {
    auto* snapshot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
    if (!snapshot) {
        std::fputs("Cannot capture LVGL display\n", stderr);
        return;
    }
    // Snapshotting the active screen excludes LVGL system layers. Composite them
    // so screenshots match Home/market on the display, rather than the Guest below.
    for (auto* layer : {lv_layer_top(), lv_layer_sys()}) {
        if (!layer || lv_obj_get_child_count(layer) == 0) continue;
        auto* overlay = lv_snapshot_take(layer, LV_COLOR_FORMAT_ARGB8888);
        if (!overlay) continue;
        const bool premultiplied = (overlay->header.flags & LV_IMAGE_FLAGS_PREMULTIPLIED) != 0;
        const auto width = std::min(snapshot->header.w, overlay->header.w);
        const auto height = std::min(snapshot->header.h, overlay->header.h);
        for (uint32_t y = 0; y < height; ++y) {
            auto* destination = snapshot->data + y * snapshot->header.stride;
            const auto* source = overlay->data + y * overlay->header.stride;
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t alpha = source[x * 4 + 3];
                for (uint32_t c = 0; c < 3; ++c) {
                    const auto channel = premultiplied ? source[x * 4 + c] * 255U : source[x * 4 + c] * alpha;
                    destination[x * 4 + c] =
                        std::min(255U, (channel + destination[x * 4 + c] * (255 - alpha) + 127) / 255);
                }
                destination[x * 4 + 3] = alpha + (destination[x * 4 + 3] * (255 - alpha) + 127) / 255;
            }
        }
        lv_draw_buf_destroy(overlay);
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = snapshot->header.w;
    image.height = snapshot->header.h;
    image.format = PNG_FORMAT_BGRA;
    if (!png_image_write_to_file(&image, path.c_str(), 0, snapshot->data, snapshot->header.stride, nullptr))
        std::fprintf(stderr, "Screenshot failed: %s\n", image.message);
    else
        std::fprintf(stderr, "Screenshot: %s\n", path.c_str());
    lv_draw_buf_destroy(snapshot);
}

}  // namespace
int main(int argc, char** argv) {
    Options options;
    if (!Parse(argc, argv, options)) return 0;
    if (!options.http.empty()) {
        std::vector<uint8_t> body;
        std::string error;
        if (!pc::HttpGet(options.http, body, error)) {
            std::fprintf(stderr, "HTTP: %s\n", error.c_str());
            return 1;
        }
        FILE* out = options.output.empty() ? stdout : std::fopen(options.output.c_str(), "wb");
        if (!out) {
            std::perror("output");
            return 1;
        }
        const bool ok = std::fwrite(body.data(), 1, body.size(), out) == body.size();
        if (out != stdout) std::fclose(out);
        return ok ? 0 : 1;
    }
    if (!options.mqtt.empty()) {
        std::string received, error;
        if (!pc::MqttRoundTrip(options.mqtt, options.topic, options.message, received, error)) {
            std::fprintf(stderr, "MQTT: %s\n", error.c_str());
            return 1;
        }
        std::printf("MQTT received: %s\n", received.c_str());
        return 0;
    }
    if (!options.install_app.empty()) {
        std::filesystem::path installed;
        std::string error;
        if (!pc::InstallStoreApp(options.install_app, installed, error)) {
            std::fprintf(stderr, "Market install failed: %s\n", error.c_str());
            return 1;
        }
        options.paths = {installed};
        options.autostart = true;
        options.market = false;
    }
    if (options.headless) {
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);
    }
    if (!wasm_runtime_init() || !micropixel_register_native_apis()) {
        std::fputs("Cannot initialize WAMR\n", stderr);
        return 1;
    }
    lv_init();
    auto* display = lv_sdl_window_create(options.width, options.height);
    if (!display) {
        std::fprintf(stderr, "SDL display: %s\n", SDL_GetError());
        return 1;
    }
    lv_sdl_window_set_resizeable(display, false);
    lv_sdl_window_set_title(display, "MicroPixel PC");
    auto* mouse = lv_sdl_mouse_create();
    lv_indev_set_display(mouse, display);
    auto* keyboard = lv_sdl_keyboard_create();
    lv_indev_set_display(keyboard, display);
    auto* focus = lv_group_create();
    lv_group_set_default(focus);
    lv_indev_set_group(keyboard, focus);
    static Events events;
    SDL_SetEventFilter(Filter, &events);
    int exit_code = 0;
    {
        pc::ScreenFont screen_font;
        pc::SdlAudio audio;
        if (!audio.Initialize()) {
            std::fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
            return 1;
        }
        pc::MediaPlayer media(audio);
        if (!options.play.empty()) media.Play(options.play);
        Random random;
        Battery battery;
        device::DeviceRegistry registry;
        if (!registry.InitializeStorage() || !registry.RegisterDisplay("SDL display") ||
            !registry.RegisterTouch("Mouse") || !registry.RegisterAudioOutput("SDL speaker") ||
            !registry.RegisterPower("AC power"))
            return 1;
        device::DisplayInfo info{.driver = "SDL2",
                                 .interface = "WSLg",
                                 .pixel_format = "XRGB8888",
                                 .width_pixels = static_cast<uint32_t>(options.width),
                                 .height_pixels = static_cast<uint32_t>(options.height),
                                 .refresh_rate_hz = 60};
        std::vector<std::unique_ptr<pc::PortableApp>> apps;
        std::vector<std::filesystem::path> files;
        if (options.paths.empty()) {
            if (options.home) {
                for (const char* name : {"clock-alarm", "netease-music", "weather"}) {
                    auto path = std::filesystem::path(options.rv32 ? "build/pc-rv32-apps" : "build/pc-apps") / name /
                                (std::string(name) + ".pc.json");
                    if (std::filesystem::exists(path)) options.paths.push_back(path);
                }
            } else if (std::filesystem::is_directory("build/pc-apps"))
                options.paths.emplace_back("build/pc-apps");
            if (std::filesystem::is_directory(pc::InstalledAppsDirectory()))
                options.paths.push_back(pc::InstalledAppsDirectory());
        }
        for (const auto& path : options.paths) {
            std::error_code error;
            if (std::filesystem::is_directory(path, error)) {
                for (const auto& file : std::filesystem::recursive_directory_iterator(path, error)) {
                    if (files.size() >= 64) break;
                    if (file.is_regular_file() && (file.path().filename().string().ends_with(".pc.json") ||
                                                   file.path().filename().string().ends_with(".bundle.bin")))
                        files.push_back(file.path());
                }
            } else
                files.push_back(path);
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            if (apps.size() == 64) break;
            auto app = std::make_unique<pc::PortableApp>();
            std::string error;
            if (!app->Load(file, error)) {
                std::fprintf(stderr, "%s: %s\n", file.c_str(), error.c_str());
                exit_code = 1;
                continue;
            }
            if (std::any_of(apps.begin(), apps.end(),
                            [&](const auto& current) { return current->app_id == app->app_id; })) {
                std::fprintf(stderr, "Duplicate app ID ignored: %s\n", app->app_id.c_str());
                continue;
            }
            apps.push_back(std::move(app));
        }
        int music_index = -1;
        for (size_t i = 0; i < apps.size(); ++i)
            if (apps[i]->app_id == "micropixel.netease-music") music_index = i;
        if (options.music && music_index < 0) {
            auto app = std::make_unique<pc::PortableApp>();
            std::string error;
            if (!app->Load(options.rv32 ? "build/pc-rv32-apps/netease-music/netease-music.pc.json"
                                        : "build/pc-apps/netease-music/netease-music.pc.json",
                           error)) {
                std::fprintf(stderr, "Build the Guest first: bash tools/pc.sh guest guest/apps/netease-music%s (%s)\n",
                             options.rv32 ? " --backend rv32" : "", error.c_str());
                return 1;
            }
            music_index = apps.size();
            apps.push_back(std::move(app));
        }
        if (options.music) {
            auto& arguments = apps[music_index]->launch_arguments;
            arguments.size = sizeof(arguments);
            auto add = [&](const std::string& text) {
                if (arguments.count >= MICROPIXEL_LAUNCH_ARGUMENT_MAX_COUNT ||
                    text.size() + 1 > MICROPIXEL_LAUNCH_ARGUMENT_MAX_BYTES - arguments.bytes_length)
                    return false;
                arguments.offsets[arguments.count++] = arguments.bytes_length;
                std::memcpy(arguments.bytes + arguments.bytes_length, text.c_str(), text.size() + 1);
                arguments.bytes_length += text.size() + 1;
                return true;
            };
            if ((options.music_query_explicit && (!add("--query") || !add(options.music_query))) ||
                (options.music_song && (!add("--song") || !add(std::to_string(options.music_song))))) {
                std::fputs("Music launch arguments exceed 512 bytes\n", stderr);
                return 1;
            }
        }
        pc::AppSessions sessions(apps, display, info, screen_font.guest_fonts, audio, random, registry, battery,
                                 options.multi_app, !options.home || (options.profile_explicit && options.multi_app));
        if (!options.remove_app.empty()) {
            for (uint32_t i = 0; i < apps.size(); ++i)
                if (apps[i]->app_id == options.remove_app) {
                    if (!sessions.Remove(i, options.uninstall)) return 1;
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                    while (sessions.Removing(i) && std::chrono::steady_clock::now() < deadline) {
                        sessions.Process();
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    return !sessions.Removing(i) && sessions.RemovalResult(i) == 0 ? 0 : 1;
                }
            std::fprintf(stderr, "App not found: %s\n", options.remove_app.c_str());
            return 1;
        }
        if (options.home && options.multi_app) {
            for (uint32_t i = 0; i < apps.size(); ++i) {
                auto& launch = apps[i]->launch_arguments;
                launch.size = sizeof(launch);
                for (const auto& arg : options.guest_arguments) {
                    if (launch.count >= MICROPIXEL_LAUNCH_ARGUMENT_MAX_COUNT ||
                        launch.bytes_length + arg.size() + 1 > sizeof(launch.bytes))
                        return 1;
                    launch.offsets[launch.count++] = launch.bytes_length;
                    std::memcpy(launch.bytes + launch.bytes_length, arg.c_str(), arg.size() + 1);
                    launch.bytes_length += arg.size() + 1;
                }
                if (apps[i]->app_id == "micropixel.weather" || apps[i]->app_id == "micropixel.clock-alarm") {
                    if (!sessions.Snapshot().apps[i].user_stopped) (void)sessions.EnsureWork(i);
                }
            }
        }
        pc::HomeApp home(apps, media, audio, &sessions);
        pc::AlarmOverlay alarm_overlay(sessions);
        pc::Market market;
        if (options.market)
            market.Show(options.market_app);
        else
            home.Show(options.width, options.height);
        if (options.music) home.requested = music_index;
        if (!options.music && (options.autostart || options.exit_when_done) && !apps.empty()) home.requested = 0;
        int active_index = -1, requested_front = -1, exit_index = -1;
        bool foreground = false;
        std::array<bool, 64> seen_running{};
        const auto started = std::chrono::steady_clock::now();
        bool finger_down = false;
        bool text_active = false;
        auto show_home = [&] {
            if (active_index >= 0 && foreground)
                std::fprintf(stderr, "APP_SHELL background: %s\n", apps[active_index]->app_id.c_str());
            sessions.ShowHome();
            requested_front = -1;
            foreground = finger_down = false;
            esp_lv_adapter_lock(-1);
            market.Hide();
            home.Show(options.width, options.height);
            lv_indev_reset(mouse, nullptr);
            lv_indev_reset(keyboard, nullptr);
            esp_lv_adapter_unlock();
        };
        auto resume_session = [&] {
            if (active_index < 0 || !sessions.Find(active_index)) return;
            requested_front = active_index;
            (void)sessions.Activate(active_index);
            std::fprintf(stderr, "APP_SHELL foreground: %s reused=1\n", apps[active_index]->app_id.c_str());
        };
        while (!events.quit) {
            sessions.Process();
            const auto& snapshot = sessions.Snapshot();
            if (options.exit_when_done && exit_index >= 0 && seen_running[exit_index] &&
                snapshot.apps[exit_index].state == micropixel::host::AppState::kStopped &&
                !sessions.Pending(exit_index)) {
                exit_code = snapshot.apps[exit_index].result;
                break;
            }
            if (active_index >= 0 && !sessions.Find(active_index) && !sessions.Pending(active_index)) {
                active_index = snapshot.foreground;
                if (active_index < 0)
                    for (uint32_t i = 0; i < snapshot.count; ++i)
                        if (sessions.Find(i)) {
                            active_index = i;
                            break;
                        }
                if (!market.Visible() && snapshot.foreground < 0) {
                    esp_lv_adapter_lock(-1);
                    home.Show(options.width, options.height);
                    esp_lv_adapter_unlock();
                }
            }
            if (requested_front >= 0 && !sessions.Find(requested_front) && !sessions.Pending(requested_front))
                requested_front = -1;
            if (requested_front >= 0 && snapshot.foreground == requested_front) {
                active_index = requested_front;
                requested_front = -1;
                esp_lv_adapter_lock(-1);
                home.Hide();
                market.Hide();
                lv_indev_reset(mouse, nullptr);
                lv_indev_reset(keyboard, nullptr);
                esp_lv_adapter_unlock();
            }
            if (snapshot.foreground < 0 && requested_front < 0 && !home.Visible() && !market.Visible()) {
                esp_lv_adapter_lock(-1);
                home.Show(options.width, options.height);
                esp_lv_adapter_unlock();
            }
            auto* session = active_index >= 0 ? sessions.Find(active_index) : nullptr;
            foreground = session && snapshot.foreground == active_index && !home.Visible() && !market.Visible();
            auto& input = sessions.Input();
            esp_lv_adapter_lock(-1);
            home.SetMedia(active_index >= 0 && sessions.Media(active_index) ? *sessions.Media(active_index) : media);
            home.SetRunning(session ? apps[active_index].get() : nullptr);
            home.Update();
            market.Update();
            alarm_overlay.Update(options.width, options.height);
            lv_timer_handler();
            esp_lv_adapter_unlock();
            static std::array<SDL_Event, 256> pending;
            size_t count;
            bool overflow;
            {
                std::lock_guard lock(events.mutex);
                count = events.count;
                std::copy_n(events.queue.begin(), count, pending.begin());
                events.count = 0;
                overflow = events.overflow;
                events.overflow = false;
            }
            const bool wants_text = session && foreground && input.TextEnabled();
            if (wants_text != text_active) {
                text_active = wants_text;
                if (text_active)
                    SDL_StartTextInput();
                else
                    SDL_StopTextInput();
            }
            bool stop = false;
            bool go_home = false;
            for (size_t i = 0; i < count; ++i) {
                const auto& event = pending[i];
                if (alarm_overlay.Visible()) {
                    if (event.type == SDL_KEYDOWN && !event.key.repeat &&
                        (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_RETURN))
                        sessions.StopAlarm();
                    continue;
                }
                if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                    if (event.key.keysym.sym == SDLK_HOME || event.key.keysym.sym == SDLK_F1) {
                        if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                            if (session && !foreground && home.Visible())
                                home.resume_requested = true;
                            else
                                go_home = true;
                        }
                        continue;
                    }
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        if (event.type == SDL_KEYDOWN && !event.key.repeat) stop = true;
                        continue;
                    }
                }
                if (!session || !foreground || go_home) continue;
                const auto now = static_cast<uint64_t>(esp_timer_get_time());
                if (event.type == SDL_TEXTINPUT) {
                    input.InjectText(event.text.text);
                } else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                    if (text_active && event.key.keysym.sym != SDLK_RETURN && event.key.keysym.sym != SDLK_BACKSPACE)
                        continue;
                    if (auto code = Key(event.key.keysym.sym))
                        (void)input.InjectKey({now, *code,
                                               event.type == SDL_KEYUP ? device::KeyPhase::kUp
                                               : event.key.repeat      ? device::KeyPhase::kRepeat
                                                                       : device::KeyPhase::kDown,
                                               static_cast<uint32_t>(event.key.repeat)});
                } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
                    if (event.button.button != SDL_BUTTON_LEFT) continue;
                    finger_down = event.type == SDL_MOUSEBUTTONDOWN;
                    (void)input.InjectTouch({now, 1, event.button.x, event.button.y,
                                             static_cast<uint16_t>(finger_down ? 1000 : 0),
                                             finger_down ? device::TouchPhase::kDown : device::TouchPhase::kUp});
                } else if (event.type == SDL_MOUSEMOTION && finger_down)
                    (void)input.InjectTouch({now, 1, event.motion.x, event.motion.y, 1000, device::TouchPhase::kMove});
            }
            if (overflow && session && foreground) {
                finger_down = false;
                (void)input.InjectTouch(
                    {static_cast<uint64_t>(esp_timer_get_time()), 1, 0, 0, 0, device::TouchPhase::kCancel});
            }
            if (go_home) show_home();
            if (stop && market.Visible()) {
                esp_lv_adapter_lock(-1);
                market.Back();
                esp_lv_adapter_unlock();
                stop = false;
            }
            if (stop && session && !foreground) {
                home.resume_requested = true;
                stop = false;
            }
            if (home.resume_requested) {
                home.resume_requested = false;
                resume_session();
            }
            if (home.close_requested) {
                home.close_requested = false;
                stop = true;
            }
            if (home.stop_requested >= 0) {
                sessions.Stop(static_cast<uint32_t>(home.stop_requested));
                home.stop_requested = -1;
                show_home();
            }
            if (home.music_requested) {
                home.music_requested = false;
                for (size_t i = 0; i < apps.size(); ++i)
                    if (apps[i]->app_id == "micropixel.netease-music") home.requested = i;
                if (home.requested < 0)
                    std::fputs("Music Guest is not loaded. Build and start with: bash tools/pc.sh run --music\n",
                               stderr);
            }
            if (home.market_requested) {
                home.market_requested = false;
                sessions.ShowHome();
                requested_front = -1;
                foreground = finger_down = false;
                esp_lv_adapter_lock(-1);
                home.Hide();
                market.Show();
                esp_lv_adapter_unlock();
            }
            if (market.TakeHallRequest()) {
                esp_lv_adapter_lock(-1);
                market.Hide();
                home.Show(options.width, options.height);
                esp_lv_adapter_unlock();
            }
            if (!foreground) {
                auto installed = market.TakeLaunchRequest();
                if (!installed.empty()) {
                    auto app = std::make_unique<pc::PortableApp>();
                    std::string error;
                    if (app->Load(installed, error)) {
                        size_t index = 0;
                        while (index < apps.size() && apps[index]->app_id != app->app_id) ++index;
                        if (index < 64) {
                            if (sessions.Install(index, std::move(app))) {
                                active_index = index;
                                requested_front = index;
                                seen_running[index] = true;
                                esp_lv_adapter_lock(-1);
                                market.Hide();
                                esp_lv_adapter_unlock();
                            }
                        } else {
                            std::fputs("Local app capacity reached (64)\n", stderr);
                            esp_lv_adapter_lock(-1);
                            market.ReportLaunchError("本地应用数量已达上限（64）。应用已保存，可重启后单独运行。");
                            esp_lv_adapter_unlock();
                        }
                    } else {
                        std::fprintf(stderr, "Installed app load failed: %s\n", error.c_str());
                        esp_lv_adapter_lock(-1);
                        market.ReportLaunchError("应用加载失败：" + error);
                        esp_lv_adapter_unlock();
                    }
                }
            }
            if (stop && active_index >= 0) {
                sessions.Stop(active_index);
                show_home();
            }
            if (home.requested >= 0) {
                const int index = home.requested;
                home.requested = -1;
                if (static_cast<size_t>(index) >= apps.size()) continue;
                if (session && index == active_index) {
                    resume_session();
                    continue;
                }
                auto& launch = apps[index]->launch_arguments;
                // Reset generic arguments on every launch; music startup options occupy the prefix.
                if (!options.music || index != music_index) {
                    launch = {};
                    launch.size = sizeof(launch);
                }
                for (const auto& argument : options.guest_arguments) {
                    if (launch.count >= MICROPIXEL_LAUNCH_ARGUMENT_MAX_COUNT ||
                        argument.size() + 1 > MICROPIXEL_LAUNCH_ARGUMENT_MAX_BYTES - launch.bytes_length) {
                        std::fputs("Guest launch arguments exceed ABI capacity\n", stderr);
                        exit_code = 1;
                        events.quit = true;
                        break;
                    }
                    launch.offsets[launch.count++] = launch.bytes_length;
                    std::memcpy(launch.bytes + launch.bytes_length, argument.c_str(), argument.size() + 1);
                    launch.bytes_length += argument.size() + 1;
                }
                if (events.quit) break;
                if (!sessions.Activate(index)) {
                    std::fputs("APP_MANAGER rejected: no capacity or busy\n", stderr);
                    continue;
                }
                requested_front = index;
                seen_running[index] = true;
                if (options.exit_when_done) exit_index = index;
                sessions.Process();
            }
            if (options.seconds > 0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= options.seconds) {
                if ((options.exit_when_done && exit_index >= 0 && sessions.Find(exit_index) &&
                     !sessions.Find(exit_index)->Done()) ||
                    (options.exit_when_audio_done && !media.Finished()))
                    exit_code = 124;
                break;
            }
            if (options.exit_when_audio_done && media.Finished()) {
                exit_code = media.Failed() ? 1 : 0;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!options.screenshot.empty()) {
            esp_lv_adapter_lock(-1);
            Screenshot(options.screenshot);
            esp_lv_adapter_unlock();
        }
        sessions.Shutdown();
        market.Hide();
        home.Hide();
        pc::JoinTasks();
    }
    SDL_SetEventFilter(nullptr, nullptr);
    lv_deinit();
    SDL_Quit();
    wasm_runtime_destroy();
    return exit_code;
}
