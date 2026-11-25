// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "sdk/application.hpp"

namespace micropixel {
namespace detail {
uint32_t EnterComponentScope(uint32_t token);
void LeaveComponentScope(uint32_t previous);
}  // namespace detail
struct ComponentStatus final {
    uint32_t activity_token{}, activity_state{}, service_token{}, service_state{}, resource_count{};
};
class ComponentContext final {
   public:
    ComponentContext(const Application& application, uint32_t token) : application_(application), token_(token) {}
    [[nodiscard]] const Application& app() const { return application_; }
    [[nodiscard]] uint32_t token() const { return token_; }
    [[nodiscard]] Result<void> StartService() const;
    [[nodiscard]] Result<void> StopService() const;
    [[nodiscard]] Result<void> CreateActivity() const;
    [[nodiscard]] Result<void> DestroyActivity() const;
    [[nodiscard]] Result<void> Exit() const;
    [[nodiscard]] Result<ComponentStatus> Status() const;
    // Explicitly select a saved component context when sharing application logic.
    // Handles created in the callback keep this owner after the scope returns.
    template <class Function>
    decltype(auto) WithScope(Function&& function) const {
        struct Guard {
            uint32_t previous;
            ~Guard() { detail::LeaveComponentScope(previous); }
        } guard{detail::EnterComponentScope(token_)};
        return function();
    }

   private:
    const Application& application_;
    uint32_t token_;
};
class Activity {
   public:
    virtual ~Activity() = default;
    virtual void OnCreate(ComponentContext&) {}
    virtual void OnResume(ComponentContext&) {}
    virtual void OnStop(ComponentContext&) {}
    virtual void OnDestroy(ComponentContext&) {}
    // Return true after handling Back within the activity.
    virtual bool OnNavigateBack(ComponentContext&) { return false; }
    virtual void OnEvent(ComponentContext&, const Event&) {}
};
class AppService {
   public:
    virtual ~AppService() = default;
    virtual void OnStart(ComponentContext&) {}
    virtual void OnStop(ComponentContext&) {}
    virtual void OnEvent(ComponentContext&, const Event&) {}
};
}  // namespace micropixel
