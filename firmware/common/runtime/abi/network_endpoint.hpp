// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>

#include "device/contracts/remote.hpp"
#include "runtime/abi/service_registry.hpp"

namespace micropixel::runtime {
class NetworkServiceEndpoint final : public ServiceHandler {
   public:
    explicit NetworkServiceEndpoint(device::HttpClient* backend) : backend_(backend) {}
    ~NetworkServiceEndpoint() override {
        for (auto handle : handles_)
            if (handle && backend_) (void)backend_->Close(handle);
    }
    ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t&) override;

   private:
    device::HttpClient* backend_{};
    std::array<uint32_t, 2> handles_{};
};
}  // namespace micropixel::runtime
