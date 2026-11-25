// SPDX-License-Identifier: Apache-2.0
#include "resource_endpoint.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "guest_runtime.hpp"
#include "image_decode.h"
#include "runtime/resources/bitmap_store.hpp"

namespace micropixel::bouffalo {
namespace {

template <typename T>
bool Read(const uint8_t* bytes, uint32_t length, T& value) {
    if (length != sizeof(value)) return false;
    std::memcpy(&value, bytes, length);
    return value.size == sizeof(value) && value.reserved0 == 0;
}

}  // namespace

runtime::ServiceDescriptor ResourceEndpoint::Describe() const {
    return {.service_id = MICROPIXEL_SERVICE_RESOURCE,
            .interface_major = MICROPIXEL_RESOURCE_INTERFACE_MAJOR,
            .interface_minor = MICROPIXEL_RESOURCE_INTERFACE_MINOR,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL,
            .max_request_bytes = sizeof(micropixel_dynamic_texture_update_request_t),
            .max_response_bytes = sizeof(micropixel_texture_info_t)};
}

int32_t ResourceEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response,
                               uint32_t capacity, uint32_t& size) {
    if (method == MICROPIXEL_RESOURCE_METHOD_TEXTURE_UNLOAD) {
        micropixel_handle_request_t wire{};
        if (!Read(request, length, wire) || wire.handle == 0) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        bitmaps_.Release(wire.handle);
        return MICROPIXEL_STATUS_OK;
    }
    if (method != MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_CREATE &&
        method != MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_UPDATE &&
        method != MICROPIXEL_RESOURCE_METHOD_TEXTURE_DECODE)
        return MICROPIXEL_STATUS_UNSUPPORTED;
    size = sizeof(micropixel_texture_info_t);
    if (capacity < size) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;

    runtime::ServiceResult<micropixel_texture_handle_t> handle;
    if (method == MICROPIXEL_RESOURCE_METHOD_TEXTURE_DECODE) {
        micropixel_texture_decode_request_t wire{};
        if (!Read(request, length, wire) || !wire.bytes || !wire.length ||
            wire.length > MICROPIXEL_ENCODED_TEXTURE_MAX_BYTES)
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        const auto* bytes = memory_.Resolve(wire.bytes, wire.length);
        if (!bytes) return MICROPIXEL_STATUS_INVALID_MEMORY;
        int width, height;
        auto* pixels = mpx_decode_image(bytes, wire.length, &width, &height);
        if (!pixels) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        const uint32_t stride = uint32_t(width) * 4;
        const auto texture = bitmaps_.Add({.data = pixels,
                                           .size = stride * uint32_t(height),
                                           .width = uint32_t(width),
                                           .height = uint32_t(height),
                                           .stride = stride,
                                           .pixel_format = MICROPIXEL_PIXEL_FORMAT_BGRA8888},
                                          true);
        if (!texture) {
            heap_caps_free(pixels);
            return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
        }
        handle = texture;
    } else if (method == MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_CREATE) {
        micropixel_dynamic_texture_create_request_t wire{};
        if (!Read(request, length, wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        const auto* pixels = memory_.Resolve(wire.pixels, wire.length);
        if ((wire.pixels || wire.length || wire.pitch) && !pixels) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        handle = bitmaps_.CreateDynamic(wire.width, wire.height, wire.pixel_format, pixels, wire.length, wire.pitch);
    } else {
        micropixel_dynamic_texture_update_request_t wire{};
        if (!Read(request, length, wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        const auto* pixels = memory_.Resolve(wire.pixels, wire.length);
        if (!pixels) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        handle = bitmaps_.UpdateDynamic(wire.texture_handle, wire.x, wire.y, wire.width, wire.height, pixels,
                                        wire.length, wire.pitch);
    }
    if (!handle) return handle.error().status;
    device::BitmapView bitmap{};
    if (!bitmaps_.Resolve(*handle, bitmap)) return MICROPIXEL_STATUS_INTERNAL;
    const micropixel_texture_info_t info{.size = sizeof(info),
                                         .texture_handle = *handle,
                                         .width = bitmap.width,
                                         .height = bitmap.height,
                                         .physical_width = bitmap.width,
                                         .physical_height = bitmap.height,
                                         .pixel_format = bitmap.pixel_format,
                                         .flags = static_cast<uint16_t>(bitmap.flags)};
    std::memcpy(response, &info, size);
    return MICROPIXEL_STATUS_OK;
}

}  // namespace micropixel::bouffalo
