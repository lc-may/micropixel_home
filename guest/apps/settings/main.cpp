// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <cstring>

#include "abi/wireless.h"
#include "sdk/fixed_string.hpp"
#include "sdk/micropixel.hpp"
#include "sdk/ui/wrap_text.hpp"

namespace {

using namespace micropixel;
constexpr auto kBackground = Color::Rgb(21, 38, 59);
constexpr auto kPaper = Color::Rgb(246, 249, 255);
constexpr auto kAccent = Color::Rgb(41, 104, 171);
constexpr auto kMuted = Color::Rgb(105, 128, 155);
constexpr int kPageSize = 5;
constexpr Rect kBack{20, 18, 80, 42};
constexpr Rect kWifiTab{20, 72, 214, 46};
constexpr Rect kBleTab{246, 72, 214, 46};
constexpr Rect kScan{20, 160, 210, 48};
constexpr Rect kSecondary{242, 160, 218, 48};
constexpr Rect kPrevious{20, 416, 108, 44};
constexpr Rect kNext{352, 416, 108, 44};
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

Rect Row(int index) { return {20, 218 + 38 * index, 440, 34}; }
Rect Key(int row, int column) { return {20 + 44 * column, 198 + row * 52, 40, 46}; }

void Text(Scene& scene, Point point, const char* label, Color color = kPaper, SystemFont font = SystemFont::kMedium,
          bool centered = false) {
    (void)scene.CreateLabel(point, label, color, font, centered);
}

void DeviceName(const Renderer& renderer, Scene& scene, Point point, const char* name, uint32_t max_width,
                Color color) {
    char label[37]{};
    const std::string_view input{name};
    if (!ui::ValidUtf8(input) || input.find_first_of("\r\n\t") != std::string_view::npos) {
        Text(scene, point, "Unnamed device", color);
        return;
    }
    const auto length = ui::FittingTextPrefix(input, max_width - 20, [&](std::string_view prefix) {
        std::memcpy(label, prefix.data(), prefix.size());
        label[prefix.size()] = 0;
        const auto dimensions = renderer.MeasureText(label, SystemFont::kMedium);
        return dimensions ? dimensions->width : UINT32_MAX;
    });
    std::memcpy(label, name, length);
    if (length < input.size())
        std::memcpy(label + length, "...", 4);
    else
        label[length] = 0;
    Text(scene, point, label, color);
}

void Button(Scene& scene, Rect bounds, const char* label, bool enabled = true) {
    (void)scene.CreateRoundedRect(bounds, {.fill = enabled ? kAccent : kMuted, .radius = 8});
    Text(scene, {bounds.x + bounds.width / 2, bounds.y + (bounds.height - 22) / 2}, label, kPaper, SystemFont::kMedium,
         true);
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
            micropixel_service_open(MICROPIXEL_SERVICE_WIRELESS, MICROPIXEL_WIRELESS_VERSION, &info, sizeof(info));
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
        if (!handle_) return "Wireless service unavailable";
        if (page_ == Page::kWifi) {
            if (status_.wifi_scanning) return "Scanning for Wi-Fi networks...";
            if (status_.wifi_state == MPX_WIFI_ONLINE) {
                line_.Clear();
                line_.Append("Connected  ");
                for (unsigned i = 0; i < 4; ++i) {
                    if (i) line_.Append(".");
                    line_.AppendUint(status_.ipv4[i]);
                }
                return line_.c_str();
            }
            if (status_.wifi_error) {
                return ErrorText("Wi-Fi operation failed", status_.wifi_error);
            }
            switch (status_.wifi_state) {
                case MPX_WIFI_STARTING:
                    return "Starting Wi-Fi...";
                case MPX_WIFI_JOINING:
                    return "Connecting...";
                case MPX_WIFI_DHCP:
                    return "Obtaining IP address...";
                case MPX_WIFI_UNAVAILABLE:
                    return "Wi-Fi unavailable";
                default:
                    return "Choose Scan to find networks";
            }
        }
        if (status_.ble_error) {
            return ErrorText("BLE operation failed", status_.ble_error);
        }
        switch (status_.ble_state) {
            case MPX_BLE_STARTING:
                return "Starting BLE...";
            case MPX_BLE_SCANNING:
                return "Discovering nearby BLE devices...";
            case MPX_BLE_ADVERTISING:
                return "Discoverable as MicroPixel Orion";
            case MPX_BLE_CONNECTING:
                return "Connecting to a BLE device...";
            case MPX_BLE_OUTGOING:
                return "Connected to a BLE device";
            case MPX_BLE_INCOMING:
                return "A BLE device is connected";
            case MPX_BLE_UNAVAILABLE:
                return "BLE unavailable";
            default:
                return "Scan or make this board discoverable";
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

    const char* SignalText(const char* name, int rssi) {
        line_.Clear();
        line_.Append(name);
        line_.Append("  ");
        line_.AppendInt(rssi);
        line_.Append(" dBm");
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
            (void)scene.CreateRoundedRect(bounds, {.fill = kPaper, .radius = 7});
            if (page_ == Page::kWifi) {
                micropixel_wireless_wifi_item_t network{};
                if (Call(MICROPIXEL_WIRELESS_WIFI_ITEM, &index, sizeof(index), &network, sizeof(network))) continue;
                DeviceName(app_.renderer(), scene, {bounds.x + 12, bounds.y + 6}, network.ssid, 280, kBackground);
                const char* security = !network.supported ? "Unsupported" : network.open ? "Open" : "Secured";
                Text(scene, {bounds.x + 292, bounds.y + 8}, SignalText(security, network.rssi), kBackground,
                     SystemFont::kSmall);
            } else {
                micropixel_wireless_ble_item_t device{};
                if (Call(MICROPIXEL_WIRELESS_BLE_ITEM, &index, sizeof(index), &device, sizeof(device))) continue;
                DeviceName(app_.renderer(), scene, {bounds.x + 12, bounds.y + 6},
                           device.name[0] ? device.name : device.address, 240, kBackground);
                Text(scene, {bounds.x + 245, bounds.y + 8}, SignalText(device.address, device.rssi), kBackground,
                     SystemFont::kSmall);
            }
        }
        if (count > kPageSize) {
            Button(scene, kPrevious, "Previous", offset_ > 0);
            Button(scene, kNext, "Next", offset_ + kPageSize < count);
        }
    }

    void DrawPassword(Scene& scene) {
        DeviceName(app_.renderer(), scene, {20, 79}, selected_.ssid, 440, kPaper);
        (void)scene.CreateRoundedRect({20, 113, 440, 49}, {.fill = kPaper, .radius = 8});
        char masked[32]{};
        const auto visible = password_length_ < 27 ? password_length_ : 27;
        std::memset(masked, '*', visible);
        Text(scene, {32, 125}, visible ? masked : "Wi-Fi password", kBackground);
        line_.Clear();
        line_.AppendUint(password_length_);
        line_.Append(" / 63 characters");
        Text(scene, {20, 168}, line_.c_str(), kPaper, SystemFont::kSmall);
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
        Button(scene, kSpace, "Space");
        Button(scene, kErase, "Delete");
        Button(scene, kJoin, "Join", password_length_ >= 8);
    }

    bool Draw() {
        auto created =
            app_.renderer().CreateScene({.logical_width = 480, .logical_height = 480, .background = kBackground});
        if (!created) {
            app_.log().Error("Settings scene creation failed");
            return false;
        }
        auto& scene = *created;
        Button(scene, kBack, "Back");
        Text(scene, {118, 24}, page_ == Page::kPassword ? "Join Wi-Fi" : "配置", kPaper, SystemFont::kTitle);
        if (page_ == Page::kPassword) {
            DrawPassword(scene);
        } else {
            Button(scene, kWifiTab, "Wi-Fi", page_ == Page::kWifi);
            Button(scene, kBleTab, "Bluetooth LE", page_ == Page::kBle);
            Text(scene, {20, 130}, WirelessStatus());
            if (page_ == Page::kWifi) {
                Button(scene, kScan, "Scan", WifiCanScan());
                if (WifiCanLeave()) Button(scene, kSecondary, "Disconnect");
            } else {
                const bool advertising = status_.ble_state == MPX_BLE_ADVERTISING;
                Button(scene, kScan, "Scan", status_.ble_state == MPX_BLE_IDLE);
                Button(scene, kSecondary,
                       BleConnected() ? "Disconnect"
                       : advertising  ? "Stop advertising"
                                      : "Make discoverable",
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
