// SPDX-License-Identifier: Apache-2.0
#include <cJSON.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "app_sessions.hpp"
#include "esp_lv_adapter.h"
#include "runtime/abi/abi_bridge.h"

namespace {
namespace pc = micropixel::pc;
namespace device = micropixel::device;
namespace host = micropixel::host;
namespace graphics = micropixel::platform::lvgl;
struct Random final : device::Random {
    int32_t GetU32(uint32_t& value) override {
        value = ++next * 3571;
        return 0;
    }
    std::atomic<uint32_t> next{};
};
struct Battery final : device::Battery {
    device::BatterySnapshot Snapshot() override {
        return {.external_power_connected = true, .external_power_available = true};
    }
    void SetStateChangeSink(device::BatteryStateChangeSink, void*) override {}
};
struct Source {
    std::atomic<unsigned> reads{}, completions{}, token{};
    std::atomic<bool> finish{};
    static device::PcmReadResult Read(void* raw, int16_t* samples, uint32_t count) {
        auto& self = *static_cast<Source*>(raw);
        ++self.reads;
        std::fill_n(samples, count, 1000);
        return {count, self.finish.load()};
    }
    static void Complete(void* raw, const device::PcmCompletion& event) {
        auto& self = *static_cast<Source*>(raw);
        self.token = event.token;
        ++self.completions;
    }
    device::PcmSource View() { return {Read, this}; }
};
template <class Predicate>
void Until(Predicate predicate, pc::AppSessions* sessions = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        if (sessions) sessions->Process();
        esp_lv_adapter_lock(-1);
        lv_timer_handler();
        esp_lv_adapter_unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(predicate());
}
void TestRequirements(const char* path) {
    std::vector<uint8_t> bytes;
    assert(pc::ReadFile(path, bytes, 65536));
    auto* root = cJSON_ParseWithLength(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    assert(root);
    auto* requirement = cJSON_GetObjectItem(root, "requirements");
    assert(requirement);
    auto* components = cJSON_GetObjectItem(requirement, "components");
    assert(components);
    auto parse = [&](micropixel_app_requirements_t& output) {
        char* text = cJSON_PrintUnformatted(root);
        assert(text);
        const auto valid =
            micropixel_parse_app_requirements_json(reinterpret_cast<uint8_t*>(text), strlen(text), &output);
        cJSON_free(text);
        return valid;
    };
    micropixel_app_requirements_t value{};
    assert(parse(value) && value.schema_version == 2 && value.widget_count == 0 && value.component_flags == 7);
    micropixel_app_environment_t environment{.core_abi = 131072};
    for (unsigned i = 0; i < MICROPIXEL_APP_SERVICE_COUNT; ++i) environment.services[i] = 65538;
    assert(micropixel_app_runtime_compatible(&value, &environment));
    environment.services[12] = 0;
    assert(!micropixel_app_runtime_compatible(&value, &environment));
    environment.services[12] = 65536;
    cJSON_ReplaceItemInObject(components, "widgets", cJSON_CreateNumber(1));
    assert(parse(value));
    assert(!micropixel_app_runtime_compatible(&value, &environment));
    for (double invalid : {-1.0, 0.5, 3.0}) {
        cJSON_ReplaceItemInObject(components, "widgets", cJSON_CreateNumber(invalid));
        assert(!parse(value));
    }
    cJSON_ReplaceItemInObject(components, "widgets", cJSON_CreateNumber(0));
    cJSON_ReplaceItemInObject(components, "activity", cJSON_CreateNumber(1));
    assert(!parse(value));
    cJSON_ReplaceItemInObject(components, "activity", cJSON_CreateBool(1));
    cJSON_ReplaceItemInObject(requirement, "schema_version", cJSON_CreateNumber(1));
    assert(!parse(value));
    cJSON_DeleteItemFromObject(requirement, "components");
    cJSON_DeleteItemFromObject(cJSON_GetObjectItem(requirement, "services"), "lifecycle");
    assert(parse(value) && value.component_flags == 0);
    assert(micropixel_app_runtime_compatible(&value, &environment));
    cJSON_Delete(root);
    std::puts("PASS requirements v1/v2 parser, admission, unsupported host/widget and malformed declarations");
}
void TestAudio(pc::SdlAudio& physical) {
    pc::ScopedAudio a(physical, 1), b(physical, 2);
    Source first, second;
    device::PcmStreamHandle ha{}, hb{};
    a.BindPcmCompletionSink(Source::Complete, &first);
    b.BindPcmCompletionSink(Source::Complete, &second);
    assert(a.StartPcm(first.View(), 11, 500, ha) == 0);
    assert(b.StartPcm(second.View(), 22, 500, hb) == 0);
    Until([&] { return first.reads > 1 && second.reads > 1; });
    assert(a.StopPcm(hb) == MICROPIXEL_STATUS_NOT_FOUND);
    assert(a.SuspendAll() == 0);
    const auto paused = first.reads.load();
    Until([&] { return second.reads > 10; });
    assert(first.reads == paused);
    assert(a.StopAll() == 0);
    assert(b.SetPcmVolume(hb, 700) == 0);
    assert(a.StopPcm(ha) == MICROPIXEL_STATUS_NOT_FOUND);
    second.finish = true;
    Until([&] { return second.completions == 1; });
    assert(second.token == 22 && first.completions == 0);
    assert(b.StopPcm(hb) == MICROPIXEL_STATUS_NOT_FOUND);
    assert(a.ResumeAll() == 0);
    first.finish = false;
    device::PcmStreamHandle newer{};
    assert(a.StartPcm(first.View(), 33, 500, newer) == 0);
    assert(newer != ha && a.PausePcm(ha) == MICROPIXEL_STATUS_NOT_FOUND);
    assert(a.StopAll() == 0);
    std::puts("PASS scoped audio completion, suspend, stop, foreign and stale handles");
}
void Click(pc::AppSessions& sessions, int x, int y) {
    assert(sessions.Input().InjectTouch({.id = 1, .x = x, .y = y, .phase = device::TouchPhase::kDown}));
    assert(sessions.Input().InjectTouch({.id = 1, .x = x, .y = y, .phase = device::TouchPhase::kUp}));
}
void TestInstances(lv_display_t* display, pc::SdlAudio& audio, const char* path) {
    std::vector<std::unique_ptr<pc::PortableApp>> apps;
    for (unsigned i = 0; i < 5; ++i) {
        auto app = std::make_unique<pc::PortableApp>();
        std::string error;
        assert(app->Load(path, error));
        app->app_id = "test.components." + std::to_string(i);
        apps.push_back(std::move(app));
    }
    Random random;
    Battery battery;
    device::DeviceRegistry registry;
    assert(registry.InitializeStorage() && registry.RegisterDisplay("test display") &&
           registry.RegisterTouch("test touch") && registry.RegisterAudioOutput("test audio"));
    device::DisplayInfo info{.width_pixels = 720, .height_pixels = 720};
    graphics::SystemFontSet fonts;
    for (unsigned i = 0; i < fonts.fonts.size(); ++i)
        fonts.fonts[i] = graphics::BuiltinLatinFont(static_cast<graphics::SystemFontRole>(i));
    pc::AppSessions sessions(apps, display, info, fonts, audio, random, registry, battery, true);
    auto foreground = [&](unsigned index) {
        assert(sessions.Activate(index));
        Until([&] { return sessions.Snapshot().foreground == static_cast<int>(index); }, &sessions);
    };
    foreground(0);
    auto first = sessions.Snapshot().apps[0].instance;
    Until([&] { return sessions.Find(0)->ComponentInfo().service_state == 2; }, &sessions);
    foreground(1);
    auto second = sessions.Snapshot().apps[1].instance;
    Until(
        [&] {
            return sessions.Find(0)->ComponentInfo().activity_state == 3 &&
                   sessions.Find(1)->ComponentInfo().resource_count == 4;
        },
        &sessions);
    const auto before = sessions.Find(0)->ComponentInfo();
    assert(before.resource_count == 4 && before.service_state == 2);
    foreground(0);
    assert(sessions.Snapshot().apps[0].instance == first);
    Until([&] { return sessions.Find(0)->ComponentInfo().activity_state == 2; }, &sessions);
    const auto old_activity = sessions.Find(0)->ComponentInfo().activity_token;
    Click(sessions, 100, 550);
    Until([&] { return sessions.Snapshot().foreground < 0 && sessions.Find(0)->ComponentInfo().activity_state == 0; },
          &sessions);
    Until([&] { return sessions.Find(0)->ComponentInfo().resource_count == 2; }, &sessions);
    assert(sessions.Find(0)->ComponentInfo().service_state == 2);
    foreground(0);
    Until(
        [&] {
            auto state = sessions.Find(0)->ComponentInfo();
            return state.activity_state == 2 && state.resource_count == 4;
        },
        &sessions);
    assert(sessions.Find(0)->ComponentInfo().activity_token != old_activity);
    Click(sessions, 350, 550);
    Until(
        [&] {
            auto state = sessions.Find(0)->ComponentInfo();
            return state.service_state == 0 && state.resource_count == 2;
        },
        &sessions);
    assert(sessions.Snapshot().foreground == 0 && sessions.Find(0)->ComponentInfo().activity_state == 2);
    Click(sessions, 350, 550);
    Until([&] { return sessions.Find(0)->ComponentInfo().service_state == 2; }, &sessions);
    assert(sessions.Find(1)->ComponentInfo().resource_count == 4);
    foreground(2);
    foreground(3);
    assert(!sessions.Activate(4));
    assert(sessions.Snapshot().foreground == 3);
    sessions.Stop(2);
    Until([&] { return !sessions.Find(2); }, &sessions);
    foreground(4);
    for (unsigned i = 1; i < 5; ++i) sessions.Stop(i);
    Until(
        [&] {
            for (unsigned i = 1; i < 5; ++i)
                if (sessions.Find(i)) return false;
            return true;
        },
        &sessions);
    foreground(0);
    assert(sessions.Snapshot().apps[0].instance == first);
    // Hold A alive while B is stopped/recreated one hundred times. The actual
    // WAMR instances, task groups, graphics, PCM rings and component records drain.
    auto previous = second;
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        foreground(1);
        Until([&] { return sessions.Find(1)->ComponentInfo().resource_count == 4; }, &sessions);
        auto key = sessions.Snapshot().apps[1].instance;
        assert(key != previous);
        previous = key;
        sessions.Stop(1);
        Until([&] { return !sessions.Find(1); }, &sessions);
        assert(sessions.Find(0)->ComponentInfo().service_state == 2 &&
               sessions.Find(0)->ComponentInfo().resource_count == 4);
    }
    sessions.Stop(0);
    Until([&] { return !sessions.Find(0); }, &sessions);
    for (const auto& state : sessions.Snapshot().apps) assert(!state.instance.valid());
    std::puts(
        "PASS real Wasm: 4 slots, capacity rejection, first-frame handoff, component domains and 100 start/stop "
        "cycles");
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
        TestRequirements(argv[1]);
        TestAudio(audio);
        TestInstances(display, audio, argv[1]);
    }
    micropixel::pc::JoinTasks();
    lv_display_delete(display);
    lv_deinit();
    wasm_runtime_destroy();
}
