// SPDX-License-Identifier: Apache-2.0
#include <charconv>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

#include "alarm_endpoint.hpp"
#include "audio_pcm_stream.h"
#include "bflb_core.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "guest_panel.hpp"
#include "guest_runner.hpp"
#include "guest_runtime.hpp"
#include "host/controller/app_manager.hpp"
#include "main_failure.h"
#include "media.hpp"
#include "media_widgets.hpp"
#include "mpx_guest_clock-alarm.h"
#include "mpx_guest_netease-music.h"
#include "mpx_guest_news.h"
#include "mpx_guest_settings.h"
#include "mpx_guest_weather.h"
#include "network.hpp"
#include "network_time.h"
#include "orion_audio_output.hpp"
#include "orion_font.hpp"
#include "orion_home.hpp"
#include "platform/audio/audio_engine.hpp"
#include "sdk_hello.h"
#include "timer_counter.h"
#include "weather_endpoint.hpp"
#include "wireless.h"
#include "work/task_policy.hpp"

extern "C" {
#include "board_gpio.h"
#include "mm.h"
#include "shell.h"
}

extern "C" {
extern char __ram_wifi_start__, __rom_text_start__, __rom_text_end__;
#if defined(CONFIG_PSRAM_COPY_CODE)
extern char __psram_text_start__, __psram_text_end__;
#endif
}

