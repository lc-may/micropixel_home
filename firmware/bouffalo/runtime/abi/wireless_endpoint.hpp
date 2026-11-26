// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "platform/wireless/wireless.h"
#include "runtime/abi/service_registry.hpp"

namespace micropixel::bouffalo {

class WirelessEndpoint final : public runtime::ServiceHandler {
   public:
    void Authorize(bool enabled) { enabled_ = enabled; }
    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response, uint32_t capacity,
                 uint32_t& size) override;

   private:
    mpx_wireless_status_t state_{};
    bool enabled_{};
};

}  // namespace micropixel::bouffalo
