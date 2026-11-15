// SPDX-License-Identifier: Apache-2.0
#include "app_sessions.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>

#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "remote.hpp"
#include "runtime/bundle/app_environment.hpp"

namespace micropixel::pc {
namespace {
namespace graphics = platform::lvgl;
struct HubState {
    std::mutex mutex;
    host::AppHub hub;
    const uint64_t boot;
    HubState() : hub(static_cast<uint64_t>(esp_timer_get_time()) + 1), boot(hub.Home().instance.boot) {}
};
class ScopedApplications final : public device::ApplicationServices {
   public:
    explicit ScopedApplications(HubState& state) : state_(state) {}
    ~ScopedApplications() override { End(); }
    void Bind(uint32_t app, runtime::InstanceKey instance) {
        std::lock_guard lock(state_.mutex);
        caller_ = {app, instance, 0};
        state_.hub.Bind(app, instance);
    }
    void End() {
        std::lock_guard lock(state_.mutex);
        state_.hub.Unbind(caller_);
        caller_ = {UINT32_MAX, {}, 0};
    }
    void BindEvents(device::AppEventSink sink) override {
        std::lock_guard lock(state_.mutex);
        state_.hub.SetSink(caller_, sink);
    }
    void SetComponentScope(uint32_t token) override { caller_.component = token; }
    void SetComponents(const micropixel_component_info_t& info) override {
        std::lock_guard lock(state_.mutex);
        state_.hub.Components(caller_, info);
    }
    void Quiesce() override {
        std::lock_guard lock(state_.mutex);
        state_.hub.Quiesce(caller_);
    }
    int32_t Call(uint32_t service, uint32_t method, const uint8_t* request, uint32_t size, uint8_t* output,
                 uint32_t capacity, uint32_t& written) override {
        std::lock_guard lock(state_.mutex);
        return state_.hub.Call(caller_, service, method, request, size, output, capacity, written,
                               esp_timer_get_time() / 1000);
    }
    bool Pending() {
        std::lock_guard lock(state_.mutex);
        return state_.hub.Pending(caller_.app, caller_.instance);
    }

