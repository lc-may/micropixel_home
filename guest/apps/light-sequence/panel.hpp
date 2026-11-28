// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "detail.hpp"
#include "sdk/ui/component.hpp"

namespace light_sequence {
inline constexpr int kCanvas = 480;

enum class Page { kHome, kRoom, kGroup, kLight };
struct Screen {
    Page page{Page::kHome};
    int room{}, id{}, scroll{};
    bool groups{};
};

enum class Command { kBack, kRoom, kGroup, kLight, kRoomTab, kBatchRoom, kBatchGroup, kToggle, kMode, kTemp, kSwatch };
struct Action {
    Command command;
    int id{};
    bool target{};
};

class Panel {
   public:
    explicit Panel(const mp::Application& app)
        : app_(app), art_(app), scene_(app.renderer().CreateScene(kBackground).value()) {}

    void Draw(const Screen& screen, const Model& model) {
        detail_.reset();
        root_.Destroy();
        toast_ = {};
        scroll_ = {};
        count_ = 0;
        root_ = ui::ComponentRoot(scene_.CreateContainer().value());
        auto& root = root_.node();
        switch (screen.page) {
            case Page::kHome:
                Home(root, model);
                break;
            case Page::kRoom:
                RoomPage(root, screen, model);
                break;
            case Page::kGroup:
                GroupPage(root, screen, model);
                break;
            case Page::kLight:
                Detail(root, screen, model);
                break;
        }
    }

    void Present() { app_.renderer().Present(scene_).value(); }
    void Adjust(const LightState& state) { detail_->Show(state); }
    bool adjusting() const { return detail_ && detail_->adjusting(); }
    int offset() const { return scroll_.valid() ? scroll_.offset() : 0; }

    void Notify(std::string_view text) {
        ClearToast();
        toast_ = root_.node().CreateContainer({.z_order = 1}).value();
        const int width = ArtLabel::Width(Text::kToast, text) + 51;
        const int x = (kCanvas - width) / 2;
        (void)Box(toast_, {x - 2, 430, width + 4, 39}, Color::Black(), 14, Color::Black(), 0, 70);
        (void)Box(toast_, {x, 426, width, 38}, Rgb(0x2f271f), 12, Rgb(0x705d47), 1);
        art_.Glyph(toast_, Icon::kInfo, Tone::kGold, 16, {x + 14, 437});
        art_.Write(toast_, Text::kToast, {x + 37, 436}, text);
    }

    bool ClearToast() {
        if (!toast_.valid()) return false;
        toast_.Destroy().value();
        toast_ = {};
        return true;
    }

    ui::UiUpdate<Action> Touch(const mp::TouchEvent& event, Screen& screen, Model& model, bool& commit) {
        if (detail_) {
            bool changed = false;
            if (detail_->Touch(event, model[screen.id], changed, commit)) return {.handled = true, .changed = changed};
        }
        bool changed = false;
        if (scroll_.valid()) {
            const auto scroll = scroll_.OnTouch(event);
            screen.scroll = scroll_.offset();
            changed = scroll.changed;
            if (scroll.dragging) {
                changed |= CancelButtons();
                return {.handled = true, .changed = changed};
            }
        }
        for (size_t i = count_; i-- > 0;) {
            auto& entry = buttons_[i];
            if (entry.in_list && !scroll_.contains(event.position())) {
                if (entry.hit.tracking()) {
                    entry.hit.Reset();
                    entry.feedback.SetVisible(false);
                    changed = true;
                }
                continue;
            }
            const auto local =
                entry.in_list ? event.WithPosition({event.x() - 20, event.y() - list_y_ + offset()}) : event;
            const auto update = entry.hit.OnTouch(local);
            if (!update.handled) continue;
            if (update.visual_changed) entry.feedback.SetVisible(entry.hit.pressed());
            return {.handled = true,
                    .changed = changed || update.visual_changed,
                    .action = update.clicked ? std::optional{entry.action} : std::nullopt};
        }
        return {.changed = changed};
    }

