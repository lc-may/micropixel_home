// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "abi/wireless.h"
#include "sdk/bluetooth.hpp"
#include "sdk/fixed_string.hpp"
#include "sdk/micropixel.hpp"
#include "sdk/ui/panel.hpp"
#include "sdk/ui/wrap_text.hpp"

namespace {

using namespace micropixel;
namespace panel = ui::panel;
constexpr int kPageSize = 5;
constexpr unsigned kMaxScanDevices = 16;
constexpr int kScanTop = 180;
constexpr int kScanHeadingHeight = 24;
constexpr int kScanRowHeight = 46;
constexpr int kScanBottom = 412;
constexpr Rect kBack{12, 4, 44, 46};
constexpr Rect kWifiTab{12, 52, 226, 44};
constexpr Rect kBleTab{242, 52, 226, 44};
constexpr Rect kRadioTabs[] = {{12, 52, 148, 44}, {166, 52, 148, 44}, {320, 52, 148, 44}};
constexpr Rect kScan{12, 104, 104, 44};
constexpr Rect kSecondary{124, 104, 154, 44};
constexpr Rect kRole{286, 104, 182, 44};
constexpr Rect kBleLink = kRole;
constexpr Rect kStream{12, 238, 146, 44};
constexpr Rect kPair{166, 238, 120, 44};
constexpr Rect kSaved{316, 420, 152, 44};
constexpr Rect kClassicNext{124, 420, 100, 44};
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

enum class Page : uint8_t { kWifi, kBle, kClassic, kPassword };

struct ScanDevice {
    micropixel_wireless_ble_item_t item;
    uint32_t index;
    uint8_t group;
    int top;
    bool heading;
};

struct ScanPage {
    unsigned first;
    unsigned count;
};

unsigned GroupOrder(uint8_t group) {
    if (group == MPX_BT_DEVICE_INPUT) return 0;
    if (group == MPX_BT_DEVICE_AUDIO) return 1;
    return 2;
}

const char* GroupLabel(uint8_t group) {
    constexpr const char* labels[] = {"鼠标、键盘和笔", "音频", "其他设备"};
    return labels[GroupOrder(group)];
}

Rect ScanRow(const ScanDevice& device) { return {12, device.top, 456, kScanRowHeight}; }

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
        if (status == MICROPIXEL_STATUS_OK) {
            handle_ = info.service_handle;
            ble_details_ = info.interface_major == 1 && info.interface_minor >= 2;
            ble_groups_ = info.interface_major == 1 && info.interface_minor >= 3;
            ble_discovery_control_ = info.interface_major == 1 && info.interface_minor >= 4;
        }
        (void)bluetooth_.Open();
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
        const int result = micropixel_service_call(handle_, method, static_cast<const uint8_t*>(request), request_size,
                                                   static_cast<uint8_t*>(response), response_size, &size);
        return result == MICROPIXEL_STATUS_OK && size != response_size ? MICROPIXEL_STATUS_INTERNAL : result;
    }

    bool Refresh() {
        if (page_ == Page::kClassic) {
            const auto revision = bt_.revision;
            (void)bluetooth_.Status(bt_);
            return revision != bt_.revision;
        }
        const uint32_t previous = status_.revision;
        if (Call(MICROPIXEL_WIRELESS_STATUS, nullptr, 0, &status_, sizeof(status_)) != MICROPIXEL_STATUS_OK)
            return false;
        if (ble_details_ && previous != status_.revision &&
            Call(MICROPIXEL_WIRELESS_BLE_INFO, nullptr, 0, &ble_info_, sizeof(ble_info_)) == MICROPIXEL_STATUS_OK) {
            if (!(ble_info_.flags & MPX_BLE_COMMAND_PENDING)) ble_request_ = 0;
        }
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
        if (ble_request_error_ == MICROPIXEL_STATUS_WOULD_BLOCK) return "无线服务正忙，请重试";
        if (ble_request_error_ == MICROPIXEL_STATUS_NOT_FOUND) return "设备列表已更新，请重新选择";
        if (ble_request_error_) return ErrorText("蓝牙操作失败", ble_request_error_);
        if (status_.ble_state != MPX_BLE_UNAVAILABLE && status_.ble_error)
            return ErrorText("蓝牙操作失败", status_.ble_error);
        if (ble_request_ || (ble_info_.flags & MPX_BLE_COMMAND_PENDING)) return "正在处理蓝牙请求…";
        if (ble_info_.flags & MPX_BLE_DISCONNECT_PENDING)
            return status_.ble_state == MPX_BLE_CONNECTING ? "正在取消连接…" : "正在断开连接…";
        if (status_.ble_state == MPX_BLE_ADVERTISING || (ble_discovery_control_ && BleDiscoveryEnabled())) {
            line_.Clear();
            line_.Append(ble_discovery_control_ ? "发现已开启 " : "可发现名称 ");
            line_.Append(ble_details_ ? ble_info_.local_name : "MicroPixel Orion");
            return line_.c_str();
        }
        switch (status_.ble_state) {
            case MPX_BLE_STARTING:
                return "正在启动蓝牙…";
            case MPX_BLE_SCANNING:
                return "正在查找蓝牙设备…";
            case MPX_BLE_CONNECTING:
                return "正在连接…";
            case MPX_BLE_OUTGOING:
                return "蓝牙已连接 · 主动连接";
            case MPX_BLE_INCOMING:
                return "蓝牙已连接 · 对端接入";
            case MPX_BLE_UNAVAILABLE:
                return status_.ble_error ? ErrorText("蓝牙不可用", status_.ble_error) : "蓝牙不可用";
            default:
                if (status_.ble_count) return "附近的设备";
                return ble_info_.flags & MPX_BLE_SCAN_COMPLETE ? "未找到可连接的设备" : "点击扫描查找设备";
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
    bool BleConnecting() const { return ble_details_ && status_.ble_state == MPX_BLE_CONNECTING; }
    bool BleDiscoveryEnabled() const { return ble_info_.flags & MPX_BLE_DISCOVERY_ENABLED; }

    bool BleBusy() const {
        return ble_request_ || (ble_info_.flags & (MPX_BLE_COMMAND_PENDING | MPX_BLE_DISCONNECT_PENDING));
    }

    int BleItemCall(uint32_t method, uint32_t index, micropixel_wireless_ble_item_t* item = nullptr) const {
        const micropixel_wireless_ble_selection_t selection{.index = index, .scan_revision = ble_info_.scan_revision};
        return Call(method, &selection, ble_details_ ? sizeof(selection) : sizeof(index), item,
                    item ? sizeof(*item) : 0);
    }

    void BleCommand(uint32_t method, uint32_t index = 0) {
        ble_request_error_ = method == MICROPIXEL_WIRELESS_BLE_CONNECT ? BleItemCall(method, index) : Call(method);
        if (ble_details_ && ble_request_error_ == MICROPIXEL_STATUS_OK) ble_request_ = method;
    }

    uint32_t ItemCount() const {
        if (page_ == Page::kWifi) return status_.wifi_scanning ? 0 : status_.wifi_count;
        if (page_ == Page::kClassic) return bt_.state == MPX_BT_IDLE ? (saved_ ? bt_.bond_count : bt_.device_count) : 0;
        return status_.ble_state == MPX_BLE_IDLE ? status_.ble_count : 0;
    }

    void DrawScanRow(Scene& scene, Rect bounds, const micropixel_wireless_ble_item_t& device) {
        const char* name = device.name[0] ? device.name : "未命名设备";
        if (!ui::ValidUtf8(name) || std::string_view{name}.find_first_of("\r\n\t") != std::string_view::npos)
            name = "无法显示的名称";
        const auto fitted = ui::WrapText<1>(name, 344, [&](std::string_view text) {
            char measured[36]{};
            std::memcpy(measured, text.data(), text.size());
            const auto size = app_.renderer().MeasureText(measured, SystemFont::kMedium);
            return size ? size->width : UINT32_MAX;
        });
        const bool busy = page_ == Page::kClassic ? bt_.pending != 0 : BleBusy();
        const Color color = busy ? panel::Disabled : panel::Text;
        Text(scene, {bounds.x + 8, bounds.y}, fitted[0].c_str(), color);
        Text(scene, {bounds.x + 8, bounds.y + 24}, device.address, panel::Muted, SystemFont::kSmall);
        line_.Clear();
        if (device.rssi == 127) {
            line_.Append("—");
        } else {
            line_.AppendInt(device.rssi);
            line_.Append(" dBm");
        }
        Text(scene, {bounds.x + bounds.width - 48, bounds.y + 12}, line_.c_str(), panel::Muted, SystemFont::kSmall,
             true);
    }

    void DrawBle(Scene& scene) {
        const bool idle = status_.ble_state == MPX_BLE_IDLE;
        const bool advertising = status_.ble_state == MPX_BLE_ADVERTISING;
        const bool stop_scan = ble_details_ && status_.ble_state == MPX_BLE_SCANNING;
        const bool connected = BleConnected();
        Button(scene, kScan, stop_scan ? "停止扫描" : "扫描", !BleBusy() && (idle || stop_scan), true);
        const bool connecting = BleConnecting();
        if (ble_discovery_control_) {
            const bool discoverable = BleDiscoveryEnabled();
            Button(scene, kSecondary, discoverable ? "停止广播" : "允许发现", !BleBusy() && (discoverable || idle));
            Button(scene, kBleLink, connecting ? "取消连接" : "断开连接", !BleBusy() && (connected || connecting));
        } else {
            Button(scene, kSecondary,
                   connecting    ? "取消连接"
                   : connected   ? "断开连接"
                   : advertising ? "停止广播"
                                 : "允许发现",
                   !BleBusy() && (connected || connecting || advertising || idle));
        }
        if (ble_info_.peer_address[0]) {
            if (ble_info_.peer_name[0])
                DeviceName(app_.renderer(), scene, {20, 186, 440, 46}, ble_info_.peer_name, panel::Text);
            Text(scene, {20, ble_info_.peer_name[0] ? 231 : 190}, ble_info_.peer_address, panel::Muted,
                 SystemFont::kSmall);
        }
    }

    bool BtCanScan() const { return !bt_.pending && bt_.state == MPX_BT_IDLE && !bt_.visible; }
    bool BtConnected() const { return bt_.state == MPX_BT_CONNECTED; }

    const char* BtLabel() {
        if (bt_request_error_ || bt_.error)
            return ErrorText("蓝牙操作失败", bt_request_error_ ? bt_request_error_ : bt_.error);
        if (bt_.pending) return "正在处理…";
        switch (bt_.state) {
            case MPX_BT_UNAVAILABLE:
                return "经典蓝牙不可用";
            case MPX_BT_SCANNING:
                return "正在搜索设备…";
            case MPX_BT_CONNECTING:
                return "正在连接…";
            case MPX_BT_CONNECTED:
                switch (bt_.stream) {
                    case MPX_BT_STREAM_CONNECTING:
                        return "正在连接 A2DP…";
                    case MPX_BT_STREAM_OPEN:
                        return "A2DP 已连接";
                    case MPX_BT_STREAM_PLAYING:
                        return "A2DP 正在传输";
                    case MPX_BT_STREAM_SUSPENDED:
                        return "A2DP 已暂停";
                    default:
                        return "蓝牙已连接";
                }
            default:
                return bt_.visible ? "附近设备可发现 Orion" : (saved_ ? "已配对设备" : "选择设备连接");
        }
    }

    void DrawClassic(Scene& scene) {
        const bool idle = bt_.state == MPX_BT_IDLE;
        const bool scanning = bt_.state == MPX_BT_SCANNING;
        const bool linking = bt_.state == MPX_BT_CONNECTING;
        Text(scene, {12, 155}, BtLabel(), bt_request_error_ || bt_.error ? panel::Red : panel::Muted,
             SystemFont::kSmall);
        Button(scene, kScan, scanning ? "停止扫描" : "扫描", BtCanScan() || (scanning && !bt_.pending), true);
        Button(scene, kSecondary,
               BtConnected() || linking ? "断开连接"
               : bt_.visible            ? "关闭发现"
                                        : "允许发现",
               !bt_.pending && (idle || linking || BtConnected()));
        Button(scene, kRole, bt_.role == MPX_BT_SOURCE ? "发送 · Source" : "接收 · Sink",
               !bt_.pending && idle && !bt_.visible);
        if (idle && (saved_ || bt_.bond_count)) Button(scene, kSaved, saved_ ? "扫描结果" : "已配对");
        if (!BtConnected() && !linking) return;
        Text(scene, {20, 187}, bt_.peer_address, panel::Text);
        if (!BtConnected()) return;
        if (bt_.role == MPX_BT_SOURCE) {
            Button(scene, kStream,
                   bt_.stream == MPX_BT_STREAM_PLAYING ? "暂停音频"
                   : bt_.stream == MPX_BT_STREAM_IDLE  ? "连接音频"
                                                       : "开始音频",
                   !bt_.pending && bt_.stream != MPX_BT_STREAM_CONNECTING);
        } else {
            Text(scene, {20, 246}, "在对端播放音频", panel::Muted, SystemFont::kSmall);
        }
        Button(scene, kPair, bt_.security >= 2 ? "已配对" : "配对", !bt_.pending && bt_.security < 2);
        if (bt_.sample_rate) {
            line_.Clear();
            line_.Append("SBC  ");
            line_.AppendUint(bt_.sample_rate);
            line_.Append(" Hz");
            Text(scene, {20, 296}, line_.c_str(), panel::Muted, SystemFont::kSmall);
        }
    }

    void DrawBondRow(Scene& scene, Rect bounds, uint32_t index) {
        micropixel_bluetooth_address_t address{};
        if (bluetooth_.Bond(index, address) != MICROPIXEL_STATUS_OK) return;
        Text(scene, {bounds.x + 8, bounds.y + 10}, address.address);
        Text(scene, {bounds.x + bounds.width - 76, bounds.y + 10}, "取消配对", panel::Muted, SystemFont::kSmall);
    }

    bool ReadScanDevice(uint32_t index, ScanDevice& device) const {
        device = {};
        device.index = index;
        if (page_ == Page::kClassic) {
            micropixel_bluetooth_device_t classic{};
            if (bluetooth_.Device({bt_.scan_revision, index}, classic) != MICROPIXEL_STATUS_OK) return false;
            std::memcpy(device.item.name, classic.name, sizeof(device.item.name));
            std::memcpy(device.item.address, classic.address, sizeof(device.item.address));
            device.item.rssi = classic.rssi;
            device.group = classic.group;
            return true;
        }
        if (!ble_groups_) return BleItemCall(MICROPIXEL_WIRELESS_BLE_ITEM, index, &device.item) == MICROPIXEL_STATUS_OK;

        const micropixel_wireless_ble_selection_t selection{.index = index, .scan_revision = ble_info_.scan_revision};
        micropixel_wireless_ble_item_details_t details{};
        if (Call(MICROPIXEL_WIRELESS_BLE_ITEM_DETAILS, &selection, sizeof(selection), &details, sizeof(details)) !=
            MICROPIXEL_STATUS_OK)
            return false;
        device.item = details.item;
        device.group = details.group;
        return true;
    }

    void LayoutScanList() {
        const auto count = std::min<unsigned>(ItemCount(), scan_devices_.size());
        unsigned loaded = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (ReadScanDevice(i, scan_devices_[loaded])) ++loaded;
        }
        std::sort(scan_devices_.begin(), scan_devices_.begin() + loaded,
                  [](const ScanDevice& left, const ScanDevice& right) {
                      const auto left_group = GroupOrder(left.group);
                      const auto right_group = GroupOrder(right.group);
                      return left_group == right_group ? left.index < right.index : left_group < right_group;
                  });
        scan_page_count_ = 0;
        int top = kScanTop;
        unsigned last_group = GroupOrder(MPX_BT_DEVICE_OTHER);
        for (unsigned i = 0; i < loaded; ++i) {
            auto& device = scan_devices_[i];
            const unsigned group = GroupOrder(device.group);
            bool heading = group != last_group;
            if (i == 0 || top + kScanRowHeight + (heading ? kScanHeadingHeight : 0) > kScanBottom) {
                scan_pages_[scan_page_count_++] = {.first = i, .count = 0};
                top = kScanTop;
                heading = true;
            }
            if (heading) top += kScanHeadingHeight;
            device.heading = heading;
            device.top = top;
            top += kScanRowHeight;
            last_group = group;
            ++scan_pages_[scan_page_count_ - 1].count;
        }
        if (scan_page_ >= scan_page_count_) scan_page_ = 0;
    }

    void DrawPager(Scene& scene, unsigned current, unsigned pages) {
        if (pages < 2) return;
        Button(scene, kPrevious, "上一页", current > 0);
        Button(scene, page_ == Page::kClassic ? kClassicNext : kNext, "下一页", current + 1 < pages);
        line_.Clear();
        line_.AppendUint(current + 1);
        line_.Append(" / ");
        line_.AppendUint(pages);
        Text(scene, {240, 431}, line_.c_str(), panel::Muted, SystemFont::kSmall, true);
    }

    void DrawGroupedScan(Scene& scene) {
        LayoutScanList();
        if (!scan_page_count_) return;
        const auto& page = scan_pages_[scan_page_];
        for (unsigned i = page.first; i < page.first + page.count; ++i) {
            const auto& device = scan_devices_[i];
            const auto bounds = ScanRow(device);
            if (device.heading)
                Text(scene, {bounds.x + 8, bounds.y - kScanHeadingHeight}, GroupLabel(device.group), panel::Muted,
                     SystemFont::kSmall);
            (void)scene.CreateShape({bounds.x, bounds.y + bounds.height - 1, bounds.width, 1}, panel::Line);
            DrawScanRow(scene, bounds, device.item);
        }
        DrawPager(scene, scan_page_, scan_page_count_);
    }

    void DrawList(Scene& scene) {
        if (page_ == Page::kBle || (page_ == Page::kClassic && !saved_)) {
            DrawGroupedScan(scene);
            return;
        }
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
                DrawBondRow(scene, bounds, index);
            }
        }
        DrawPager(scene, offset_ / kPageSize, (count + kPageSize - 1) / kPageSize);
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
            const bool classic = bluetooth_.available();
            Tab(scene, classic ? kRadioTabs[0] : kWifiTab, "Wi-Fi", page_ == Page::kWifi);
            Tab(scene, classic ? kRadioTabs[1] : kBleTab, classic ? "BLE" : "蓝牙", page_ == Page::kBle);
            if (classic) Tab(scene, kRadioTabs[2], "经典蓝牙", page_ == Page::kClassic);
            if (page_ != Page::kClassic) {
                Text(scene, {12, 155}, WirelessStatus(),
                     (page_ == Page::kWifi ? status_.wifi_error != 0 : status_.ble_error || ble_request_error_)
                         ? panel::Red
                         : panel::Muted,
                     SystemFont::kSmall);
            }
            if (page_ == Page::kWifi) {
                Button(scene, kScan, "扫描", WifiCanScan(), true);
                if (WifiCanLeave()) Button(scene, kSecondary, "断开连接");
            } else if (page_ == Page::kBle) {
                DrawBle(scene);
            } else {
                DrawClassic(scene);
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
        } else if (page_ == Page::kBle && status_.ble_state == MPX_BLE_IDLE && !BleBusy()) {
            BleCommand(MICROPIXEL_WIRELESS_BLE_CONNECT, index);
        } else if (page_ == Page::kClassic && bt_.state == MPX_BT_IDLE && !bt_.pending) {
            micropixel_bluetooth_device_t device{};
            if (bluetooth_.Device({bt_.scan_revision, index}, device) == MICROPIXEL_STATUS_OK) {
                micropixel_bluetooth_address_t address{};
                std::memcpy(address.address, device.address, sizeof(address.address));
                bt_request_error_ = bluetooth_.Connect(address);
            }
        }
    }

    void ClickGroupedScan(Point position) {
        if (!scan_page_count_) return;
        if (Inside(kPrevious, position) && scan_page_ > 0) {
            --scan_page_;
            return;
        }
        if (Inside(page_ == Page::kClassic ? kClassicNext : kNext, position) && scan_page_ + 1 < scan_page_count_) {
            ++scan_page_;
            return;
        }
        const auto& page = scan_pages_[scan_page_];
        for (unsigned i = page.first; i < page.first + page.count; ++i) {
            const auto& device = scan_devices_[i];
            if (Inside(ScanRow(device), position)) {
                Select(device.index);
                return;
            }
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

    void ClickClassic(Point position) {
        if (bt_.pending) return;
        if (Inside(kScan, position)) {
            if (bt_.state == MPX_BT_SCANNING) {
                bt_request_error_ = bluetooth_.StopScan();
            } else if (BtCanScan()) {
                scan_page_ = 0;
                saved_ = false;
                bt_request_error_ = bluetooth_.Scan();
            }
        } else if (Inside(kSecondary, position)) {
            if (BtConnected() || bt_.state == MPX_BT_CONNECTING)
                bt_request_error_ = bluetooth_.Disconnect();
            else if (bt_.state == MPX_BT_IDLE)
                bt_request_error_ = bluetooth_.SetVisible(!bt_.visible);
        } else if (Inside(kRole, position) && BtCanScan()) {
            bt_request_error_ = bluetooth_.SetRole(bt_.role == MPX_BT_SINK ? MPX_BT_SOURCE : MPX_BT_SINK);
        } else if (BtConnected() && Inside(kPair, position) && bt_.security < 2) {
            bt_request_error_ = bluetooth_.Pair();
        } else if (BtConnected() && bt_.role == MPX_BT_SOURCE && bt_.stream != MPX_BT_STREAM_CONNECTING &&
                   Inside(kStream, position)) {
            if (bt_.stream == MPX_BT_STREAM_IDLE)
                bt_request_error_ = bluetooth_.OpenStream();
            else if (bt_.stream == MPX_BT_STREAM_PLAYING)
                bt_request_error_ = bluetooth_.SuspendStream();
            else
                bt_request_error_ = bluetooth_.StartStream();
        } else if (bt_.state == MPX_BT_IDLE) {
            if (Inside(kSaved, position) && (saved_ || bt_.bond_count)) {
                saved_ = !saved_;
                offset_ = 0;
                scan_page_ = 0;
                return;
            }
            if (!saved_) {
                ClickGroupedScan(position);
                return;
            }
            if (Inside(kPrevious, position) && offset_ > 0) offset_ -= kPageSize;
            if (Inside(kClassicNext, position) && offset_ + kPageSize < ItemCount()) offset_ += kPageSize;
            for (uint32_t row = 0; row < kPageSize && offset_ + row < ItemCount(); ++row) {
                if (!Inside(Row(static_cast<int>(row)), position)) continue;
                micropixel_bluetooth_address_t address{};
                if (bluetooth_.Bond(offset_ + row, address) == MICROPIXEL_STATUS_OK)
                    bt_request_error_ = position.x >= 376 ? bluetooth_.Forget(address) : bluetooth_.Connect(address);
                return;
            }
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
        const bool classic = bluetooth_.available();
        const bool wifi = Inside(classic ? kRadioTabs[0] : kWifiTab, position);
        const bool ble = Inside(classic ? kRadioTabs[1] : kBleTab, position);
        const bool bt = classic && Inside(kRadioTabs[2], position);
        if (wifi || ble || bt) {
            page_ = wifi ? Page::kWifi : ble ? Page::kBle : Page::kClassic;
            offset_ = 0;
            scan_page_ = 0;
            return;
        }
        if (page_ == Page::kClassic) {
            ClickClassic(position);
            return;
        }
        if (Inside(kScan, position)) {
            if (page_ == Page::kWifi && WifiCanScan()) (void)Call(MICROPIXEL_WIRELESS_WIFI_SCAN);
            if (page_ == Page::kBle && !BleBusy()) {
                if (status_.ble_state == MPX_BLE_IDLE)
                    BleCommand(MICROPIXEL_WIRELESS_BLE_SCAN);
                else if (ble_details_ && status_.ble_state == MPX_BLE_SCANNING)
                    BleCommand(MICROPIXEL_WIRELESS_BLE_STOP_SCAN);
            }
            offset_ = 0;
            scan_page_ = 0;
            return;
        }
        if (page_ == Page::kBle) {
            if (BleBusy()) return;
            if (ble_discovery_control_ && Inside(kBleLink, position)) {
                if (BleConnected() || BleConnecting()) BleCommand(MICROPIXEL_WIRELESS_BLE_DISCONNECT);
                return;
            }
            if (Inside(kSecondary, position)) {
                if (!ble_discovery_control_ && (BleConnected() || BleConnecting()))
                    BleCommand(MICROPIXEL_WIRELESS_BLE_DISCONNECT);
                else if (ble_discovery_control_ ? BleDiscoveryEnabled() : status_.ble_state == MPX_BLE_ADVERTISING)
                    BleCommand(MICROPIXEL_WIRELESS_BLE_HIDE);
                else if (status_.ble_state == MPX_BLE_IDLE)
                    BleCommand(MICROPIXEL_WIRELESS_BLE_ADVERTISE);
                return;
            }
            ClickGroupedScan(position);
            return;
        }
        if (Inside(kSecondary, position)) {
            if (WifiCanLeave()) (void)Call(MICROPIXEL_WIRELESS_WIFI_LEAVE);
            return;
        }
        if (Inside(kPrevious, position) && offset_ > 0) offset_ -= kPageSize;
        if (Inside(kNext, position) && offset_ + kPageSize < ItemCount()) offset_ += kPageSize;
        for (uint32_t row = 0; row < kPageSize && offset_ + row < ItemCount(); ++row) {
            if (Inside(Row(static_cast<int>(row)), position)) Select(offset_ + row);
        }
    }

    Application app_;
    Bluetooth bluetooth_;
    micropixel_bluetooth_status_t bt_{};
    int bt_request_error_{};
    bool saved_{};
    micropixel_service_handle_t handle_{};
    micropixel_wireless_status_t status_{};
    micropixel_wireless_ble_info_t ble_info_{};
    micropixel_wireless_wifi_item_t selected_{};
    micropixel_wireless_join_t join_{};
    FixedString<96> line_;
    Page page_{Page::kWifi};
    Point down_{};
    unsigned offset_{};
    std::array<ScanDevice, kMaxScanDevices> scan_devices_{};
    std::array<ScanPage, kMaxScanDevices> scan_pages_{};
    unsigned scan_page_{};
    unsigned scan_page_count_{};
    uint32_t ble_request_{};
    int ble_request_error_{};
    uint8_t keyboard_{};
    uint8_t password_length_{};
    bool closed_{};
    bool ble_details_{};
    bool ble_groups_{};
    bool ble_discovery_control_{};
};

}  // namespace

int main() {
    Settings settings;
    return settings.Run();
}
