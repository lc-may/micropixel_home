// SPDX-License-Identifier: Apache-2.0
#include <string>

#include "sdk/micropixel.hpp"

using namespace micropixel;

[[gnu::noinline]] void CheckOwnership(Scene& scene) {
    const char* labels[] = {"First", "Second"};
    const ui::AppBarAction actions[] = {{.text = "Action", .width = 100}};
    const auto baseline = scene.node_count();
    {
        auto invalid = ui::AppBar::Create(scene, {.bounds = {0, 0, 20, 84}, .actions = actions});
        Assert(!invalid && scene.node_count() == baseline, "failed header leaked its partial subtree");
        auto original = ui::SegmentedControl<2>::Create(scene, {24, 96, 672, 64}, labels).value();
        auto moved = std::move(original);
        Assert(!original.valid() && moved.valid(), "component move did not transfer ownership");
        auto destination = ui::SegmentedControl<2>::Create(scene, {24, 96, 672, 64}, labels).value();
        destination = std::move(moved);
        Assert(!moved.valid() && destination.valid(), "component move assignment did not empty source");
        destination.Destroy();
        destination.Destroy();
    }
    Assert(scene.node_count() == baseline, "component subtree was retained after destruction");
}

[[gnu::noinline]] void CheckTextCompaction(Application& app) {
    auto scene = app.renderer().CreateScene(Color::White()).value();
    const auto baseline = scene.node_count();
    // A new label reuses the lower shape slot while live text starts at arena offset zero.
    auto hole = scene.CreateShape({0, 0, 1, 1}, Color::Black()).value();
    auto kept = scene.CreateLabel({0, 0}, "Preserve live text during slot reuse", Color::Black()).value();
    hole.Destroy();
    const std::string text(127, 'x');
    for (unsigned i = 0; i < 64; ++i) {
        auto label = scene.CreateLabel({0, 40}, text.c_str(), Color::Black()).value();
        label.Destroy();
    }
    app.renderer().Present(scene).value();
    kept.Destroy();
    Assert(scene.node_count() == baseline, "text compaction retained temporary nodes");
}

[[gnu::noinline]] void CheckHeaderProperties(Scene& scene) {
    const auto baseline = scene.node_count();
    auto invalid = ui::AppBar::Create(scene, {.leading = ui::AppBarAction{.text = ""}});
    Assert(!invalid && scene.node_count() == baseline, "empty leading action accepted");
    auto empty = ui::AppBar::Create(scene, {.title = ""}).value();
    empty.Destroy();
    Assert(scene.node_count() == baseline, "empty-title header retained nodes");
}

