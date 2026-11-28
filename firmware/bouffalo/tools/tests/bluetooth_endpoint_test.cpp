// SPDX-License-Identifier: Apache-2.0
#include "runtime/abi/bluetooth_endpoint.hpp"

#include <cassert>
#include <cstring>
#include <vector>

#include "platform/wireless/avrcp_track.hpp"
#include "platform/wireless/classic_bluetooth.h"

namespace {
constexpr uint32_t kSession = 27;
uint32_t remote_calls{};
uint32_t settings_calls{};

void TrackResponses() {
    using namespace micropixel::bouffalo::avrcp;
    const auto command = TrackCommand(TrackAction::kRead);
    assert(command.size() == 31 && command[6] == 0x20 && command[18] == 3);
    assert(TrackCommand(TrackAction::kContinue)[6] == 0x40);
    assert(TrackCommand(TrackAction::kAbort)[6] == 0x41);

    const std::vector<uint8_t> attributes{3,    0, 0, 0, 1, 0, 106, 0, 6, 0xe9, 0x9f, 0xb3, 0xe4, 0xb9,
                                          0x90, 0, 0, 0, 2, 0, 106, 0, 6, 'A',  'r',  't',  'i',  's',
                                          't',  0, 0, 0, 3, 0, 106, 0, 5, 'A',  'l',  'b',  'u',  'm'};
    const auto frame = [](uint8_t label, uint8_t fragment, std::span<const uint8_t> data) {
        std::vector<uint8_t> result{static_cast<uint8_t>(label << 4 | 2),
                                    0x11,
                                    0x0e,
                                    0x0c,
                                    0x48,
                                    0,
                                    0,
                                    0x19,
                                    0x58,
                                    0x20,
                                    fragment,
                                    static_cast<uint8_t>(data.size() >> 8),
                                    static_cast<uint8_t>(data.size())};
        result.insert(result.end(), data.begin(), data.end());
        return result;
    };
    TrackResponse response;
    micropixel_bluetooth_remote_track_t track{};
    response.Expect(TrackAction::kRead, 3);
    assert(response.Accept(frame(4, 0, attributes), track) == TrackResult::kIgnored);
    assert(response.Accept(frame(3, 1, std::span{attributes}.first(11)), track) == TrackResult::kIncomplete);
    assert(response.next() == TrackAction::kContinue && track.title[0] == '\0');
    response.Expect(TrackAction::kContinue, 4);
    assert(response.Accept(frame(4, 3, std::span{attributes}.subspan(11)), track) == TrackResult::kComplete);
    assert(std::strcmp(track.title, "音乐") == 0 && std::strcmp(track.artist, "Artist") == 0 &&
           std::strcmp(track.album, "Album") == 0);

    response.Expect(TrackAction::kRead, 5);
    assert(response.Accept(frame(5, 0, std::span{attributes}.first(5)), track) == TrackResult::kUnavailable);
    response.Expect(TrackAction::kRead, 6);
    assert(response.Accept(frame(6, 1, std::span{attributes}.first(11)), track) == TrackResult::kIncomplete);
    response.Expect(TrackAction::kContinue, 7);
    response.Expire();
    assert(response.next() == TrackAction::kAbort);

    response.Expect(TrackAction::kRead, 8);
    response.Reset();
    assert(response.Accept(frame(8, 0, attributes), track) == TrackResult::kIgnored);

    std::vector<uint8_t> long_title{1, 0, 0, 0, 1, 0, 106, 0, 129};
    long_title.insert(long_title.end(), 126, 'A');
    long_title.insert(long_title.end(), {0xe9, 0x9f, 0xb3});
    response.Expect(TrackAction::kRead, 9);
    assert(response.Accept(frame(9, 0, long_title), track) == TrackResult::kComplete);
    assert(std::strlen(track.title) == 126);
}
}  // namespace

