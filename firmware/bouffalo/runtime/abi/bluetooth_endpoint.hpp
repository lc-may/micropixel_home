// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "runtime/abi/service_registry.hpp"

namespace micropixel::bouffalo {

class BluetoothEndpoint final : public runtime::ServiceHandler {
   public:
    void Authorize(bool settings, bool speaker) {
        settings_ = settings;
        speaker_ = speaker;
    }
    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response, uint32_t capacity,
                 uint32_t& size) override;

   private:
    bool settings_{};
    bool speaker_{};
};

}  // namespace micropixel::bouffalo