int main() {
    Application app;
    app.renderer().ConfigureDisplay({.logical_size = {720, 720}, .scale_mode = DisplayScaleMode::kAspectFit}).value();
    auto scene = app.renderer().CreateScene(ui::kPanelTheme.background).value();
    const char* labels[] = {"First", "Second"};
    const ui::AppBarAction actions[] = {{.text = "Action", .width = 100}};
    CheckOwnership(scene);
    CheckHeaderProperties(scene);
    CheckTextCompaction(app);
    auto header_owner = std::make_unique<ui::AppBar>();
    auto& header = *header_owner;
    header = ui::AppBar::Create(scene, {.title = "Composite widgets", .actions = actions}).value();
    Assert(!header.SetTitle(std::string(129, 'x').c_str()), "unbounded header text accepted");
    Assert(!header.SetSubtitle(std::string(129, 'x').c_str()), "unbounded subtitle accepted");
    Assert(!header.SetActionText(0, std::string(129, 'x').c_str()), "unbounded action text accepted");
    Assert(!header.SetBounds({0, 0, 50, 84}), "invalid header resize accepted");
    auto tabs_owner = std::make_unique<ui::SegmentedControl<2>>();
    auto& tabs = *tabs_owner;
    tabs = ui::SegmentedControl<2>::Create(scene, {24, 96, 672, 64}, labels).value();
    auto parent = scene.CreateContainer({.clip = {0, 0, 500, 300}, .translation = {24, 180}}).value();
    auto create = [](Container& content, Rect bounds) { return content.CreateListItem(bounds); };
    auto undersized = ui::VirtualList<ui::ListItem, 2>::Create(
        parent, {.bounds = {16, 20, 400, 240}, .item_width = 396, .item_height = 74}, create);
    Assert(!undersized, "insufficient row pool accepted");
    auto list_owner = std::make_unique<ui::VirtualList<ui::ListItem, 8>>();
    auto& list = *list_owner;
    list = ui::VirtualList<ui::ListItem, 8>::Create(
               parent, {.bounds = {16, 20, 400, 240}, .item_width = 396, .item_height = 74}, create)
               .value();
    Assert(list.valid(), "list root lost on move");
    uint32_t generation = 1, binds = 0;
    auto key = [&](uint32_t index) -> uint64_t { return uint64_t(generation) * 1000 + index + 1; };
    auto bind = [&](auto& row, uint32_t index, Rect bounds) {
        ++binds;
        row.SetPosition({bounds.x, bounds.y});
        FixedString<64> title;
        title.Append("Stable key ");
        title.AppendUint(uint32_t(key(index)));
        auto text = row.SetText(title.c_str(), "Body and trailing action", "1");
        Assert(text.has_value(), "row text bind failed");
    };
    auto items = list.SetItems(8, generation);
    if (!items) Panic(items.error().name());
    list.BindRows(key, bind);
    const auto first_binds = binds;
    list.BindRows(key, bind);
    Assert(binds == first_binds, "unchanged list rebound rows");
    list.ScrollTo(120);
    list.SetItems(9, generation).value();
    Assert(list.offset() == 120, "appending reset the offset");
    Assert(!list.SetItems(UINT32_MAX, generation), "overflowed content height accepted");
    Assert(list.offset() == 120, "failed item update changed the offset");
    list.SetItems(8, generation, true).value();
    list.BindRows(key, bind);
    auto message = ui::ContentStateView::Create(scene, {24, 530, 672, 120}).value();
    message.Set({.phase = ui::ContentPhase::kLoading, .message = "Loading state"}).value();
    message.Set({.phase = ui::ContentPhase::kFailed, .has_content = true, .message = "Cached data remains"}).value();
    Assert(!message.visible(), "cached content covered by status");
    Assert(!message.Set({.message = std::string(257, 'x').c_str()}) && !message.visible(),
           "invalid status changed visibility");
    message.Set({.phase = ui::ContentPhase::kReady, .message = "Drag; Enter replaces data; Up resets scroll"}).value();
    const auto steady_nodes = scene.node_count();
    unsigned bodies = 0, trailing = 0, selections = 0, header_actions = 0;
    auto log = [&](const char* label, uint32_t value) {
        FixedString<80> text;
        text.Append(label);
        text.AppendUint(value);
        app.log().Info(text.c_str());
    };
    auto first_frame = app.renderer().Present(scene);
    if (!first_frame) Panic(first_frame.error().name());
    app.log().Info("UI COMPONENTS ready: ownership, rollback, bounds, append and bind cache passed");
    app.Run([&](const Event& event) {
        bool changed = false;
        if (const auto* pressed = event.key(); pressed && pressed->phase() == KeyPhase::kDown) {
            if (pressed->code() == KeyCode::kConfirm) {
                list.SetItems(8, ++generation).value();
                list.BindRows(key, bind);
                log("UI dataset=", generation);
            } else if (pressed->code() == KeyCode::kUp) {
                list.ScrollTo(0);
                list.BindRows(key, bind);
                app.log().Info("UI scroll reset");
            } else if (pressed->code() == KeyCode::kLeft) {
                tabs.SetEnabled(1, false);
                app.log().Info("UI tab disabled");
            } else if (pressed->code() == KeyCode::kRight) {
                header.SetActionEnabled(0, false);
                app.log().Info("UI header disabled");
            }
            changed = true;
        }
        if (const auto* touch = event.touch()) {
            auto top = header.OnTouch(*touch);
            changed |= top.changed;
            if (top.action) log("UI header action=", ++header_actions);
            auto tab = tabs.OnTouch(*touch);
            changed |= tab.changed;
            if (tab.action) {
                tabs.SetSelected(*tab.action);
                log("UI selection=", ++selections);
            }
            auto result = list.OnTouch(*touch, key, bind, [](auto& row, const auto& input) {
                auto value = row.OnTouch(input);
                return ui::RowUpdate{value.redraw, value.clicked, value.action_clicked};
            });
            changed |= result.changed;
            if (result.drag_ended) log("UI scroll=", uint32_t(list.offset()));
            if (result.action) {
                Assert(result.action->key == key(result.action->index) && result.action->generation == generation,
                       "stale list identity reached the application");
                if (result.action->region == ui::ItemRegion::kBody)
                    log("UI body=", ++bodies);
                else
                    log("UI trailing=", ++trailing);
                log("UI key=", uint32_t(result.action->key));
            }
        }
        Assert(scene.node_count() == steady_nodes, "interaction grew the retained node pool");
        if (changed) app.renderer().Present(scene).value();
    });
    return 0;
}
