// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <cstring>

#include "abi/wireless.h"
#include "sdk/fixed_string.hpp"
#include "sdk/micropixel.hpp"
#include "sdk/ui/panel.hpp"
#include "sdk/ui/wrap_text.hpp"

namespace {

using namespace micropixel;
namespace panel = ui::panel;
constexpr int kPageSize = 5;
constexpr Rect kBack{12, 4, 44, 46};
constexpr Rect kWifiTab{12, 52, 226, 44};
constexpr Rect kBleTab{242, 52, 226, 44};
constexpr Rect kScan{12, 104, 104, 44};
constexpr Rect kSecondary{124, 104, 154, 44};
constexpr Rect kPrevious{12, 420, 108, 44};
constexpr Rect kNext{360, 420, 108, 44};
constexpr Rect kShift{20, 414, 112, 46};
constexpr Rect kSpace{140, 414, 102, 46};
constexpr Rect kErase{250, 414, 84, 46};
constexpr Rect kJoin{342, 414, 118, 46};
constexpr const char* kKeys[][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.@"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_.@"},
    {"!#$%&()*+=", "[]{}<>?/|\\", "~`^\"':;,@_", ".,-       "},
};

enum class Page : uint8_t { kWifi, kBle, kPassword };

bool Inside(Rect area, Point point) {
    return point.x >= area.x && point.y >= area.y && point.x < area.x + area.width && point.y < area.y + area.height;
}

Rect Row(int index) { return {12, 180 + 46 * index, 456, 46}; }
Rect Key(int row, int column) { return {20 + 44 * column, 198 + row * 52, 40, 46}; }

const char* BandLabel(uint8_t channel, uint8_t ssid_bands = 0) {
    const bool dual = ssid_bands == (MPX_WIFI_BAND_2_4_GHZ | MPX_WIFI_BAND_5_GHZ);
    if (channel >= 1 && channel <= 14) return dual ? "2.4G 双频" : "2.4G";
    if (channel >= 32 && channel <= 196) return dual ? "5G 双频" : "5G";
    return "";
}

void Text(Scene& scene, Point point, const char* label, Color color = panel::Text,
          SystemFont font = SystemFont::kMedium, bool centered = false) {
    (void)scene.CreateLabel(point, label, color, font, centered);
}

void DeviceName(const Renderer& renderer, Scene& scene, Rect bounds, const char* name, Color color) {
    if (!ui::ValidUtf8(name) || std::string_view{name}.find_first_of("\r\n\t") != std::string_view::npos) {
        name = "无法显示的名称";
    }
    const auto width = static_cast<uint32_t>(bounds.width);
    for (const auto font : {SystemFont::kMedium, SystemFont::kSmall}) {
        const auto size = renderer.MeasureText(name, font);
        if (size && size->width <= width) {
            Text(scene, {bounds.x, bounds.y + (bounds.height - int(size->height)) / 2}, name, color, font);
            return;
        }
    }
    char line[33]{};
    constexpr int kLineHeight = 21;
    const auto split = ui::FittingTextPrefix(name, width, [&](std::string_view prefix) {
        std::memcpy(line, prefix.data(), prefix.size());
        line[prefix.size()] = 0;
        const auto dimensions = renderer.MeasureText(line, SystemFont::kSmall);
        return dimensions ? dimensions->width : UINT32_MAX;
    });
    std::memcpy(line, name, split);
    line[split] = 0;
    const int top = bounds.y + (bounds.height - 2 * kLineHeight) / 2;
    Text(scene, {bounds.x, top}, line, color, SystemFont::kSmall);
    Text(scene, {bounds.x, top + kLineHeight}, name + split, color, SystemFont::kSmall);
}

void Button(Scene& scene, Rect bounds, const char* label, bool enabled = true, bool primary = false) {
    const auto style = panel::Style(enabled && primary);
    (void)scene.CreateRoundedRect(bounds, {.fill = style.background, .radius = 10});
    Text(scene, {bounds.x + bounds.width / 2, bounds.y + (bounds.height - 22) / 2}, label,
         enabled ? style.text : panel::Disabled, style.font, true);
}

