// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <optional>
#include <string>

#include "sdk/json.hpp"
#include "sdk/micropixel.hpp"
namespace weather {
using namespace micropixel;
struct City {
    const char* name;
    const char* coordinates;
};
inline constexpr City cities[] = {{"上海", "latitude=31.23&longitude=121.47"},
                                  {"北京", "latitude=39.90&longitude=116.40"},
                                  {"深圳", "latitude=22.54&longitude=114.06"}};
struct Reading {
    int32_t temperature{}, code{};
    uint64_t utc{};
    int32_t humidity{-1}, wind_tenths{-1};
};
class WeatherClient final {
   public:
    bool Start(const Application& app, uint32_t city) {
        request_.reset();
        const char* test = app.launch_arguments().FindValue("--weather-origin");
        std::string url = test ? test : "https://api.open-meteo.com/v1/forecast";
        url += "?";
        url += cities[city % 3].coordinates;
        url +=
            "&current=temperature_2m,weather_code,relative_humidity_2m,wind_speed_10m&timeformat=unixtime&timezone=GMT";
        auto opened = app.network().Exchange(HttpMethod::kGet, url, {}, {}, 8192, {.receive_timeout_ms = 10000});
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
        if (state->phase != HttpPhase::kComplete || !state->body_length || state->body_length > 8192) return -1;
        auto metadata = request_->Metadata();
        if (!metadata || metadata->status_code != 200) return -1;
        std::string body(state->body_length, '\0');
        auto read = request_->Read(0, {reinterpret_cast<uint8_t*>(body.data()), body.size()});
        if (!read || *read != body.size()) return -1;
        Json json(body, 128);
        if (!json.valid()) return -1;
        const auto current = json.Field(0, "current");
        const auto temperature = json.Real(json.Field(current, "temperature_2m"));
        const auto code = json.Field(current, "weather_code"), time = json.Field(current, "time");
        if (!temperature || *temperature < -100 || *temperature > 80 || json.Type(code) != Json::kNumber ||
            json.Type(time) != Json::kNumber)
            return -1;
        auto utc = json.Number(time), weather = json.Number(code);
        if (utc < 1577836800 || utc > 4102444800LL || weather < 0 || weather > 99) return -1;
        out = {static_cast<int32_t>(*temperature * 10 + (*temperature < 0 ? -0.5 : 0.5)), static_cast<int32_t>(weather),
               static_cast<uint64_t>(utc)};
        auto humidity = json.Real(json.Field(current, "relative_humidity_2m"));
        auto wind = json.Real(json.Field(current, "wind_speed_10m"));
        if (humidity && *humidity >= 0 && *humidity <= 100) out.humidity = static_cast<int32_t>(*humidity);
        if (wind && *wind >= 0 && *wind <= 500) out.wind_tenths = static_cast<int32_t>(*wind * 10 + 0.5);
        return 1;
    }
    void Cancel() { request_.reset(); }

   private:
    std::optional<HttpRequest> request_;
};
}  // namespace weather
