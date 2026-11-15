// SPDX-License-Identifier: Apache-2.0
#include <array>

#include "sdk/micropixel.hpp"
using namespace micropixel;
using namespace micropixel::literals;
int main() {
    Application app;
    app.renderer().ConfigureDisplay({.logical_size = {720, 720}, .scale_mode = DisplayScaleMode::kAspectFit}).value();
    auto scene = std::move(app.renderer().CreateScene(Color::White()).value());
    auto slider = scene.CreateSlider({.bounds = {24, 24, 400, 40}, .maximum = UINT32_MAX});
    slider.SetValue(UINT32_MAX);
    Assert(slider.value() == UINT32_MAX, "slider 32-bit maximum");
    slider.SetMaximum(100);
    Assert(slider.value() == 100, "slider range clamp");
    slider.SetMaximum(0);
    Assert(slider.value() == 0, "zero slider range");
    slider.SetMaximum(UINT32_MAX);
    slider.SetValue(0);
    auto toggle = scene.CreateIconButton(
        {.bounds = {500, 24, 64, 48},
         .icon = "☆",
         .selected_icon = "★",
         .style = {.background = Color::White(), .text = Color::Black(), .font = SystemFont::kLarge}});
    auto scroll = scene.CreateScrollView({24, 120, 400, 240});
    scroll.SetContentHeight(592);
    scroll.ScrollTo(INT32_MAX);
    Assert(scroll.offset() == 352, "scroll lower clamp");
    Assert(scroll.at_end(), "scroll end detection");
    scroll.ScrollTo(INT32_MIN);
    Assert(scroll.offset() == 0, "scroll upper clamp");
    Assert(!scroll.at_end(), "scroll start is not end");
    std::array<ui::ListItem, 8> rows;
    for (unsigned i = 0; i < rows.size(); ++i) {
        rows[i] = scroll.content().CreateListItem({0, int(i) * 74, 396, 74});
        rows[i].SetText("Two line item", "Independent trailing action", "1").value();
    }
    std::string too_long(129, 'x');
    Assert(!rows[0].SetText(too_long.c_str(), "subtitle"), "unbounded list text");
    auto status =
        scene.CreateLabel({24, 400}, "Drag the slider and scroll the list", Color::Black(), SystemFont::kMedium)
            .value();
    unsigned commits = 0, body_clicks = 0, action_clicks = 0, toggles = 0;
    const auto log = [&](const char* label, unsigned value) {
        FixedString<80> line;
        line.Append(label);
        line.AppendUint(value);
        app.log().Info(line.c_str());
    };
    app.renderer().Present(scene).value();
    app.log().Info("WIDGETS ready");
    app.Run([&](const Event& event) -> EventResult {
        if (event.type() == EventType::kStop) return EventResult::kExit;
        bool changed = false;
        if (const auto* key = event.key(); key && key->phase() == KeyPhase::kDown && key->code() == KeyCode::kConfirm) {
            slider.Cancel();
            log("WIDGET slider cancelled value: ", slider.value());
            changed = true;
        }
        if (const auto* touch = event.touch()) {
            auto range = slider.OnTouch(*touch);
            changed |= range.changed;
            if (range.committed) {
                log("WIDGET slider commits: ", ++commits);
                log("WIDGET slider value: ", slider.value());
            }
            auto toggle_update = toggle.OnTouch(*touch);
            changed |= toggle_update.redraw();
            if (toggle_update.clicked) {
                toggle.SetSelected(!toggle.selected());
                toggle.SetPending(true);
                log("WIDGET toggle: ", ++toggles);
            }
            auto update = scroll.OnTouch(*touch);
            changed |= update.changed;
            if (update.dragging) {
                for (auto& row : rows) row.Reset();
                log("WIDGET scroll offset: ", scroll.offset());
            } else if (touch->phase() != TouchPhase::kDown || scroll.contains(touch->position())) {
                for (auto& row : rows) {
                    auto click = row.OnTouch(*touch);
                    changed |= click.redraw;
                    if (click.clicked) log("WIDGET row body: ", ++body_clicks);
                    if (click.action_clicked) log("WIDGET row action: ", ++action_clicks);
                }
            }
        }
        if (changed) {
            status.SetText("SDK widgets: touch and selection");
            app.renderer().Present(scene).value();
        }
        return EventResult::kContinue;
    });
    return 0;
}
