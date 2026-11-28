// SPDX-License-Identifier: Apache-2.0
#include "weather_endpoint.hpp"

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/controller/media_widgets.hpp"

namespace micropixel::bouffalo {

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

}  // namespace micropixel::bouffalo
