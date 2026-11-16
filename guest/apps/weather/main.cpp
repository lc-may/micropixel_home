// SPDX-License-Identifier: Apache-2.0
#include "sdk/calendar.hpp"
#include "sdk/ui/panel.hpp"
#include "weather_client.hpp"
namespace weather {
struct Model {
    struct Saved {
        uint32_t version{2}, city{};
        Reading reading{};
    } saved;
    bool failed{}, busy{};
    uint32_t revision{};
    explicit Model(Application& app) {
        auto n = app.storage().GetBytes("weather.v1", reinterpret_cast<uint8_t*>(&saved), sizeof(saved));
        if (n && *n == 24 && saved.version == 1)
            saved.version = 2;
        else if (!n || *n != sizeof(saved) || saved.version != 2 || saved.city >= 3)
            saved = {};
        if (saved.city >= 3) saved = {};
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
            if (t < 0 && t > -10) text.Append("-");
            text.AppendInt(t / 10);
            text.Append(".");
            text.AppendUint(t < 0 ? (-t) % 10 : t % 10);
            text.Append(" °C · ");
            text.Append(ui::panel::WeatherName(saved.reading.code));
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
    explicit WeatherActivity(Model& model) : model_(model) {}
    void OnCreate(ComponentContext& context) override {
        const auto& app = context.app();
        app.renderer()
            .ConfigureDisplay({.logical_size = {720, 720}, .scale_mode = DisplayScaleMode::kAspectFit})
            .value();
        scene_.emplace(app.renderer().CreateScene(ui::panel::Background).value());
        using namespace ui::panel;
        (void)scene_->CreateLabel({28, 26}, "天气", Text, SystemFont::kTitle).value();
        (void)scene_->CreateLabel({572, 36}, "实时天气", Muted, SystemFont::kMedium).value();
        for (unsigned i = 0; i < 3; ++i)
            cities_[i] = scene_->CreateTextButton(
                {.bounds = {24 + int(i) * 232, 92, 208, 52}, .text = cities[i].name, .style = Style()});
        Card(*scene_, {24, 164, 672, 304});
        number_ = Digits::Create(*scene_, {60, 246}, 96, Text, 5);
        city_ = scene_->CreateLabel({56, 192}, " ", Muted, SystemFont::kLarge).value();
        unit_ = scene_->CreateLabel({350, 270}, "°C", Muted, SystemFont::kTitle).value();
        glyph_ = WeatherGlyph::Create(*scene_, {480, 242}, 128);
        condition_ = scene_->CreateLabel({56, 392}, "等待天气", Text, SystemFont::kLarge).value();
        for (unsigned i = 0; i < 3; ++i) {
            Card(*scene_, {24 + int(i) * 232, 488, 208, 102});
        }
        (void)scene_->CreateLabel({44, 506}, "湿度", Muted, SystemFont::kSmall).value();
        (void)scene_->CreateLabel({276, 506}, "风速", Muted, SystemFont::kSmall).value();
        (void)scene_->CreateLabel({508, 506}, "更新时间", Muted, SystemFont::kSmall).value();
        humidity_ = scene_->CreateLabel({44, 540}, "--", Text, SystemFont::kLarge).value();
        wind_ = scene_->CreateLabel({276, 540}, "--", Text, SystemFont::kLarge).value();
        updated_ = scene_->CreateLabel({508, 540}, "--:--", Text, SystemFont::kLarge).value();
        refresh_ = scene_->CreateTextButton({.bounds = {24, 612, 672, 56}, .text = "刷新天气", .style = Style(true)});
        status_ = scene_->CreateLabel({28, 682}, "Open-Meteo · CC BY 4.0", Muted, SystemFont::kSmall).value();
        timer_.emplace(app.timers().Every(Duration::Milliseconds(250)).value());
        Draw(app);
        app.log().Info("WEATHER activity created");
    }
    void OnResume(ComponentContext& c) override { Draw(c.app()); }
    void OnStop(ComponentContext&) override {
        refresh_.Reset();
        for (auto& b : cities_) b.Reset();
    }
    void OnDestroy(ComponentContext&) override {
        timer_.reset();
        scene_.reset();
    }
    void OnEvent(ComponentContext& context, const Event& event) override {
        const auto& app = context.app();
        if (auto* touch = event.touch()) {
            bool refresh = refresh_.OnTouch(*touch).clicked;
            for (unsigned i = 0; i < 3; ++i)
                if (cities_[i].OnTouch(*touch).clicked && model_.saved.city != i) {
                    model_.saved.city = i;
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
        using namespace ui::panel;
        const auto& reading = model_.saved.reading;
        FixedString<16> temperature;
        if (reading.utc)
            temperature.AppendFixed(float(reading.temperature) / 10, reading.temperature % 10 ? 1 : 0);
        else
            temperature.Append("--");
        number_.Set(temperature.c_str());
        glyph_.Set(reading.code, reading.utc != 0);
        city_.SetText(cities[model_.saved.city].name);
        condition_.SetText(!reading.utc ? "等待首次刷新" : WeatherName(reading.code));
        FixedString<32> humidity, wind;
        if (reading.humidity >= 0) {
            humidity.AppendInt(reading.humidity);
            humidity.Append(" %");
        } else
            humidity.Append("--");
        if (reading.wind_tenths >= 0) {
            wind.AppendFixed(float(reading.wind_tenths) / 10, 1);
            wind.Append(" km/h");
        } else
            wind.Append("--");
        humidity_.SetText(humidity.c_str());
        wind_.SetText(wind.c_str());
        auto now = app.jobs().Now();
        auto local = calendar::LocalSeconds(reading.utc, now ? now->offset_minutes : 480);
        auto time = calendar::FormatTime(uint32_t(local % 86400) / 60);
        updated_.SetText(reading.utc ? time.c_str() : "--:--");
        refresh_.SetText(model_.busy ? "正在刷新…" : "刷新天气").value();
        refresh_.SetEnabled(!model_.busy);
        status_.SetText(model_.failed ? "刷新失败，保留上次天气 · Open-Meteo"
                                      : "每 15 分钟更新 · Open-Meteo · CC BY 4.0");
        for (unsigned i = 0; i < 3; ++i) cities_[i].SetStyle(Style(i == model_.saved.city)).value();
        app.renderer().Present(*scene_).value();
        seen_ = model_.revision;
    }
    Model& model_;
    std::optional<Scene> scene_;
    std::optional<Timer> timer_;
    ui::panel::Digits number_;
    ui::panel::WeatherGlyph glyph_;
    LabelNode city_, unit_, condition_, humidity_, wind_, updated_, status_;
    std::array<ui::TextButton, 3> cities_;
    ui::TextButton refresh_;
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
