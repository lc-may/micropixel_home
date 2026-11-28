// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>

#include "audio/orion_audio_output.hpp"
#include "bflb_core.h"
#include "bflb_mtimer.h"
#include "display/orion_font.hpp"
#include "host/controller/app_manager.hpp"
#include "host/controller/media_widgets.hpp"
#include "host/network/network.hpp"
#include "platform/audio/audio_engine.hpp"
#include "platform/audio/media.hpp"
#include "platform/network/network_time.h"
#include "platform/storage/file_store.h"
#include "platform/wireless/classic_bluetooth.h"
#include "platform/wireless/wireless.h"
#include "runtime/abi/alarm_endpoint.hpp"
#include "runtime/abi/weather_endpoint.hpp"
#include "runtime/graphics/guest_panel.hpp"
#include "runtime/guest_runner.hpp"
#include "runtime/wamr/guest_runtime.hpp"
#include "ui/orion_home.hpp"
#include "work/task_policy.hpp"
#include "work/task_stack.h"

#if WASM_ENABLE_AOT
#include "runtime/bundle/app_store.hpp"
#else
#include "audio_pcm_stream.h"
#include "main_failure.h"
#if defined(CONFIG_BT_A2DP)
#include "mpx_guest_bluetooth-speaker.h"
#endif
#include "mpx_guest_clock-alarm.h"
#include "mpx_guest_netease-music.h"
#include "mpx_guest_news.h"
#include "mpx_guest_settings.h"
#include "mpx_guest_weather.h"
#include "sdk_hello.h"
#include "timer_counter.h"
#endif

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
constexpr uint8_t kSettingsGrowthPages = 2;
constexpr uint8_t kClockGrowthPages = 18;
constexpr uint8_t kAtlasAppGrowthPages = 12;
constexpr uint8_t kMusicGrowthPages = 15;
#if WASM_ENABLE_AOT
#define ORION_CATALOG_IMAGE(symbol) .wasm = {}, .unpacked_size = 0
#else
#define ORION_CATALOG_IMAGE(symbol) .wasm = symbol, .unpacked_size = symbol##_wasm_size
#endif
constexpr EmbeddedApp kApps[] = {
    {.name = "settings",
     ORION_CATALOG_IMAGE(settings),
     .activity = true,
     .wireless_settings = true,
     .linear_growth_pages = kSettingsGrowthPages,
     .bundle_id = "micropixel.settings"},
    {.name = "clock-alarm",
     ORION_CATALOG_IMAGE(clock_alarm),
     .activity = true,
     .component_flags = kActivityAndService,
     .clock = true,
     .linear_growth_pages = kClockGrowthPages,
     .bundle_id = MPX_CLOCK_APP_ID},
    {.name = "weather",
     ORION_CATALOG_IMAGE(weather),
     .activity = true,
     .component_flags = kActivityAndService,
     .weather = true,
     .linear_growth_pages = kAtlasAppGrowthPages,
     .bundle_id = "micropixel.weather"},
    {.name = "netease-music",
     ORION_CATALOG_IMAGE(netease_music),
     .activity = true,
     .component_flags = kActivityAndService,
     .music = true,
     .linear_growth_pages = kMusicGrowthPages,
     .bundle_id = "micropixel.netease-music"},
    {.name = "news",
     ORION_CATALOG_IMAGE(news),
     .activity = true,
     .component_flags = kActivity,
     .linear_growth_pages = kAtlasAppGrowthPages,
     .bundle_id = "micropixel.news"},
#if defined(CONFIG_BT_A2DP)
    {.name = "bluetooth-speaker",
     ORION_CATALOG_IMAGE(bluetooth_speaker),
     .activity = true,
     .component_flags = kActivityAndService | MICROPIXEL_COMPONENT_FLAG_BACKGROUND,
     .bluetooth_speaker = true,
     .linear_growth_pages = kAtlasAppGrowthPages,
     .bundle_id = "micropixel.bluetooth-speaker"},
#endif
    {.name = "sdk_hello", ORION_CATALOG_IMAGE(sdk_hello), .bundle_id = "micropixel.sdk_hello"},
    {.name = "timer_counter", ORION_CATALOG_IMAGE(timer_counter), .bundle_id = "micropixel.timer_counter"},
    {.name = "audio_pcm_stream", ORION_CATALOG_IMAGE(audio_pcm_stream), .bundle_id = "micropixel.audio_pcm_stream"},
    {.name = "main_failure", ORION_CATALOG_IMAGE(main_failure), .bundle_id = "micropixel.main_failure"},
};
#undef ORION_CATALOG_IMAGE

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
#if defined(CONFIG_BT_A2DP)
    {"蓝牙音箱", HomeAppRole::kDiagnostic},
