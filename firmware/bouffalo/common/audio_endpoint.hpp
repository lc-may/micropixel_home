// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "runtime/abi/service_registry.hpp"
#include "runtime/abi/url_media_endpoint.hpp"
#include "runtime/audio/pcm_stream_service.hpp"

namespace micropixel::platform::audio {
class AudioEngine;
}

namespace micropixel::bouffalo {

class AudioEndpoint final : public runtime::ServiceHandler {
   public:
    AudioEndpoint(platform::audio::AudioEngine& engine, runtime::PcmStreamService& pcm, uint32_t owner = 0,
                  device::UrlMedia* media = nullptr);
    int32_t Stop();
    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response, uint32_t capacity,
                 uint32_t& size) override;

   private:
    int32_t Dispatch(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response);
    platform::audio::AudioEngine& audio_;
    runtime::PcmStreamService& pcm_;
    uint32_t owner_;
    runtime::UrlMediaEndpoint media_;
};

}  // namespace micropixel::bouffalo
