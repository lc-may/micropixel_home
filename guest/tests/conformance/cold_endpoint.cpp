// SPDX-License-Identifier: Apache-2.0
#include <optional>

#include "sdk/micropixel.hpp"
using namespace micropixel;
class Endpoint final : public AppService {
    void OnStart(ComponentContext& context) override {
        sender_ = context.app().launch_arguments().HasFlag("--sender");
        if (sender_)
            timer_.emplace(context.app().timers().Every(Duration::Milliseconds(50)).value());
        else {
            context.app().messages().AllowColdStart("micropixel.cold.sender", 1U << 3).value();
            context.app().log().Info("COLD receiver ready");
        }
    }
    void OnEvent(ComponentContext& context, const Event& event) override {
        const auto& app = context.app();
        if (auto* message = event.message()) {
            auto payload = app.messages().Read(message->ticket).value();
            Assert(std::string_view(payload.source_app.c_str()) == "micropixel.cold.sender", "wrong cold sender");
            app.storage().SetU32("received", app.storage().GetU32Or("received", 0) + 1).value();
            app.messages().Ack(message->ticket).value();
            app.log().Info("COLD received");
        }
        if (sender_ && timer_ && event.TimerFrom(*timer_)) {
            if (!ticket_) {
                auto sent = app.messages().Send("micropixel.cold.receiver", 3, {}, 5000, 1);
                if (sent)
                    ticket_ = *sent;
                else
                    Assert(sent.error().code() == ErrorCode::kWouldBlock, "unauthorized cold send result");
            } else {
                auto result = app.messages().ResultOf(ticket_).value();
                if (result.complete) {
                    Assert(result.status == 0, "cold delivery failed");
                    app.log().Info("COLD sender PASS");
                    context.Exit().value();
                }
            }
        }
    }
    std::optional<Timer> timer_;
    bool sender_{};
    uint32_t ticket_{};
};
int main() {
    Application app;
    Endpoint endpoint;
    app.RunComponents(nullptr, &endpoint, false);
    return 0;
}
