// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "sdk/ui/panel.hpp"
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
}  // namespace weather