namespace micropixel::bouffalo {
namespace {

using host::AppManager;
using host::ManagerResult;

constexpr uint32_t kControllerStackBytes = 12U * 1024U;
constexpr uint32_t kClockApp = 1;
constexpr uint32_t kWeatherApp = 2;
constexpr uint32_t kMusicApp = 3;
constexpr uint16_t kActivity = MICROPIXEL_COMPONENT_FLAG_ACTIVITY;
constexpr uint16_t kActivityAndService = kActivity | MICROPIXEL_COMPONENT_FLAG_SERVICE;
constexpr EmbeddedApp kApps[] = {
    {"settings", settings, settings_wasm_size, true, true},
    {"clock-alarm", clock_alarm, clock_alarm_wasm_size, true, false, kActivityAndService, true},
    {"weather", weather, weather_wasm_size, true, false, kActivityAndService, false, true},
    {.name = "netease-music",
     .wasm = netease_music,
     .unpacked_size = netease_music_wasm_size,
     .activity = true,
     .component_flags = kActivityAndService,
     .music = true},
    {"news", news, news_wasm_size, true, false, kActivity},
    {"sdk_hello", sdk_hello, sdk_hello_wasm_size},
    {"timer_counter", timer_counter, timer_counter_wasm_size},
    {"audio_pcm_stream", audio_pcm_stream, audio_pcm_stream_wasm_size},
    {"main_failure", main_failure, main_failure_wasm_size},
};
struct HomeTile {
    const char* title;
    HomeAppRole role;
};
constexpr HomeTile kHomeTiles[]{
    {"配置", HomeAppRole::kSettings},
    {"闹钟", HomeAppRole::kClock},
    {"天气", HomeAppRole::kWeather},
    {"网易云音乐", HomeAppRole::kMusic},
    {"新闻", HomeAppRole::kNews},
    {"SDK Hello", HomeAppRole::kDiagnostic},
    {"定时器测试", HomeAppRole::kDiagnostic},
    {"音频测试", HomeAppRole::kDiagnostic},
    {"错误处理测试", HomeAppRole::kDiagnostic},
};
static_assert(std::size(kApps) == std::size(kHomeTiles));
static_assert(kApps[kClockApp].clock);
static_assert(kApps[kMusicApp].music);

constexpr const char* AppStateName(host::AppState state) {
    constexpr const char* kStates[] = {"stopped", "starting", "running", "suspended", "stopping"};
    return kStates[static_cast<unsigned>(state)];
}

enum class Action : uint8_t { kList, kStatus, kPerf, kRun, kStop, kTime, kHome, kBack, kScreenshot };
struct Command final {
    Action action;
    uint32_t app;
    uint64_t utc;
    GuestLaunchOptions options;
};
QueueHandle_t command_queue;

void Shell(int argc, char** argv) {
    const std::string_view action = argc > 1 ? argv[1] : "";
    Command command{};
    if (action == "list" && argc == 2) {
        command.action = Action::kList;
    } else if (action == "home" && argc == 2) {
        command.action = Action::kHome;
    } else if (action == "back" && argc == 2) {
        command.action = Action::kBack;
    } else if (action == "status" && argc == 2) {
        command.action = Action::kStatus;
    } else if (action == "perf" && argc == 2) {
        command.action = Action::kPerf;
    } else if (action == "screenshot" && argc == 2) {
        command.action = Action::kScreenshot;
    } else if (action == "time" && argc == 3) {
        command.action = Action::kTime;
        const std::string_view value(argv[2]);
        const auto end = value.data() + value.size();
        const auto parsed = std::from_chars(value.data(), end, command.utc);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            std::puts("Expected UTC epoch seconds");
            return;
        }
    } else if ((action == "run" && (argc == 3 || argc == 4)) || (action == "stop" && argc == 3)) {
        command.action = action == "run" ? Action::kRun : Action::kStop;
        while (command.app < std::size(kApps) && std::string_view(argv[2]) != kApps[command.app].name) ++command.app;
        if (command.app == std::size(kApps)) {
            std::puts("Unknown MicroPixel app");
            return;
        }
        if (argc == 4) {
            std::string_view option = argv[3];
            command.options.alarm_test = kApps[command.app].clock && option == "--alarm-test";
            command.options.music_login = kApps[command.app].music && option == "--login";
            if (kApps[command.app].music && option.starts_with("--song=")) {
                option.remove_prefix(sizeof("--song=") - 1);
                const auto end = option.data() + option.size();
                const auto parsed = std::from_chars(option.data(), end, command.options.song);
                if (parsed.ec != std::errc{} || parsed.ptr != end || command.options.song > 9007199254740991ULL)
                    command.options.song = 0;
            }
            if (!command.options.alarm_test && !command.options.song && !command.options.music_login) {
                std::puts("Unknown launch argument");
                return;
            }
        }
    } else {
        std::puts(
            "Usage: mpx list | mpx status | mpx perf | mpx home | mpx back | mpx screenshot | mpx time <utc-seconds> | "
            "mpx run "
            "<app> "
            "[--alarm-test|--song=ID|--login] | mpx "
            "stop <app>");
        return;
    }
    if (xQueueSend(command_queue, &command, 0) != pdTRUE) std::puts("MicroPixel command queue is full");
}

SHELL_CMD_EXPORT_ALIAS(Shell, mpx, MicroPixel app catalog and lifecycle);

class FirmwareApp final {
   public:
    bool Initialize() {
        command_queue = xQueueCreate(8, sizeof(Command));
        if (command_queue == nullptr || board_shared_i2c_init() != 0 || !InitializeGuestRuntime()) return false;
        if (!InitializeOrionFont()) std::puts("Orion expanded font unavailable");
        GuestPanel::SetFontFallback(OrionFontFallback());
        if (audio_.Initialize(output_, output_.SampleRate()) != ESP_OK) return false;
#if defined(CONFIG_FHOST)
        if (!InitializeNetwork())
            std::puts("MicroPixel HTTPS worker initialization failed");
        else if (!InitializeMedia(audio_, output_.SampleRate()))
            std::puts("MicroPixel URL media initialization failed");
#endif
        if (!alarms_.Initialize(kClockApp)) return false;
        media_widgets_ = MediaWidgets::Create(kMusicApp, "micropixel.netease-music", boot_);
        if (!media_widgets_) return false;
        for (auto& runner : runners_)
            if (!runner.Initialize(*media_widgets_)) return false;
        for (uint32_t i = 0; i < std::size(kApps); ++i) {
            if (!manager_.Register(i, {.background = kApps[i].KeepService() || !kApps[i].activity,
                                       .activity = kApps[i].activity,
                                       .messages = kApps[i].weather}))
                return false;
        }
        return xTaskCreatePinnedToCore(Controller, "mpx_host", kControllerStackBytes, this, task_policy::kHostPriority,
                                       nullptr, task_policy::kSystemCore) == pdPASS;
    }

   private:
    static void Controller(void* self) { static_cast<FirmwareApp*>(self)->Run(); }

