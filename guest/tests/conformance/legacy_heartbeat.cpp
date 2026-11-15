// SPDX-License-Identifier: Apache-2.0
#include "sdk/micropixel.hpp"
using namespace micropixel;
using namespace micropixel::literals;
int main() {
    Application app;
    auto scene = app.renderer().CreateScene(Color::Rgb(225, 230, 240)).value();
    (void)scene.CreateLabel({40, 80}, "Legacy Application::Run", Color::Black()).value();
    app.renderer().Present(scene).value();
    auto timer = app.timers().Every(25_ms).value();
    unsigned ticks = 0;
    app.Run([&](const Event& event) {
        if (event.TimerFrom(timer) && ++ticks % 4 == 0) app.log().Info("LEGACY heartbeat");
        return event.type() == EventType::kStop ? EventResult::kExit : EventResult::kContinue;
    });
    return 0;
}
