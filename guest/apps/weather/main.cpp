// SPDX-License-Identifier: Apache-2.0
#include "weather_client.hpp"
namespace weather {
struct Model {
    struct Saved {
        uint32_t version{1}, city{};
        Reading reading{};
    } saved;
    bool failed{}, busy{};
    uint32_t revision{};
    explicit Model(Application& app) {
        auto n = app.storage().GetBytes("weather.v1", reinterpret_cast<uint8_t*>(&saved), sizeof(saved));
        if (!n || *n != sizeof(saved) || saved.version != 1 || saved.city >= 3) saved = {};
    }
    void Save(const Application& app) {
        (void)app.storage().SetBytes("weather.v1", reinterpret_cast<const uint8_t*>(&saved), sizeof(saved));
    }
    FixedString<96> Text() const {
        FixedString<96> text;
        if (!saved.reading.utc)
            text.Append("等待首次刷新");
        else {
            auto t = saved.reading.temperature;
            text.AppendInt(t / 10);
            text.Append(".");
            text.AppendUint(t < 0 ? (-t) % 10 : t % 10);
            text.Append(" °C  · 天气代码 ");
            text.AppendInt(saved.reading.code);
        }
        return text;
    }
    void Publish(const Application& app) {
        auto text = Text();
        auto result = app.widgets().Publish(1, {.kind = WidgetTemplate::kWeather,
                                                .title = cities[saved.city].name,
                                                .subtitle = std::string_view(text.c_str()),
                                                .updated_utc = saved.reading.utc,
                                                .valid_for_seconds = 1800,
                                                .value = saved.reading.temperature,
                                                .auxiliary = saved.reading.code,
                                                .failed = failed});
        (void)result;
        ++revision;
    }
};
class WeatherService final : public AppService {
   public:
    explicit WeatherService(Model& model) : model_(model) {}
    void OnStart(ComponentContext& context) override {
        auto& app = context.app();
        app_ = &app;
        timer_.emplace(app.timers().Every(Duration::Milliseconds(100)).value());
        model_.Publish(app);
        auto now = app.jobs().Now();
        if (now && now->valid) {
            auto registered = app.jobs().Register({.deadline_ms = Budget(), .next_utc = now->utc});
            if (!registered) app.log().Info("WEATHER job registration failed");
        }
        app.log().Info("WEATHER service ready");
    }
    void OnStop(ComponentContext&) override {
        client_.Cancel();
        timer_.reset();
    }
    void OnEvent(ComponentContext&, const Event& event) override {
        if (auto* job = event.job()) {
            if (job->cancelled()) {
                if (job->ticket == active_) {
                    client_.Cancel();
                    Finish(false);
                } else {
                    if (queued_ == job->ticket) queued_ = 0;
                    (void)app_->jobs().Complete(job->ticket);
                }
                return;
            }
            if (active_) {
                queued_ = job->ticket;
                return;
            }
            Start(job->ticket);
            return;
        }
        if (auto* incoming = event.message()) {
            auto m = app_->messages().Read(incoming->ticket);
            if (!m) return;
            if (m->type != static_cast<uint16_t>(AppAction::kRefresh) || message_) {
                (void)app_->messages().Ack(incoming->ticket, -4);
                return;
            }
            auto now = app_->jobs().Now();
            if (!now || !now->valid) {
                (void)app_->messages().Ack(incoming->ticket, -10);
                return;
            }
            auto registered =
                app_->jobs().Register({.id = 2, .interval_seconds = 0, .deadline_ms = Budget(), .next_utc = now->utc});
            if (!registered) {
                (void)app_->messages().Ack(incoming->ticket, -5);
                return;
            }
            message_ = incoming->ticket;
            return;
        }
        if (timer_ && event.TimerFrom(*timer_) && active_) {
            Reading reading{};
            int result = client_.Poll(reading);
            if (!result) return;
            if (result > 0 && active_city_ != model_.saved.city) result = -1;
            if (result > 0) {
                model_.saved.reading = reading;
                model_.Save(*app_);
            }
            Finish(result > 0);
        }
    }