    void Cancel(const Screen& screen, Model& model) {
        CancelButtons();
        if (scroll_.valid()) scroll_.Cancel();
        if (detail_) detail_->Cancel(model[screen.id]);
    }

   private:
    enum class PowerStyle { kOff, kWarm, kActive, kDisabled };
    enum class PowerSize { kTile, kRow, kHeader };
    struct Target {
        ui::Button hit;
        mp::RoundedRectNode feedback;
        Action action{Command::kBack};
        bool in_list{};
    };

    void Bind(Container& root, Rect bounds, uint32_t radius, Action action, bool in_list = false) {
        mp::Assert(count_ < buttons_.size(), "light-sequence touch capacity");
        auto feedback = Box(root, bounds, Color::White(), radius, Color::Black(), 0, 18);
        feedback.SetVisible(false);
        buttons_[count_++] = {ui::Button(bounds), feedback, action, in_list};
    }

    bool CancelButtons() {
        bool changed = false;
        for (size_t i = 0; i < count_; ++i) {
            auto& entry = buttons_[i];
            if (entry.hit.pressed()) {
                entry.feedback.SetVisible(false);
                changed = true;
            }
            entry.hit.Reset();
        }
        return changed;
    }

    void Power(Container& root, Rect bounds, Icon icon, const char* label, PowerStyle style, Action action,
               PowerSize size = PowerSize::kHeader, bool in_list = false) {
        constexpr std::array tones{Tone::kPower, Tone::kGold, Tone::kDark, Tone::kDisabled};
        constexpr std::array fills{Rgb(0x282a30), Rgb(0x4a3727), kGold, Rgb(0x222329)};
        constexpr std::array strokes{Rgb(0x42444c), Rgb(0x7a5d3f), Rgb(0xffdbab), Rgb(0x33353a)};
        constexpr Text labels[][4]{
            {Text::kRoomPowerPower, Text::kRoomPowerGold, Text::kRoomPowerDark, Text::kRoomPowerDisabled},
            {Text::kRowPowerPower, Text::kRowPowerGold, Text::kRowPowerDark, Text::kRowPowerDisabled},
            {Text::kPowerPower, Text::kPowerGold, Text::kPowerDark, Text::kPowerDisabled},
        };
        const auto setting = static_cast<size_t>(style);
        const auto text = labels[static_cast<size_t>(size)][setting];
        const bool disabled = style == PowerStyle::kDisabled;
        auto layer = root.CreateContainer({.opacity = static_cast<uint8_t>(disabled ? 209 : 255)}).value();
        const int radius = size == PowerSize::kRow ? 10 : 13;
        (void)Box(layer, bounds, fills[setting], radius, strokes[setting], 1);
        if (size == PowerSize::kTile) {
            art_.Glyph(layer, icon, tones[setting], 17, {bounds.center_x() - 8, bounds.y + 11});
            art_.Write(layer, text, {bounds.center_x(), bounds.y + 33}, label, Align::kCenter);
        } else {
            const bool narrow = bounds.width < 70;
            const int glyph = narrow ? 15 : 17, gap = narrow ? 4 : 6;
            const int x = bounds.center_x() - (glyph + gap + ArtLabel::Width(text, label)) / 2;
            art_.Glyph(layer, icon, tones[setting], glyph, {x, bounds.center_y() - glyph / 2});
            art_.Write(layer, text, {x + glyph + gap, bounds.center_y() - 6}, label);
        }
        if (!disabled) Bind(root, bounds, radius, action, in_list);
    }

    void BatchPower(Container& root, Rect bounds, Counts counts, Command command, int id, PowerSize size,
                    bool in_list = false) {
        const bool turn_on = !counts.on;
        const bool online = counts.online() > 0;
        const auto style = !online                     ? PowerStyle::kDisabled
                           : turn_on                   ? PowerStyle::kOff
                           : counts.on == counts.total ? PowerStyle::kActive
                                                       : PowerStyle::kWarm;
        Power(root, bounds, online ? Icon::kPower : Icon::kOffline, online ? (turn_on ? "全开" : "全关") : "离线",
              style, {command, id, turn_on}, size, in_list);
    }

