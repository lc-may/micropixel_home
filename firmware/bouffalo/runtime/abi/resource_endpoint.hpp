// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "runtime/abi/service_registry.hpp"
#if WASM_ENABLE_AOT
#include "runtime/bundle/bundle_reader.h"
#include "runtime/services/service_result.hpp"
#endif

namespace micropixel::runtime {
class BitmapStore;
}

namespace micropixel::bouffalo {
struct GuestMemory;

class ResourceEndpoint final : public runtime::ServiceHandler {
   public:
    ResourceEndpoint(runtime::BitmapStore& bitmaps, const GuestMemory& memory) : bitmaps_(bitmaps), memory_(memory) {}

#if WASM_ENABLE_AOT
    void BindBundle(const micropixel_aot_package_t& package) { bundle_ = &package; }
#endif
    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t method, const uint8_t* request, uint32_t length, uint8_t* response, uint32_t capacity,
                 uint32_t& size) override;

   private:
#if WASM_ENABLE_AOT
    runtime::ServiceResult<micropixel_texture_handle_t> LoadBitmap(const micropixel_texture_load_request_t& wire,
                                                                   uint32_t& width, uint32_t& height);
    const micropixel_aot_package_t* bundle_{};
#endif
    runtime::BitmapStore& bitmaps_;
    const GuestMemory& memory_;
};
}  // namespace micropixel::bouffalo
