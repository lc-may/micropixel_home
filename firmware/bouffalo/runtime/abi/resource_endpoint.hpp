// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "runtime/abi/service_registry.hpp"

namespace micropixel::runtime {
class BitmapStore;
}

namespace micropixel::bouffalo {
struct GuestMemory;

class ResourceEndpoint final : public runtime::ServiceHandler {
   public:
    ResourceEndpoint(runtime::BitmapStore& bitmaps, const GuestMemory& memory) : bitmaps_(bitmaps), memory_(memory) {}

    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response, uint32_t capacity,
                 uint32_t& size) override;

   private:
    runtime::BitmapStore& bitmaps_;
    const GuestMemory& memory_;
};
}  // namespace micropixel::bouffalo
