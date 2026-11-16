// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <optional>
#include <string>

#include "cities.hpp"
#include "sdk/json.hpp"
#include "sdk/micropixel.hpp"
namespace weather {
using namespace micropixel;
struct Hour {
    uint64_t utc{};
    int32_t temperature{}, code{-1};
};
struct Day {
    uint64_t utc{};
    int32_t low{}, high{}, code{-1};
};
struct Reading {
    int32_t temperature{}, code{};
    uint64_t utc{};
    int32_t humidity{-1}, wind_tenths{-1};
    std::array<Hour, 5> hours{};
    std::array<Day, 4> days{};
    uint32_t hour_count{}, day_count{};
};
class WeatherClient final {
   public:
    bool Start(const Application& app, uint32_t city) {
        request_.reset();
        if (city >= cities.size()) return false;
        const char* test = app.launch_arguments().FindValue("--weather-origin");
        std::string url = test ? test : "https://api.open-meteo.com/v1/forecast";
        url += "?";
        url += cities[city].coordinates;
        url +=
            "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m&hourly=temperature_2m,weather_"
            "code&daily=temperature_2m_min,temperature_2m_max,weather_code&forecast_hours=8&forecast_days=4&timeformat="
            "unixtime&timezone=Asia%2FShanghai";
        auto opened = app.network().Exchange(HttpMethod::kGet, url, {}, {}, 16384, {.receive_timeout_ms = 10000});
        if (!opened) return false;
        request_.emplace(std::move(*opened));
        return true;
    }
    // 0 pending, 1 success, -1 failed. Never overwrite a successful reading on failure.
    int Poll(Reading& out) {
        if (!request_) return -1;
        auto state = request_->State();
        if (!state) return -1;
        if (state->phase == HttpPhase::kPending) return 0;
        if (state->phase != HttpPhase::kComplete || !state->body_length || state->body_length > 16384) return -1;
        auto metadata = request_->Metadata();
        if (!metadata || metadata->status_code != 200) return -1;
        std::string body(state->body_length, '\0');
        auto read = request_->Read(0, {reinterpret_cast<uint8_t*>(body.data()), body.size()});
        if (!read || *read != body.size()) return -1;
        return Parse(body, out);
    }
    static int Parse(std::string_view body, Reading& out) {
        Json json(body, 512);
        if (!json.valid()) return -1;
        const auto current = json.Field(0, "current");
        const auto temperature = json.Real(json.Field(current, "temperature_2m"));
        const auto code = json.Field(current, "weather_code"), time = json.Field(current, "time");
        if (!temperature || *temperature < -100 || *temperature > 80 || json.Type(code) != Json::kNumber ||
            json.Type(time) != Json::kNumber)
            return -1;
        auto utc = json.Number(time), weather = json.Number(code);
        if (utc < 1577836800 || utc > 4102444800LL || weather < 0 || weather > 99) return -1;
        auto numeric_code = json.Real(code);
        if (!numeric_code || *numeric_code != weather) return -1;
        out = {static_cast<int32_t>(*temperature * 10 + (*temperature < 0 ? -0.5 : 0.5)), static_cast<int32_t>(weather),
               static_cast<uint64_t>(utc)};
        auto humidity = json.Real(json.Field(current, "relative_humidity_2m"));
        auto wind = json.Real(json.Field(current, "wind_speed_10m"));
        if (humidity && *humidity >= 0 && *humidity <= 100) out.humidity = static_cast<int32_t>(*humidity);
        if (wind && *wind >= 0 && *wind <= 500) out.wind_tenths = static_cast<int32_t>(*wind * 10 + 0.5);
        auto values = [&](int object, const char* field, unsigned i) { return json.At(json.Field(object, field), i); };
        auto valid_code = [&](int i) {
            return json.Type(i) == Json::kNumber && json.Number(i) >= 0 && json.Number(i) <= 99;
        };
        const int hourly = json.Field(0, "hourly"), daily = json.Field(0, "daily");
        for (unsigned i = 0; i < std::min(24U, json.Count(json.Field(hourly, "time"))) && out.hour_count < 5; ++i) {
            auto t = values(hourly, "time", i), c = values(hourly, "weather_code", i);
            auto temp = json.Real(values(hourly, "temperature_2m", i));
            auto time = json.Number(t);
            if (json.Type(t) != Json::kNumber || time < utc - 3600 || time > utc + 86400 || !temp || *temp < -100 ||
                *temp > 80 || !valid_code(c))
                continue;
            if (out.hour_count && time <= static_cast<int64_t>(out.hours[out.hour_count - 1].utc)) continue;
            out.hours[out.hour_count++] = {static_cast<uint64_t>(time),
                                           static_cast<int32_t>(*temp * 10 + (*temp < 0 ? -.5 : .5)),
                                           static_cast<int32_t>(json.Number(c))};
        }
        for (unsigned i = 0; i < std::min(7U, json.Count(json.Field(daily, "time"))) && out.day_count < 4; ++i) {
            auto t = values(daily, "time", i), c = values(daily, "weather_code", i);
            auto low = json.Real(values(daily, "temperature_2m_min", i)),
                 high = json.Real(values(daily, "temperature_2m_max", i));
            auto time = json.Number(t);
            if (json.Type(t) != Json::kNumber || time < utc - 86400 || time > utc + 7 * 86400 || !low || !high ||
                *low < -100 || *high > 80 || *low > *high || !valid_code(c))
                continue;
            if (out.day_count && time <= static_cast<int64_t>(out.days[out.day_count - 1].utc)) continue;
            out.days[out.day_count++] = {
                static_cast<uint64_t>(time), static_cast<int32_t>(*low * 10 + (*low < 0 ? -.5 : .5)),
                static_cast<int32_t>(*high * 10 + (*high < 0 ? -.5 : .5)), static_cast<int32_t>(json.Number(c))};
        }
        return 1;
    }
    void Cancel() { request_.reset(); }

   private:
    std::optional<HttpRequest> request_;
};
}  // namespace weather
