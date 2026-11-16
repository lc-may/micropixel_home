// SPDX-License-Identifier: Apache-2.0
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "apps/weather/weather_client.hpp"
#include "sdk/ui/material_icons_data.hpp"
int main() {
    assert(weather::provinces.size() == 34 && weather::cities.size() == 394 && weather::PopularCityCount == 10);
    constexpr const char* popular[]{"上海", "北京", "广州", "深圳", "杭州", "成都", "武汉", "南京", "重庆", "西安"};
    for (unsigned i = 0; i < 10; ++i) assert(std::string_view(weather::cities[i].name) == popular[i]);
    std::array<unsigned, 34> province_counts{};
    for (const auto& city : weather::cities) {
        assert(city.province < province_counts.size() && std::string_view(city.coordinates).starts_with("latitude="));
        ++province_counts[city.province];
        for (const auto& other : weather::cities)
            if (&city != &other) assert(std::string_view(city.name) != other.name);
    }
    for (auto count : province_counts) assert(count > 0 && count <= 64);
    constexpr auto current =
        R"("current":{"temperature_2m":21.5,"weather_code":1,"time":1700000000,"relative_humidity_2m":65,"wind_speed_10m":13.2})";
    weather::Reading reading;
    std::string json =
        "{" + std::string(current) +
        R"(,"hourly":{"time":[1699999200,1700002800,1700006400,1700010000,1700013600],"temperature_2m":[21.5,22,23,22,20],"weather_code":[1,0,2,3,61]},"daily":{"time":[1699920000,1700006400,1700092800,1700179200],"temperature_2m_min":[18,17,16,15],"temperature_2m_max":[24,25,23,21],"weather_code":[1,2,3,61]}})";
    assert(weather::WeatherClient::Parse(json, reading) == 1);
    assert(reading.temperature == 215 && reading.humidity == 65 && reading.wind_tenths == 132 &&
           reading.hour_count == 5 && reading.day_count == 4);
    assert(reading.hours[4].code == 61 && reading.days[3].low == 150 && reading.days[3].high == 210);
    auto saved = reading;
    assert(weather::WeatherClient::Parse(R"({"current":{"temperature_2m":null,"weather_code":1,"time":1700000000}})",
                                         reading) == -1);
    assert(reading.temperature == saved.temperature && reading.day_count == saved.day_count);
    assert(weather::WeatherClient::Parse(R"({"current":{"temperature_2m":21.5,"weather_code":1.5,"time":1700000000}})",
                                         reading) == -1);
    assert(reading.temperature == saved.temperature && reading.day_count == saved.day_count);
    assert(weather::WeatherClient::Parse("{" + std::string(current) + "}", reading) == 1);
    assert(!reading.hour_count && !reading.day_count);
    json =
        "{" + std::string(current) +
        R"(,"hourly":{"time":[1700002800,1700006400],"temperature_2m":[null,25],"weather_code":[1,61]},"daily":{"time":[1700006400],"temperature_2m_min":[30],"temperature_2m_max":[10],"weather_code":[1]}})";
    assert(weather::WeatherClient::Parse(json, reading) == 1 && reading.hour_count == 1 && !reading.day_count &&
           reading.hours[0].temperature == 250);
    using namespace micropixel::ui::material_art;
    assert(Index(0) == unsigned(Symbol::Sunny) && Index(2) == unsigned(Symbol::PartlyCloudy) &&
           Index(3) == unsigned(Symbol::Cloud));
    assert(Index(45) == unsigned(Symbol::Fog) && Index(53) == unsigned(Symbol::Drizzle) &&
           Index(61) == unsigned(Symbol::Rain));
    assert(Index(71) == unsigned(Symbol::Snow) && Index(85) == unsigned(Symbol::Snow) &&
           Index(95) == unsigned(Symbol::Thunder) && Index(-1) == unsigned(Symbol::Unavailable));
    std::vector<uint8_t> pixels(Width * Height * 4);
    assert(Expand(pixels));
    for (unsigned icon = 0; icon < SymbolCount; ++icon) {
        unsigned visible = 0;
        for (unsigned y = 0; y < Cell; ++y)
            for (unsigned x = 0; x < Cell; ++x)
                visible += pixels[((icon / Columns * Cell + y) * Width + icon % Columns * Cell + x) * 4 + 3] > 0;
        assert(visible > 30);
    }
    assert(!Expand({pixels.data(), pixels.size() - 1}));
    std::puts(
        "PASS weather current/forecast parsing, missing/null/range fallback, cache preservation and Material "
        "glyphs/WMO mapping; 394 unique locations/34 regions and popular city ordering");
}
