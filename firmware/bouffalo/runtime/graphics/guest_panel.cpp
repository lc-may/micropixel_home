// SPDX-License-Identifier: Apache-2.0
#include "guest_panel.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

#include "bflb_mtimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "mm.h"
#include "platform/graphics/app_surface_compositor.hpp"
#include "platform/lvgl/fonts/bitmap_font_rasterizer.hpp"
#include "runtime/event_queue.hpp"
#include "runtime/resources/bitmap_store.hpp"

namespace micropixel::bouffalo {
namespace {

using namespace platform::graphics;
constexpr size_t kNodes = 384;
constexpr size_t kContainers = 128;
constexpr size_t kInstances = 384;
constexpr size_t kDraws = kNodes + kInstances;
constexpr size_t kMaxSceneBytes = 32768;
constexpr size_t kMaxText = 256;
constexpr uint16_t kMaxCanvasExtent = 720;
const lv_font_t* fallback_font;

lv_font_t WithFallback(lv_font_t font) {
    if (!fallback_font) return font;
    font.fallback = fallback_font;
    const auto ascent =
        std::max(font.line_height - font.base_line, fallback_font->line_height - fallback_font->base_line);
    font.base_line = std::max(font.base_line, fallback_font->base_line);
    font.line_height = ascent + font.base_line;
    return font;
}

bool ResolveBitmap(void* context, micropixel_texture_handle_t handle, device::BitmapView& view) {
    return static_cast<runtime::BitmapStore*>(context)->Resolve(handle, view);
}

template <typename T>
int32_t Reply(const T& value, uint8_t* response, uint32_t capacity, uint32_t& size) {
    size = sizeof(value);
    if (capacity < size) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    std::memcpy(response, &value, size);
    return MICROPIXEL_STATUS_OK;
}

}  // namespace

void GuestPanel::SetFontFallback(const lv_font_t& font) { fallback_font = &font; }

const lv_font_t* GuestPanel::Font(micropixel_font_handle_t font) {
    static const auto small = WithFallback(lv_font_montserrat_18);
    static const auto medium = WithFallback(lv_font_montserrat_20);
    static const auto large = WithFallback(lv_font_montserrat_24);
    switch (font) {
        case MICROPIXEL_SYSTEM_FONT_SMALL:
            return &small;
        case MICROPIXEL_SYSTEM_FONT_MEDIUM:
            return &medium;
        case MICROPIXEL_SYSTEM_FONT_LARGE:
        case MICROPIXEL_SYSTEM_FONT_TITLE:
            return &large;
        default:
            return nullptr;
    }
}

struct alignas(64) GuestPanel::Storage final {
    Storage(uint16_t width, uint16_t height, uint64_t start_us)
        : started_us(start_us),
          destination{nullptr, uint32_t(width) * height * 2U, width,
                      height,  uint32_t(width) * 2U,          SurfacePixelFormat::kRgb565},
          compositor({operations, stale, sorted, ancestry}, pixels, {64, uint32_t(width) * height}, &text_rasterizer),
          image{.header = {.magic = LV_IMAGE_HEADER_MAGIC,
                           .cf = LV_COLOR_FORMAT_RGB565,
                           .w = width,
                           .h = height,
                           .stride = uint16_t(destination.stride)},
                .data_size = uint32_t(destination.size),
                .data = nullptr},
          scale(std::min(kExtent * LV_SCALE_NONE / width, kContentHeight * LV_SCALE_NONE / height)),
          view_width((uint32_t(width) * scale + LV_SCALE_NONE - 1U) / LV_SCALE_NONE),
          view_height((uint32_t(height) * scale + LV_SCALE_NONE - 1U) / LV_SCALE_NONE),
          view_x((kExtent - view_width) / 2),
          view_y((kContentHeight - view_height) / 2) {}

    const uint64_t started_us;
    StaticSemaphore_t lock_buffer{};
    SemaphoreHandle_t lock{xSemaphoreCreateMutexStatic(&lock_buffer)};
    std::array<GuestSceneNode, kNodes * 2> nodes{};
    std::array<GuestSceneContainer, (kContainers + 1) * 2> containers{};
    std::array<GuestSceneSpriteInstance, kInstances * 2> instances{};
    std::array<uint16_t, kNodes * 2> order{};
    std::array<uint8_t, kNodes> changes{}, marks{};
    std::array<uint8_t, kContainers + 1> container_changes{}, container_marks{};
    std::array<uint8_t, kInstances> instance_changes{}, instance_marks{};
    std::array<char, kMaxSceneBytes * 4> text{};
    GuestScene scene{{nodes, instances, containers, order, changes, instance_changes, container_changes, marks,
                      instance_marks, container_marks, text}};