#endif
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

enum class Action : uint8_t {
    kList,
    kStatus,
    kPerf,
    kRun,
    kStop,
    kTime,
    kHome,
    kBack,
    kScreenshot,
    kBundleBegin,
    kBundleChunk,
    kBundleReady,
    kBundleApply,
    kBundleCancel,
    kBundleRemove,
    kBundleList
};
struct Command final {
    Action action;
    char id[65];
    uint64_t utc;
    uint32_t offset, size;
    std::array<uint8_t, 44> bytes;
    GuestLaunchOptions options;
};
QueueHandle_t command_queue;

template <typename T>
bool ParseNumber(std::string_view text, T& result, int base = 10) {
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, result, base);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool CopyId(std::string_view id, Command& command) {
    if (id.empty() || id.size() >= sizeof(command.id)) return false;
    std::memcpy(command.id, id.data(), id.size());
    return true;
}

#if WASM_ENABLE_AOT
bool ParseBundleCommand(int argc, char** argv, Command& command) {
    if (argc < 3) return false;
    const std::string_view operation = argv[2];
    if (operation == "begin" && argc == 5) {
        command.action = Action::kBundleBegin;
        return ParseNumber(argv[3], command.offset) && ParseNumber(argv[4], command.size);
    }
    if (operation == "chunk" && argc == 5) {
        command.action = Action::kBundleChunk;
        const std::string_view bytes = argv[4];
        if (bytes.empty() || bytes.size() % 2 != 0 || bytes.size() / 2 > command.bytes.size() ||
            !ParseNumber(argv[3], command.offset))
            return false;
        command.size = bytes.size() / 2;
        for (uint32_t i = 0; i < command.size; ++i) {
            unsigned number{};
            if (!ParseNumber(bytes.substr(2 * i, 2), number, 16)) return false;
            command.bytes[i] = number;
        }
        return true;
    }
    if (operation == "remove" && argc == 4) {
        command.action = Action::kBundleRemove;
        return CopyId(argv[3], command);
    }
    if (argc != 3) return false;
    if (operation == "ready")
        command.action = Action::kBundleReady;
    else if (operation == "apply")
        command.action = Action::kBundleApply;
    else if (operation == "cancel")
        command.action = Action::kBundleCancel;
    else if (operation == "list")
        command.action = Action::kBundleList;
    else
        return false;
    return true;
}
#endif

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
#if WASM_ENABLE_AOT
    } else if (action == "bundle") {
        if (!ParseBundleCommand(argc, argv, command)) {
            std::puts("MPX STORE syntax invalid");
            return;
        }
#endif
    } else if ((action == "run" && (argc == 3 || argc == 4)) || (action == "stop" && argc == 3)) {
        command.action = action == "run" ? Action::kRun : Action::kStop;
        if (!CopyId(argv[2], command)) return;
        if (argc == 4) {
            std::string_view option = argv[3];
            command.options.alarm_test = option == "--alarm-test";
            command.options.music_login = option == "--login";
            if (option.starts_with("--song=")) {
                if (!ParseNumber(option.substr(sizeof("--song=") - 1), command.options.song) ||
                    command.options.song > 9007199254740991ULL)
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
            "stop <app> | mpx bundle <list|begin|chunk|ready|apply|cancel|remove>");
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
#if WASM_ENABLE_AOT
        static_assert(kCatalogSlots <= AppManager::kMaxApps);
        auto* storage = kmalloc(sizeof(Records), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE);
        if (!storage) return false;
        records_ = std::construct_at(static_cast<Records*>(storage));
#endif
        if (!InitializeOrionFont()) std::puts("Orion expanded font unavailable");
        GuestPanel::SetFontFallback(OrionFontFallback());
        if (audio_.Initialize(output_, output_.SampleRate()) != micropixel::platform::audio::kAudioOk) return false;
#if defined(CONFIG_BT_A2DP)
        if (!classic::Initialize(audio_, output_.SampleRate()))
            std::puts("MicroPixel Bluetooth Classic audio unavailable");
#endif
#if defined(CONFIG_FHOST)
        if (!InitializeNetwork())
            std::puts("MicroPixel HTTPS worker initialization failed");
        else if (!InitializeMedia(audio_, output_.SampleRate()))
            std::puts("MicroPixel URL media initialization failed");
#endif
#if WASM_ENABLE_AOT
        (void)store_.Mount();
#endif
#if defined(CONFIG_LITTLEFS)
        if (!mpx_app_data_migrate()) std::puts("MPX DATA migration incomplete");
#if !WASM_ENABLE_AOT
        for (const auto& app : apps_) EnsureAppData(app.bundle_id);
#endif
#endif
        if (!alarms_.Initialize(kClockApp)) return false;
        media_widgets_ = MediaWidgets::Create(kMusicApp, "micropixel.netease-music", boot_);
        if (!media_widgets_) return false;
        for (auto& runner : runners_)
            if (!runner.Initialize(*media_widgets_)) return false;
        for (uint32_t i = 0; i < apps_.size(); ++i) {
            if (!Register(i)) return false;
        }
        return mpx_create_task(Controller, "mpx_host", kControllerStackBytes, this, task_policy::kHostPriority,
                               nullptr) == pdPASS;
    }

   private:
    static void Controller(void* self) { static_cast<FirmwareApp*>(self)->Run(); }

    void Run() {
        const auto start_us = bflb_mtimer_get_time_us();
        shell_init_with_task(bflb_device_get_by_name("uart0"));
#if WASM_ENABLE_AOT
        if (!store_.Visit(
                [](void* context, const AppRecord& app) {
                    auto& self = *static_cast<FirmwareApp*>(context);
                    if (self.AppCount() >= kMaxInstalledApps) return false;
                    const auto index = self.InstallIndex(app.id.data());
                    return index && self.SetInstalled(*index, app);
                },
                this))
            std::puts("MPX STORE mount failed");
        for (const auto& app : apps_)
            if (app.Available()) EnsureAppData(app.bundle_id);
#endif
        std::puts("MicroPixel Orion Host ready. Type mpx list.");
        const bool has_display = ConfigureHome();
        if (has_display)
            std::printf("MPX BOOT home_setup_us=%lu\n",
                        static_cast<unsigned long>(bflb_mtimer_get_time_us() - start_us));
        for (;;) {
            SyncNetworkClock();
            const auto now_ms = bflb_mtimer_get_time_us() / 1000U;
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
#if WASM_ENABLE_AOT
            .manage = [](void* context, ManageAction action,
                         uint32_t app) { static_cast<FirmwareApp*>(context)->Manage(action, app); },
#endif
            .media_action =
                [](void* context, const host::WidgetSnapshot& snapshot, uint16_t type, uint32_t value) {
                    static_cast<FirmwareApp*>(context)->media_widgets_->Action(snapshot, type, value);
                },
        });
        if (has_display) RefreshCatalog();
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
                            static_cast<unsigned long>(mpx_stack_free_bytes(nullptr)));
                PrintMusicStatus();
                break;
            case Action::kPerf:
                PrintPerformance();
                break;
            case Action::kScreenshot:
                if (!has_display || !home_.CaptureFrame()) std::puts("MPXFB capture failed");
                break;
            case Action::kRun:
            case Action::kStop: {
                const auto app = Find(command.id);
                if (!app) {
                    std::puts("Unknown MicroPixel app");
                } else if (command.action == Action::kRun) {
                    if ((command.options.alarm_test && !apps_[*app].clock) ||
                        ((command.options.song || command.options.music_login) && !apps_[*app].music)) {
                        std::puts("Unknown launch argument");
                    } else {
                        QueueLaunch(*app, command.options);
                    }
                } else {
                    if (launch_ && launch_->app == *app) launch_.reset();
                    ReportCommand(manager_.Stop(*app));
                }
                break;
            }
            case Action::kHome:
                ShowHome();
                break;
            case Action::kBack:
                NavigateBack();
                break;
            case Action::kTime:
                if (alarms_.SetUtc(command.utc, bflb_mtimer_get_time_us() / 1000U)) {
                    std::puts("MicroPixel UTC set");
                } else {
                    std::puts("MicroPixel UTC out of range");
                }
                break;
            default:
