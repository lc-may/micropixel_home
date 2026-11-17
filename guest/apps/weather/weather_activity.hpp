// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <memory>

#include "city_picker.hpp"
#include "sdk/calendar.hpp"
#include "sdk/ui/numerals.hpp"
#include "sdk/ui/weather_icons.hpp"
#include "weather_model.hpp"
namespace weather {
class WeatherActivity final : public Activity {
   public:
    explicit WeatherActivity(Model& model) : model_(model) {}
    void OnCreate(ComponentContext& c) override {
        const auto& app = c.app();
        scene_.emplace(app.renderer().CreateScene(ui::panel::Background).value());
        font_ = std::make_unique<ui::NumeralAtlas>(app);
        icons_ = std::make_unique<ui::WeatherAtlas>(app);
        using namespace ui::panel;
        const ui::AppBarAction actions[] = {{.icon = ui::MaterialSymbol::Refresh}};
        auto header = ui::AppBar::Create(*scene_, {.actions = actions, .subtitle_width = 230, .title_action = true},
                                         icons_.get());
        Assert(header.has_value(), "weather AppBar creation failed");
        header_ = std::move(*header);
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
        header_.CancelInput();
        if (picker_) picker_->Cancel();
    }
    void OnDestroy(ComponentContext&) override {
        timer_.reset();
        picker_.reset();
        header_.Destroy();
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
                auto update = header_.OnTouch(*t);
                if (update.action) {
                    if (update.action->kind == ui::AppBarActionKind::kTitle) {
                        picker_->Show(model_.saved.city);
                        redraw = true;
                    } else
                        refresh = true;
                }
                redraw |= update.changed;
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
        header_.SetTitle(city.c_str()).value();
        condition_.SetText(!r.utc ? "暂无天气" : WeatherName(r.code));
        auto now = app.jobs().Now();
        auto date = calendar::LocalDate(now ? now->utc : r.utc, 480);
        FixedString<48> ds;
        ds.AppendUint(date.month);
        ds.Append("月");
        ds.AppendUint(date.day);
        ds.Append("日 ");
        ds.Append(calendar::WeekdayName(date));
        header_.SetSubtitle(ds.c_str()).value();
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
        header_.SetActionEnabled(0, !model_.busy);
        FixedString<96> status;
        status.Append(model_.busy ? "正在更新 · " : model_.failed ? "刷新失败 · 显示缓存 · " : "北京时间 · ");
        status.Append("Open-Meteo · CC BY 4.0");
        status_.SetText(status.c_str());
        auto presented = app.renderer().Present(*scene_);
        if (!presented) Panic(presented.error().name());
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
    LabelNode range_, condition_, humidity_, wind_, status_;
    std::array<LabelNode, 5> hours_, hour_values_;
    std::array<LabelNode, 3> days_, day_values_;
    std::array<RoundedRectNode, 3> bars_, highs_;
    std::unique_ptr<CityPicker> picker_;
    ui::AppBar header_;
    uint32_t seen_{};
};
}  // namespace weather
