// SPDX-License-Identifier: Apache-2.0
#include "app_storage_endpoint.hpp"

#include <algorithm>
#include <cstring>

#include "bflb_mtimer.h"
#if defined(CONFIG_LITTLEFS)
#include "lfs.h"
#include "platform/storage/file_store.h"
#endif

namespace micropixel::bouffalo {

runtime::ServiceDescriptor AppStorageEndpoint::Describe() const {
    return {.service_id = MICROPIXEL_SERVICE_STORAGE,
            .interface_major = 1,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL,
            .max_request_bytes = sizeof(micropixel_storage_set_request_t) + MICROPIXEL_STORAGE_MAX_KEY_BYTES +
                                 MICROPIXEL_STORAGE_MAX_VALUE_BYTES,
            .max_response_bytes = MICROPIXEL_STORAGE_MAX_VALUE_BYTES};
}

bool AppStorageEndpoint::ConsumeWriteToken() {
    constexpr uint64_t kRefillUs = 1'000'000;
    const uint64_t now = bflb_mtimer_get_time_us();
    const auto elapsed = (now - last_refill_us_) / kRefillUs;
    tokens_ = std::min<uint64_t>(8, tokens_ + elapsed);
    last_refill_us_ += elapsed * kRefillUs;
    if (!tokens_) return false;
    --tokens_;
    return true;
}

int32_t AppStorageEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response,
                                 uint32_t capacity, uint32_t& written) {
#if defined(CONFIG_LITTLEFS)
    if (!app_id_) return MICROPIXEL_STATUS_PERMISSION_DENIED;
    if (method != MICROPIXEL_STORAGE_METHOD_SET && method != MICROPIXEL_STORAGE_METHOD_GET &&
        method != MICROPIXEL_STORAGE_METHOD_REMOVE)
        return MICROPIXEL_STATUS_UNSUPPORTED;
    const bool set = method == MICROPIXEL_STORAGE_METHOD_SET;
    const size_t header = set ? sizeof(micropixel_storage_set_request_t) : sizeof(micropixel_storage_key_request_t);
    if (!request || length < header) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    micropixel_storage_set_request_t wire{};
    std::memcpy(&wire, request, header);
    if (wire.size != length || !wire.key_length || wire.key_length > MICROPIXEL_STORAGE_MAX_KEY_BYTES ||
        wire.value_length > MICROPIXEL_STORAGE_MAX_VALUE_BYTES ||
        header + wire.key_length + wire.value_length != length)
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    char key[MICROPIXEL_STORAGE_MAX_KEY_BYTES + 1]{};
    for (size_t i = 0; i < wire.key_length; ++i) {
        const auto c = request[header + i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.'))
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        key[i] = c;
    }
    if (!std::strcmp(key, ".") || !std::strcmp(key, "..")) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    const bool get = method == MICROPIXEL_STORAGE_METHOD_GET;
    if (!get && !ConsumeWriteToken()) return MICROPIXEL_STATUS_RATE_LIMITED;
    int status;
    if (get) {
        size_t required = 0;
        status = mpx_app_data_read(app_id_, key, response, capacity, &required);
        if (!status || status == LFS_ERR_NOSPC) written = required;
    } else if (set) {
        status = mpx_app_data_write(app_id_, key, request + header + wire.key_length, wire.value_length);
    } else {
        status = mpx_app_data_remove(app_id_, key);
    }
    if (status == LFS_ERR_NOENT) return MICROPIXEL_STATUS_NOT_FOUND;
    if (status == LFS_ERR_INVAL) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (status == LFS_ERR_NOSPC) return get ? MICROPIXEL_STATUS_BUFFER_TOO_SMALL : MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    return status ? MICROPIXEL_STATUS_INTERNAL : MICROPIXEL_STATUS_OK;
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