void Tab(Scene& scene, Rect bounds, const char* label, bool selected) {
    const int center = bounds.x + bounds.width / 2;
    Text(scene, {center, bounds.y + 9}, label, selected ? panel::Accent : panel::Text, SystemFont::kMedium, true);
    if (selected) (void)scene.CreateShape({center - 20, bounds.y + bounds.height - 4, 40, 3}, panel::Accent);
}

class Settings final {
   public:
    int Run() {
        app_.renderer()
            .ConfigureDisplay({.logical_size = {480, 480}, .scale_mode = DisplayScaleMode::kAspectFit})
            .value();
        status_.wifi_state = MPX_WIFI_UNAVAILABLE;
        status_.ble_state = MPX_BLE_UNAVAILABLE;
        micropixel_service_info_t info{};
        const int status =
            micropixel_service_open(MICROPIXEL_SERVICE_WIRELESS, MICROPIXEL_WIRELESS_MIN_VERSION, &info, sizeof(info));
        if (status == MICROPIXEL_STATUS_OK) handle_ = info.service_handle;
        Refresh();
        if (!Draw()) return 1;
        app_.log().Info("Settings Guest ready");
        int result = 0;
        while (!closed_) {
            Event event;
            bool changed = false;
            if (app_.WaitEventFor(event, Duration::Milliseconds(150))) {
                if (event.type() == EventType::kStop) break;
                if (event.type() == EventType::kNavigateBack) {
                    Back();
                    changed = true;
                }
                if (const auto* touch = event.touch()) {
                    if (touch->phase() == TouchPhase::kDown) down_ = touch->position();
                    if (touch->phase() == TouchPhase::kUp && math::Abs(touch->x() - down_.x) < 24 &&
                        math::Abs(touch->y() - down_.y) < 24) {
                        Click(touch->position());
                        changed = true;
                    }
                }
            }
            changed |= Refresh();
            if (changed && !closed_ && !Draw()) {
                result = 2;
                break;
            }
        }
        ClearPassword();
        return result;
    }

   private:
    int Call(uint32_t method, const void* request = nullptr, uint32_t request_size = 0, void* response = nullptr,
             uint32_t response_size = 0) const {
        if (!handle_) return MICROPIXEL_STATUS_UNSUPPORTED;
        uint32_t size{};
        return micropixel_service_call(handle_, method, static_cast<const uint8_t*>(request), request_size,
                                       static_cast<uint8_t*>(response), response_size, &size);
    }

    bool Refresh() {
        const uint32_t previous = status_.revision;
        if (Call(MICROPIXEL_WIRELESS_STATUS, nullptr, 0, &status_, sizeof(status_)) != MICROPIXEL_STATUS_OK)
            return false;
        return previous != status_.revision && page_ != Page::kPassword;
    }

    const char* WirelessStatus() {
        if (!handle_) return "无线服务不可用";
        if (page_ == Page::kWifi) {
            if (status_.wifi_scanning) return "正在扫描…";
            if (status_.wifi_error) {
                return ErrorText("Wi-Fi 操作失败", status_.wifi_error);
            }
            switch (status_.wifi_state) {
                case MPX_WIFI_STARTING:
                    return "正在启动 Wi-Fi…";
                case MPX_WIFI_JOINING:
                    return "正在连接…";
                case MPX_WIFI_DHCP:
                    return "正在获取 IP 地址…";
                case MPX_WIFI_ONLINE:
                    return "Wi-Fi 已连接";
                case MPX_WIFI_UNAVAILABLE:
                    return "Wi-Fi 不可用";
                default:
                    return status_.wifi_count ? "附近的网络" : "点击扫描查找网络";
            }
        }
        if (status_.ble_error) {
            return ErrorText("蓝牙操作失败", status_.ble_error);
        }
        switch (status_.ble_state) {
            case MPX_BLE_STARTING:
                return "正在启动蓝牙…";
            case MPX_BLE_SCANNING:
                return "正在查找蓝牙设备…";
            case MPX_BLE_ADVERTISING:
                return "可发现名称 MicroPixel Orion";
            case MPX_BLE_CONNECTING:
                return "正在连接…";
            case MPX_BLE_OUTGOING:
            case MPX_BLE_INCOMING:
                return "蓝牙已连接";
            case MPX_BLE_UNAVAILABLE:
                return "蓝牙不可用";
            default:
                return status_.ble_count ? "附近的设备" : "点击扫描查找设备";
        }
    }