#if WASM_ENABLE_AOT
                HandleBundleCommand(command);
#endif
                break;
        }
    }

    [[gnu::noinline]] void UpdateSessions(uint64_t now_ms) {
        ProcessLaunch();
        manager_.Process(now_ms);
#if WASM_ENABLE_AOT
        UpdateStore(now_ms);
#endif
        KeepWeatherRunning(now_ms);
        for (auto& runner : runners_) runner.SetLaunchOptions({});
        if (apps_[kClockApp].Available()) alarms_.Process(now_ms, audio_);
    }

    [[gnu::noinline]] void Draw(uint64_t now_ms) {
        auto* foreground = Foreground();
        if (auto* surface = home_.GuestSurface(foreground != nullptr)) foreground->Panel().Render(surface);
        home_.ShowKeyboard(foreground && foreground->WantsText());
        home_.Update(alarms_.ReadClock(now_ms));
        home_.UpdateWeather(weather_state_);
        home_.UpdateMusic(*media_widgets_);
        home_.Refresh();
    }

    struct Launch final {
        uint32_t app;
        GuestLaunchOptions options;
    };

    void OpenFromHome(uint32_t app) {
        std::printf("Orion Home open %s\n", apps_[app].name);
        QueueLaunch(app, {});
    }

    void QueueLaunch(uint32_t app, GuestLaunchOptions options) {
#if WASM_ENABLE_AOT
        if (mutation_ && mutation_->app == app) return;
#endif
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
#if WASM_ENABLE_AOT
        if (mutation_ && mutation_->app == kWeatherApp) return;
#endif
        if (!apps_[kWeatherApp].Available() || launch_ || now_ms < weather_retry_ms_ || weather.user_stopped ||
            weather.state != host::AppState::kStopped)
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
            if (!apps_[i].Available()) continue;
            const auto& app = snapshot.apps[i];
            std::printf("%s %s exit=%ld", apps_[i].name, AppStateName(app.state), static_cast<long>(app.result));
#if WASM_ENABLE_AOT
            std::printf(" id=%s version=%s", apps_[i].bundle_id, (*records_)[i].version.data());
#endif
            std::puts("");
        }
    }

    bool Register(uint32_t index) {
        const auto& app = apps_[index];
        return manager_.Register(index, {.background = app.KeepService() || !app.activity,
                                         .activity = app.activity,
                                         .messages = app.weather,
                                         .available = app.Available()});
    }