   private:
    HubState& state_;
    host::AppCaller caller_{UINT32_MAX, {}, 0};
};
struct Slot final : runtime::SessionRunner {
    Slot(HubState& hub, uint32_t index, std::vector<std::unique_ptr<PortableApp>>& catalog, lv_display_t* display,
         device::DisplayInfo info, const graphics::SystemFontSet& system_fonts, SdlAudio& physical_audio,
         device::Random& random, device::DeviceRegistry& registry, device::Battery& battery)
        : apps(catalog),
          engine(info.width_pixels, info.height_pixels, fonts),
          graphics_context{.engine = &engine},
          graphics_device(graphics::MakeGuestGraphicsOperations(graphics_context)),
          input(info.width_pixels, info.height_pixels),
          audio(physical_audio, index + 1),
          media(physical_audio, index + 1),
          url_media(media),
          applications(hub),
          devices(graphics_device, info, input, audio, random, registry, registry, registry, registry, battery, &http,
                  &url_media, &applications) {
        (void)fonts.Activate(system_fonts);
        input.SetFocused(false);
        engine.SetPresentationHooks(
            {.context = this, .prepare_frame_locked = [](void* raw, lv_obj_t* frame, bool, bool&) {
                 auto& self = *static_cast<Slot*>(raw);
                 lv_obj_set_hidden(frame, !self.visible.load());
                 self.frame_ready.store(true, std::memory_order_release);
             }});
        valid = audio.valid() && engine.Initialize(display, nullptr) == ESP_OK;
    }
    ~Slot() override {
        if (session) session->Stop();
        engine.Shutdown();
    }
    bool BeginStart(uint32_t app, runtime::InstanceKey key) override {
        if (!valid || session || app >= apps.size()) return false;
        const auto environment = runtime::AppEnvironment(devices);
        if (!micropixel_app_runtime_compatible(&apps[app]->requirements, &environment)) {
            std::fputs("App requirements not supported\n", stderr);
            return false;
        }
        owner = key;
        app_index = app;
        visible = false;
        frame_ready = false;
        (void)audio.ResumeAll();
        audio.SetComponentScope(0);
        http.SetComponentScope(0);
        applications.Bind(app, key);
        session = std::make_unique<Session>(*apps[app], devices);
        apps[app]->launch_arguments = {};
        session->Start();
        std::fprintf(stderr, "APP_SHELL started: %s\n", apps[app]->app_id.c_str());
        return true;
    }
    void RequestStop() override {
        applications.Quiesce();
        if (session) session->RequestStop();
    }
    void RequestSuspend() override {
        if (session) session->RequestSuspend();
    }
    void RequestResume() override {
        if (session) session->RequestResume();
    }
    void RequestBackgroundResume() override {
        if (session) session->RequestResume(false);
    }
    void SetVisible(bool value) override {
        visible.store(value);
        if (session) session->SetVisible(value);
        input.SetFocused(value);
        esp_lv_adapter_lock(-1);
        if (auto* frame = engine.FrameLocked()) {
            lv_obj_set_hidden(frame, !value);
            if (value) lv_obj_move_foreground(frame);
        }
        esp_lv_adapter_unlock();
    }
    runtime::SessionObservation Observe() override {
        runtime::SessionObservation value{.owner = owner};
        if (!session) {
            value.exited = true;
            return value;
        }
        value.ready = session->Ready();
        value.suspended = session->Suspended();
        value.activity_available = session->ActivityAvailable();
        const auto& requirement = apps[app_index]->requirements;
        value.background_eligible = !requirement.component_flags || (requirement.background_kinds & 2) ||
                                    media.HasBackgroundLease(esp_timer_get_time() / 1000) || http.ServiceBusy();
        value.work_pending = applications.Pending();
        esp_lv_adapter_lock(-1);
        value.frame_ready = engine.FrameLocked() != nullptr;
        esp_lv_adapter_unlock();
        if (session->Done()) {
            value.result = session->Result();
            session.reset();
            applications.End();
            value.exited = true;
            std::fprintf(stderr, "APP_SHELL stopped: %s\n", apps[app_index]->app_id.c_str());
        }
        return value;
    }
    std::vector<std::unique_ptr<PortableApp>>& apps;
    graphics::FontRegistry fonts;
    graphics::GuestGraphicsEngine engine;
    graphics::GuestGraphicsOperationsContext graphics_context;
    platform::adapters::GraphicsAdapter graphics_device;
    PcInput input;
    ScopedAudio audio;
    MediaPlayer media;
    GuestHttp http;
    GuestMedia url_media;
    ScopedApplications applications;
    device::DeviceServices devices;
    std::unique_ptr<Session> session;
    runtime::InstanceKey owner{};
    uint32_t app_index{};
    std::atomic<bool> visible{}, frame_ready{};
    bool valid{};
};
}  // namespace
struct AppSessions::Impl {
    HubState hub;
    std::array<std::unique_ptr<Slot>, 4> slots;
    std::array<runtime::SessionRunner*, 4> runners{};
    std::vector<std::unique_ptr<PortableApp>>& apps;
    const uint32_t capacity;
    std::optional<host::AppManager> manager;
    PcInput empty_input;
    int32_t deferred{-1};
    int32_t replacement{-1};
    std::unique_ptr<PortableApp> package;
    Impl(std::vector<std::unique_ptr<PortableApp>>& catalog, lv_display_t* display, device::DisplayInfo info,
         const graphics::SystemFontSet& fonts, SdlAudio& audio, device::Random& random,
         device::DeviceRegistry& registry, device::Battery& battery, bool multi)
        : apps(catalog), capacity(multi ? 4 : 1), empty_input(info.width_pixels, info.height_pixels) {
        for (uint32_t i = 0; i < capacity; ++i) {
            slots[i] = std::make_unique<Slot>(hub, i, apps, display, info, fonts, audio, random, registry, battery);
            runners[i] = slots[i].get();
        }
        manager.emplace(std::span(runners.data(), capacity), hub.boot);
        for (uint32_t i = 0; i < apps.size(); ++i) Register(i);
    }
    void Register(uint32_t app) {
        const auto& req = apps[app]->requirements;
        {
            std::lock_guard lock(hub.mutex);
            (void)hub.hub.Register(app, apps[app]->app_id, req.widget_count);
        }
        (void)manager->Register(
            app, {.background = capacity == 1 || (req.component_flags & MICROPIXEL_COMPONENT_FLAG_BACKGROUND),
                  .activity = req.schema_version != 2 || (req.component_flags & MICROPIXEL_COMPONENT_FLAG_ACTIVITY),
                  .messages = static_cast<bool>(req.component_flags & MICROPIXEL_COMPONENT_FLAG_SERVICE)});
    }
    Slot* Find(uint32_t app) {
        for (uint32_t i = 0; i < capacity; ++i)
            if (slots[i]->session && slots[i]->app_index == app) return slots[i].get();
        return nullptr;
    }
};
AppSessions::AppSessions(std::vector<std::unique_ptr<PortableApp>>& apps, lv_display_t* display,
                         device::DisplayInfo info, const graphics::SystemFontSet& fonts, SdlAudio& audio,
                         device::Random& random, device::DeviceRegistry& registry, device::Battery& battery, bool multi)
    : impl_(std::make_unique<Impl>(apps, display, info, fonts, audio, random, registry, battery, multi)) {}
AppSessions::~AppSessions() { Shutdown(); }
void AppSessions::Process() {
    auto& self = *impl_;
    {
        std::lock_guard lock(self.hub.mutex);
        self.hub.hub.Process(esp_timer_get_time() / 1000);
    }
    self.manager->Process(esp_timer_get_time() / 1000);
    if (self.replacement >= 0 && !self.Find(self.replacement)) {
        self.apps[self.replacement] = std::move(self.package);
        self.Register(self.replacement);
        self.replacement = -1;
    }
    if (self.deferred >= 0 && self.replacement < 0 &&
        self.manager->Activate(self.deferred) == host::ManagerResult::kAccepted) {
        self.deferred = -1;
        self.manager->Process(esp_timer_get_time() / 1000);
    }
}
bool AppSessions::Activate(uint32_t app) {
    auto& self = *impl_;
    if (app >= self.apps.size()) return false;
    if (self.capacity > 1 && self.apps[app]->rv32) {
        std::fputs("RV32 multi-app execution is reserved for D8; use the single-slot profile\n", stderr);
        return false;
    }
    auto result = self.manager->Activate(app);
    if (result == host::ManagerResult::kAccepted) {
        self.deferred = -1;
        return true;
    }
    if (result == host::ManagerResult::kNoCapacity && self.capacity == 1) {
        for (uint32_t i = 0; i < self.apps.size(); ++i)
            if (self.Find(i)) (void)self.manager->Stop(i, false);
        self.deferred = app;
        return true;
    }
    return false;
}
void AppSessions::ShowHome() {
    impl_->deferred = -1;
    (void)impl_->manager->ShowHome();
}
void AppSessions::Stop(uint32_t app) { (void)impl_->manager->Stop(app); }
bool AppSessions::Install(uint32_t app, std::unique_ptr<PortableApp> package) {
    auto& self = *impl_;
    if (self.package || app >= 64) return false;
    if (app == self.apps.size()) {
        self.apps.push_back(std::move(package));
        self.Register(app);
        return Activate(app);
    }
    if (auto* slot = self.Find(app)) {
        (void)slot;
        self.replacement = app;
        self.package = std::move(package);
        self.deferred = app;
        (void)self.manager->Stop(app, false);
        return true;
    }
    self.apps[app] = std::move(package);
    self.Register(app);
    return Activate(app);
}
Session* AppSessions::Find(uint32_t app) {
    auto* slot = impl_->Find(app);
    return slot ? slot->session.get() : nullptr;
}
bool AppSessions::Pending(uint32_t app) const {
    return impl_->deferred == static_cast<int32_t>(app) || Snapshot().pending == static_cast<int32_t>(app);
}
PcInput& AppSessions::Input() {
    const auto app = Snapshot().foreground;
    auto* slot = app >= 0 ? impl_->Find(app) : nullptr;
    return slot ? slot->input : impl_->empty_input;
}
MediaPlayer* AppSessions::Media(uint32_t app) {
    auto* slot = impl_->Find(app);
    return slot ? &slot->media : nullptr;
}
const host::ManagerSnapshot& AppSessions::Snapshot() const { return impl_->manager->Snapshot(); }
bool AppSessions::Widget(uint32_t index, host::WidgetSnapshot& out) {
    auto& shared = impl_->hub;
    std::lock_guard lock(shared.mutex);
    return shared.hub.Widget(index, esp_timer_get_time() / 1000, out);
}
int32_t AppSessions::WidgetAction(const host::WidgetSnapshot& widget, uint16_t type, uint32_t value, uint64_t sequence,
                                  uint32_t& ticket) {
    auto& shared = impl_->hub;
    std::lock_guard lock(shared.mutex);
    return shared.hub.Action(widget, type, value, sequence, esp_timer_get_time() / 1000, ticket);
}
micropixel_message_result_t AppSessions::WidgetResult(uint32_t ticket) {
    auto& shared = impl_->hub;
    std::lock_guard lock(shared.mutex);
    return shared.hub.Result(shared.hub.Home(), ticket);
}
void AppSessions::Shutdown() {
    if (!impl_) return;
    for (uint32_t i = 0; i < impl_->capacity; ++i)
        if (impl_->slots[i]->session) impl_->slots[i]->RequestStop();
    for (uint32_t i = 0; i < impl_->capacity; ++i)
        if (impl_->slots[i]->session) impl_->slots[i]->session->Stop();
}
}  // namespace micropixel::pc
