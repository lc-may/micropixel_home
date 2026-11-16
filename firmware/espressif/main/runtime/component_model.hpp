// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::runtime {
enum class ActivityState : uint32_t { kNone, kCreated, kResumed, kStopped };
enum class ServiceState : uint32_t { kStopped, kStarting, kRunning, kStopping };
// The caller serializes this bounded state machine. No Guest code runs here.
class ComponentModel final {
   public:
    struct Component {
        uint32_t token{}, state{}, transition{}, command{};
        bool in_flight{};
    };
    uint16_t declared{};
    bool registered{}, visible{}, want_activity{}, want_service{};
    Component activity{}, service{};
    uint32_t current_scope{};
    bool Alive(uint32_t token) const {
        return token &&
               ((activity.token == token && activity.state != 0) || (service.token == token && service.state != 0));
    }
    bool CanSuspend() const {
        if (!declared) return true;
        if (!registered || visible || activity.in_flight || service.in_flight) return false;
        const bool activity_settled =
            want_activity ? (activity.state == 1 || activity.state == 3) : activity.state == 0;
        const bool service_settled = want_service ? service.state == 2 : service.state == 0;
        return activity_settled && service_settled;
    }
    bool Next(micropixel_component_event_payload_t& event) {
        if (!registered) return false;
        uint32_t command = 0;
        auto* target = &activity;
        if (!activity.in_flight) {
            if (want_activity && activity.state == 0)
                command = MICROPIXEL_COMPONENT_CREATE;
            else if (activity.state == 2 && (!visible || !want_activity))
                command = MICROPIXEL_COMPONENT_STOP;
            else if (activity.state && !want_activity)
                command = MICROPIXEL_COMPONENT_DESTROY;
            else if (activity.state && activity.state != 2 && visible)
                command = MICROPIXEL_COMPONENT_RESUME;
        }
        if (!command && !service.in_flight) {
            target = &service;
            if (want_service && service.state == 0)
                command = MICROPIXEL_COMPONENT_START;
            else if (!want_service && service.state == 2)
                command = MICROPIXEL_COMPONENT_STOP;
        }
        if (!command) return false;
        if (command == MICROPIXEL_COMPONENT_CREATE || command == MICROPIXEL_COMPONENT_START) {
            const uint32_t kind = target == &activity ? MICROPIXEL_COMPONENT_ACTIVITY : MICROPIXEL_COMPONENT_SERVICE;
            if (++epochs_[kind - 1] == 0 || epochs_[kind - 1] > (UINT32_MAX >> 2)) epochs_[kind - 1] = 1;
            target->token = (epochs_[kind - 1] << 2) | kind;
            target->state = 1;
        }
        if (target == &service && command == MICROPIXEL_COMPONENT_STOP) target->state = 3;
        target->command = command;
        target->in_flight = true;
        if (++sequence_ == 0) ++sequence_;
        target->transition = sequence_;
        event = {target->token, command, sequence_, 0};
        return true;
    }
    bool Ack(const micropixel_component_ack_t& ack) {
        auto* component = ack.token == activity.token ? &activity : ack.token == service.token ? &service : nullptr;
        if (!component || !component->in_flight || component->transition != ack.transition ||
            component->command != ack.command)
            return false;
        component->in_flight = false;
        if (component == &activity) {
            component->state = ack.command == MICROPIXEL_COMPONENT_DESTROY  ? 0
                               : ack.command == MICROPIXEL_COMPONENT_RESUME ? 2
                               : ack.command == MICROPIXEL_COMPONENT_STOP   ? 3
                                                                            : 1;
        } else
            component->state = ack.command == MICROPIXEL_COMPONENT_STOP ? 0 : 2;
        if (!component->state) {
            component->token = 0;
            if (current_scope == ack.token) current_scope = 0;
        }
        return true;
    }

   private:
    std::array<uint32_t, 2> epochs_{};
    uint32_t sequence_{};
};
}  // namespace micropixel::runtime