#if defined(CONFIG_LITTLEFS)
    static void EnsureAppData(const char* id) {
        if (mpx_app_data_ensure(id) != 0) std::printf("MPX DATA directory unavailable for %s\n", id);
    }
#endif

    std::optional<uint32_t> Find(std::string_view id, bool aliases = true) const {
        for (uint32_t i = 0; i < apps_.size(); ++i)
            if (apps_[i].bundle_id && id == apps_[i].bundle_id) return i;
        if (aliases) {
            for (uint32_t i = 0; i < apps_.size(); ++i)
                if (apps_[i].Available() && id == apps_[i].name) return i;
        }
        return std::nullopt;
    }

    void RefreshCatalog() {
        home_.ClearApps();
        for (uint32_t i = 0; i < apps_.size(); ++i) {
            if (!apps_[i].Available()) continue;
            auto role = i < std::size(kHomeTiles) ? kHomeTiles[i].role : HomeAppRole::kDiagnostic;
#if WASM_ENABLE_AOT
            const auto& app = (*records_)[i];
            const std::string_view id = app.id.data();
            if (id == "micropixel.light-sequence")
                role = HomeAppRole::kLightSequence;
            else if (id == "micropixel.central-ac")
                role = HomeAppRole::kCentralAc;
            home_.AddApp(i, app.title.data(), role);
            home_.AddManagedApp(i, app.title.data(), app.version.data());
#else
            home_.AddApp(i, kHomeTiles[i].title, role);
#endif
        }
#if WASM_ENABLE_AOT
        home_.ManageSpace(AppCount(), kMaxInstalledApps, store_.FreeBytes());
        if (!store_.available()) home_.ManageStatus("应用存储不可用", false, false);
#endif
    }