    const char* ErrorText(const char* message, int error) {
        line_.Clear();
        line_.Append(message);
        line_.Append(" (");
        line_.AppendInt(error);
        line_.Append(")");
        return line_.c_str();
    }

    bool WifiCanScan() const {
        return handle_ && !status_.wifi_scanning &&
               (status_.wifi_state == MPX_WIFI_IDLE || status_.wifi_state == MPX_WIFI_FAILED ||
                status_.wifi_state == MPX_WIFI_ONLINE);
    }

    bool WifiCanLeave() const {
        return status_.wifi_state == MPX_WIFI_ONLINE || status_.wifi_state == MPX_WIFI_JOINING ||
               status_.wifi_state == MPX_WIFI_DHCP;
    }

    bool BleConnected() const { return status_.ble_state == MPX_BLE_INCOMING || status_.ble_state == MPX_BLE_OUTGOING; }

    uint32_t ItemCount() const {
        if (page_ == Page::kWifi) return status_.wifi_scanning ? 0 : status_.wifi_count;
        return status_.ble_state == MPX_BLE_SCANNING ? 0 : status_.ble_count;
    }

    void DrawList(Scene& scene) {
        const uint32_t count = ItemCount();
        if (offset_ >= count) offset_ = 0;
        for (uint32_t row = 0; row < kPageSize && offset_ + row < count; ++row) {
            const uint32_t index = offset_ + row;
            const auto bounds = Row(static_cast<int>(row));
            (void)scene.CreateShape({bounds.x, bounds.y + bounds.height - 1, bounds.width, 1}, panel::Line);
            Rect name{bounds.x + 8, bounds.y, bounds.width - 16, bounds.height};
            if (page_ == Page::kWifi) {
                micropixel_wireless_wifi_item_t network{};
                if (Call(MICROPIXEL_WIRELESS_WIFI_ITEM, &index, sizeof(index), &network, sizeof(network))) continue;
                const char* band = BandLabel(network.channel, network.ssid_bands);
                if (*band) {
                    const Rect badge{bounds.x + bounds.width - 100, bounds.y + 9, 92, 28};
                    name.width = badge.x - name.x - 8;
                    (void)scene.CreateRoundedRect(badge, {.fill = panel::Surface, .radius = 10});
                    Text(scene, {badge.x + badge.width / 2, badge.y + 3}, band, panel::Muted, SystemFont::kSmall, true);
                }
                DeviceName(app_.renderer(), scene, name, network.length ? network.ssid : "隐藏网络",
                           network.supported ? panel::Text : panel::Muted);
            } else {
                micropixel_wireless_ble_item_t device{};
                if (Call(MICROPIXEL_WIRELESS_BLE_ITEM, &index, sizeof(index), &device, sizeof(device))) continue;
                DeviceName(app_.renderer(), scene, name, device.name[0] ? device.name : device.address, panel::Text);
            }
        }
        if (count > kPageSize) {
            Button(scene, kPrevious, "上一页", offset_ > 0);
            Button(scene, kNext, "下一页", offset_ + kPageSize < count);
            line_.Clear();
            line_.AppendUint(offset_ / kPageSize + 1);
            line_.Append(" / ");
            line_.AppendUint((count + kPageSize - 1) / kPageSize);
            Text(scene, {240, 431}, line_.c_str(), panel::Muted, SystemFont::kSmall, true);
        }
    }

