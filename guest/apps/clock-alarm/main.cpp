// SPDX-License-Identifier: Apache-2.0
#include "clock_activity.hpp"
#include "clock_service.hpp"

int main() {
    micropixel::Application app;
    app.renderer()
        .ConfigureDisplay({.logical_size = {720, 720}, .scale_mode = micropixel::DisplayScaleMode::kAspectFit})
        .value();
    clock_app::Model model;
    clock_app::ClockActivity page(model);
    clock_app::ClockService service(model);
    app.RunComponents(&page, &service);
    return 0;
}