    void Home(Container& root, const Model& model) {
        art_.Draw(root, Surface::kBrand, {20, 22});
        art_.Draw(root, Surface::kDemo, {385, 24});
        art_.Draw(root, Surface::kIntro, {21, 62});
        art_.Draw(root, Surface::kFooter, {20, 440});
        Copy count;
        count.AppendInt(model.TotalLit());
        count.Append(" / ");
        count.AppendInt(kLights.size());
        constexpr auto tail = " 盏已亮";
        const int tail_x = 458 - ArtLabel::Width(Text::kCountTail, tail);
        const int count_x = tail_x - ArtLabel::Width(Text::kCount, count.c_str());
        art_.Glyph(root, Icon::kSun, Tone::kGold, 17, {count_x - 24, 101});
        art_.Write(root, Text::kCount, {count_x, 99}, count.c_str());
        art_.Write(root, Text::kCountTail, {tail_x, 101}, tail);

        constexpr std::array icons{Icon::kSofa, Icon::kBed, Icon::kDine, Icon::kDoor};
        for (size_t i = 0; i < kRooms.size(); ++i) {
            const auto& room = kRooms[i];
            const auto counts = model.Count(room.lights);
            const bool warm = counts.on > 0;
            const int x = 20 + static_cast<int>(i % 2) * 226, y = 136 + static_cast<int>(i / 2) * 152;
            art_.Draw(root, warm ? Surface::kRoomWarm : Surface::kRoomCold, {x, y});
            (void)Box(root, {x + 15, y + 14, 45, 45}, warm ? kGold : Color::White(), 13, warm ? kGold : Color::White(),
                      1, warm ? 20 : 12);
            art_.Glyph(root, icons[i], warm ? Tone::kRoomWarm : Tone::kRoomCold, 25, {x + 25, y + 24});
            art_.Glyph(root, Icon::kArrow, Tone::kRoomArrow, 17, {x + 182, y + 18});
            art_.Write(root, Text::kRoomTitle, {x + 15, y + 71}, room.name);
            art_.Write(root, Text::kNote, {x + 15, y + 106}, CountNote(counts).c_str());
            Bind(root, {x, y, 214, 140}, 18, {Command::kRoom, static_cast<int>(i)});
            BatchPower(root, {x + 152, y + 74, 49, 54}, counts, Command::kBatchRoom, static_cast<int>(i),
                       PowerSize::kTile);
        }
    }

    void Header(Container& root, const char* name, std::string_view context) {
        const Rect back{20, 22, 50, 50};
        (void)Box(root, back, Rgb(0x202228), 14, kEdge, 1);
        art_.Glyph(root, Icon::kBack, Tone::kInk, 22, {34, 36});
        art_.Write(root, Text::kHeading, {82, 22}, name);
        art_.Write(root, Text::kNote, {82, 55}, context);
        Bind(root, back, 14, {Command::kBack});
    }

    void Ribbon(Container& root, Counts counts, bool group, int id) {
        art_.Draw(root, Surface::kRibbon, {20, 84});
        (void)Box(root, {34, 101, 37, 37}, kGold, 19, kGold, 1, 23);
        art_.Glyph(root, group ? Icon::kLayers : Icon::kSun, Tone::kGold, 21, {42, 109});
        art_.Write(root, Text::kStatus, {81, 99}, group ? "组内灯光" : "本区灯光");
        art_.Write(root, Text::kNote, {81, 123}, CountNote(counts).c_str());
        const auto command = group ? Command::kBatchGroup : Command::kBatchRoom;
        for (auto on : {true, false}) {
            const auto style = !counts.online()                    ? PowerStyle::kDisabled
                               : (on ? !counts.on : counts.on > 0) ? PowerStyle::kActive
                                                                   : PowerStyle::kOff;
            Power(root, {on ? 309 : 383, 95, 66, 48}, on ? Icon::kSun : Icon::kMoon, on ? "全开" : "全关", style,
                  {command, id, on});
        }
    }

