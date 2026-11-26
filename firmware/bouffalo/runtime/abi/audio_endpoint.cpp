// SPDX-License-Identifier: Apache-2.0
#include "audio_endpoint.hpp"

#include <algorithm>
#include <cstring>

#include "platform/audio/audio_engine.hpp"

namespace micropixel::bouffalo {
namespace {
using runtime::ServiceResult;

template <typename T>
bool ReadRequest(const uint8_t* request, uint32_t size, T& value) {
    if (request == nullptr || size < sizeof(value)) return false;
    std::memcpy(&value, request, sizeof(value));
    return value.size == size;
}

template <typename T>
int32_t WriteResponse(const T& value, uint8_t* response) {
    std::memcpy(response, &value, sizeof(value));
    return MICROPIXEL_STATUS_OK;
}

template <typename T>
int32_t WriteResponse(const ServiceResult<T>& result, uint8_t* response) {
    return result ? WriteResponse(*result, response) : result.error().status;
}

}  // namespace

AudioEndpoint::AudioEndpoint(platform::audio::AudioEngine& engine, runtime::PcmStreamService& pcm, uint32_t owner,
                             device::UrlMedia* media)
    : audio_(engine), pcm_(pcm), owner_(owner), media_(media) {}

int32_t AudioEndpoint::Stop() {
    media_.Close();
    pcm_.CloseAll();
    return audio_.StopTonesForOwner(owner_);
}

runtime::ServiceDescriptor AudioEndpoint::Describe() const {
    return {
        .service_id = MICROPIXEL_SERVICE_AUDIO,
        .interface_major = MICROPIXEL_AUDIO_INTERFACE_MAJOR,
        .interface_minor = MICROPIXEL_AUDIO_INTERFACE_MINOR,
        .flags = MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS,
        .capabilities =
            MICROPIXEL_AUDIO_CAPABILITY_PCM_STREAM | (media_.Available() ? MICROPIXEL_AUDIO_CAPABILITY_URL_MEDIA : 0U),
        .max_request_bytes = MICROPIXEL_AUDIO_PCM_MAX_WRITE_BYTES,
        .max_response_bytes = std::max(sizeof(micropixel_audio_info_t), sizeof(micropixel_media_state_t)),
    };
}

int32_t AudioEndpoint::Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response,
                            uint32_t capacity, uint32_t& size) {
    if (method >= MICROPIXEL_AUDIO_METHOD_MEDIA_OPEN && method <= MICROPIXEL_AUDIO_METHOD_MEDIA_CLOSE)
        return media_.Call(method, request, length, response, capacity, size);
    uint32_t required = 0U;
    if (method == MICROPIXEL_AUDIO_METHOD_GET_INFO) required = sizeof(micropixel_audio_info_t);
    if (method == MICROPIXEL_AUDIO_METHOD_PCM_STREAM_OPEN)
        required = sizeof(micropixel_audio_pcm_stream_open_response_t);
    if (method == MICROPIXEL_AUDIO_METHOD_PCM_STREAM_WRITE)
        required = sizeof(micropixel_audio_pcm_stream_write_response_t);
    if (required > capacity) {
        size = required;
        return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    }
    const int32_t status = Dispatch(method, request, length, response);
    if (status == MICROPIXEL_STATUS_OK) size = required;
    return status;
}

int32_t AudioEndpoint::Dispatch(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response) {
    switch (method) {
        case MICROPIXEL_AUDIO_METHOD_GET_INFO: {
            if (length != 0U) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            micropixel_audio_info_t info{};
            const int32_t status = audio_.GetInfo(info);
            if (status != MICROPIXEL_STATUS_OK) return status;
            info.capabilities = MICROPIXEL_AUDIO_CAPABILITY_PCM_STREAM |
                                (media_.Available() ? MICROPIXEL_AUDIO_CAPABILITY_URL_MEDIA : 0U);
            info.max_clips = 0U;
            info.max_playbacks = 0U;
            return WriteResponse(info, response);
        }
        case MICROPIXEL_AUDIO_METHOD_TONE_PLAY: {
            micropixel_audio_tone_t tone{};
            if (length != sizeof(tone) || !ReadRequest(request, length, tone)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            return audio_.PlayToneForOwner(tone, owner_);
        }
        case MICROPIXEL_AUDIO_METHOD_STOP_ALL:
            if (length != 0U) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return Stop();
        case MICROPIXEL_AUDIO_METHOD_PCM_STREAM_OPEN: {
            micropixel_audio_pcm_stream_open_request_t open{};
            if (length != sizeof(open) || !ReadRequest(request, length, open)) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            return WriteResponse(pcm_.Open(open), response);
        }
        case MICROPIXEL_AUDIO_METHOD_PCM_STREAM_WRITE: {
            micropixel_audio_pcm_stream_write_request_t write{};
            if (!ReadRequest(request, length, write) || reinterpret_cast<uintptr_t>(request) % alignof(int16_t) != 0U)
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            return WriteResponse(
                pcm_.Write(write, reinterpret_cast<const int16_t*>(request + sizeof(write)), length - sizeof(write)),
                response);
        }
        case MICROPIXEL_AUDIO_METHOD_PCM_STREAM_CLOSE: {
            micropixel_handle_request_t close{};
            if (length != sizeof(close) || !ReadRequest(request, length, close) || close.reserved0 != 0U) {
                return MICROPIXEL_STATUS_INVALID_ARGUMENT;
            }
            const auto result = pcm_.Close(close.handle);
            return result ? MICROPIXEL_STATUS_OK : result.error().status;
        }
        default:
            return MICROPIXEL_STATUS_UNSUPPORTED;
    }
}

}  // namespace micropixel::bouffalo
