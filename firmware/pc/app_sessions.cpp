// SPDX-License-Identifier: Apache-2.0
#include "app_sessions.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>

#include "backend_limits.hpp"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "remote.hpp"
#include "runtime/bundle/app_environment.hpp"
#include "runtime/services/app_storage.hpp"
#include "schedule_store.hpp"
#include "store.hpp"

namespace micropixel::pc {
namespace {
namespace graphics = platform::lvgl;
struct HubState {
    std::mutex mutex;
    host::AppHub hub;
    host::JobScheduler jobs;
    host::AlarmScheduler alarms;
    host::SystemClock clock;
    ScheduleStore store;
    std::array<std::string, 64> ids;
    std::array<bool, 64> stopped{};
    uint64_t home_sequence{};
    std::atomic<bool> input_blocked{};
    struct Wake {
        uint32_t app{UINT32_MAX};
        host::AppCaller source{};
        uint64_t sequence{}, deadline{};
    };
    std::array<Wake, 8> wakes{};
    const uint64_t boot;
    HubState() : hub(static_cast<uint64_t>(esp_timer_get_time()) + 1), boot(hub.Home().instance.boot) {
        jobs.SetPersistence({this, [](void* raw, uint32_t app, uint32_t id, const micropixel_job_plan_t* plan) {
                                 auto& self = *static_cast<HubState*>(raw);
                                 return app < self.ids.size() && self.store.SaveJob(self.ids[app], id, plan);
                             }});
        store.LoadTime(clock);
        alarms.SetPersistence(
            {this, [](void* raw, const host::AlarmScheduler::Record* record, uint32_t app, uint32_t id) {
                 auto& self = *static_cast<HubState*>(raw);
                 return app < self.ids.size() && self.store.SaveAlarm(self.ids[app], id, record);
             }});
    }
};
class ScopedApplications final : public device::ApplicationServices {
   public:
    explicit ScopedApplications(HubState& state, GuestHttp& http) : state_(state), http_(http) {}
    ~ScopedApplications() override { End(); }
    void Bind(uint32_t app, runtime::InstanceKey instance) {
        std::lock_guard lock(state_.mutex);
        caller_ = {app, instance, 0};
        service_token_ = 0;
        http_.SetWorkScope(0);
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
        if (service_token_ && service_token_ != info.service_token) {
            auto retired = caller_;
            retired.component = service_token_;
            for (auto ticket : state_.jobs.Retire(retired))
                if (ticket) http_.CancelWork(ticket);
            http_.SetWorkScope(0);
        }
        service_token_ = info.service_token;
        state_.hub.Components(caller_, info);
    }
    void Quiesce() override {
        std::lock_guard lock(state_.mutex);
        state_.hub.Quiesce(caller_);
        state_.jobs.Stop(caller_.app);
    }
    int32_t Call(uint32_t service, uint32_t method, const uint8_t* request, uint32_t size, uint8_t* output,
                 uint32_t capacity, uint32_t& written) override {
        std::lock_guard lock(state_.mutex);
        if (service == MICROPIXEL_SERVICE_ALARM) {
            written = 0;
            auto now = state_.clock.Now(std::time(nullptr));
            if (method == MICROPIXEL_ALARM_NOW) {
                if (size) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                if (capacity < sizeof(now)) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
                std::memcpy(output, &now, sizeof(now));
                written = sizeof(now);
                return 0;
            }
            if (!caller_.component || !state_.hub.Accepting(caller_)) return MICROPIXEL_STATUS_PERMISSION_DENIED;
            if (method == MICROPIXEL_ALARM_CONFIGURE_TIME) {
                if (state_.ids[caller_.app] != "micropixel.clock-alarm" ||
                    (caller_.component & 3) != MICROPIXEL_COMPONENT_ACTIVITY)
                    return MICROPIXEL_STATUS_PERMISSION_DENIED;
                micropixel_wall_time_t value{};
                if (size != sizeof(value)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                std::memcpy(&value, request, sizeof(value));
                if (value.size != sizeof(value) || value.valid > 1 || value.reserved0 || value.reserved1 ||
                    value.offset_minutes < -720 || value.offset_minutes > 840 ||
                    (value.utc && (value.utc < 1577836800ULL || value.utc > 4102444800ULL)))
                    return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                host::SystemClock::Settings settings{
                    value.offset_minutes, value.valid != 0,
                    value.utc ? static_cast<int64_t>(value.utc) - static_cast<int64_t>(std::time(nullptr)) : 0};
                if (!state_.store.SaveTime(settings)) return MICROPIXEL_STATUS_INTERNAL;
                return state_.clock.Restore(settings) ? 0 : MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            if (method == MICROPIXEL_ALARM_REGISTER) {
                micropixel_alarm_plan_t plan{};
                if (size != sizeof(plan)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                std::memcpy(&plan, request, sizeof(plan));
                return state_.alarms.Register(caller_.app, plan, now.utc, now.valid);
            }
            if (method == MICROPIXEL_ALARM_STOP) {
                if (size) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                state_.alarms.StopRinging(caller_.app);
                return 0;
            }
            micropixel_handle_request_t id{};
            if (size != sizeof(id)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            std::memcpy(&id, request, sizeof(id));
            if (id.size != sizeof(id) || id.reserved0 || id.handle < 1 || id.handle > 8)
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            if (method == MICROPIXEL_ALARM_REMOVE) return state_.alarms.Remove(caller_.app, id.handle);
            if (method == MICROPIXEL_ALARM_GET) {
                if (capacity < sizeof(micropixel_alarm_plan_t)) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
                auto* record = state_.alarms.Get(caller_.app, id.handle);
                if (!record) return MICROPIXEL_STATUS_NOT_FOUND;
                std::memcpy(output, &record->plan, sizeof(record->plan));
                written = sizeof(record->plan);
                return 0;
            }
            return MICROPIXEL_STATUS_UNSUPPORTED;
        }
        if (service == MICROPIXEL_SERVICE_JOB) {
            written = 0;
            if (method == MICROPIXEL_JOB_NOW) {
                if (size) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                if (capacity < sizeof(micropixel_wall_time_t)) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
                const auto now = state_.clock.Now(std::time(nullptr));
                std::memcpy(output, &now, sizeof(now));
                written = sizeof(now);
                return 0;
            }
            host::AppCaller owner{};
            device::AppEventSink sink{};
            if (!state_.hub.ReadyService(caller_.app, owner, sink, false) || owner.instance != caller_.instance ||
                owner.component != caller_.component)
                return MICROPIXEL_STATUS_PERMISSION_DENIED;
            if (method == MICROPIXEL_JOB_REGISTER) {
                micropixel_job_plan_t plan{};
                if (size != sizeof(plan) || !request) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
                std::memcpy(&plan, request, sizeof(plan));
                return state_.jobs.Register(caller_.app, plan, state_.clock.Now(std::time(nullptr)).utc);
            }
            micropixel_handle_request_t value{};
            if (size != sizeof(value) || !request) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            std::memcpy(&value, request, sizeof(value));
            if (value.size != sizeof(value) || value.reserved0 || !value.handle)
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            if (method == MICROPIXEL_JOB_CANCEL) return state_.jobs.Remove(caller_.app, value.handle);
            if (method == MICROPIXEL_JOB_COMPLETE) {
                auto status = state_.jobs.Complete(caller_, value.handle);
                if (!status) http_.SetWorkScope(0);
                return status;
            }
            if (method == MICROPIXEL_JOB_SCOPE) {
                if (!state_.jobs.Owns(caller_, value.handle)) return MICROPIXEL_STATUS_STALE_STATE;
                http_.SetWorkScope(value.handle);
                return 0;
            }
            return MICROPIXEL_STATUS_UNSUPPORTED;
        }
        const auto now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
        if (service == MICROPIXEL_SERVICE_MESSAGE && method == MICROPIXEL_MESSAGE_ALLOW_WAKE) {
            auto status = state_.hub.Call(caller_, service, MICROPIXEL_MESSAGE_ALLOW, request, size, output, capacity,
                                          written, now);
            if (status) return status;
            micropixel_message_allow_t wire{};
            std::memcpy(&wire, request, sizeof(wire));
            if (!state_.store.SaveWake(state_.ids[caller_.app], wire.caller, wire.types))
                return MICROPIXEL_STATUS_INTERNAL;
            return state_.hub.AllowWake(caller_.app, wire.caller, wire.types) ? 0
                                                                              : MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        auto status = state_.hub.Call(caller_, service, method, request, size, output, capacity, written, now);
        if (service == MICROPIXEL_SERVICE_MESSAGE && method == MICROPIXEL_MESSAGE_SEND &&
            size == sizeof(micropixel_message_send_t) &&
            (status == MICROPIXEL_STATUS_NOT_FOUND || status == MICROPIXEL_STATUS_STALE_STATE ||
             status == MICROPIXEL_STATUS_WOULD_BLOCK)) {
            micropixel_message_send_t wire{};
            std::memcpy(&wire, request, sizeof(wire));
            auto target = state_.hub.WakeTarget(caller_, wire);
            if (target < 0) return status;
            if (state_.stopped[target]) return MICROPIXEL_STATUS_CANCELLED;
            for (const auto& wake : state_.wakes)
                if (wake.app == static_cast<uint32_t>(target) && wake.source.instance == caller_.instance &&
                    wake.sequence == wire.sequence)
                    return now < wake.deadline ? MICROPIXEL_STATUS_WOULD_BLOCK : MICROPIXEL_STATUS_TIMEOUT;
            for (auto& wake : state_.wakes)
                if (wake.app == UINT32_MAX) {
                    wake = {static_cast<uint32_t>(target), caller_, wire.sequence, now + wire.ttl_ms};
                    return MICROPIXEL_STATUS_WOULD_BLOCK;
                }
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        return status;
    }
    bool Pending() {
        std::lock_guard lock(state_.mutex);
        bool wake = false;
        for (const auto& w : state_.wakes)
            wake |= w.app == caller_.app && static_cast<uint64_t>(esp_timer_get_time() / 1000) < w.deadline;
        return state_.hub.Pending(caller_.app, caller_.instance) || state_.jobs.Pending(caller_.app) || wake;
    }

   private:
    HubState& state_;
    GuestHttp& http_;
    host::AppCaller caller_{UINT32_MAX, {}, 0};
    uint32_t service_token_{};
};
struct Slot final : runtime::SessionRunner {
    Slot(HubState& hub, uint32_t index, std::vector<std::unique_ptr<PortableApp>>& catalog, lv_display_t* display,
         device::DisplayInfo info, const graphics::SystemFontSet& system_fonts, SdlAudio& physical_audio,
         device::Random& random, device::DeviceRegistry& registry, device::Battery& battery, bool legacy_tasks)
        : allow_legacy_tasks(legacy_tasks),
          system_hub(hub),
          apps(catalog),
          engine(info.width_pixels, info.height_pixels, fonts),
          graphics_context{.engine = &engine},
          graphics_device(graphics::MakeGuestGraphicsOperations(graphics_context)),
          input(info.width_pixels, info.height_pixels),
          audio(physical_audio, index + 1),
          media(physical_audio, index + 1),
          url_media(media),
          applications(hub, http),
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
        service_bootstrap_until = 0;
        (void)audio.ResumeAll();
        audio.SetComponentScope(0);
        http.SetComponentScope(0);
        applications.Bind(app, key);
        session = std::make_unique<Session>(*apps[app], devices, key);
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
    void RequestWork() override {
        if (session) {
            if (session->ComponentInfo().service_state != 2 && !service_bootstrap_until)
                service_bootstrap_until =
                    static_cast<uint64_t>(esp_timer_get_time() / 1000) + StartupTimeout(*apps[app_index]);
            session->RequestWork();
        }
    }
    void SetVisible(bool value) override {
        visible.store(value);
        if (session) session->SetVisible(value);
        input.SetFocused(value && !system_hub.input_blocked.load());
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
        value.background_eligible =
            !requirement.component_flags ||
            (allow_legacy_tasks && (requirement.background_kinds & 2) && !(requirement.services[15])) ||
            media.HasBackgroundLease(esp_timer_get_time() / 1000) || http.ServiceBusy();
        if (session->ComponentInfo().service_state == 2) service_bootstrap_until = 0;
        value.work_pending =
            applications.Pending() || static_cast<uint64_t>(esp_timer_get_time() / 1000) < service_bootstrap_until;
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
    const bool allow_legacy_tasks;
    HubState& system_hub;
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
    uint64_t service_bootstrap_until{};
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
    SdlAudio& physical_audio;
    bool focus_active{};
    struct Focus {
        runtime::InstanceKey instance{};
        MediaPlayer::Intent intent{};
        bool notified{};
    };
    std::array<Focus, 4> focus{};
    std::optional<host::AppManager> manager;
    PcInput empty_input;
    int32_t deferred{-1};
    int32_t replacement{-1};
    std::unique_ptr<PortableApp> package;
    struct PendingAction {
        uint32_t app{}, ticket{}, routed{}, value{};
        uint16_t type{};
        uint64_t sequence{}, deadline{};
        int32_t result{};
        bool complete{};
    };
    std::array<PendingAction, 8> actions{};
    uint32_t action_sequence{};
    std::array<uint8_t, 64> removing{};
    std::array<int32_t, 64> removal_result{};
    Impl(std::vector<std::unique_ptr<PortableApp>>& catalog, lv_display_t* display, device::DisplayInfo info,
         const graphics::SystemFontSet& fonts, SdlAudio& audio, device::Random& random,
         device::DeviceRegistry& registry, device::Battery& battery, bool multi, bool development_tasks)
        : apps(catalog),
          capacity(multi ? 4 : 1),
          physical_audio(audio),
          empty_input(info.width_pixels, info.height_pixels) {
        for (uint32_t i = 0; i < capacity; ++i) {
            slots[i] = std::make_unique<Slot>(hub, i, apps, display, info, fonts, audio, random, registry, battery,
                                              development_tasks);
            runners[i] = slots[i].get();
        }
        manager.emplace(std::span(runners.data(), capacity), hub.boot, host::AppLimits{kSessionMemoryBudget, 4});
        for (uint32_t i = 0; i < apps.size(); ++i) Register(i);
        for (uint32_t i = 0; i < apps.size(); ++i) {
            hub.store.LoadJobs(apps[i]->app_id, i, hub.jobs);
            hub.store.LoadWake(apps[i]->app_id, i, hub.hub);
            hub.store.LoadAlarms(apps[i]->app_id, i, hub.alarms);
            hub.stopped[i] = hub.store.Stopped(apps[i]->app_id);
            manager->RestoreUserStopped(i, hub.stopped[i]);
            if (hub.store.Removed(apps[i]->app_id)) {
                apps[i]->available = false;
                (void)manager->Deactivate(i);
                hub.hub.Remove(i);
            }
        }
    }
    void Register(uint32_t app) {
        const auto& req = apps[app]->requirements;
        {
            std::lock_guard lock(hub.mutex);
            hub.ids[app] = apps[app]->app_id;
            (void)hub.hub.Register(app, apps[app]->app_id, req.widget_count);
            if (apps[app]->app_id == "micropixel.clock-alarm")
                (void)hub.hub.KeepClock(app, esp_timer_get_time() / 1000);
        }
        (void)manager->Register(
            app, {.background = capacity == 1 || (req.component_flags & MICROPIXEL_COMPONENT_FLAG_BACKGROUND),
                  .activity = req.schema_version != 2 || (req.component_flags & MICROPIXEL_COMPONENT_FLAG_ACTIVITY),
                  .messages = static_cast<bool>(req.component_flags & MICROPIXEL_COMPONENT_FLAG_SERVICE),
                  .start_timeout_ms = StartupTimeout(*apps[app]),
                  .memory_bytes = MemoryReservation(*apps[app]),
                  .subprocesses = apps[app]->rv32 ? 1U : 0U});
    }
    Slot* Find(uint32_t app) {
        for (uint32_t i = 0; i < capacity; ++i)
            if (slots[i]->session && slots[i]->app_index == app) return slots[i].get();
        return nullptr;
    }
};
AppSessions::AppSessions(std::vector<std::unique_ptr<PortableApp>>& apps, lv_display_t* display,
                         device::DisplayInfo info, const graphics::SystemFontSet& fonts, SdlAudio& audio,
                         device::Random& random, device::DeviceRegistry& registry, device::Battery& battery, bool multi,
                         bool development_tasks)
    : impl_(std::make_unique<Impl>(apps, display, info, fonts, audio, random, registry, battery, multi,
                                   development_tasks)) {}
AppSessions::~AppSessions() { Shutdown(); }
void AppSessions::Process() {
    auto& self = *impl_;
    {
        std::lock_guard lock(self.hub.mutex);
        const auto now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
        self.hub.hub.Process(now);
        const auto wall = self.hub.clock.Now(std::time(nullptr));
        self.hub.jobs.Process(now, wall.utc);
        self.hub.alarms.Process(now, wall.utc, wall.offset_minutes, wall.valid);
        for (auto& wake : self.hub.wakes) {
            if (wake.app == UINT32_MAX) continue;
            if (now >= wake.deadline || self.hub.stopped[wake.app] || !self.hub.hub.AliveCaller(wake.source)) {
                wake = {};
                continue;
            }
            (void)self.manager->EnsureWork(wake.app);
        }
        for (uint32_t app = 0; app < self.apps.size(); ++app) {
            if (self.hub.stopped[app]) {
                self.hub.jobs.Stop(app);
                continue;
            }
            if (!self.hub.jobs.Pending(app)) continue;
            (void)self.manager->EnsureWork(app);
            host::AppCaller caller{};
            device::AppEventSink sink{};
            if (self.hub.hub.ReadyService(app, caller, sink)) self.hub.jobs.Deliver(app, caller, sink);
            const auto cancelled = self.hub.jobs.Cancelled(app);
            if (cancelled)
                if (auto* slot = self.Find(app)) slot->http.CancelWork(cancelled);
        }
    }
    {
        std::lock_guard lock(self.hub.mutex);
        const auto now = static_cast<uint64_t>(esp_timer_get_time() / 1000);
        for (auto& action : self.actions) {
            if (!action.ticket || action.complete || action.routed) continue;
            if (now >= action.deadline) {
                action.complete = true;
                action.result = MICROPIXEL_STATUS_TIMEOUT;
                continue;
            }
            (void)self.manager->EnsureWork(action.app, true);
            host::AppCaller ready{};
            device::AppEventSink sink{};
            if (!self.hub.hub.ReadyService(action.app, ready, sink)) continue;
            auto status = self.hub.hub.SendHome(action.app, action.type, action.value, ++self.hub.home_sequence, now,
                                                action.routed);
            if (status != MICROPIXEL_STATUS_WOULD_BLOCK && status) {
                action.complete = true;
                action.result = status;
            }
        }
    }
    bool ringing = false;
    {
        std::lock_guard lock(self.hub.mutex);
        ringing = self.hub.alarms.Ringing();
    }
    if (ringing != self.focus_active) {
        self.hub.input_blocked = ringing;
        for (uint32_t i = 0; i < self.capacity; ++i)
            self.slots[i]->input.SetFocused(!ringing && self.slots[i]->visible.load());
        self.physical_audio.SetAlarmFocus(ringing);
        for (uint32_t i = 0; i < self.capacity; ++i) {
            auto& slot = *self.slots[i];
            auto& focus = self.focus[i];
            if (ringing) {
                focus = {slot.owner, slot.media.PlaybackIntent(), false};
            } else {
                auto current = slot.media.PlaybackIntent();
                const bool same = focus.instance == slot.owner && current.generation == focus.intent.generation &&
                                  current.version == focus.intent.version;
                std::fprintf(stderr, "AUDIO_FOCUS release: slot=%u intent_preserved=%d\n", i, same);
                focus.notified = false;
            }
        }
        self.focus_active = ringing;
        std::fprintf(stderr, "ALARM focus active=%d\n", ringing);
    }
    {
        std::lock_guard lock(self.hub.mutex);
        for (uint32_t i = 0; i < self.capacity; ++i) {
            auto& slot = *self.slots[i];
            auto& focus = self.focus[i];
            if (!slot.session) continue;
            if (focus.instance != slot.owner) {
                focus.instance = slot.owner;
                focus.notified = !ringing;
            }
            if (focus.notified) continue;
            host::AppCaller caller{};
            device::AppEventSink sink{};
            if (!self.hub.hub.ReadyService(slot.app_index, caller, sink)) continue;
            micropixel_event_t event{};
            event.size = sizeof(event);
            event.service_id = MICROPIXEL_SERVICE_ALARM;
            event.event_id = MICROPIXEL_ALARM_EVENT_FOCUS;
            event.source = caller.component;
            micropixel_alarm_focus_t payload{ringing ? 1U : 0U, ringing ? 200U : 1000U};
            std::memcpy(event.payload, &payload, sizeof(payload));
            if (sink.push(sink.context, event)) focus.notified = true;
        }
    }
    self.manager->Process(esp_timer_get_time() / 1000);
    for (uint32_t app = 0; app < self.apps.size(); ++app)
        if (self.removing[app] && self.manager->Snapshot().apps[app].state == host::AppState::kStopped) {
            const bool uninstall = self.removing[app] == 2;
            bool ok = ClearSchedules(app) && runtime::EraseAppStorage(self.apps[app]->app_id) == ESP_OK;
            if (ok && uninstall) {
                std::error_code error;
                std::filesystem::remove(InstalledAppsDirectory() / (self.apps[app]->app_id + ".bundle.bin"), error);
                ok = !error;
            }
            if (ok) {
                std::lock_guard lock(self.hub.mutex);
                ok = uninstall ? self.hub.store.SetRemoved(self.apps[app]->app_id, true)
                               : self.hub.store.SetStopped(self.apps[app]->app_id, true);
                if (ok && uninstall) {
                    self.hub.hub.Remove(app);
                    self.apps[app]->available = false;
                    (void)self.manager->Deactivate(app);
                }
            }
            self.removal_result[app] = ok ? 0 : MICROPIXEL_STATUS_INTERNAL;
            self.removing[app] = 0;
            std::fprintf(stderr, "APP_SHELL %s: %s status=%d\n", uninstall ? "uninstalled" : "data cleared",
                         self.apps[app]->app_id.c_str(), self.removal_result[app]);
        }
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
    if (app >= self.apps.size() || !self.apps[app]->available || self.removing[app]) return false;
    auto result = self.manager->Activate(app);
    if (result == host::ManagerResult::kAccepted) {
        {
            std::lock_guard lock(self.hub.mutex);
            self.hub.stopped[app] = false;
            (void)self.hub.store.SetStopped(self.apps[app]->app_id, false);
        }
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
bool AppSessions::EnsureWork(uint32_t app, bool user) {
    auto& self = *impl_;
    if (app >= self.apps.size() || !self.apps[app]->available || self.removing[app]) return false;
    auto result = self.manager->EnsureWork(app, user);
    if (result != host::ManagerResult::kAccepted) return false;
    if (user) {
        std::lock_guard lock(self.hub.mutex);
        if (!self.hub.store.SetStopped(self.apps[app]->app_id, false)) return false;
        self.hub.stopped[app] = false;
    }
    return true;
}
bool AppSessions::Remove(uint32_t app, bool uninstall) {
    auto& self = *impl_;
    if (app >= self.apps.size() || self.removing[app]) return false;
    auto result = self.manager->Stop(app);
    if (result != host::ManagerResult::kAccepted) return false;
    if (self.replacement == static_cast<int32_t>(app)) {
        self.package.reset();
        self.replacement = -1;
    }
    if (self.deferred == static_cast<int32_t>(app)) self.deferred = -1;
    self.removing[app] = uninstall ? 2 : 1;
    for (auto& action : self.actions)
        if (action.ticket && action.app == app && !action.complete) {
            action.complete = true;
            action.result = MICROPIXEL_STATUS_CANCELLED;
            action.routed = 0;
        }
    self.removal_result[app] = MICROPIXEL_STATUS_WOULD_BLOCK;
    {
        std::lock_guard lock(self.hub.mutex);
        self.hub.stopped[app] = true;
        self.hub.jobs.Stop(app);
    }
    return true;
}
bool AppSessions::Removing(uint32_t app) const { return app < impl_->apps.size() && impl_->removing[app]; }
int32_t AppSessions::RemovalResult(uint32_t app) const {
    return app < impl_->apps.size() ? impl_->removal_result[app] : MICROPIXEL_STATUS_INVALID_ARGUMENT;
}
bool AppSessions::ClearSchedules(uint32_t app) {
    auto& self = *impl_;
    if (app >= self.apps.size()) return false;
    std::lock_guard lock(self.hub.mutex);
    if (!self.hub.store.Delete(self.apps[app]->app_id)) return false;
    self.hub.jobs.Stop(app);
    self.hub.hub.ClearWake(app);
    self.hub.alarms.Forget(app);
    for (auto& wake : self.hub.wakes)
        if (wake.app == app) wake = {};
    for (uint32_t id = 1; id <= 2; ++id) (void)self.hub.jobs.Remove(app, id);
    return true;
}
void AppSessions::Stop(uint32_t app, bool user) {
    if (app >= impl_->apps.size()) return;
    {
        std::lock_guard lock(impl_->hub.mutex);
        if (!impl_->hub.store.SetStopped(impl_->apps[app]->app_id, user)) return;
        impl_->hub.stopped[app] = user;
        impl_->hub.jobs.Stop(app);
    }
    (void)impl_->manager->Stop(app, user);
}
bool AppSessions::Install(uint32_t app, std::unique_ptr<PortableApp> package) {
    auto& self = *impl_;
    if (self.package || !package || app >= 64 || app > self.apps.size() ||
        (app < self.apps.size() && (self.removing[app] || package->app_id != self.apps[app]->app_id)))
        return false;
    for (uint32_t i = 0; i < self.apps.size(); ++i)
        if (i != app && self.apps[i]->app_id == package->app_id) return false;
    {
        std::lock_guard lock(self.hub.mutex);
        if (!package || !self.hub.store.SetRemoved(package->app_id, false)) return false;
    }
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
    if (!shared.hub.Widget(index, esp_timer_get_time() / 1000, out)) return false;
    if (out.data.template_id == MICROPIXEL_WIDGET_CLOCK) {
        auto now = shared.clock.Now(std::time(nullptr));
        if (now.valid) {
            auto local = static_cast<int64_t>(now.utc) + int64_t(now.offset_minutes) * 60;
            auto day = local % 86400;
            std::snprintf(out.data.title, sizeof(out.data.title), "%02lld:%02lld:%02lld",
                          static_cast<long long>(day / 3600), static_cast<long long>((day / 60) % 60),
                          static_cast<long long>(day % 60));
            const host::AlarmScheduler::Record* next = nullptr;
            for (const auto& record : shared.alarms.Records())
                if (record.app == out.app && (record.plan.flags & 1)) {
                    next = &record;
                    break;
                }
            std::snprintf(out.data.subtitle, sizeof(out.data.subtitle), next ? "闹钟已开启" : "暂无闹钟");
            out.data.updated_utc = now.utc;
            out.expired = false;
        } else {
            std::snprintf(out.data.title, sizeof(out.data.title), "时间未同步");
            out.expired = true;
        }
    }
    return true;
}
int32_t AppSessions::WidgetAction(const host::WidgetSnapshot& widget, uint16_t type, uint32_t value, uint64_t sequence,
                                  uint32_t& ticket) {
    auto& shared = impl_->hub;
    std::lock_guard lock(shared.mutex);
    if (widget.app >= impl_->apps.size() || !impl_->apps[widget.app]->available || impl_->removing[widget.app])
        return MICROPIXEL_STATUS_CANCELLED;
    if (!widget.online &&
        ((type == MICROPIXEL_ACTION_REFRESH && widget.data.template_id == MICROPIXEL_WIDGET_WEATHER) ||
         (type == MICROPIXEL_ACTION_OPEN_PAGE && widget.data.template_id == MICROPIXEL_WIDGET_CLOCK && value <= 2))) {
        bool current = false;
        host::WidgetSnapshot item{};
        for (uint32_t i = 0; i < 8; ++i)
            if (shared.hub.Widget(i, esp_timer_get_time() / 1000, item) && item.app == widget.app &&
                item.revision == widget.revision)
                current = true;
        if (!current || widget.app >= impl_->apps.size()) return MICROPIXEL_STATUS_STALE_STATE;
        for (auto& action : impl_->actions)
            if (!action.ticket || action.complete) {
                if (impl_->action_sequence >= 0x7fffffff) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
                if (!shared.store.SetStopped(impl_->apps[widget.app]->app_id, false)) return MICROPIXEL_STATUS_INTERNAL;
                shared.stopped[widget.app] = false;
                impl_->manager->RestoreUserStopped(widget.app, false);
                ticket = 0x80000000U | ++impl_->action_sequence;
                action = {widget.app,
                          ticket,
                          0,
                          value,
                          type,
                          sequence,
                          static_cast<uint64_t>(esp_timer_get_time() / 1000) + StartupTimeout(*impl_->apps[widget.app]),
                          0,
                          false};
                return 0;
            }
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    shared.home_sequence = std::max(shared.home_sequence, sequence);
    return shared.hub.Action(widget, type, value, sequence, esp_timer_get_time() / 1000, ticket);
}
micropixel_message_result_t AppSessions::WidgetResult(uint32_t ticket) {
    auto& shared = impl_->hub;
    std::lock_guard lock(shared.mutex);
    if (ticket & 0x80000000U) {
        for (auto& action : impl_->actions)
            if (action.ticket == ticket) {
                if (action.routed) {
                    auto result = shared.hub.Result(shared.hub.Home(), action.routed);
                    action.complete = result.complete;
                    action.result = result.result;
                }
                return {sizeof(micropixel_message_result_t), static_cast<uint16_t>(action.complete), ticket,
                        action.result};
            }
        return {sizeof(micropixel_message_result_t), 1, ticket, MICROPIXEL_STATUS_STALE_STATE};
    }
    return shared.hub.Result(shared.hub.Home(), ticket);
}
uint64_t AppSessions::NextHomeRequest() {
    auto& state = impl_->hub;
    std::lock_guard lock(state.mutex);
    return state.home_sequence == UINT64_MAX ? 0 : ++state.home_sequence;
}
AppSessions::PlannerSummary AppSessions::Planner() {
    auto& state = impl_->hub;
    std::lock_guard lock(state.mutex);
    PlannerSummary summary;
    for (const auto& record : state.alarms.Records()) {
        if (record.app >= impl_->apps.size() || impl_->apps[record.app]->app_id != "micropixel.clock-alarm") continue;
        if (record.plan.id >= 3) ++summary.note_records;
        const auto next = state.alarms.NextOccurrence(record.app, record.plan.id);
        if (!next) continue;
        if (record.plan.id < 3) {
            if (!summary.alarm_utc || next < summary.alarm_utc) summary.alarm_utc = next;
        } else {
            ++summary.pending_notes;
            if (!summary.note_utc || next < summary.note_utc) {
                summary.note_utc = next;
                std::memcpy(summary.note.data(), record.plan.label, summary.note.size());
            }
        }
    }
    return summary;
}
AppSessions::AlarmState AppSessions::Alarm() {
    auto& state = impl_->hub;
    std::lock_guard lock(state.mutex);
    AlarmState result{state.alarms.Ringing(), state.alarms.RingCount(), state.alarms.RingRevision(), {}};
    std::memcpy(result.label.data(), state.alarms.RingLabel(), result.label.size());
    return result;
}
void AppSessions::StopAlarm() {
    auto& state = impl_->hub;
    std::lock_guard lock(state.mutex);
    state.alarms.StopRinging();
}
micropixel_wall_time_t AppSessions::Clock() {
    auto& state = impl_->hub;
    std::lock_guard lock(state.mutex);
    return state.clock.Now(std::time(nullptr));
}
void AppSessions::Shutdown() {
    if (!impl_) return;
    impl_->physical_audio.SetAlarmFocus(false);
    for (uint32_t i = 0; i < impl_->capacity; ++i)
        if (impl_->slots[i]->session) impl_->slots[i]->RequestStop();
    for (uint32_t i = 0; i < impl_->capacity; ++i)
        if (impl_->slots[i]->session) impl_->slots[i]->session->Stop();
}
}  // namespace micropixel::pc