   private:
    uint32_t Budget() const {
        const char* value = app_->launch_arguments().FindValue("--weather-budget-ms");
        if (value) {
            uint32_t n = 0;
            for (; *value >= '0' && *value <= '9'; ++value) n = n * 10 + *value - '0';
            if (n >= 1000 && n <= 60000) return n;
        }
        return 20000;
    }
    void Start(uint32_t ticket) {
        active_ = ticket;
        active_city_ = model_.saved.city;
        auto entered = app_->jobs().Enter(ticket);
        if (!entered) {
            (void)app_->jobs().Complete(ticket);
            active_ = 0;
            return;
        }
        model_.busy = true;
        ++model_.revision;
        if (!client_.Start(*app_, model_.saved.city))
            Finish(false);
        else
            app_->log().Info("WEATHER refresh started");
    }
    void Finish(bool success) {
        client_.Cancel();
        model_.busy = false;
        model_.failed = !success;
        model_.Publish(*app_);
        (void)app_->jobs().Complete(active_);
        active_ = 0;
        app_->log().Info(success ? "WEATHER refresh success" : "WEATHER refresh failed; previous value retained");
        if (message_) {
            (void)app_->messages().Ack(message_, success ? 0 : -5);
            message_ = 0;
        }
        if (queued_) {
            auto next = queued_;
            queued_ = 0;
            Start(next);
        }
    }
    Model& model_;
    const Application* app_{};
    WeatherClient client_;
    std::optional<Timer> timer_;
    uint32_t active_{}, queued_{}, message_{}, active_city_{};
};
class WeatherActivity final : public Activity {
   public:
    explicit WeatherActivity(Model& m) : model_(m) {}
    void OnCreate(ComponentContext& context) override {
        auto& app = context.app();
        scene_.emplace(app.renderer().CreateScene(Color::Rgb(241, 245, 251)).value());
        (void)scene_->CreateLabel({32, 40}, "天气", Color::Black(), SystemFont::kTitle).value();
        city_ =
            scene_->CreateLabel({32, 140}, cities[model_.saved.city].name, Color::Black(), SystemFont::kTitle).value();
        value_ = scene_->CreateLabel({32, 240}, "", Color::Black(), SystemFont::kLarge).value();
        status_ = scene_->CreateLabel({32, 320}, "", Color::Black()).value();
        refresh_ = scene_->CreateTextButton({.bounds = {32, 420, 300, 64}, .text = "刷新天气"});
        change_ = scene_->CreateTextButton({.bounds = {360, 420, 320, 64}, .text = "切换城市"});
        (void)scene_->CreateLabel({32, 570}, "数据：Open-Meteo.com · CC BY 4.0", Color::Black(), SystemFont::kSmall)
            .value();
        (void)scene_
            ->CreateLabel({32, 620}, "关闭应用后停止自动刷新；卡片刷新可重新开启", Color::Black(), SystemFont::kSmall)
            .value();
        timer_.emplace(app.timers().Every(Duration::Milliseconds(250)).value());
        Draw(app);
        app.log().Info("WEATHER activity created");
    }
    void OnResume(ComponentContext& c) override { Draw(c.app()); }
    void OnStop(ComponentContext&) override {
        refresh_.Reset();
        change_.Reset();
    }
    void OnDestroy(ComponentContext&) override {
        timer_.reset();
        scene_.reset();
    }
    void OnEvent(ComponentContext& context, const Event& event) override {
        auto& app = context.app();
        if (auto* touch = event.touch()) {
            bool refresh = refresh_.OnTouch(*touch).clicked;
            if (change_.OnTouch(*touch).clicked) {
                model_.saved.city = (model_.saved.city + 1) % 3;
                model_.saved.reading = {};
                model_.Save(app);
                ++model_.revision;
                refresh = true;
            }
            if (refresh) (void)app.messages().Send("micropixel.weather", static_cast<uint16_t>(AppAction::kRefresh));
            Draw(app);
        } else if (timer_ && event.TimerFrom(*timer_) && seen_ != model_.revision)
            Draw(app);
    }

   private:
    void Draw(const Application& app) {
        if (!scene_) return;
        auto text = model_.Text();
        city_.SetText(cities[model_.saved.city].name);
        value_.SetText(text.c_str());
        status_.SetText(model_.busy ? "正在刷新…" : model_.failed ? "刷新失败，保留上次天气" : "每 15 分钟后台刷新");
        app.renderer().Present(*scene_).value();
        seen_ = model_.revision;
    }
    Model& model_;
    std::optional<Scene> scene_;
    std::optional<Timer> timer_;
    LabelNode city_, value_, status_;
    ui::TextButton refresh_, change_;
    uint32_t seen_{};
};
}  // namespace weather
int main() {
    micropixel::Application app;
    weather::Model model(app);
    weather::WeatherActivity page(model);
    weather::WeatherService service(model);
    app.RunComponents(&page, &service);
    return 0;
}
