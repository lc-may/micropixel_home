// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "planner.hpp"
namespace clock_app {
class ClockService final : public AppService {
   public:
    explicit ClockService(Model& model) : model_(model) {}

   private:
    void OnStart(ComponentContext& c) override {
        (void)c.app().widgets().Publish(
            1, {.kind = WidgetTemplate::kClock, .title = "日历与提醒", .subtitle = "时钟、闹钟与日程"});
        c.app().log().Info("CLOCK service ready");
    }
    void OnEvent(ComponentContext& c, const Event& event) override {
        if (auto* incoming = event.message()) {
            auto message = c.app().messages().Read(incoming->ticket);
            int status = -2;
            if (message && message->type == static_cast<uint16_t>(AppAction::kOpenPage)) {
                auto page = message->U32();
                if (page && *page <= Notes) {
                    model_.page = *page;
                    ++model_.revision;
                    status = 0;
                    c.app().log().Info("CLOCK page requested");
                }
            }
            (void)c.app().messages().Ack(incoming->ticket, status);
        }
    }
    Model& model_;
};
}  // namespace clock_app