    void Run() {
        const auto start_us = esp_timer_get_time();
        shell_init_with_task(bflb_device_get_by_name("uart0"));
        std::puts("MicroPixel Orion Host ready. Type mpx list.");
        const bool has_display = ConfigureHome();
        if (has_display)
            std::printf("MPX BOOT home_setup_us=%lu\n", static_cast<unsigned long>(esp_timer_get_time() - start_us));
        for (;;) {
            SyncNetworkClock();
            const auto now_ms = static_cast<uint64_t>(esp_timer_get_time()) / 1000U;
            mpx_wireless_poll(static_cast<uint32_t>(now_ms));
            Command command{};
            if (xQueueReceive(command_queue, &command, pdMS_TO_TICKS(20)) == pdTRUE)
                HandleCommand(command, has_display);
            UpdateSessions(now_ms);
            if (has_display) Draw(now_ms);
        }
    }

    // Keep command handling, compositing and the LVGL refresh on separate
    // stack frames under LTO. They run serially on the controller task.
    [[gnu::noinline]] bool ConfigureHome() {
        const bool has_display = home_.Initialize({
            .context = this,
            .launch = [](void* context, uint32_t app) { static_cast<FirmwareApp*>(context)->OpenFromHome(app); },
            .home = [](void* context) { static_cast<FirmwareApp*>(context)->ShowHome(); },
            .back = [](void* context) { static_cast<FirmwareApp*>(context)->NavigateBack(); },
            .touch =
                [](void* context, int32_t x, int32_t y, bool pressed) {
                    if (auto* runner = static_cast<FirmwareApp*>(context)->Foreground()) runner->Touch(x, y, pressed);
                },
            .submit_text =
                [](void* context, const char* text) {
                    auto* runner = static_cast<FirmwareApp*>(context)->Foreground();
                    return runner && runner->SubmitText(text);
                },
            .volume =
                [](void* context, uint8_t percent) {
                    static_cast<FirmwareApp*>(context)->audio_.SetMasterVolumePercent(percent);
                },
            .media_action =
                [](void* context, const host::WidgetSnapshot& snapshot, uint16_t type, uint32_t value) {
                    static_cast<FirmwareApp*>(context)->media_widgets_->Action(snapshot, type, value);
                },
        });
        if (has_display) {
            for (uint32_t index = 0; index < std::size(kApps); ++index) {
                home_.AddApp(index, kHomeTiles[index].title, kHomeTiles[index].role);
            }
        }
        return has_display;
    }

    [[gnu::noinline]] void HandleCommand(const Command& command, bool has_display) {
        switch (command.action) {
            case Action::kList:
                List();
                break;
            case Action::kStatus:
                std::printf("Orion LCD frames=%lu, PCM signal writes=%lu, host stack min free=%lu bytes\n",
                            static_cast<unsigned long>(home_.Frames()),
                            static_cast<unsigned long>(output_.NonzeroWrites()),
                            static_cast<unsigned long>(uxTaskGetStackHighWaterMark2(nullptr)));
                PrintMusicStatus();
                break;
            case Action::kPerf:
                PrintPerformance();
                break;
            case Action::kScreenshot:
                if (!has_display || !home_.CaptureFrame()) std::puts("MPXFB capture failed");
                break;
            case Action::kRun:
                QueueLaunch(command.app, command.options);
                break;
            case Action::kStop:
                if (launch_ && launch_->app == command.app) launch_.reset();
                ReportCommand(manager_.Stop(command.app));
                break;
            case Action::kHome:
                ShowHome();
                break;
            case Action::kBack:
                NavigateBack();
                break;
            case Action::kTime:
                if (alarms_.SetUtc(command.utc, esp_timer_get_time() / 1000U)) {
                    std::puts("MicroPixel UTC set");
                } else {
                    std::puts("MicroPixel UTC out of range");
                }
                break;
        }
    }

    [[gnu::noinline]] void UpdateSessions(uint64_t now_ms) {
        ProcessLaunch();
        manager_.Process(now_ms);
        KeepWeatherRunning(now_ms);
        for (auto& runner : runners_) runner.SetLaunchOptions({});
        alarms_.Process(now_ms, audio_);
    }

    [[gnu::noinline]] void Draw(uint64_t now_ms) {
        auto* foreground = Foreground();
        if (auto* surface = home_.GuestSurface(foreground != nullptr)) foreground->Panel().Render(surface);
        home_.ShowKeyboard(foreground && foreground->WantsText());
        micropixel_wall_time_t wall{};
        uint64_t next_alarm;
        alarms_.ReadClock(now_ms, kClockApp, wall, next_alarm);
        home_.Update(wall, next_alarm);
        home_.UpdateWeather(weather_state_);
        home_.UpdateMusic(*media_widgets_);
        home_.Refresh();
    }

