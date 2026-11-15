// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>

#include "freertos/FreeRTOS.h"
#include "runtime/component_model.hpp"

namespace micropixel::runtime {
class GuestContext;
class Components final {
   public:
    Components(GuestContext& context, uint16_t declaration) : context_(context) { model_.declared = declaration; }
    bool Available() const { return model_.declared != 0; }
    void SetVisible(bool value, bool create = true);
    void EnsureActivity();
    void EnsureService();
    bool TakeEvent(micropixel_event_t& event);
    int32_t Call(uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t&);
    int32_t BeforeCall(uint32_t service, uint32_t method) const;
    void AfterCall(uint32_t service_handle, uint32_t service, uint32_t method, const uint8_t* request,
                   uint32_t request_size, const uint8_t* response, uint32_t response_size, int32_t status);
    micropixel_component_info_t Info() const;
    bool CanDraw() const;
    bool StopAudioScoped();
    void Wake();

   private:
    struct Resource {
        uint32_t service_handle{}, service{}, handle{}, token{}, close_method{};
    };
    void Cleanup(uint32_t token);
    static uint32_t CloseMethod(uint32_t service, uint32_t create_method);
    GuestContext& context_;
    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    ComponentModel model_{};
    std::array<Resource, 128> resources_{};
};
}  // namespace micropixel::runtime
