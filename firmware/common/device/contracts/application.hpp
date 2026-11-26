// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {
struct AppEventSink {
    void* context{};
    bool (*push)(void*, const micropixel_event_t&){};
};
// Instance-scoped Host control capabilities, supplied by the composition root.
// Implementations copy all wire data and never keep Guest linear pointers.
class ApplicationServices {
   public:
    virtual ~ApplicationServices() = default;
    virtual void BindEvents(AppEventSink) = 0;
    virtual void SetComponentScope(uint32_t) = 0;
    virtual void SetComponents(const micropixel_component_info_t&) = 0;
    virtual void Quiesce() = 0;
    virtual int32_t Call(uint32_t service, uint32_t method, const uint8_t* request, uint32_t size, uint8_t* response,
                         uint32_t capacity, uint32_t& written) = 0;
};
}  // namespace micropixel::device