    void Tabs(Container& root, int y, int height) {
        (void)Box(root, {20, y, 440, height}, Rgb(0x111317), 14, Rgb(0x2e3035), 1);
    }

    void Tab(Container& root, Rect bounds, Icon icon, const char* label, bool selected, bool enabled, Action action,
             int count = -1) {
        auto group = root.CreateContainer({.opacity = static_cast<uint8_t>(enabled ? 255 : 140)}).value();
        const auto role = selected ? Text::kTabSelected : Text::kTab;
        if (selected) (void)Box(group, bounds, Rgb(0x302d2a), 11, Rgb(0x594a3a), 1);
        Copy note;
        if (count >= 0) note.AppendInt(count);
        const auto text_width = ArtLabel::Width(role, label);
        const auto number_width = count < 0 ? 0 : ArtLabel::Width(Text::kNote, note.c_str()) + 8;
        const int x = bounds.center_x() - (25 + text_width + number_width) / 2;
        art_.Glyph(group, icon, selected ? Tone::kSelected : Tone::kTab, 17, {x, bounds.center_y() - 8});
        art_.Write(group, role, {x + 25, bounds.center_y() - 10}, label);
        if (count >= 0) art_.Write(group, Text::kNote, {x + 33 + text_width, bounds.center_y() - 8}, note.c_str());
        if (enabled) Bind(root, bounds, 11, action);
    }

    Container& List(Container& root, int y, int offset, size_t count) {
        list_y_ = y;
        scroll_ = root.CreateScrollView({20, y, 440, 462 - y});
        scroll_.SetContentHeight(count ? static_cast<int>(count) * 61 - 7 : 108);
        scroll_.ScrollTo(offset);
        return scroll_.content();
    }

    void Device(Container& list, int y, bool lit, Icon icon, const char* name, std::string_view note, bool online,
                Action action, bool group) {
        (void)Box(list, {0, y, 440, 54}, Rgb(0x202227), 13, kEdge, 1);
        (void)Box(list, {9, y + 10, 35, 35}, lit ? Rgb(0x413429) : Rgb(0x2c2e35), 10);
        art_.Glyph(list, icon, lit ? Tone::kRowWarm : Tone::kRowCold, 19, {17, y + 18});
        art_.Write(list, Text::kRowTitle, {53, y + 11}, name);
        art_.Write(list, online ? Text::kRowNote : Text::kOfflineNote, {53, y + 30}, note);
        art_.Glyph(list, Icon::kArrow, Tone::kRowArrow, 15, {group ? 358 : 361, y + 20});
        Bind(list, {1, y + 1, group ? 372 : 378, 52}, 12, action, true);
    }

    void LampRow(Container& list, int id, int y, const Model& model) {
        const auto& light = kLights[id];
        const bool lit = model[id].on;
        Device(list, y, lit, FixtureIcon(light.fixture), light.name, LightNote(light, model[id]).c_str(), light.online,
               {Command::kLight, id}, false);
        if (!light.online) {
            art_.Glyph(list, Icon::kOffline, Tone::kOffline, 19, {400, y + 18});
            return;
        }
        (void)Box(list, {390, y + 16, 38, 23}, lit ? kGold : Rgb(0x34363e), 12, lit ? Rgb(0xffdda8) : Rgb(0x585a61), 1);
        (void)Box(list, {lit ? 410 : 393, y + 20, 15, 15}, lit ? Rgb(0x39291c) : Rgb(0xbcbcc4), 8);
        Bind(list, {379, y + 1, 60, 52}, 12, {Command::kToggle, id}, true);
    }