namespace micropixel::bouffalo::classic {
micropixel_bluetooth_status_t Status() { return {.state = MPX_BT_CONNECTED, .role = MPX_BT_SINK}; }
int32_t Device(const micropixel_bluetooth_selection_t&, micropixel_bluetooth_device_t&) {
    return MICROPIXEL_STATUS_NOT_FOUND;
}
int32_t Bond(uint32_t, micropixel_bluetooth_address_t&) { return MICROPIXEL_STATUS_NOT_FOUND; }
int32_t Command(uint32_t, uint32_t, const micropixel_bluetooth_address_t*) {
    ++settings_calls;
    return MICROPIXEL_STATUS_OK;
}
int32_t ReadPcm(uint32_t, uint8_t*, uint32_t, uint32_t&) { return MICROPIXEL_STATUS_CLOSED; }
int32_t WritePcm(uint32_t, const uint8_t*, uint32_t) { return MICROPIXEL_STATUS_CLOSED; }
micropixel_bluetooth_remote_status_t RemoteStatus() {
    return {.session = kSession, .position_ms = 500, .duration_ms = 1200, .link = MPX_BT_REMOTE_READY};
}
micropixel_bluetooth_remote_track_t RemoteTrack() { return {.session = kSession, .title = "Track"}; }
int32_t RemoteKey(const micropixel_bluetooth_remote_key_t& request) {
    ++remote_calls;
    assert(request.key == MPX_BT_REMOTE_NEXT);
    return request.session == kSession ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_STALE_STATE;
}
}  // namespace micropixel::bouffalo::classic

int main() {
    TrackResponses();
    micropixel::bouffalo::BluetoothEndpoint endpoint;
    micropixel::runtime::ServiceRegistry services(endpoint);
    micropixel_service_info_t service{};
    assert(services.Open(MICROPIXEL_SERVICE_BLUETOOTH, MICROPIXEL_BLUETOOTH_MIN_VERSION, service, sizeof(service)) ==
           MICROPIXEL_STATUS_OK);
    assert(service.interface_major == 1 && service.interface_minor == 3);
    uint32_t size{};
    const auto call = [&](uint32_t method, const void* input, uint32_t length, void* output, uint32_t capacity) {
        return services.Call(service.service_handle, method, static_cast<const uint8_t*>(input), length,
                             static_cast<uint8_t*>(output), capacity, size);
    };
    micropixel_bluetooth_status_t audio{};
    micropixel_bluetooth_remote_status_t remote{};
    micropixel_bluetooth_remote_track_t track{};
    micropixel_bluetooth_remote_key_t key{kSession, MPX_BT_REMOTE_NEXT};
    assert(call(MICROPIXEL_BT_STATUS, nullptr, 0, &audio, sizeof(audio)) == MICROPIXEL_STATUS_PERMISSION_DENIED);
    assert(call(MICROPIXEL_BT_REMOTE_KEY, &key, sizeof(key), nullptr, 0) == MICROPIXEL_STATUS_PERMISSION_DENIED);
    assert(call(MICROPIXEL_BT_REMOTE_TRACK, nullptr, 0, &track, sizeof(track)) == MICROPIXEL_STATUS_PERMISSION_DENIED);
    endpoint.Authorize(false, true);
    assert(call(MICROPIXEL_BT_STATUS, nullptr, 0, &audio, sizeof(audio)) == MICROPIXEL_STATUS_OK);
    assert(audio.role == MPX_BT_SINK);
    assert(call(MICROPIXEL_BT_REMOTE_STATUS, nullptr, 0, &remote, sizeof(remote) - 1) ==
           MICROPIXEL_STATUS_BUFFER_TOO_SMALL);
    assert(size == sizeof(remote));
    assert(call(MICROPIXEL_BT_REMOTE_STATUS, nullptr, 0, &remote, sizeof(remote)) == MICROPIXEL_STATUS_OK);
    assert(remote.session == kSession && remote.link == MPX_BT_REMOTE_READY);
    assert(call(MICROPIXEL_BT_REMOTE_TRACK, nullptr, 0, &track, sizeof(track)) == MICROPIXEL_STATUS_OK);
    assert(track.session == kSession && std::strcmp(track.title, "Track") == 0);
    assert(call(MICROPIXEL_BT_REMOTE_KEY, &key, sizeof(key) - 1, nullptr, 0) == MICROPIXEL_STATUS_INVALID_ARGUMENT);
    assert(remote_calls == 0);
    assert(call(MICROPIXEL_BT_REMOTE_KEY, &key, sizeof(key), nullptr, 0) == MICROPIXEL_STATUS_OK);
    --key.session;
    assert(call(MICROPIXEL_BT_REMOTE_KEY, &key, sizeof(key), nullptr, 0) == MICROPIXEL_STATUS_STALE_STATE);
    assert(remote_calls == 2);
    assert(call(MICROPIXEL_BT_DISCONNECT, nullptr, 0, nullptr, 0) == MICROPIXEL_STATUS_PERMISSION_DENIED);
    assert(settings_calls == 0);
    endpoint.Authorize(true, false);
    assert(call(MICROPIXEL_BT_DISCONNECT, nullptr, 0, nullptr, 0) == MICROPIXEL_STATUS_OK);
    assert(settings_calls == 1);
}