    struct Launch final {
        uint32_t app;
        GuestLaunchOptions options;
    };

    void OpenFromHome(uint32_t app) {
        std::printf("Orion Home open %s\n", kApps[app].name);
        QueueLaunch(app, {});
    }

    void QueueLaunch(uint32_t app, GuestLaunchOptions options) {
        launch_ = Launch{app, options};
        home_.LaunchStatus("正在打开应用");
    }

    void ShowHome() {
        launch_.reset();
        home_.LaunchStatus("");
        ReportCommand(manager_.ShowHome());
    }

    void NavigateBack() {
        if (auto* runner = Foreground(); runner && runner->NavigateBack()) std::puts("Orion Back queued");
    }

    GuestRunner* Foreground() {
        const auto& snapshot = manager_.Snapshot();
        return snapshot.foreground < 0 ? nullptr : &runners_[snapshot.apps[snapshot.foreground].instance.slot];
    }

    void KeepWeatherRunning(uint64_t now_ms) {
        const auto& weather = manager_.Snapshot().apps[kWeatherApp];
        if (launch_ || now_ms < weather_retry_ms_ || weather.user_stopped || weather.state != host::AppState::kStopped)
            return;
        weather_retry_ms_ = now_ms + 5000;
        (void)manager_.EnsureWork(kWeatherApp);
    }

    void SyncNetworkClock() {
#if defined(CONFIG_FHOST)
        mpx_network_time_sync_t sync{};
        if (mpx_network_time_take_sync(&sync) &&
            alarms_.SetUtc(sync.seconds, sync.monotonic_ms, sync.microseconds / 1000U))
            std::printf("MicroPixel wall clock synchronized UTC=%lu\n", static_cast<unsigned long>(sync.seconds));
#endif
    }

    void ProcessLaunch() {
        if (!launch_) return;
        const auto request = *launch_;
        const auto& snapshot = manager_.Snapshot();
        if (snapshot.apps[request.app].state == host::AppState::kStopping) return;
        const auto result = manager_.Activate(request.app);
        if (result == ManagerResult::kBusy) return;
        launch_.reset();
        for (auto& runner : runners_) {
            runner.SetLaunchOptions(result == ManagerResult::kAccepted ? request.options : GuestLaunchOptions{});
        }
        home_.LaunchStatus(result == ManagerResult::kAccepted     ? ""
                           : result == ManagerResult::kNoCapacity ? "应用槽已满"
                                                                  : "无法打开应用");
        ReportCommand(result);
    }

    static void ReportCommand(ManagerResult result) {
        switch (result) {
            case ManagerResult::kAccepted:
                break;
            case ManagerResult::kNoCapacity:
                std::puts("MicroPixel Guest slot in use");
                break;
            default:
                std::printf("MicroPixel command rejected (%u)\n", static_cast<unsigned>(result));
                break;
        }
    }

    void List() const {
        const auto& snapshot = manager_.Snapshot();
        for (uint32_t i = 0; i < snapshot.count; ++i) {
            const auto& app = snapshot.apps[i];
            std::printf("%s %s exit=%ld\n", kApps[i].name, AppStateName(app.state), static_cast<long>(app.result));
        }
    }

    [[gnu::noinline]] void PrintMusicStatus() {
        host::WidgetSnapshot snapshot{};
        bool pending{};
        if (!media_widgets_->Read(snapshot, pending)) return;
        const auto& media = snapshot.data;
        std::printf("Orion Music widget online=%u playing=%u track=%u position_ms=%lu pending=%u\n",
                    unsigned(snapshot.online), unsigned(snapshot.online && (media.flags & MICROPIXEL_WIDGET_PLAYING)),
                    unsigned(media.value != 0 || media.auxiliary != 0), static_cast<unsigned long>(media.position_ms),
                    unsigned(pending));
    }

