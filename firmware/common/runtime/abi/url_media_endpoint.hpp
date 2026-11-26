// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "device/contracts/remote.hpp"

namespace micropixel::runtime {
class UrlMediaEndpoint final {
   public:
    explicit UrlMediaEndpoint(device::UrlMedia* backend) : backend_(backend) {}
    ~UrlMediaEndpoint() { Close(); }
    bool Available() const { return backend_ != nullptr; }
    void Close() {
        if (handle_ && backend_) (void)backend_->Close(handle_);
        handle_ = 0;
    }
    int32_t Call(uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t&);

   private:
    device::UrlMedia* backend_{};
    uint32_t handle_{};
};

}  // namespace micropixel::runtime
