// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <optional>

#include "sdk/micropixel.hpp"
namespace widget_fixture {
using namespace micropixel;
class Page final : public Activity {
    void OnCreate(ComponentContext& context) override {
        scene_.emplace(context.app().renderer().CreateScene(Color::Rgb(240, 244, 249)).value());
        (void)scene_->CreateLabel({40, 80}, "Widget provider fixture", Color::Black()).value();
        context.app().renderer().Present(*scene_).value();
    }
    void OnDestroy(ComponentContext&) override { scene_.reset(); }
    std::optional<Scene> scene_;
};
class Provider final : public AppService {
   public:
    explicit Provider(WidgetTemplate kind) : kind_(kind) {}
    void OnStart(ComponentContext& context) override {
        timer_.emplace(context.app().timers().Every(Duration::Seconds(1)).value());
        Publish(context.app());
    }
    void OnStop(ComponentContext&) override { timer_.reset(); }
    void OnEvent(ComponentContext& context, const Event& event) override {
        const auto& app = context.app();
        if (const auto* incoming = event.message()) {
            auto message = app.messages().Read(incoming->ticket);
            if (!message) return;
            Assert(std::string_view(message->source_app.c_str()) == "micropixel.home",
                   "message sender was not injected by Host");
            if (message->type == static_cast<uint16_t>(AppAction::kRefresh)) {
                ++refresh_;
                Publish(app);
                app.log().Info("WIDGET_PROVIDER refreshed");
                app.messages().Ack(incoming->ticket).value();
            } else
                app.messages().Ack(incoming->ticket, -1).value();
        }
        if (timer_ && event.TimerFrom(*timer_)) Publish(app);
    }

   private:
    void Publish(const Application& app) {
        FixedString<64> text;
        text.Append(kind_ == WidgetTemplate::kWeather ? "Sunny 23 C" : "Alarm 08:00");
        text.Append(" / ");
        text.AppendUint(refresh_);
        (void)app.widgets()
            .Publish(1, {.kind = kind_,
                         .title = kind_ == WidgetTemplate::kWeather ? "Weather fixture" : "Clock fixture",
                         .subtitle = std::string_view(text.c_str()),
                         .valid_for_seconds = 60,
                         .value = 230,
                         .auxiliary = 0})
            .value();
        if (!logged_) {
            logged_ = true;
            app.log().Info(kind_ == WidgetTemplate::kWeather ? "WIDGET_PROVIDER weather ready"
                                                             : "WIDGET_PROVIDER clock ready");
        }
    }
    WidgetTemplate kind_;
    std::optional<Timer> timer_;
    unsigned refresh_{};
    bool logged_{};
};
inline int Run(WidgetTemplate kind) {
    Application app;
    Page page;
    Provider provider(kind);
    app.RunComponents(&page, &provider);
    return 0;
}
}  // namespace widget_fixture
