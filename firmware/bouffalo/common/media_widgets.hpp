// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "host/controller/app_hub.hpp"
#include "runtime/abi/service_registry.hpp"

namespace micropixel::runtime {
class EventQueue;
}

namespace micropixel::bouffalo {

// The controller and Guest worker share the PC widget and message model.
class MediaWidgets final {
   public:
    static MediaWidgets* Create(uint32_t app, std::string_view id, uint64_t boot);
    void Bind(const host::AppCaller&, runtime::EventQueue&);
    void SetComponents(const host::AppCaller&, const micropixel_component_info_t&);
    void Quiesce(const host::AppCaller&);
    void Unbind(const host::AppCaller&);
    bool Read(host::WidgetSnapshot&, bool& pending);
    void Action(const host::WidgetSnapshot&, uint16_t type, uint32_t value);
    int32_t Call(const host::AppCaller&, uint32_t service, uint32_t method, const uint8_t*, uint32_t, uint8_t*,
                 uint32_t, uint32_t&);

   private:
    explicit MediaWidgets(uint64_t boot) : hub_(boot) {}
    host::AppHub hub_;
    uint64_t sequence_{};
    uint32_t ticket_{};
};

class MediaWidgetSession final {
   public:
    MediaWidgetSession(MediaWidgets* widgets, host::AppCaller caller) : widgets_(widgets), caller_(caller) {}
    bool Available() const { return widgets_ != nullptr; }
    void Bind(runtime::EventQueue& events) {
        if (widgets_) widgets_->Bind(caller_, events);
    }
    void SetScope(uint32_t token, const micropixel_component_info_t& info) {
        caller_.component = token;
        SetComponents(info);
    }
    void SetComponents(const micropixel_component_info_t& info) {
        if (widgets_) widgets_->SetComponents(caller_, info);
    }
    void Quiesce() {
        if (widgets_) widgets_->Quiesce({caller_.app, caller_.instance, 0});
    }
    void Unbind() {
        if (widgets_) widgets_->Unbind(caller_);
    }
    int32_t Call(uint32_t service, uint32_t method, const uint8_t* request, uint32_t size, uint8_t* response,
                 uint32_t capacity, uint32_t& written) {
        if (!widgets_) return MICROPIXEL_STATUS_PERMISSION_DENIED;
        return widgets_->Call(caller_, service, method, request, size, response, capacity, written);
    }

   private:
    MediaWidgets* widgets_;
    host::AppCaller caller_;
};

class MediaMessageEndpoint final : public runtime::ServiceHandler {
   public:
    explicit MediaMessageEndpoint(MediaWidgetSession& session) : session_(session) {}
    runtime::ServiceDescriptor Describe() const override;
    int32_t Call(uint32_t, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t&) override;

   private:
    MediaWidgetSession& session_;
};

}  // namespace micropixel::bouffalo