    [[gnu::noinline]] static void PrintPerformance() {
        const auto size = [](const char& start, const char& end) {
            return static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&end) - reinterpret_cast<uintptr_t>(&start));
        };
        const auto flash_text = size(__rom_text_start__, __rom_text_end__);
#if defined(CONFIG_PSRAM_COPY_CODE)
        const auto psram_text = size(__psram_text_start__, __psram_text_end__);
#else
        const unsigned long psram_text = 0;
#endif
        std::printf("MPX TEXT flash=%lu psram=%lu\n", flash_text, psram_text);
        const auto heap = [](const char* name, uint32_t id) {
            std::printf("MPX HEAP %s free=%lu largest=%lu\n", name, static_cast<unsigned long>(kfree_size(id)),
                        static_cast<unsigned long>(kmax_free_size(id)));
        };
        heap("OCRAM", MM_HEAP_OCRAM_0);
        heap("WRAM", MM_HEAP_WRAM_0);
        heap("PSRAM", MM_HEAP_PSRAM_0);

        struct Snapshot {
            TaskStatus_t task[40];
            char names[40][configMAX_TASK_NAME_LEN];
        };
        auto* snapshot = static_cast<Snapshot*>(heap_caps_malloc(sizeof(Snapshot), MALLOC_CAP_SPIRAM));
        if (snapshot == nullptr) return;
        vTaskSuspendAll();
        const auto count = uxTaskGetSystemState(snapshot->task, std::size(snapshot->task), nullptr);
        for (size_t i = 0; i < count; ++i)
            std::memcpy(snapshot->names[i], snapshot->task[i].pcTaskName, configMAX_TASK_NAME_LEN);
        (void)xTaskResumeAll();
        for (size_t i = 0; i < count; ++i) {
            const auto& task = snapshot->task[i];
            const auto stack = reinterpret_cast<uintptr_t>(task.pxStackBase);
            const auto wram = reinterpret_cast<uintptr_t>(&__ram_wifi_start__);
            const char* bank = stack >= CONFIG_PSRAM_ADDRESS && stack - CONFIG_PSRAM_ADDRESS < CONFIG_PSRAM_LENGTH
                                   ? "PSRAM"
                               : stack >= wram && stack - wram < CONFIG_WRAM_FOR_AP_SIZE ? "WRAM"
                                                                                         : "OCRAM";
            std::printf("MPX TASK %s id=%lu pri=%lu free=%lu bank=%s\n", snapshot->names[i],
                        static_cast<unsigned long>(task.xTaskNumber),
                        static_cast<unsigned long>(task.uxCurrentPriority),
                        static_cast<unsigned long>(task.usStackHighWaterMark) * sizeof(StackType_t), bank);
        }
        heap_caps_free(snapshot);
        std::puts("MPX PERF end");
    }

    inline static OrionAudioOutput output_;
    platform::audio::AudioEngine audio_;
    std::array<GuestPanel, AppManager::kMaxInstances> panels_;
    AlarmHost alarms_;
    WeatherState weather_state_;
    MediaWidgets* media_widgets_{};
    static uint32_t SignalWrites() { return output_.NonzeroWrites(); }
    std::array<GuestRunner, AppManager::kMaxInstances> runners_{{
        {kApps, audio_, panels_[0], alarms_, weather_state_, SignalWrites},
        {kApps, audio_, panels_[1], alarms_, weather_state_, SignalWrites},
        {kApps, audio_, panels_[2], alarms_, weather_state_, SignalWrites},
        {kApps, audio_, panels_[3], alarms_, weather_state_, SignalWrites},
    }};
    OrionHome home_;
    std::optional<Launch> launch_;
    uint64_t weather_retry_ms_{};
    runtime::SessionRunner* slots_[AppManager::kMaxInstances]{&runners_[0], &runners_[1], &runners_[2], &runners_[3]};
    const uint64_t boot_{static_cast<uint64_t>(esp_timer_get_time()) | 1U};
    AppManager manager_{slots_, boot_};
};

}  // namespace
}  // namespace micropixel::bouffalo

int main() {
    const auto start_us = esp_timer_get_time();
    mpx_wireless_init();
    static micropixel::bouffalo::FirmwareApp app;
    if (app.Initialize()) {
        std::printf("MPX BOOT host_setup_us=%lu sysinit_stack_free=%lu\n",
                    static_cast<unsigned long>(esp_timer_get_time() - start_us),
                    static_cast<unsigned long>(uxTaskGetStackHighWaterMark2(nullptr)));
        return 0;
    }
    std::puts("Unable to start MicroPixel Orion Host");
    return 1;
}