    runtime::BitmapStore bitmaps;
    std::array<micropixel_texture_handle_t, kNodes * 2> retained{};
    size_t retained_count{};
    void ReleaseTextures() {
        while (retained_count != 0) bitmaps.ReleaseSceneReference(retained[--retained_count]);
    }

    SoftwarePixelCompositor pixels;
    platform::lvgl::BitmapFontRasterizer text_rasterizer{
        nullptr, [](const void*, micropixel_font_handle_t handle) { return GuestPanel::Font(handle); }};
    std::array<AppDrawOperation, kDraws * 2> operations{};
    std::array<uint16_t, kDraws> stale{}, sorted{};
    std::array<uint16_t, kContainers + 1> ancestry{};
    PixelSurface destination;
    AppSurfaceCompositor compositor;
    lv_image_dsc_t image;
    const uint32_t scale, view_width, view_height;
    const int32_t view_x, view_y;
    lv_obj_t* image_view{};
    std::atomic<bool> has_frame{};
    bool dirty{};
    bool displayed{};
    bool allocation_failed{};
};

bool GuestPanel::Initialize(uint16_t canvas_extent) {
    if (storage_) return true;
    if (canvas_extent > kMaxCanvasExtent) return false;
    const auto start_us = bflb_mtimer_get_time_us();
    const uint16_t width = canvas_extent ? canvas_extent : kExtent;
    const uint16_t height = canvas_extent ? canvas_extent : kContentHeight;
    static_assert(alignof(Storage) == 64U);
    auto* memory = static_cast<Storage*>(kmalloc(sizeof(Storage), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_64BYTE));
    if (!memory) return false;
    storage_ = std::construct_at(memory, width, height, start_us);
    if (storage_->lock && storage_->bitmaps.valid()) return true;
    if (storage_->lock) vSemaphoreDelete(storage_->lock);
    std::destroy_at(storage_);
    kfree(storage_);
    storage_ = nullptr;
    return false;
}

void GuestPanel::SetVisible(bool visible) {
    if (visible || !storage_) return;
    auto& state = *storage_;
    if (state.image_view) {
        lv_obj_delete(state.image_view);
        lv_image_cache_drop(&state.image);
    }
    xSemaphoreTake(state.lock, portMAX_DELAY);
    // The retained scene is sufficient to reconstruct the image on foregrounding.
    kfree(state.destination.pixels);
    state.destination.pixels = nullptr;
    state.image.data = nullptr;
    state.compositor.Reset();
    state.dirty = state.has_frame.load(std::memory_order_acquire);
    state.displayed = false;
    state.allocation_failed = false;
    xSemaphoreGive(state.lock);
}

void GuestPanel::Release() {
    if (!storage_) return;
    if (storage_->image_view) lv_obj_delete(storage_->image_view);
    Reset();
    vSemaphoreDelete(storage_->lock);
    kfree(storage_->destination.pixels);
    std::destroy_at(storage_);
    kfree(storage_);
    storage_ = nullptr;
}

runtime::BitmapStore& GuestPanel::Bitmaps() { return storage_->bitmaps; }

bool GuestPanel::HasFrame() const { return storage_->has_frame.load(std::memory_order_acquire); }

uint16_t GuestPanel::Width() const { return storage_->destination.width; }
uint16_t GuestPanel::Height() const { return storage_->destination.height; }

void GuestPanel::MapTouch(int32_t& x, int32_t& y) const {
    const auto& state = *storage_;
    x = (x - state.view_x) * int32_t(Width()) / int32_t(state.view_width);
    y = (y - state.view_y) * int32_t(Height()) / int32_t(state.view_height);
}

void GuestPanel::Reset() {
    auto& state = *storage_;
    xSemaphoreTake(state.lock, portMAX_DELAY);
    state.ReleaseTextures();
    state.scene.Reset();
    state.compositor.Reset();
    state.bitmaps.ReleaseAll();
    state.dirty = false;
    state.displayed = false;
    state.has_frame.store(false, std::memory_order_release);
    xSemaphoreGive(state.lock);
}

runtime::ServiceDescriptor GuestPanel::Describe() const {
    return {.service_id = MICROPIXEL_SERVICE_GRAPHICS,
            .interface_major = MICROPIXEL_GRAPHICS_INTERFACE_MAJOR,
            .interface_minor = MICROPIXEL_GRAPHICS_INTERFACE_MINOR,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_SUBMIT,
            .max_request_bytes = sizeof(micropixel_graphics_measure_text_request_t) + kMaxText,
            .max_response_bytes = sizeof(micropixel_graphics_info_t),
            .max_submit_bytes = kMaxSceneBytes};
}

int32_t GuestPanel::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response, uint32_t capacity,
                         uint32_t& size) {
    if (method == MICROPIXEL_GRAPHICS_METHOD_GET_INFO && length == 0) {
        const micropixel_graphics_info_t info{.size = sizeof(info),
                                              .width = Width(),
                                              .height = Height(),
                                              .pixel_format = MICROPIXEL_PIXEL_FORMAT_RGB565,
                                              .max_full_frame_fps = 30,
                                              .max_text_bytes = kMaxText,
                                              .max_scene_bytes = kMaxSceneBytes};
        return Reply(info, response, capacity, size);
    }
    if (method != MICROPIXEL_GRAPHICS_METHOD_MEASURE_TEXT) return MICROPIXEL_STATUS_UNSUPPORTED;
    micropixel_graphics_measure_text_request_t header{};
    if (length < sizeof(header)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    std::memcpy(&header, request, sizeof(header));
    const auto* font = Font(header.font_handle);
    if (!font || header.size != length || header.text_length != length - sizeof(header) ||
        header.text_length > kMaxText)
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    char text[kMaxText + 1]{};
    std::memcpy(text, request + sizeof(header), header.text_length);
    auto& state = *storage_;
    xSemaphoreTake(state.lock, portMAX_DELAY);
    lv_point_t dimensions{};
    lv_text_get_size(&dimensions, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const micropixel_text_metrics_t metrics{.size = sizeof(metrics),
                                            .width = static_cast<uint32_t>(dimensions.x),
                                            .height = static_cast<uint32_t>(dimensions.y),
                                            .baseline = font->line_height - font->base_line};
    xSemaphoreGive(state.lock);
    return Reply(metrics, response, capacity, size);
}

int32_t GuestPanel::Submit(uint32_t channel, const uint8_t* bytes, uint32_t length) {
    if (channel != MICROPIXEL_GRAPHICS_CHANNEL_SCENE) return MICROPIXEL_STATUS_UNSUPPORTED;
    auto& state = *storage_;
    xSemaphoreTake(state.lock, portMAX_DELAY);
    if (state.scene.TextUsed() + length * 2 > state.scene.TextCapacity()) {
        state.scene.CompactText();
        state.compositor.InvalidateRetainedText();
    }
    const auto status = state.scene.Apply(
        bytes, length, Width(), Height(), ResolveBitmap, &state.bitmaps,
        [](void*, uint32_t font) { return Font(font) != nullptr; }, nullptr);
    if (status == MICROPIXEL_STATUS_OK) {
        // Retain the new frame's pixels before releasing the previous references.
        const size_t previous_count = state.retained_count;
        size_t count = 0;
        for (uint16_t i = 0; i < state.scene.NodeCount(); ++i) {
            const auto handle = state.scene.Nodes()[i].texture_handle;
            if (handle != 0 && state.bitmaps.RetainSceneReference(handle)) state.retained[kNodes + count++] = handle;
        }
        for (size_t i = 0; i < previous_count; ++i) state.bitmaps.ReleaseSceneReference(state.retained[i]);
        std::copy_n(state.retained.data() + kNodes, count, state.retained.data());
        state.retained_count = count;
        state.dirty = true;
        state.has_frame.store(true, std::memory_order_release);
    }
    xSemaphoreGive(state.lock);
    return status;
}

[[gnu::noinline]] void GuestPanel::Render(lv_obj_t* surface) {
    auto& state = *storage_;
    if (xSemaphoreTake(state.lock, 0) != pdTRUE) return;
    if (state.dirty) {
        if (!state.destination.pixels) {
            // A service-only Session needs no pixels. The first successful
            // composition redraws the entire destination before displaying it.
            state.destination.pixels =
                static_cast<uint8_t*>(kmalloc(state.destination.size, MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_64BYTE));
            if (!state.destination.pixels) {
                if (!state.allocation_failed) std::puts("Orion Guest framebuffer allocation failed");
                state.allocation_failed = true;
                xSemaphoreGive(state.lock);
                return;
            }
            state.image.data = state.destination.pixels;
            state.allocation_failed = false;
        }
        const auto frame = state.compositor.PresentScene(state.scene, state.destination, ResolveBitmap, &state.bitmaps);
        if (frame.status == AppSurfaceStatus::kOk) {
            if (!state.image_view) {
                state.image_view = lv_image_create(surface);
                if (state.image_view) {
                    lv_image_set_src(state.image_view, &state.image);
                    lv_image_set_pivot(state.image_view, 0, 0);
                    lv_image_set_scale(state.image_view, state.scale);
                    lv_obj_set_pos(state.image_view, state.view_x, state.view_y);
                    lv_obj_add_event_cb(
                        state.image_view,
                        [](lv_event_t* event) {
                            static_cast<Storage*>(lv_event_get_user_data(event))->image_view = nullptr;
                        },
                        LV_EVENT_DELETE, &state);
                }
            }
            if (state.image_view && !state.displayed) {
                printf("Orion Guest frame displayed after %lu us\n",
                       static_cast<unsigned long>(bflb_mtimer_get_time_us() - state.started_us));
                state.displayed = true;
                lv_obj_invalidate(state.image_view);
            }
            if (state.image_view && frame.visual_changed) lv_obj_invalidate(state.image_view);
        } else {
            const auto& failure = state.compositor.LastRenderFailure();
            printf("Orion Guest composition failed (%u, draw=%u, pixel format=%lu)\n",
                   static_cast<unsigned>(frame.status), static_cast<unsigned>(failure.kind),
                   static_cast<unsigned long>(failure.bitmap_format));
        }
        state.dirty = false;
    }
    xSemaphoreGive(state.lock);
}

runtime::ServiceDescriptor TouchEndpoint::Describe() const {
    return {.service_id = MICROPIXEL_SERVICE_INPUT,
            .interface_major = MICROPIXEL_INPUT_INTERFACE_MAJOR,
            .interface_minor = MICROPIXEL_INPUT_INTERFACE_MINOR,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS,
            .max_request_bytes = sizeof(micropixel_text_enable_request_t),
            .max_response_bytes = std::max(sizeof(micropixel_input_info_t), sizeof(micropixel_text_response_t))};
}

bool TouchEndpoint::WantsText() const {
    taskENTER_CRITICAL();
    const bool wants_text = enabled_ && !submitted_;
    taskEXIT_CRITICAL();
    return wants_text;
}

bool TouchEndpoint::SubmitText(std::string_view text) {
    if (text.empty() || text.size() > sizeof(text_) || text.find('\0') != text.npos) return false;
    taskENTER_CRITICAL();
    const bool accepted = enabled_ && !submitted_;
    if (accepted) {
        std::memcpy(text_, text.data(), text.size());
        read_ = 0;
        end_ = text.size();
        submitted_ = true;
    }
    taskEXIT_CRITICAL();
    return accepted;
}

void TouchEndpoint::ClearText() {
    taskENTER_CRITICAL();
    enabled_ = submitted_ = false;
    read_ = end_ = 0;
    taskEXIT_CRITICAL();
}

int32_t TouchEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response,
                            uint32_t capacity, uint32_t& size) {
    if (method == MICROPIXEL_INPUT_METHOD_TEXT_ENABLE) {
        micropixel_text_enable_request_t enable{};
        if (!request || length != sizeof(enable)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        std::memcpy(&enable, request, sizeof(enable));
        if (enable.size != sizeof(enable) || enable.enabled > 1) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        taskENTER_CRITICAL();
        enabled_ = enable.enabled;
        submitted_ = false;
        read_ = end_ = 0;
        taskEXIT_CRITICAL();
        size = 0;
        return MICROPIXEL_STATUS_OK;
    }
    if (method == MICROPIXEL_INPUT_METHOD_TEXT_READ) {
        if (length) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        micropixel_text_response_t text{.size = sizeof(text)};
        if (capacity < sizeof(text)) return Reply(text, response, capacity, size);
        taskENTER_CRITICAL();
        uint32_t stop = std::min<uint32_t>(end_, read_ + sizeof(text.bytes));
        while (stop > read_ && stop < end_ && (static_cast<uint8_t>(text_[stop]) & 0xc0) == 0x80) --stop;
        text.length = stop - read_;
        std::memcpy(text.bytes, text_ + read_, text.length);
        read_ = stop;
        const bool confirm = submitted_ && read_ == end_;
        taskEXIT_CRITICAL();
        // The Guest receives the last text chunk before processing Confirm.
        if (confirm) {
            const micropixel_key_event_payload_t key{.code = MICROPIXEL_KEY_CONFIRM,
                                                     .phase = MICROPIXEL_KEY_DOWN_PHASE};
            micropixel_event_t event{.size = sizeof(event),
                                     .event_id = MICROPIXEL_INPUT_EVENT_KEY,
                                     .service_id = MICROPIXEL_SERVICE_INPUT,
                                     .timestamp_us = uint64_t(bflb_mtimer_get_time_us() - origin_us_),
                                     .sequence = ++key_sequence_};
            std::memcpy(event.payload, &key, sizeof(key));
            if (events_.PushAdvisory(event)) ClearText();
        }
        return Reply(text, response, capacity, size);
    }
    if (method != MICROPIXEL_INPUT_METHOD_GET_INFO) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (length != 0) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    const micropixel_input_info_t info{.size = sizeof(info),
                                       .max_touch_points = 1,
                                       .capabilities = MICROPIXEL_INPUT_CAP_NAVIGATE_BACK | MICROPIXEL_INPUT_CAP_TEXT |
                                                       MICROPIXEL_INPUT_CAP_KEY_EVENTS,
                                       .logical_width = panel_.Width(),
                                       .logical_height = panel_.Height()};
    return Reply(info, response, capacity, size);
}

}  // namespace micropixel::bouffalo