#if WASM_ENABLE_AOT
    uint32_t AppCount() const {
        return std::count_if(apps_.begin(), apps_.end(), [](const auto& app) { return app.Available(); });
    }

    std::optional<uint32_t> InstallIndex(std::string_view id) const {
        if (const auto existing = Find(id, false)) return existing;
        auto short_id = id;
        if (short_id.starts_with("micropixel.")) {
            short_id.remove_prefix(sizeof("micropixel.") - 1);
            for (uint32_t i = 0; i < std::size(kApps); ++i)
                if (short_id == kApps[i].name) return i;
        }
        for (uint32_t i = std::size(kApps); i < apps_.size(); ++i)
            if (!apps_[i].Available()) return i;
        return std::nullopt;
    }

    bool SetInstalled(uint32_t index, const AppRecord& app) {
        const bool existed = apps_[index].Available();
        auto& record = (*records_)[index];
        record = app;
        auto& target = apps_[index];
        target = index < std::size(kApps)
                     ? kApps[index]
                     : EmbeddedApp{
                           .name = record.id.data(),
                           .activity = record.component_flags == 0 ||
                                       (record.component_flags & MICROPIXEL_COMPONENT_FLAG_ACTIVITY) != 0,
                           .component_flags = record.component_flags,
                           .linear_growth_pages = 12,
                       };
        target.bundle_id = record.id.data();
        if (!existed) manager_.RestoreUserStopped(index, false);
        return Register(index);
    }

    void StoreReply(const char* operation, StoreResult result) {
        std::printf("MPX STORE %s %s\n", operation, StoreResultName(result));
    }

    void StoreStatus(StoreResult result, const char* success) {
        const char* message = success;
        switch (result) {
            case StoreResult::kOk:
                break;
            case StoreResult::kUnavailable:
                message = "应用存储不可用";
                break;
            case StoreResult::kInvalidPackage:
                message = "应用包无效或目标不匹配";
                break;
            case StoreResult::kNoSpace:
                message = "空间不足";
                break;
            case StoreResult::kIo:
                message = "应用存储操作失败";
                break;
            case StoreResult::kBusy:
                message = "应用管理正在处理操作";
                break;
            case StoreResult::kVersion:
                message = "升级需要更高的版本";
                break;
            case StoreResult::kFull:
                message = "已达到应用数量上限";
                break;
            case StoreResult::kNotFound:
                message = "应用未安装";
                break;
        }
        home_.ManageStatus(message, false, false);
    }

    StoreResult StageRemove(uint32_t index) {
        if (mutation_ || store_.Receiving() || store_.Pending() || remove_) return StoreResult::kBusy;
        if (!apps_[index].Available()) return StoreResult::kNotFound;
        remove_ = index;
        char text[128];
        std::snprintf(text, sizeof(text), "卸载 %s？", (*records_)[index].title.data());
        home_.ManageStatus(text, true, true);
        return StoreResult::kOk;
    }

    StoreResult StageInstall() {
        const auto result = store_.Prepare();
        if (result != StoreResult::kOk) return result;
        const auto& candidate = *store_.Pending();
        const auto existing = Find(candidate.id.data(), false);
        if (existing) {
            const auto& current = (*records_)[*existing];
            if (!micropixel_app_version_update(current.version.data(), candidate.version.data(), false))
                return StoreResult::kVersion;
        } else if (AppCount() >= kMaxInstalledApps || !InstallIndex(candidate.id.data())) {
            return StoreResult::kFull;
        }
        char message[152];
        std::snprintf(message, sizeof(message), "%s %s  v%s", existing ? "升级" : "安装", candidate.title.data(),
                      candidate.version.data());
        home_.ManageStatus(message, true, true);
        return StoreResult::kOk;
    }

    StoreResult StartMutation() {
        if (mutation_) return StoreResult::kBusy;
        const auto* pending = store_.Pending();
        const auto index = remove_ ? remove_ : pending ? InstallIndex(pending->id.data()) : std::nullopt;
        if (!index) return StoreResult::kInvalidPackage;
        const auto app = *index;
        if (manager_.Snapshot().apps[app].state != host::AppState::kStopped &&
            manager_.Stop(app, false) != ManagerResult::kAccepted)
            return StoreResult::kBusy;
        if (launch_ && launch_->app == app) launch_.reset();
        mutation_ = Mutation{app, bflb_mtimer_get_time_us() / 1000U + 8000U, remove_.has_value()};
        remove_.reset();
        home_.ManageStatus("正在等待应用退出并提交", false, false);
        return StoreResult::kOk;
    }

    void UpdateStore(uint64_t now_ms) {
        if (!mutation_) {
            if (store_.Receiving() && now_ms >= upload_deadline_) {
                store_.Cancel();
                StoreStatus(StoreResult::kInvalidPackage, "");
                std::puts("MPX STORE upload timeout");
            }
            return;
        }
        const auto pending = *mutation_;
        if (manager_.Snapshot().apps[pending.app].state != host::AppState::kStopped) {
            if (now_ms < pending.deadline) return;
            mutation_.reset();
            store_.Cancel();
            home_.ManageStatus("应用退出超时，已保留现有版本", false, false);
            std::puts("MPX STORE apply stop_timeout");
            return;
        }
        StoreResult result;
        if (pending.remove) {
            result = store_.Remove(apps_[pending.app].bundle_id);
            if (result == StoreResult::kOk) {
                (void)manager_.Deactivate(pending.app);
                apps_[pending.app] = {};
                if (pending.app == kWeatherApp) {
                    micropixel_widget_snapshot_t empty{};
                    weather_state_.Update(empty);
                }
            }
        } else {
            const auto candidate = *store_.Pending();
            result = store_.Commit();
            if (result == StoreResult::kOk && !SetInstalled(pending.app, candidate)) result = StoreResult::kIo;
        }
        mutation_.reset();
        store_.Cancel();
        RefreshCatalog();
        StoreStatus(result, pending.remove ? "已卸载" : "应用已安装");
        StoreReply("apply", result);
        std::printf("MPX STORE controller stack min free=%lu bytes\n",
                    static_cast<unsigned long>(mpx_stack_free_bytes(nullptr)));
    }

    void Manage(ManageAction action, uint32_t index) {
        switch (action) {
            case ManageAction::kOpen:
                home_.ManageSpace(AppCount(), kMaxInstalledApps, store_.FreeBytes());
                break;
            case ManageAction::kRemove:
                (void)StageRemove(index);
                break;
            case ManageAction::kConfirm: {
                const auto result = StartMutation();
                if (result == StoreResult::kOk)
                    std::puts("MPX STORE apply pending");
                else
                    StoreReply("apply", result);
                break;
            }
            case ManageAction::kCancel:
                if (mutation_) break;
                remove_.reset();
                store_.Cancel();
                home_.ManageStatus("通过 UART 导入 Bundle 以安装或升级", false, false);
                StoreReply("cancel", StoreResult::kOk);
                break;
        }
    }

    [[gnu::noinline]] void HandleBundleCommand(const Command& command) {
        switch (command.action) {
            case Action::kBundleBegin: {
                const auto result =
                    (mutation_ || remove_) ? StoreResult::kBusy : store_.Begin(command.offset, command.size);
                if (result == StoreResult::kOk) {
                    upload_deadline_ = bflb_mtimer_get_time_us() / 1000U + 15000U;
                    home_.ManageStatus("正在接收应用包", false, true);
                }
                StoreReply("begin", result);
                break;
            }
            case Action::kBundleChunk: {
                const auto result = store_.Append(command.offset, {command.bytes.data(), command.size});
                if (result == StoreResult::kOk) upload_deadline_ = bflb_mtimer_get_time_us() / 1000U + 15000U;
                std::printf("MPX STORE chunk %s %lu\n", StoreResultName(result),
                            static_cast<unsigned long>(command.offset));
                break;
            }
            case Action::kBundleReady: {
                const auto result = mutation_ ? StoreResult::kBusy : StageInstall();
                if (result != StoreResult::kOk && result != StoreResult::kBusy) {
                    store_.Cancel();
                    StoreStatus(result, "");
                }
                StoreReply("ready", result);
                break;
            }
            case Action::kBundleRemove: {
                const auto app = Find(command.id, false);
                StoreReply("remove", app ? StageRemove(*app) : StoreResult::kNotFound);
                break;
            }
            case Action::kBundleApply:
                Manage(ManageAction::kConfirm, 0);
                break;
            case Action::kBundleCancel:
                if (mutation_)
                    StoreReply("cancel", StoreResult::kBusy);
                else
                    Manage(ManageAction::kCancel, 0);
                break;
            case Action::kBundleList:
                for (uint32_t i = 0; i < apps_.size(); ++i) {
                    if (apps_[i].Available())
                        std::printf("MPX APP %s %s %s\n", apps_[i].bundle_id, (*records_)[i].version.data(),
                                    AppStateName(manager_.Snapshot().apps[i].state));
                }
                std::printf("MPX STORE list %s %lu %lu\n", store_.available() ? "ok" : "store_unavailable",
                            static_cast<unsigned long>(AppCount()), static_cast<unsigned long>(store_.FreeBytes()));
                break;
            default:
                break;
        }
    }