    void DrawPassword(Scene& scene) {
        DeviceName(app_.renderer(), scene, {20, 56, 440, 48}, selected_.ssid, panel::Text);
        (void)scene.CreateRoundedRect({20, 113, 440, 49}, {.fill = panel::Surface, .radius = 10});
        char masked[32]{};
        const auto visible = password_length_ < 27 ? password_length_ : 27;
        std::memset(masked, '*', visible);
        Text(scene, {32, 126}, visible ? masked : "Wi-Fi 密码", visible ? panel::Text : panel::Muted);
        line_.Clear();
        line_.AppendUint(password_length_);
        line_.Append(" / 63");
        Text(scene, {20, 168}, line_.c_str(), panel::Muted, SystemFont::kSmall);
        if (selected_.preferred_channel)
            Text(scene, {408, 168}, BandLabel(selected_.preferred_channel), panel::Muted, SystemFont::kSmall);
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 10; ++column) {
                const char key = kKeys[keyboard_][row][column];
                if (key != ' ') {
                    const char label[]{key, 0};
                    Button(scene, Key(row, column), label);
                }
            }
        }
        Button(scene, kShift, keyboard_ == 0 ? "ABC" : keyboard_ == 1 ? "#+=" : "abc");
        Button(scene, kSpace, "空格");
        Button(scene, kErase, "删除");
        Button(scene, kJoin, "连接", password_length_ >= 8, true);
    }

    bool Draw() {
        auto created =
            app_.renderer().CreateScene({.logical_width = 480, .logical_height = 480, .background = panel::Background});
        if (!created) {
            app_.log().Error("Settings scene creation failed");
            return false;
        }
        auto& scene = *created;
        Text(scene, {kBack.x + kBack.width / 2, 13}, symbols::kLeft, panel::Text, SystemFont::kLarge, true);
        Text(scene, {64, 13}, page_ == Page::kPassword ? "连接 Wi-Fi" : "配置", panel::Text, SystemFont::kTitle);
        if (page_ == Page::kPassword) {
            DrawPassword(scene);
        } else {
            Tab(scene, kWifiTab, "Wi-Fi", page_ == Page::kWifi);
            Tab(scene, kBleTab, "蓝牙", page_ == Page::kBle);
            Text(scene, {12, 155}, WirelessStatus(),
                 (page_ == Page::kWifi ? status_.wifi_error : status_.ble_error) ? panel::Red : panel::Muted,
                 SystemFont::kSmall);
            if (page_ == Page::kWifi) {
                Button(scene, kScan, "扫描", WifiCanScan(), true);
                if (WifiCanLeave()) Button(scene, kSecondary, "断开连接");
            } else {
                const bool advertising = status_.ble_state == MPX_BLE_ADVERTISING;
                Button(scene, kScan, "扫描", status_.ble_state == MPX_BLE_IDLE, true);
                Button(scene, kSecondary,
                       BleConnected() ? "断开连接"
                       : advertising  ? "停止广播"
                                      : "允许发现",
                       BleConnected() || advertising || status_.ble_state == MPX_BLE_IDLE);
            }
            DrawList(scene);
        }
        if (app_.renderer().Present(scene)) return true;
        app_.log().Error("Settings scene submit failed");
        return false;
    }

    void ClearPassword() {
        for (volatile char& character : join_.password) character = 0;
        password_length_ = 0;
        join_.length = 0;
    }

    void Join() {
        join_.length = password_length_;
        (void)Call(MICROPIXEL_WIRELESS_WIFI_JOIN, &join_, sizeof(join_));
        ClearPassword();
        page_ = Page::kWifi;
    }

    void Type(Point position) {
        if (Inside(kShift, position)) {
            keyboard_ = (keyboard_ + 1) % 3;
            return;
        }
        if (Inside(kJoin, position) && password_length_ >= 8) {
            Join();
            return;
        }
        if (Inside(kErase, position) && password_length_) {
            join_.password[--password_length_] = 0;
            return;
        }
        if (password_length_ == 63) return;
        if (Inside(kSpace, position)) {
            join_.password[password_length_++] = ' ';
            return;
        }
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 10; ++column) {
                const char letter = kKeys[keyboard_][row][column];
                if (letter != ' ' && Inside(Key(row, column), position)) {
                    join_.password[password_length_++] = letter;
                    return;
                }
            }
        }
    }

    void Select(uint32_t index) {
        if (page_ == Page::kWifi && WifiCanScan() && !WifiCanLeave()) {
            if (Call(MICROPIXEL_WIRELESS_WIFI_ITEM, &index, sizeof(index), &selected_, sizeof(selected_)) ||
                !selected_.supported)
                return;
            join_.index = index;
            if (selected_.open) {
                Join();
            } else {
                keyboard_ = 0;
                page_ = Page::kPassword;
            }
        } else if (page_ == Page::kBle && status_.ble_state == MPX_BLE_IDLE) {
            (void)Call(MICROPIXEL_WIRELESS_BLE_CONNECT, &index, sizeof(index));
        }
    }

    void Back() {
        if (page_ == Page::kPassword) {
            ClearPassword();
            page_ = Page::kWifi;
        } else {
            closed_ = true;
        }
    }

    void Click(Point position) {
        if (Inside(kBack, position)) {
            Back();
            return;
        }
        if (page_ == Page::kPassword) {
            Type(position);
            return;
        }
        if (Inside(kWifiTab, position) || Inside(kBleTab, position)) {
            page_ = Inside(kWifiTab, position) ? Page::kWifi : Page::kBle;
            offset_ = 0;
            return;
        }
        if (Inside(kScan, position)) {
            if (page_ == Page::kWifi && WifiCanScan()) (void)Call(MICROPIXEL_WIRELESS_WIFI_SCAN);
            if (page_ == Page::kBle && status_.ble_state == MPX_BLE_IDLE) (void)Call(MICROPIXEL_WIRELESS_BLE_SCAN);
            offset_ = 0;
            return;
        }
        if (Inside(kSecondary, position)) {
            if (page_ == Page::kWifi && WifiCanLeave()) (void)Call(MICROPIXEL_WIRELESS_WIFI_LEAVE);
            if (page_ == Page::kBle) {
                if (BleConnected())
                    (void)Call(MICROPIXEL_WIRELESS_BLE_DISCONNECT);
                else if (status_.ble_state == MPX_BLE_ADVERTISING)
                    (void)Call(MICROPIXEL_WIRELESS_BLE_HIDE);
                else if (status_.ble_state == MPX_BLE_IDLE)
                    (void)Call(MICROPIXEL_WIRELESS_BLE_ADVERTISE);
            }
            return;
        }
        if (Inside(kPrevious, position) && offset_ > 0) offset_ -= kPageSize;
        if (Inside(kNext, position) && offset_ + kPageSize < ItemCount()) offset_ += kPageSize;
        for (uint32_t row = 0; row < kPageSize && offset_ + row < ItemCount(); ++row) {
            if (Inside(Row(static_cast<int>(row)), position)) Select(offset_ + row);
        }
    }

    Application app_;
    micropixel_service_handle_t handle_{};
    micropixel_wireless_status_t status_{};
    micropixel_wireless_wifi_item_t selected_{};
    micropixel_wireless_join_t join_{};
    FixedString<96> line_;
    Page page_{Page::kWifi};
    Point down_{};
    unsigned offset_{};
    uint8_t keyboard_{};
    uint8_t password_length_{};
    bool closed_{};
};

}  // namespace

int main() {
    Settings settings;
    return settings.Run();
}
