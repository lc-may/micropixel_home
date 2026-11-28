// SPDX-License-Identifier: Apache-2.0
#include "resource_endpoint.hpp"

#include <cstring>

#include "mm.h"
#include "platform/graphics/image_decode.h"
#include "runtime/resources/bitmap_store.hpp"
#if WASM_ENABLE_AOT
#include "runtime/bundle/app_store.hpp"
#endif
#include "runtime/wamr/guest_runtime.hpp"

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

#if WASM_ENABLE_AOT
runtime::ServiceResult<micropixel_texture_handle_t> ResourceEndpoint::LoadBitmap(
    const micropixel_texture_load_request_t& wire, uint32_t& width, uint32_t& height) {
    using runtime::FailService;
    if (!bundle_ || !wire.asset_id || !wire.scale_numerator || !wire.scale_denominator)
        return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);

    struct Asset {
        micropixel_bundle_asset_mapping_t lease{};
        ~Asset() { micropixel_close_asset_mapping(&lease); }
    } asset;
    if (!micropixel_bundle_open_asset(bundle_, wire.asset_id, &asset.lease))
        return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_NOT_FOUND);
    const auto& bitmap = asset.lease.asset;
    width = bitmap.width;
    height = bitmap.height;
    const auto target_dimension = [&](uint32_t dimension) {
        return (uint64_t(dimension) * wire.scale_numerator + wire.scale_denominator / 2U) / wire.scale_denominator;
    };
    const auto target_width = target_dimension(width), target_height = target_dimension(height);
    constexpr uint32_t kMaxEdge = MICROPIXEL_ENCODED_TEXTURE_MAX_EDGE;
    if (!width || !height || width > kMaxEdge || height > kMaxEdge || !target_width || !target_height ||
        target_width > kMaxEdge || target_height > kMaxEdge)
        return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);

    uint32_t stride = bitmap.stride;
    uint32_t format, pixel_bytes;
    const uint8_t* source = bitmap.data;
    AppBytes decoded;
    switch (bitmap.format) {
        case MICROPIXEL_BUNDLE_FORMAT_RAW_RGB565:
            format = MICROPIXEL_PIXEL_FORMAT_RGB565;
            pixel_bytes = 2;
            break;
        case MICROPIXEL_BUNDLE_FORMAT_RAW_BGR888:
            format = MICROPIXEL_PIXEL_FORMAT_BGR888;
            pixel_bytes = 3;
            break;
        case MICROPIXEL_BUNDLE_FORMAT_RAW_BGRA8888:
            format = MICROPIXEL_PIXEL_FORMAT_BGRA8888;
            pixel_bytes = 4;
            break;
        case MICROPIXEL_BUNDLE_FORMAT_PNG:
        case MICROPIXEL_BUNDLE_FORMAT_JPEG: {
            int decoded_width, decoded_height;
            decoded.reset(mpx_decode_image(source, bitmap.size, &decoded_width, &decoded_height));
            if (!decoded || uint32_t(decoded_width) != width || uint32_t(decoded_height) != height)
                return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
            source = decoded.get();
            format = MICROPIXEL_PIXEL_FORMAT_BGRA8888;
            pixel_bytes = 4;
            stride = width * pixel_bytes;
            break;
        }
        default:
            return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_UNSUPPORTED);
    }

    const uint32_t output_stride = target_width * pixel_bytes;
    const uint32_t output_size = output_stride * target_height;
    if (!decoded && (stride != width * pixel_bytes || uint64_t(stride) * height != bitmap.size))
        return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    AppBytes output;
    if (decoded && target_width == width && target_height == height) {
        output = std::move(decoded);
    } else {
        output.reset(static_cast<uint8_t*>(kmalloc(output_size, MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE)));
        if (!output) return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        for (uint32_t y = 0; y < target_height; ++y) {
            const auto* row = source + (y * height / target_height) * stride;
            auto* destination = output.get() + y * output_stride;
            if (target_width == width) {
                std::memcpy(destination, row, output_stride);
            } else {
                for (uint32_t x = 0; x < target_width; ++x)
                    std::memcpy(destination + x * pixel_bytes, row + (x * width / target_width) * pixel_bytes,
                                pixel_bytes);
            }
        }
    }
    const auto handle = bitmaps_.Add({.data = output.get(),
                                      .size = output_size,
                                      .width = uint32_t(target_width),
                                      .height = uint32_t(target_height),
                                      .stride = output_stride,
                                      .pixel_format = format},
                                     true);
    if (!handle) return FailService<micropixel_texture_handle_t>(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
    (void)output.release();
    return handle;
}
#endif

int32_t ResourceEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response,
                               uint32_t capacity, uint32_t& size) {
    if (method == MICROPIXEL_RESOURCE_METHOD_TEXTURE_UNLOAD) {
        micropixel_handle_request_t wire{};
        if (!Read(request, length, wire) || wire.handle == 0) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        bitmaps_.Release(wire.handle);
        return MICROPIXEL_STATUS_OK;
    }
    if (method != MICROPIXEL_RESOURCE_METHOD_TEXTURE_LOAD &&
        method != MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_CREATE &&
        method != MICROPIXEL_RESOURCE_METHOD_DYNAMIC_TEXTURE_UPDATE &&
        method != MICROPIXEL_RESOURCE_METHOD_TEXTURE_DECODE)
        return MICROPIXEL_STATUS_UNSUPPORTED;
    size = sizeof(micropixel_texture_info_t);
    if (capacity < size) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;

    runtime::ServiceResult<micropixel_texture_handle_t> handle;
    uint32_t logical_width = 0, logical_height = 0;
    if (method == MICROPIXEL_RESOURCE_METHOD_TEXTURE_LOAD) {
#if WASM_ENABLE_AOT
        micropixel_texture_load_request_t wire{};
        if (!Read(request, length, wire)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        handle = LoadBitmap(wire, logical_width, logical_height);
#else
        return MICROPIXEL_STATUS_UNSUPPORTED;
#endif
    } else if (method == MICROPIXEL_RESOURCE_METHOD_TEXTURE_DECODE) {
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
            kfree(pixels);
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
                                         .width = logical_width ? logical_width : bitmap.width,
                                         .height = logical_height ? logical_height : bitmap.height,
                                         .physical_width = bitmap.width,
                                         .physical_height = bitmap.height,
                                         .pixel_format = bitmap.pixel_format,
                                         .flags = static_cast<uint16_t>(bitmap.flags)};
    std::memcpy(response, &info, size);
    return MICROPIXEL_STATUS_OK;
}

}  // namespace micropixel::bouffalo
