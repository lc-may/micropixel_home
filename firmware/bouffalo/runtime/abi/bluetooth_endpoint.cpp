// SPDX-License-Identifier: Apache-2.0
#include "bluetooth_endpoint.hpp"

#include <cstring>

#include "platform/wireless/classic_bluetooth.h"

namespace micropixel::bouffalo {
namespace {
template <typename T>
bool Request(T& value, const uint8_t* data, uint32_t size) {
    if (size != sizeof(value)) return false;
    std::memcpy(&value, data, size);
    return true;
}

template <typename T>
int32_t Reply(const T& value, uint8_t* response, uint32_t capacity, uint32_t& size) {
    size = sizeof(value);
    if (capacity < size) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    std::memcpy(response, &value, size);
    return MICROPIXEL_STATUS_OK;
}
}  // namespace

runtime::ServiceDescriptor BluetoothEndpoint::Describe() const {
#if defined(CONFIG_BT_AVRCP)
    constexpr uint32_t version = MICROPIXEL_BLUETOOTH_VERSION;
#else
    constexpr uint32_t version = MICROPIXEL_BLUETOOTH_GROUPS_VERSION;
#endif
    return {.service_id = MICROPIXEL_SERVICE_BLUETOOTH,
            .interface_major = version >> 16,
            .interface_minor = version & 0xffff,
            .flags = MICROPIXEL_SERVICE_FLAG_CALL,
            .max_request_bytes = MICROPIXEL_BLUETOOTH_PCM_BYTES + sizeof(micropixel_bluetooth_pcm_t),
            .max_response_bytes = MICROPIXEL_BLUETOOTH_PCM_BYTES};
}

int32_t BluetoothEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response,
                                uint32_t capacity, uint32_t& size) {
    size = 0;
    const bool speaker_method = method == MICROPIXEL_BT_STATUS || method == MICROPIXEL_BT_REMOTE_STATUS ||
                                method == MICROPIXEL_BT_REMOTE_KEY || method == MICROPIXEL_BT_REMOTE_TRACK;
    if (!settings_ && !(speaker_ && speaker_method)) return MICROPIXEL_STATUS_PERMISSION_DENIED;
    switch (method) {
        case MICROPIXEL_BT_STATUS:
            if (length) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return Reply(classic::Status(), response, capacity, size);
        case MICROPIXEL_BT_DEVICE: {
            micropixel_bluetooth_selection_t selection{};
            if (!Request(selection, request, length)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            micropixel_bluetooth_device_t device{};
            const auto result = classic::Device(selection, device);
            return result == MICROPIXEL_STATUS_OK ? Reply(device, response, capacity, size) : result;
        }
        case MICROPIXEL_BT_BOND: {
            uint32_t index{};
            if (!Request(index, request, length)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            micropixel_bluetooth_address_t address{};
            const auto result = classic::Bond(index, address);
            return result == MICROPIXEL_STATUS_OK ? Reply(address, response, capacity, size) : result;
        }
        case MICROPIXEL_BT_CONNECT:
        case MICROPIXEL_BT_FORGET: {
            micropixel_bluetooth_address_t address{};
            if (!Request(address, request, length)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return classic::Command(method, 0, &address);
        }
        case MICROPIXEL_BT_SET_ROLE:
        case MICROPIXEL_BT_SET_VISIBLE:
        case MICROPIXEL_BT_SET_AUDIO_ROUTE: {
            uint32_t value{};
            if (!Request(value, request, length)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return classic::Command(method, value);
        }
        case MICROPIXEL_BT_SCAN:
        case MICROPIXEL_BT_STOP_SCAN:
        case MICROPIXEL_BT_DISCONNECT:
        case MICROPIXEL_BT_PAIR:
        case MICROPIXEL_BT_OPEN_STREAM:
        case MICROPIXEL_BT_START_STREAM:
        case MICROPIXEL_BT_SUSPEND_STREAM:
            if (length) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return classic::Command(method);
        case MICROPIXEL_BT_READ_PCM: {
            micropixel_bluetooth_pcm_t pcm{};
            if (!Request(pcm, request, length)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return classic::ReadPcm(pcm.stream_revision, response, capacity, size);
        }
        case MICROPIXEL_BT_WRITE_PCM: {
            micropixel_bluetooth_pcm_t pcm{};
            if (length <= sizeof(pcm) || !Request(pcm, request, sizeof(pcm))) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return classic::WritePcm(pcm.stream_revision, request + sizeof(pcm), length - sizeof(pcm));
        }
#if defined(CONFIG_BT_AVRCP)
        case MICROPIXEL_BT_REMOTE_STATUS:
            if (length) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return Reply(classic::RemoteStatus(), response, capacity, size);
        case MICROPIXEL_BT_REMOTE_TRACK:
            if (length) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return Reply(classic::RemoteTrack(), response, capacity, size);
        case MICROPIXEL_BT_REMOTE_KEY: {
            micropixel_bluetooth_remote_key_t key{};
            if (!Request(key, request, length)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return classic::RemoteKey(key);
        }
#endif
        default:
            return MICROPIXEL_STATUS_UNSUPPORTED;
    }
}
}  // namespace micropixel::bouffalo