#endif

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
        auto* snapshot = static_cast<Snapshot*>(kmalloc(sizeof(Snapshot), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE));
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
        kfree(snapshot);
        std::puts("MPX PERF end");
    }

    inline static OrionAudioOutput output_;
    platform::audio::AudioEngine audio_;
    std::array<GuestPanel, AppManager::kMaxInstances> panels_;
    AlarmHost alarms_;
    WeatherState weather_state_;
    MediaWidgets* media_widgets_{};
    static uint32_t SignalWrites() { return output_.NonzeroWrites(); }
#if WASM_ENABLE_AOT
    static constexpr size_t kCatalogSlots = std::size(kApps) + kMaxInstalledApps;
    using Records = std::array<AppRecord, kCatalogSlots>;
    struct Mutation {
        uint32_t app;
        uint64_t deadline;
        bool remove;
    };
    Records* records_{};
    OrionAppStore store_;
    std::optional<Mutation> mutation_;
    std::optional<uint32_t> remove_;
    uint64_t upload_deadline_{};
    std::array<EmbeddedApp, kCatalogSlots> apps_{};
#else
    std::array<EmbeddedApp, std::size(kApps)> apps_{std::to_array(kApps)};
#endif
    std::array<GuestRunner, AppManager::kMaxInstances> runners_{{
        {apps_, audio_, panels_[0], alarms_, weather_state_, SignalWrites},
        {apps_, audio_, panels_[1], alarms_, weather_state_, SignalWrites},
        {apps_, audio_, panels_[2], alarms_, weather_state_, SignalWrites},
        {apps_, audio_, panels_[3], alarms_, weather_state_, SignalWrites},
    }};
    OrionHome home_;
    std::optional<Launch> launch_;
    uint64_t weather_retry_ms_{};
    runtime::SessionRunner* slots_[AppManager::kMaxInstances]{&runners_[0], &runners_[1], &runners_[2], &runners_[3]};
    const uint64_t boot_{bflb_mtimer_get_time_us() | 1U};
    AppManager manager_{slots_, boot_};
};

}  // namespace
}  // namespace micropixel::bouffalo

int main() {
    const auto start_us = bflb_mtimer_get_time_us();
    constexpr uint32_t general_heap[] = {MM_HEAP_PSRAM_0};
    if (mm_heap_set_any_alloc_order(general_heap, std::size(general_heap)) != 0) return 1;
    mpx_wireless_init();
    static micropixel::bouffalo::FirmwareApp app;
    if (app.Initialize()) {
        std::printf("MPX BOOT host_setup_us=%lu sysinit_stack_free=%lu\n",
                    static_cast<unsigned long>(bflb_mtimer_get_time_us() - start_us),
                    static_cast<unsigned long>(mpx_stack_free_bytes(nullptr)));
        return 0;
    }
    std::puts("Unable to start MicroPixel Orion Host");
    return 1;
}
