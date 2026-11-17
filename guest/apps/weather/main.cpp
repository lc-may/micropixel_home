// SPDX-License-Identifier: Apache-2.0
#include "weather_activity.hpp"
#include "weather_service.hpp"

int main() {
    micropixel::Application app;
    app.renderer()
        .ConfigureDisplay({.logical_size = {720, 720}, .scale_mode = micropixel::DisplayScaleMode::kAspectFit})
        .value();
    weather::Model model(app);
    weather::WeatherActivity page(model);
    weather::WeatherService service(model);
    app.RunComponents(&page, &service);
    return 0;
}