    void GroupRow(Container& list, int id, int y, const Model& model) {
        const auto& group = kGroups[id];
        const auto counts = model.Count(group.members);
        Device(list, y, counts.on > 0, Icon::kLayers, group.name, CountNote(counts).c_str(), true,
               {Command::kGroup, id}, true);
        BatchPower(list, {376, y + 2, 60, 50}, counts, Command::kBatchGroup, id, PowerSize::kRow, true);
    }

    void RoomPage(Container& root, const Screen& screen, const Model& model) {
        const auto& room = kRooms[screen.room];
        Header(root, room.name, "灯光 / 区域控制");
        Ribbon(root, model.Count(room.lights), false, screen.room);
        Tabs(root, 164, 50);
        Tab(root, {24, 165, 214, 48}, Icon::kBulb, "单灯", !screen.groups, true, {Command::kRoomTab, 0, false},
            static_cast<int>(room.lights.size()));
        Tab(root, {242, 165, 214, 48}, Icon::kLayers, "灯组", screen.groups, true, {Command::kRoomTab, 0, true},
            static_cast<int>(room.groups.size()));
        const auto ids = screen.groups ? room.groups : room.lights;
        auto& list = List(root, 224, screen.scroll, ids.size());
        if (ids.empty()) art_.Draw(list, Surface::kEmpty, {0, 0});
        int y = 0;
        for (int id : ids) {
            if (screen.groups)
                GroupRow(list, id, y, model);
            else
                LampRow(list, id, y, model);
            y += 61;
        }
    }

    void GroupPage(Container& root, const Screen& screen, const Model& model) {
        const auto& group = kGroups[screen.id];
        Copy context;
        context.Append(kRooms[screen.room].name);
        context.Append(" / 灯组控制");
        Header(root, group.name, context.c_str());
        Ribbon(root, model.Count(group.members), true, screen.id);
        art_.Write(root, Text::kLedger, {22, 172}, "组内灯具");
        Copy count;
        count.AppendInt(group.members.size());
        count.Append(" 盏");
        art_.Write(root, Text::kNote, {458, 175}, count.c_str(), Align::kRight);
        auto& list = List(root, 207, screen.scroll, group.members.size());
        int y = 0;
        for (int id : group.members) {
            LampRow(list, id, y, model);
            y += 61;
        }
    }

    void Detail(Container& root, const Screen& screen, const Model& model) {
        const auto& light = kLights[screen.id];
        const auto& state = model[screen.id];
        const bool lit = light.online && state.on;
        Copy context;
        context.Append(kRooms[screen.room].name);
        context.Append(" / ");
        context.Append(light.type());
        Header(root, light.name, context.c_str());
        Power(root, {367, 22, 93, 50}, light.online ? Icon::kPower : Icon::kOffline,
              !light.online ? "离线"
              : lit         ? "关灯"
                            : "开灯",
              !light.online ? PowerStyle::kDisabled
              : lit         ? PowerStyle::kActive
                            : PowerStyle::kOff,
              {Command::kToggle, screen.id});
        detail_.emplace(art_, root, light, state);
        if (light.HasColor()) {
            Tabs(root, 266, 48);
            Tab(root, {24, 267, 214, 46}, Icon::kSun, "白光", !state.rgb, lit, {Command::kMode, 0, false});
            Tab(root, {242, 267, 214, 46}, Icon::kColor, "彩光", state.rgb, lit, {Command::kMode, 0, true});
        }
        detail_->Settings(
            root, [&](Rect bounds, int kelvin) { Bind(root, bounds, 10, {Command::kTemp, kelvin}); },
            [&](Rect bounds, int index) { Bind(root, bounds, 9, {Command::kSwatch, index}); });
        detail_->Show(state);
    }

    const mp::Application& app_;
    Art art_;
    mp::Scene scene_;
    ui::ComponentRoot root_;
    mp::ContainerNode toast_;
    ui::ScrollView scroll_;
    std::optional<LightDetail> detail_;
    std::array<Target, 16> buttons_;
    size_t count_{};
    int list_y_{};
};
}  // namespace light_sequence
