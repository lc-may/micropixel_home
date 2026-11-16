// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "city_picker.hpp"
#include "sdk/calendar.hpp"
#include "sdk/ui/numerals.hpp"
#include "sdk/ui/panel.hpp"
#include "sdk/ui/weather_icons.hpp"
#include "weather_client.hpp"
namespace weather {
struct Model {
    struct Saved {
        uint32_t version{4}, city{};
        Reading reading{};
    } saved;
    bool failed{}, busy{};
    uint32_t revision{};
    explicit Model(Application& app) {
        auto n = app.storage().GetBytes("weather.v1", reinterpret_cast<uint8_t*>(&saved), sizeof(saved));
        if (!n || *n != sizeof(saved) || saved.version != 4 || saved.city >= cities.size()) saved = {};
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
    void OnCreate(ComponentContext& c) override {
        const auto& app = c.app();
        app.renderer()
            .ConfigureDisplay({.logical_size = {720, 720}, .scale_mode = DisplayScaleMode::kAspectFit})
            .value();
        scene_.emplace(app.renderer().CreateScene(ui::panel::Background).value());
        font_ = std::make_unique<ui::NumeralAtlas>(app);
        icons_ = std::make_unique<ui::WeatherAtlas>(app);
        using namespace ui::panel;
        city_ = scene_->CreateLabel({24, 20}, " ", Text, SystemFont::kLarge).value();
        date_ = scene_->CreateLabel({370, 23}, " ", Muted, SystemFont::kMedium).value();
        city_hit_ = ui::Button({24, 0, 300, 84});
        refresh_ =
            ui::MaterialIconButton::Create(*scene_, *icons_, {612, 0, 84, 84}, ui::MaterialSymbol::Refresh, Background);
        number_ = ui::Numerals::Create(*scene_, *font_, {24, 78}, 126, 7);
        range_ = scene_->CreateLabel({24, 216}, " ", Text, SystemFont::kLarge).value();
        glyph_ = ui::WeatherIcon::Create(*scene_, *icons_, {480, 96}, 117);
        condition_ = scene_->CreateLabel({455, 216}, " ", Muted, SystemFont::kLarge).value();
        (void)scene_->CreateLabel({24, 250}, "逐小时预报", Muted, SystemFont::kSmall).value();
        for (unsigned i = 0; i < 5; ++i) {
            int x = 24 + int(i) * 138;
            Card(*scene_, {x, 280, 120, 132});
            hours_[i] = scene_->CreateLabel({x + 60, 291}, "--", Muted, SystemFont::kSmall, true).value();
            hourly_glyphs_[i] = ui::WeatherIcon::Create(*scene_, *icons_, {x + 42, 321}, 36);
            hour_values_[i] = scene_->CreateLabel({x + 60, 371}, "--°", Text, SystemFont::kLarge, true).value();
        }
        for (unsigned i = 0; i < 3; ++i) {
            int y = 430 + int(i) * 60;
            days_[i] = scene_->CreateLabel({24, y + 13}, "--", Muted, SystemFont::kMedium).value();
            daily_glyphs_[i] = ui::WeatherIcon::Create(*scene_, *icons_, {126, y + 9}, 36);
            (void)scene_->CreateRoundedRect({192, y + 28, 318, 7}, {.fill = Line, .radius = 4}).value();
            bars_[i] = scene_->CreateRoundedRect({200, y + 28, 270, 7}, {.fill = Blue, .radius = 4}).value();
            highs_[i] = scene_->CreateRoundedRect({460, y + 28, 18, 7}, {.fill = Accent, .radius = 4}).value();
            day_values_[i] = scene_->CreateLabel({540, y + 13}, "--° / --°", Text, SystemFont::kMedium).value();
        }
        Card(*scene_, {24, 627, 672, 69});
        humidity_ = scene_->CreateLabel({48, 650}, "湿度 --", Text, SystemFont::kMedium).value();
        wind_ = scene_->CreateLabel({320, 650}, "风速 --", Muted, SystemFont::kMedium).value();
        status_ = scene_->CreateLabel({24, 700}, "Open-Meteo · CC BY 4.0", Muted, SystemFont::kSmall).value();
        picker_ = std::make_unique<CityPicker>(*scene_, *icons_);
        timer_.emplace(app.timers().Every(Duration::Milliseconds(250)).value());
        Draw(app);
        app.log().Info("WEATHER activity created");
    }
    void OnResume(ComponentContext& c) override { Draw(c.app()); }
    void OnStop(ComponentContext&) override {
        refresh_.Reset();
        city_hit_.Reset();
        if (picker_) picker_->Cancel();
    }
    void OnDestroy(ComponentContext&) override {
        timer_.reset();
        picker_.reset();
        scene_.reset();
        font_.reset();
        icons_.reset();
    }
    void OnEvent(ComponentContext& c, const Event& event) override {
        const auto& app = c.app();
        if (auto* t = event.touch()) {
            bool refresh = false;
            bool redraw = false;
            if (picker_->visible()) {
                auto update = picker_->OnTouch(*t);
                if (update.city && *update.city != model_.saved.city) {
                    model_.saved.city = *update.city;
                    model_.saved.reading = {};
                    model_.Save(app);
                    model_.Publish(app);
                    FixedString<96> log;
                    log.Append("WEATHER city selected: ");
                    log.Append(cities[*update.city].name);
                    app.log().Info(log.c_str());
                    refresh = true;
                    redraw = true;
                } else if (update.changed)
                    app.renderer().Present(*scene_).value();
            } else {
                if (city_hit_.OnTouch(*t).clicked) {
                    picker_->Show(model_.saved.city);
                    redraw = true;
                }
                refresh = refresh_.OnTouch(*t).clicked;
            }
            if (refresh) (void)app.messages().Send("micropixel.weather", static_cast<uint16_t>(AppAction::kRefresh));
            // Icon buttons have no pressed visual. Service revisions drive refresh
            // rendering; a full redraw on every pointer event delays RV32 input.
            if (redraw) Draw(app);
        } else if (timer_ && event.TimerFrom(*timer_) && seen_ != model_.revision)
            Draw(app);
    }

   private:
    static FixedString<32> Temp(int32_t value) {
        FixedString<32> s;
        s.AppendFixed(float(value) / 10, value % 10 ? 1 : 0);
        s.Append("°");
        return s;
    }
    void Draw(const Application& app) {
        using namespace ui::panel;
        const auto& r = model_.saved.reading;
        auto value = r.utc ? Temp(r.temperature) : FixedString<32>{};
        if (!r.utc) value.Append("--°");
        number_.Set(value.c_str());
        glyph_.Set(r.code, r.utc != 0);
        FixedString<48> city;
        city.Append(cities[model_.saved.city].name);
        if (std::string_view(cities[model_.saved.city].name).size() <= 24) city.Append(" · 切换");
        city_.SetText(city.c_str());
        condition_.SetText(!r.utc ? "暂无天气" : WeatherName(r.code));
        auto now = app.jobs().Now();
        auto date = calendar::LocalDate(now ? now->utc : r.utc, 480);
        FixedString<48> ds;
        ds.AppendUint(date.month);
        ds.Append("月");
        ds.AppendUint(date.day);
        ds.Append("日 ");
        ds.Append(calendar::WeekdayName(date));
        date_.SetText(ds.c_str());
        FixedString<96> range;
        if (r.day_count) {
            range.Append("最高 ");
            range.Append(Temp(r.days[0].high).c_str());
            range.Append(" · 最低 ");
            range.Append(Temp(r.days[0].low).c_str());
        } else
            range.Append(model_.failed ? "连接失败，保留上次天气" : "最高 --° · 最低 --°");
        range_.SetText(range.c_str());
        for (unsigned i = 0; i < 5; ++i) {
            bool known = i < r.hour_count;
            FixedString<24> hour;
            if (known) {
                if (!i)
                    hour.Append("现在");
                else {
                    hour.AppendUint(uint32_t(calendar::LocalSeconds(r.hours[i].utc, 480) % 86400) / 3600);
                    hour.Append("时");
                }
            } else
                hour.Append("--");
            hours_[i].SetText(hour.c_str());
            hourly_glyphs_[i].Set(known ? r.hours[i].code : 0, known);
            hour_values_[i].SetText(known ? Temp(r.hours[i].temperature).c_str() : "--°");
        }
        int32_t low = 1000, high = -1000;
        for (unsigned i = 1; i < r.day_count; ++i) {
            low = std::min(low, r.days[i].low);
            high = std::max(high, r.days[i].high);
        }
        for (unsigned i = 0; i < 3; ++i) {
            bool known = i + 1 < r.day_count;
            const auto& day = r.days[i + 1];
            days_[i].SetText(!known ? "--" : !i ? "明天" : calendar::WeekdayName(calendar::LocalDate(day.utc, 480)));
            daily_glyphs_[i].Set(day.code, known);
            FixedString<64> text;
            if (known) {
                text.Append(Temp(day.low).c_str());
                text.Append(" / ");
                text.Append(Temp(day.high).c_str());
            } else
                text.Append("--° / --°");
            day_values_[i].SetText(text.c_str());
            int x = 192 + (known ? (day.low - low) * 300 / std::max(10, high - low) : 0);
            int end = 192 + (known ? (day.high - low) * 300 / std::max(10, high - low) : 0);
            bars_[i].SetRect({x, 458 + int(i) * 60, std::max(12, end - x), 7});
            highs_[i].SetRect({std::max(x, end - 16), 458 + int(i) * 60, 16, 7});
            bars_[i].SetVisible(known);
            highs_[i].SetVisible(known);
        }
        FixedString<64> h, w;
        h.Append("湿度 ");
        if (r.humidity >= 0) {
            h.AppendInt(r.humidity);
            h.Append("%");
        } else
            h.Append("--");
        w.Append("风速 ");
        if (r.wind_tenths >= 0) {
            w.AppendFixed(float(r.wind_tenths) / 10, 1);
            w.Append(" km/h");
        } else
            w.Append("--");
        humidity_.SetText(h.c_str());
        wind_.SetText(w.c_str());
        refresh_.SetEnabled(!model_.busy);
        FixedString<96> status;
        status.Append(model_.busy ? "正在更新 · " : model_.failed ? "刷新失败 · 显示缓存 · " : "北京时间 · ");
        status.Append("Open-Meteo · CC BY 4.0");
        status_.SetText(status.c_str());
        app.renderer().Present(*scene_).value();
        seen_ = model_.revision;
    }
    Model& model_;
    std::optional<Scene> scene_;
    std::unique_ptr<ui::NumeralAtlas> font_;
    std::unique_ptr<ui::WeatherAtlas> icons_;
    std::optional<Timer> timer_;
    ui::Numerals number_;
    ui::WeatherIcon glyph_;
    std::array<ui::WeatherIcon, 5> hourly_glyphs_;
    std::array<ui::WeatherIcon, 3> daily_glyphs_;
    LabelNode city_, date_, range_, condition_, humidity_, wind_, status_;
    std::array<LabelNode, 5> hours_, hour_values_;
    std::array<LabelNode, 3> days_, day_values_;
    std::array<RoundedRectNode, 3> bars_, highs_;
    std::unique_ptr<CityPicker> picker_;
    ui::MaterialIconButton refresh_;
    ui::Button city_hit_;
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
