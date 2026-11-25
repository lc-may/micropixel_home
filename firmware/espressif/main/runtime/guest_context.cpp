#include "runtime/guest_context.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "runtime/wamr/diagnostics.h"
#include "sdkconfig.h"

namespace micropixel::runtime {

namespace {
constexpr char kTag[] = "micropixel_guest";
}

GuestContext::GuestContext(const micropixel_aot_package_t& package, device::DeviceServices& devices,
                           work::BackgroundExecutor& background_executor, std::string_view effective_locale,
                           const micropixel_system_launch_arguments_response_t& launch_arguments,
                           GuestLogSink* log_sink, uint16_t component_flags)
    : devices_(devices),
      log_sink_(log_sink),
      clock_origin_us_(esp_timer_get_time()),
      events_{},
      timers_(events_, clock_origin_us_),
      sensors_(devices_.sensors(), timers_),
      gpio_(devices_.gpio(), events_, timers_),
      haptics_(devices_.haptics(), events_, timers_),
      resources_(package, background_executor, devices_.graphics()),
      audio_playback_(package, devices_.audio(), events_, clock_origin_us_),
      pcm_stream_(devices_.audio(), events_, clock_origin_us_),
      direct_surface_(devices_.graphics(), events_, clock_origin_us_),
#if CONFIG_MICROPIXEL_RASTER_KERNELS
      raster_(true),
#else
      raster_(false),
#endif
      storage_(package),
      touch_events_(events_, devices_.input(), clock_origin_us_),
      key_events_(events_, devices_.input(), clock_origin_us_),
      components_(
          events_, service_registry_, component_flags,
          {.context = this,
           .now = [](void* raw) { return static_cast<GuestContext*>(raw)->timers_.Now(); },
           .stop = [](void* raw) { return static_cast<GuestContext*>(raw)->RequestStop(); },
           .scope =
               [](void* raw, uint32_t token, const micropixel_component_info_t& info) {
                   auto& context = *static_cast<GuestContext*>(raw);
                   context.devices_.audio().SetComponentScope(token);
                   if (auto* apps = context.devices_.applications()) {
                       apps->SetComponents(info);
                       apps->SetComponentScope(token);
                   }
                   if (auto* http = context.devices_.http()) http->SetComponentScope(token);
               },
           .changed =
               [](void* raw, const micropixel_component_info_t& info) {
                   if (auto* apps = static_cast<GuestContext*>(raw)->devices_.applications()) apps->SetComponents(info);
               },
           .stop_audio = [](void* raw,
                            uint32_t token) { static_cast<GuestContext*>(raw)->devices_.audio().StopComponent(token); },
           .release_activity =
               [](void* raw) {
                   auto& context = *static_cast<GuestContext*>(raw);
                   context.direct_surface_.Shutdown();
                   context.devices_.graphics().ReleaseActivityResources();
                   (void)context.devices_.input().EnableText(false);
               }}),
      lifecycle_endpoint_(*this),
      widget_endpoint_(*this, MICROPIXEL_SERVICE_WIDGET),
      message_endpoint_(*this, MICROPIXEL_SERVICE_MESSAGE),
      job_endpoint_(*this, MICROPIXEL_SERVICE_JOB),
      alarm_endpoint_(*this, MICROPIXEL_SERVICE_ALARM),
      timer_endpoint_(*this),
      system_endpoint_(effective_locale, launch_arguments),
      storage_endpoint_(*this),
      resource_endpoint_(*this),
      random_endpoint_(*this),
      graphics_endpoint_(*this),
      input_endpoint_(*this),
      audio_endpoint_(*this),
      devices_endpoint_(*this),
      sensors_endpoint_(*this),
      gpio_endpoint_(*this),
      haptics_endpoint_(*this),
      power_info_endpoint_(*this),
      network_endpoint_(devices.http()),
      url_media_(devices.media()),
      service_registry_(timer_endpoint_, system_endpoint_, storage_endpoint_, resource_endpoint_, random_endpoint_,
                        graphics_endpoint_, input_endpoint_, audio_endpoint_, devices_endpoint_, sensors_endpoint_,
                        gpio_endpoint_, haptics_endpoint_, power_info_endpoint_, network_endpoint_, lifecycle_endpoint_,
                        widget_endpoint_, message_endpoint_, job_endpoint_, alarm_endpoint_) {
    (void)std::snprintf(app_id_.data(), app_id_.size(), "%s", reinterpret_cast<const char*>(package.app_id));
    if (auto* apps = devices_.applications())
        apps->BindEvents({this, [](void* raw, const micropixel_event_t& event) {
                              auto& context = *static_cast<GuestContext*>(raw);
                              auto copy = event;
                              copy.timestamp_us = context.AppTimeNow();
                              return context.events_.PushAdvisory(copy);
                          }});
    const auto audio_result = devices_.audio().ResumeAll();
    audio_foreground_ready_ = audio_result || audio_result.error().status == MICROPIXEL_STATUS_UNSUPPORTED;
    if (!valid()) {
        ESP_LOGE(kTag,
                 "Guest service init failed: events=%u timers=%u sensors=%u gpio=%u haptics=%u resources=%u "
                 "audio=%u storage=%u foreground=%u internal_free=%zu internal_largest=%zu",
                 events_.valid(), timers_.valid(), sensors_.valid(), gpio_.valid(), haptics_.valid(),
                 resources_.valid(), audio_playback_.valid(), storage_.valid(), audio_foreground_ready_,
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
}

GuestContext::~GuestContext() {
    if (auto* apps = devices_.applications()) apps->BindEvents({});
    micropixel_check_heap("GuestContext teardown begin");
    // Returns the panel to LVGL and drains in-flight Guest buffers while the
    // event queue and the release sink (this object) are still alive.
    direct_surface_.Shutdown();
    raster_.Shutdown();
    micropixel_check_heap("direct surface and raster shutdown");
    events_.Close();
    (void)devices_.input().EnableText(false);
    key_events_.Shutdown();
    touch_events_.Shutdown();
    sensors_.Shutdown();
    gpio_.Shutdown();
    haptics_.Shutdown();
    url_media_.Close();
    audio_playback_.Shutdown();
    pcm_stream_.Shutdown();
    (void)devices_.audio().StopAll();
    (void)devices_.audio().SuspendAll();
    micropixel_check_heap("before Guest graphics release");
    devices_.graphics().ReleaseGuestResources();
    micropixel_check_heap("after Guest graphics release");
    resources_.Shutdown();
    micropixel_check_heap("after Guest textures release");
}

EventWaitResult GuestContext::WaitEvent(micropixel_event_t& event, uint64_t timeout_us) {
    const auto start = static_cast<uint64_t>(esp_timer_get_time());
    for (;;) {
        if (!stopping_.load() && components_.TakeEvent(event)) return EventWaitResult::kReceived;
        uint64_t remaining = timeout_us;
        if (timeout_us != UINT64_MAX) {
            const auto elapsed = static_cast<uint64_t>(esp_timer_get_time()) - start;
            remaining = elapsed >= timeout_us ? 0 : timeout_us - elapsed;
        }
        const auto result = events_.Wait(event, remaining);
        if (result != EventWaitResult::kReceived || event.service_id != MICROPIXEL_SERVICE_LIFECYCLE ||
            event.event_id != 0)
            return result;
    }
}
int32_t GuestContext::ServiceCall(micropixel_service_handle_t handle, uint32_t method, const uint8_t* request,
                                  uint32_t request_size, uint8_t* response, uint32_t capacity, uint32_t& written) {
    written = 0;
    const auto service = service_registry_.ServiceId(handle);
    if (service == MICROPIXEL_SERVICE_AUDIO && method == MICROPIXEL_AUDIO_METHOD_STOP_ALL && request_size == 0 &&
        components_.StopAudioScoped())
        return MICROPIXEL_STATUS_OK;
    const auto allowed = components_.BeforeCall(service, method);
    if (allowed != MICROPIXEL_STATUS_OK) return allowed;
    const auto result = service_registry_.Call(handle, method, request, request_size, response, capacity, written);
    if (service != MICROPIXEL_SERVICE_LIFECYCLE &&
        (result == MICROPIXEL_STATUS_OK || result == MICROPIXEL_STATUS_NOT_FOUND ||
         result == MICROPIXEL_STATUS_CLOSED || result == MICROPIXEL_STATUS_STALE_STATE))
        components_.AfterCall(handle, service, method, request, request_size, response, written, result);
    return result;
}

void GuestContext::WriteLog(uint32_t level, const uint8_t* bytes, uint32_t length) {
    constexpr std::string_view kPanicPrefix = "panic: ";
    if (level == MICROPIXEL_LOG_ERROR && length > kPanicPrefix.size() &&
        std::memcmp(bytes, kPanicPrefix.data(), kPanicPrefix.size()) == 0) {
        const uint32_t copied = std::min<uint32_t>(length, last_panic_.size() - 1U);
        std::memcpy(last_panic_.data(), bytes, copied);
        last_panic_[copied] = '\0';
    }
    if (log_sink_ != nullptr) {
        log_sink_->WriteGuestLog(app_id_.data(), level, bytes, length, static_cast<uint64_t>(esp_timer_get_time()));
    }
}

bool GuestContext::Suspend(TickType_t timeout) {
    if (suspended_) {
        return true;
    }
    // First: the Guest may still be presenting until it reaches WaitEvent, and
    // the Hall snapshot below needs LVGL to own the panel again.
    direct_surface_.Suspend();
    touch_events_.Suspend();
    key_events_.Suspend();
    sensors_.Suspend();
    gpio_.Suspend();
    haptics_.Suspend();
    const bool timers_suspended = timers_.Suspend();
    auto audio_result = devices_.audio().SuspendAll();
    const bool audio_suspended = audio_result || audio_result.error().status == MICROPIXEL_STATUS_UNSUPPORTED;
    if (!timers_suspended || !audio_suspended || !events_.Suspend(timeout)) {
        (void)timers_.Resume();
        (void)devices_.audio().ResumeAll();
        (void)sensors_.Resume();
        (void)gpio_.Resume();
        touch_events_.Resume();
        key_events_.Resume();
        direct_surface_.Resume();
        events_.Resume();
        return false;
    }
    suspended_ = true;
    ESP_LOGI(kTag, "Guest services suspended at a WaitEvent safe point");
    return true;
}

bool GuestContext::Resume() {
    if (!suspended_) {
        return true;
    }
    micropixel_event_t resume_event{};
    resume_event.size = sizeof(resume_event);
    resume_event.event_id = MICROPIXEL_CORE_EVENT_RESUME;
    resume_event.timestamp_us = timers_.Now();
    resume_event.sequence = ++core_sequence_;
    resume_event.status = MICROPIXEL_STATUS_OK;
    if (!events_.PrepareResume(resume_event)) {
        return false;
    }
    const bool timers_resumed = timers_.Resume();
    auto audio_result = devices_.audio().ResumeAll();
    const bool audio_resumed = audio_result || audio_result.error().status == MICROPIXEL_STATUS_UNSUPPORTED;
    if (!timers_resumed || !audio_resumed) {
        // Wake the safe point even on failure. AppController immediately
        // requests stop, and a blocked Guest must be allowed to observe it.
        events_.Resume();
        return false;
    }
    const bool sensors_resumed = sensors_.Resume();
    const bool gpio_resumed = sensors_resumed && gpio_.Resume();
    if (!sensors_resumed || !gpio_resumed) {
        sensors_.Suspend();
        gpio_.Suspend();
        events_.Resume();
        return false;
    }
    touch_events_.Resume();
    key_events_.Resume();
    // Before the Guest wakes: its first frame after RESUME must be shown.
    direct_surface_.Resume();
    events_.Resume();
    suspended_ = false;
    ESP_LOGI(kTag, "Guest services resumed from the same AppSession: event=%" PRIu32, core_sequence_);
    return true;
}

bool GuestContext::RequestStop() {
    stopping_.store(true);
    if (auto* apps = devices_.applications()) apps->Quiesce();
    touch_events_.Suspend();
    key_events_.Suspend();
    sensors_.Suspend();
    gpio_.Suspend();
    haptics_.Suspend();
    (void)timers_.Suspend();
    (void)devices_.audio().SuspendAll();
    micropixel_event_t stop_event{};
    stop_event.size = sizeof(stop_event);
    stop_event.event_id = MICROPIXEL_CORE_EVENT_STOP;
    stop_event.timestamp_us = timers_.Now();
    stop_event.sequence = ++core_sequence_;
    stop_event.status = MICROPIXEL_STATUS_OK;
    const bool requested = events_.RequestStop(stop_event);
    if (requested) {
        ESP_LOGI(kTag, "Guest cooperative stop requested: event=%" PRIu32, core_sequence_);
    }
    return requested;
}

void GuestContext::ForceStop() {
    direct_surface_.Shutdown();
    touch_events_.Suspend();
    key_events_.Suspend();
    sensors_.Suspend();
    gpio_.Suspend();
    haptics_.Suspend();
    (void)timers_.Suspend();
    (void)devices_.audio().SuspendAll();
    events_.Close();
}

bool GuestContext::ResolveTextureForGraphics(void* context, micropixel_texture_handle_t texture_handle,
                                             device::BitmapView& view_out) {
    return context != nullptr && static_cast<GuestContext*>(context)->ResolveTexture(texture_handle, view_out);
}

bool GuestContext::RetainTextureForGraphics(void* context, micropixel_texture_handle_t texture_handle) {
    return context != nullptr && static_cast<GuestContext*>(context)->resources_.RetainSceneTexture(texture_handle);
}

void GuestContext::ReleaseTextureForGraphics(void* context, micropixel_texture_handle_t texture_handle) {
    if (context != nullptr) {
        static_cast<GuestContext*>(context)->resources_.ReleaseSceneTexture(texture_handle);
    }
}

device::TextureAccess GuestContext::GraphicsTextureAccess() {
    return device::TextureAccess{
        .context = this,
        .resolve = ResolveTextureForGraphics,
        .retain = RetainTextureForGraphics,
        .release = ReleaseTextureForGraphics,
    };
}

device::DeviceResult<micropixel_graphics_info_t> GuestContext::GraphicsInfo() const {
    auto info = devices_.graphics().GetInfo();
    if (info) {
        info->max_raster_bytes = RasterAvailable() ? device::graphics_limits::kMaxRasterBytes : 0U;
    }
    return info;
}

device::DeviceResult<void> GuestContext::GraphicsSubmit(const uint8_t* bytes, uint32_t length) {
    uint64_t touch_sample_us = touch_events_.TakeSampleForGraphics();
    const device::TextureAccess textures = GraphicsTextureAccess();
    auto result = devices_.graphics().Submit(bytes, length, textures);
    if (result) {
        touch_events_.NoteGraphicsSubmitComplete(touch_sample_us);
    }
    return result;
}

ServiceResult<micropixel_font_info_t> GuestContext::LoadFont(uint32_t resource_id) {
    auto resource = resources_.FindFont(resource_id);
    if (!resource) {
        return FailService<micropixel_font_info_t>(resource.error().status);
    }
    auto result = devices_.graphics().LoadFont(*resource);
    return result ? ServiceResult<micropixel_font_info_t>{*result}
                  : FailService<micropixel_font_info_t>(result.error().status);
}

ServiceResult<void> GuestContext::ReleaseFont(micropixel_font_handle_t font_handle) {
    auto result = devices_.graphics().ReleaseFont(font_handle);
    return result ? ServiceResult<void>{} : FailService<void>(result.error().status);
}

ServiceResult<micropixel_text_metrics_t> GuestContext::MeasureText(micropixel_font_handle_t font_handle,
                                                                   const char* text, uint32_t text_length) {
    auto result = devices_.graphics().MeasureText(font_handle, text, text_length);
    return result ? ServiceResult<micropixel_text_metrics_t>{*result}
                  : FailService<micropixel_text_metrics_t>(result.error().status);
}

}  // namespace micropixel::runtime
