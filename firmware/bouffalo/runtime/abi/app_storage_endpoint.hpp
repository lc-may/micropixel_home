// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "runtime/abi/service_registry.hpp"

namespace micropixel::bouffalo {

class AppStorageEndpoint final : public runtime::ServiceHandler {
   public:
    explicit AppStorageEndpoint(const char* app_id) : app_id_(app_id) {}
    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t&) override;

   private:
    bool ConsumeWriteToken();
    const char* app_id_;
    uint64_t last_refill_us_{};
    uint32_t tokens_{8};
};

}  // namespace micropixel::bouffalo
