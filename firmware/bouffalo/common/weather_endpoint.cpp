// SPDX-License-Identifier: Apache-2.0
#include "weather_endpoint.hpp"

#include <cstring>
#include <string_view>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_widgets.hpp"

#if defined(CONFIG_LITTLEFS)
#include "file_store.h"
#include "lfs.h"
#endif

namespace micropixel::bouffalo {
namespace {

constexpr char kCacheName[] = "weather.v1";
constexpr char kMusicAccount[] = "ncm.auth";
constexpr char kNotesDoneKey[] = "notes.done";
constexpr char kNotesDoneFile[] = "clock.notes";
constexpr uint32_t kMaxAccountBytes = 3072;

}  // namespace

void WeatherState::Update(const micropixel_widget_snapshot_t& value) {
    taskENTER_CRITICAL();
    value_ = value;
    ++revision_;
    taskEXIT_CRITICAL();
}

bool WeatherState::Read(micropixel_widget_snapshot_t& value, uint32_t& revision) const {
    taskENTER_CRITICAL();
    const bool changed = revision != revision_;
    if (changed) {
        value = value_;
        revision = revision_;
    }
    taskEXIT_CRITICAL();
    return changed;
}

runtime::ServiceDescriptor HomeWidgetEndpoint::Describe() const {
    return {.service_id = MICROPIXEL_SERVICE_WIDGET,
            .interface_major = 1,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL,
            .capabilities = weather_ || media_.Available() ? 1U : 0U,
            .max_request_bytes = sizeof(micropixel_widget_snapshot_t),
            .max_response_bytes = sizeof(micropixel_handle_response_t)};
}

int32_t HomeWidgetEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response,
                                 uint32_t capacity, uint32_t& written) {
    if (method != MICROPIXEL_WIDGET_PUBLISH) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (!weather_ && !media_.Available()) return MICROPIXEL_STATUS_PERMISSION_DENIED;
    micropixel_widget_snapshot_t value{};
    if (!request || length != sizeof(value)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    std::memcpy(&value, request, sizeof(value));
    if (media_.Available()) {
        if (value.template_id != MICROPIXEL_WIDGET_MEDIA) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        return media_.Call(MICROPIXEL_SERVICE_WIDGET, method, request, length, response, capacity, written);
    }
    if (value.size != sizeof(value) || value.template_id != MICROPIXEL_WIDGET_WEATHER || value.widget_id != 1 ||
        !value.revision || value.reserved0 || !std::memchr(value.title, '\0', sizeof(value.title)) ||
        !std::memchr(value.subtitle, '\0', sizeof(value.subtitle)) || value.value < -1000 || value.value > 800)
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    written = sizeof(micropixel_handle_response_t);
    if (!response || capacity < written) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    weather_->Update(value);
    const micropixel_handle_response_t result{.size = sizeof(result), .handle = value.widget_id};
    std::memcpy(response, &result, sizeof(result));
    return MICROPIXEL_STATUS_OK;
}

runtime::ServiceDescriptor AppStorageEndpoint::Describe() const {
    return {.service_id = MICROPIXEL_SERVICE_STORAGE,
            .interface_major = 1,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL,
            .max_request_bytes =
                sizeof(micropixel_storage_set_request_t) + MICROPIXEL_STORAGE_MAX_KEY_BYTES + kMaxAccountBytes,
            .max_response_bytes = kMaxAccountBytes};
}

int32_t AppStorageEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response,
                                 uint32_t capacity, uint32_t& written) {
#if defined(CONFIG_LITTLEFS)
    if (!weather_ && !music_ && !clock_) return MICROPIXEL_STATUS_PERMISSION_DENIED;
    const std::string_view key = clock_ ? kNotesDoneKey : weather_ ? kCacheName : kMusicAccount;
    const char* const file = clock_ ? kNotesDoneFile : key.data();
    const uint32_t max_size = clock_ ? sizeof(uint32_t) : weather_ ? 512 : kMaxAccountBytes;
    if (method == MICROPIXEL_STORAGE_METHOD_SET) {
        micropixel_storage_set_request_t wire{};
        if (length < sizeof(wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        std::memcpy(&wire, request, sizeof(wire));
        if (wire.size != length || wire.key_length != key.size() || wire.value_length > max_size ||
            (clock_ && wire.value_length != max_size) || sizeof(wire) + wire.key_length + wire.value_length != length ||
            std::string_view(reinterpret_cast<const char*>(request + sizeof(wire)), wire.key_length) != key)
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        const auto* value = request + sizeof(wire) + wire.key_length;
        return mpx_file_write(file, value, wire.value_length) ? MICROPIXEL_STATUS_INTERNAL : MICROPIXEL_STATUS_OK;
    }
    if (method != MICROPIXEL_STORAGE_METHOD_GET && method != MICROPIXEL_STORAGE_METHOD_REMOVE)
        return MICROPIXEL_STATUS_UNSUPPORTED;
    micropixel_storage_key_request_t wire{};
    if (length != sizeof(wire) + key.size()) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    std::memcpy(&wire, request, sizeof(wire));
    if (wire.size != length || wire.key_length != key.size() ||
        std::string_view(reinterpret_cast<const char*>(request + sizeof(wire)), wire.key_length) != key)
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    size_t read = 0;
    const int result = method == MICROPIXEL_STORAGE_METHOD_GET ? mpx_file_read(file, response, capacity, &read)
                                                               : mpx_file_remove(file);
    if (!result || result == LFS_ERR_NOSPC) written = read;
    if (result == LFS_ERR_NOENT) return MICROPIXEL_STATUS_NOT_FOUND;
    if (result == LFS_ERR_NOSPC) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    return result ? MICROPIXEL_STATUS_INTERNAL : MICROPIXEL_STATUS_OK;
#else
    (void)method;
    (void)request;
    (void)length;
    (void)response;
    (void)capacity;
    (void)written;
    return MICROPIXEL_STATUS_UNSUPPORTED;
#endif
}

}  // namespace micropixel::bouffalo
