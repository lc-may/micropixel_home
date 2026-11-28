// SPDX-License-Identifier: Apache-2.0
#include "classic_bluetooth.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>

#include "avrcp_track.hpp"
#include "bluetooth_scan.h"
#include "platform/audio/audio_engine.hpp"
#include "work/task_policy.hpp"
#include "work/task_stack.h"

extern "C" {
#include "FreeRTOS.h"
#include "bluetooth.h"
#include "conn.h"
#include "hci_err.h"
#include "l2cap.h"
#include "mm.h"
#include "oi_codec_sbc.h"
#include "queue.h"
#include "zephyr.h"

// AVDTP declarations require the SDK's L2CAP and work-queue types.
#include "a2dp.h"
#if defined(CONFIG_BT_AVRCP)
#include "avrcp.h"

// This entry point is exported by the pinned AVCTP library. Using its
// transaction counter keeps metadata requests on the same control channel.
net_buf* bt_avctp_create_pdu(uint8_t* label, uint8_t fragment, uint8_t direction, uint16_t pid);
#endif

int mpx_classic_sdk_role(uint8_t role);
int mpx_classic_sdk_forget(const bt_addr_t* address);
void mpx_classic_sdk_source_format(uint32_t* rate, uint8_t* channels, uint16_t* frame_bytes);
}

namespace micropixel::bouffalo::classic {
namespace {
constexpr unsigned kDeviceCount = 12;
constexpr unsigned kBondCount = 8;  // BR/EDR link-key slots in the pinned host stack.
constexpr unsigned kPcmBytes = MICROPIXEL_BLUETOOTH_PCM_BYTES;
constexpr unsigned kRingSize = 8192;
constexpr unsigned kPacketDepth = 10;
constexpr int32_t kOk = MICROPIXEL_STATUS_OK;

struct Lock {
    Lock() { taskENTER_CRITICAL(); }
    ~Lock() { taskEXIT_CRITICAL(); }
};

struct Packet {
    uint32_t revision;
    unsigned size;
    uint8_t data[kPcmBytes];
};

struct Request {
    uint32_t method;
    uint32_t value;
    bt_addr_t address;
};

struct State {
    platform::audio::AudioEngine* audio;
    uint32_t mix_rate;
    QueueHandle_t packets{};
    micropixel_bluetooth_status_t status{.revision = 1, .role = MPX_BT_SINK};
    Request command{};
    bt_addr_t outgoing{};
    bt_conn* peer{};
    bt_conn* rejected{};
    bool connectable{};
    TickType_t profile_started{};
#if defined(CONFIG_BT_AVRCP)
    micropixel_bluetooth_remote_status_t remote{.position_ms = MPX_BT_REMOTE_UNKNOWN_TIME,
                                                .duration_ms = MPX_BT_REMOTE_UNKNOWN_TIME};
    uint32_t remote_key{};
    TickType_t remote_action_at{};
    TickType_t remote_reply_at{};
    uint8_t remote_attempts{};
    bt_l2cap_chan* remote_channel{};
    micropixel_bluetooth_remote_track_t remote_track{};
    avrcp::TrackResponse track_response{};
    TickType_t track_query_at{}, track_sent_at{}, track_reply_at{};
#endif
    bool sending{};
    bool source_clock{};
    uint16_t frame_bytes{};
    uint32_t phase{};
    int32_t sum{};
    unsigned samples{};
    uint32_t head{}, tail{};
    int16_t pcm[kRingSize];
    bt_br_discovery_result results[kDeviceCount];
    micropixel_bluetooth_device_t devices[kDeviceCount];
    micropixel_bluetooth_address_t bonds[kBondCount];
    Packet input{}, encoded{};
    OI_CODEC_SBC_DECODER_CONTEXT decoder{};
    OI_CODEC_SBC_CODEC_DATA_STEREO decoder_storage{};
    uint32_t decoder_revision{};
    OI_INT16 decoded[256];
    int16_t send[2][kPcmBytes / sizeof(int16_t)];
    unsigned send_slot{};
    device::PcmStreamHandle voice{};
    uint32_t voice_revision{};
    uint16_t sink_gain{1000};
    uint16_t voice_gain{1000};
};

State* state;
char local_name[] = CONFIG_BT_DEVICE_NAME;
uint8_t eir[240];

unsigned Available() { return state->head - state->tail; }
unsigned Free() { return kRingSize - Available(); }
bool Playing(uint8_t role, uint8_t route) {
    const auto& status = state->status;
    return status.state == MPX_BT_CONNECTED && status.role == role && status.stream == MPX_BT_STREAM_PLAYING &&
           status.audio_route == route;
}
void Push(int16_t sample) { state->pcm[state->head++ % kRingSize] = sample; }
int16_t Pop() { return state->pcm[state->tail++ % kRingSize]; }

void Flush() {
    auto& status = state->status;
    ++status.stream_revision;
    ++status.revision;
    state->head = state->tail = state->phase = 0;
    state->sum = 0;
    state->samples = 0;
}

void SetStream(uint8_t stream) {
    auto& status = state->status;
    if (stream == MPX_BT_STREAM_IDLE) {
        status.sample_rate = status.channels = 0;
        state->frame_bytes = 0;
    }
    if (status.stream == stream) return;
    status.stream = stream;
    state->source_clock = false;
    if (stream == MPX_BT_STREAM_PLAYING) {
        status.rx_sbc_frames = status.tx_pcm_frames = status.dropped_frames = status.decode_errors = 0;
        status.rx_dropped_packets = 0;
    }
    Flush();
}

#if defined(CONFIG_BT_AVRCP)
constexpr auto kTrackInterval = pdMS_TO_TICKS(2000);
constexpr auto kTrackTimeout = pdMS_TO_TICKS(1500);

void ResetRemote(uint8_t link) {
    state->remote = {.session = state->remote.session + 1,
                     .position_ms = MPX_BT_REMOTE_UNKNOWN_TIME,
                     .duration_ms = MPX_BT_REMOTE_UNKNOWN_TIME,
                     .link = link};
    state->remote_key = 0;
    state->remote_action_at = xTaskGetTickCount();
    state->remote_reply_at = state->remote_action_at;
    state->remote_channel = nullptr;
    state->remote_track = {};
    state->track_response.Reset();
    state->track_query_at = state->remote_action_at - kTrackInterval;
}

void RemoteLink(bt_conn* conn, uint8_t event) {
    const char* message = nullptr;
    {
        Lock lock;
        if ((conn && state->peer != conn) || state->status.state != MPX_BT_CONNECTED ||
            state->status.role != MPX_BT_SINK)
            return;
        if (conn && event == BT_AVRCP_CHAIN_CONNECTED && state->remote.link != MPX_BT_REMOTE_READY) {
            ResetRemote(MPX_BT_REMOTE_READY);
            state->remote_attempts = 0;
            message = "MicroPixel AVRCP connected";
        } else if (event == BT_AVRCP_CHAIN_DISCONNECTED && state->remote.link == MPX_BT_REMOTE_READY) {
            ResetRemote(MPX_BT_REMOTE_IDLE);
            message = "MicroPixel AVRCP disconnected";
        }
    }
    if (message) std::puts(message);
}

void RemotePlayback(uint32_t duration, uint32_t position, uint8_t playback) {
    uint8_t decoded = MPX_BT_REMOTE_UNKNOWN;
    switch (playback) {
        case PLAY_STATUS_STOPPED:
            decoded = MPX_BT_REMOTE_STOPPED;
            break;
        case PLAY_STATUS_PLAYING:
            decoded = MPX_BT_REMOTE_PLAYING;
            break;
        case PLAY_STATUS_PAUSED:
            decoded = MPX_BT_REMOTE_PAUSED;
            break;
    }
    Lock lock;
    if (state->remote.link != MPX_BT_REMOTE_READY) return;
    state->remote.playback = decoded;
    state->remote.duration_ms = decoded && duration ? duration : MPX_BT_REMOTE_UNKNOWN_TIME;
    state->remote.position_ms = decoded ? position : MPX_BT_REMOTE_UNKNOWN_TIME;
    state->remote_reply_at = xTaskGetTickCount();
}

void RemoteKeyAccepted(uint8_t released, uint8_t operation) {
    if (released == PASTHR_STATE_RELEASED)
        std::printf("MicroPixel AVRCP key=0x%02x accepted\n", static_cast<unsigned>(operation));
}

void RemoteVolume(uint8_t volume) {
    Lock lock;
    if (state->status.role != MPX_BT_SINK || state->remote.link != MPX_BT_REMOTE_READY) return;
    state->sink_gain = static_cast<uint16_t>(((volume & ABS_VOL_MASK) * 1000U + 63U) / 127U);
}

avrcp_callback remote_callbacks{
    .chain = RemoteLink, .abs_vol = RemoteVolume, .play_status = RemotePlayback, .rp_passthrough = RemoteKeyAccepted};

void ObserveRemoteChannel(bt_l2cap_chan* channel) {
    if (!state || !channel || !channel->conn) return;
    RemoteLink(channel->conn, BT_AVRCP_CHAIN_CONNECTED);
    Lock lock;
    if (state->remote.link == MPX_BT_REMOTE_READY && state->peer == channel->conn) state->remote_channel = channel;
}

void ReceiveTrack(bt_l2cap_chan* channel, std::span<const uint8_t> frame) {
    unsigned fields = 0;
    {
        Lock lock;
        if (state->remote.link != MPX_BT_REMOTE_READY || state->remote_channel != channel) return;
        micropixel_bluetooth_remote_track_t track{.session = state->remote.session};
        const auto result = state->track_response.Accept(frame, track);
        if (result == avrcp::TrackResult::kComplete) {
            if (std::memcmp(&track, &state->remote_track, sizeof(track)) != 0)
                fields = (track.title[0] != '\0') + (track.artist[0] != '\0') + (track.album[0] != '\0');
            state->remote_track = track;
            state->track_reply_at = xTaskGetTickCount();
        }
    }
    if (fields) std::printf("MicroPixel AVRCP track metadata updated fields=%u\n", fields);
}

void SendTrack(bt_l2cap_chan* channel, uint32_t session, avrcp::TrackAction action) {
    const auto command = avrcp::TrackCommand(action);
    auto* packet = bt_avctp_create_pdu(nullptr, AVCTP_PKT_TYPE_SIGNLE, AVCTP_CR_CMD, AVCTP_PID);
    bool current;
    {
        Lock lock;
        current = state->remote.session == session && state->remote_channel == channel && channel->conn == state->peer;
        if (current && packet) {
            state->track_response.Expect(action, packet->data[0] >> 4);
            state->track_sent_at = xTaskGetTickCount();
        } else if (current) {
            state->track_response.Reset();
        }
    }
    if (!packet) return;
    if (current && net_buf_tailroom(packet) >= command.size()) {
        net_buf_add_mem(packet, command.data(), command.size());
        if (bt_l2cap_chan_send(channel, packet) >= 0) return;
    }
    net_buf_unref(packet);
}

uint8_t RemoteOperation(uint32_t key) {
    switch (key) {
        case MPX_BT_REMOTE_PLAY:
            return AVRCP_KEY_PLAY;
        case MPX_BT_REMOTE_PAUSE:
            return AVRCP_KEY_PAUSE;
        case MPX_BT_REMOTE_PREVIOUS:
            return AVRCP_KEY_BACKWARD;
        case MPX_BT_REMOTE_NEXT:
            return AVRCP_KEY_FORWARD;
        default:
            return 0;
    }
}

void PollRemote() {
    bt_conn* peer = nullptr;
    bt_l2cap_chan* track_channel = nullptr;
    auto track_action = avrcp::TrackAction::kNone;
    uint32_t session = 0;
    uint8_t operation = 0;
    bool query = false;
    {
        Lock lock;
        const auto now = xTaskGetTickCount();
        auto& status = state->status;
        auto& remote = state->remote;
        if (!state->peer || status.state != MPX_BT_CONNECTED || status.role != MPX_BT_SINK) return;
        if (state->remote_channel && state->remote_channel->conn != state->peer) ResetRemote(MPX_BT_REMOTE_IDLE);
        if (remote.link == MPX_BT_REMOTE_READY) {
            session = remote.session;
            operation = RemoteOperation(state->remote_key);
            state->remote_key = 0;
            if (now - state->remote_reply_at >= pdMS_TO_TICKS(8000)) {
                remote.playback = MPX_BT_REMOTE_UNKNOWN;
                remote.position_ms = remote.duration_ms = MPX_BT_REMOTE_UNKNOWN_TIME;
            }
            if (state->remote_track.session && now - state->track_reply_at >= pdMS_TO_TICKS(8000))
                state->remote_track = {};
            if (!operation && state->remote_channel) {
                auto& response = state->track_response;
                if (response.waiting() && now - state->track_sent_at >= kTrackTimeout) response.Expire();
                track_action = response.next();
                if (!response.waiting() && track_action == avrcp::TrackAction::kNone &&
                    now - state->track_query_at >= kTrackInterval) {
                    state->track_query_at = now;
                    track_action = avrcp::TrackAction::kRead;
                }
                if (track_action != avrcp::TrackAction::kNone) track_channel = state->remote_channel;
            }
            if (!operation && !track_channel && now - state->remote_action_at >= pdMS_TO_TICKS(2000)) {
                state->remote_action_at = now;
                query = true;
            }
        } else if (status.stream == MPX_BT_STREAM_OPEN || status.stream == MPX_BT_STREAM_PLAYING ||
                   status.stream == MPX_BT_STREAM_SUSPENDED) {
            if (remote.link == MPX_BT_REMOTE_IDLE) {
                ResetRemote(MPX_BT_REMOTE_CONNECTING);
                return;
            }
            // Let the audio source open AVCTP first. Retry once if it does not.
            const auto wait = pdMS_TO_TICKS(state->remote_attempts ? 5000 : 750);
            if (remote.link == MPX_BT_REMOTE_CONNECTING && now - state->remote_action_at >= wait) {
                state->remote_action_at = now;
                if (state->remote_attempts < 2) {
                    ++state->remote_attempts;
                    peer = bt_conn_ref(state->peer);
                } else {
                    ResetRemote(MPX_BT_REMOTE_UNAVAILABLE);
                }
            }
        }
    }
    // The SDK's AVRCP controller uses a single AVCTP control channel for NULL.
    if (peer) {
        (void)bt_avrcp_connect(peer);
        bt_conn_unref(peer);
    }
    if (track_channel) SendTrack(track_channel, session, track_action);
    if (query) (void)avrcp_get_play_status_cmd(nullptr);
    if (!operation) return;
    int error = avrcp_pasthr_cmd(nullptr, PASTHR_STATE_PRESSED, operation);
    if (!error) error = avrcp_pasthr_cmd(nullptr, PASTHR_STATE_RELEASED, operation);
    std::printf("MicroPixel AVRCP key=0x%02x result=%d\n", static_cast<unsigned>(operation), error);
    Lock lock;
    if (state->remote.session == session) {
        state->remote.error = error;
        state->remote.pending = 0;
    }
}
#endif

void SetIdle() {
    state->status.state = MPX_BT_IDLE;
    state->status.peer_address[0] = '\0';
    state->status.security = 0;
    state->sink_gain = 1000;
#if defined(CONFIG_BT_AVRCP)
    ResetRemote(MPX_BT_REMOTE_IDLE);
    state->remote_attempts = 0;
#endif
    SetStream(MPX_BT_STREAM_IDLE);
    ++state->status.revision;
}

void Connected(bt_conn* conn, uint8_t error) {
    bt_conn_info info{};
    if (bt_conn_get_info(conn, &info) || info.type != BT_CONN_TYPE_BR) return;
    bt_conn* release = nullptr;
    {
        Lock lock;
        auto& status = state->status;
        const bool outgoing = status.state == MPX_BT_CONNECTING && bt_addr_cmp(&state->outgoing, info.br.dst) == 0;
        if (error) {
            if (outgoing || state->peer == conn) {
                release = state->peer;
                state->peer = nullptr;
                SetIdle();
                status.error = error;
            }
        } else if (outgoing || state->peer == conn ||
                   (!state->peer && state->connectable && status.state == MPX_BT_IDLE)) {
            if (!state->peer) state->peer = bt_conn_ref(conn);
            status.state = MPX_BT_CONNECTED;
            status.security = bt_conn_get_security(conn);
            bt_addr_to_str(info.br.dst, status.peer_address, sizeof(status.peer_address));
            ++status.revision;
        } else if (!state->rejected) {
            state->rejected = bt_conn_ref(conn);
        }
    }
    if (release) bt_conn_unref(release);
}

void Disconnected(bt_conn* conn, uint8_t reason) {
    {
        Lock lock;
        if (state->peer != conn) return;
        state->peer = nullptr;
        SetIdle();
        if (reason != BT_HCI_ERR_REMOTE_USER_TERM_CONN && reason != BT_HCI_ERR_LOCALHOST_TERM_CONN)
            state->status.error = reason;
    }
    bt_conn_unref(conn);
}

void SecurityChanged(bt_conn* conn, bt_security_t level, bt_security_err error) {
    Lock lock;
    if (state->peer != conn) return;
    state->status.security = level;
    if (error) state->status.error = -EACCES;
    ++state->status.revision;
}

bt_conn_cb connection_callbacks{
    .connected = Connected, .disconnected = Disconnected, .security_changed = SecurityChanged};

void ProfileLink(bt_conn* conn, uint8_t event) {
    Lock lock;
    if (state->peer != conn || state->status.state != MPX_BT_CONNECTED) return;
    if (event == BT_A2DP_CHAIN_DISCONNECTED) SetStream(MPX_BT_STREAM_IDLE);
    if (event == BT_A2DP_CHAIN_CONNECTED &&
        (state->status.stream == MPX_BT_STREAM_IDLE || state->status.stream == MPX_BT_STREAM_CONNECTING))
        SetStream(MPX_BT_STREAM_OPEN);
}

void StartSource() {
    uint32_t rate{};
    uint8_t channels{};
    uint16_t frame_bytes{};
    mpx_classic_sdk_source_format(&rate, &channels, &frame_bytes);
    Lock lock;
    auto& status = state->status;
    if (status.state != MPX_BT_CONNECTED || status.role != MPX_BT_SOURCE) return;
    if (!rate || !frame_bytes || frame_bytes > kPcmBytes) {
        status.error = -EPROTO;
        ++status.revision;
        return;
    }
    status.sample_rate = rate;
    status.channels = channels;
    state->frame_bytes = frame_bytes;
    SetStream(MPX_BT_STREAM_PLAYING);
}

void Suspend() {
    Lock lock;
    if (state->status.stream == MPX_BT_STREAM_PLAYING) SetStream(MPX_BT_STREAM_SUSPENDED);
}

void ProfileStream(uint8_t event) {
    if (event == BT_A2DP_STREAM_START) {
        bool source;
        {
            Lock lock;
            source = state->status.role == MPX_BT_SOURCE;
        }
        if (source) StartSource();
    }
    if (event == BT_A2DP_STREAM_SUSPEND) Suspend();
}

a2dp_callback profile_callbacks{
    .chain = ProfileLink, .stream = ProfileStream, .start_cfm = StartSource, .suspend_cfm = Suspend};

void ScanDone(bt_br_discovery_result* results, size_t count) {
    Lock lock;
    auto& status = state->status;
    if (status.state != MPX_BT_SCANNING) return;
    count = std::min<size_t>(count, kDeviceCount);
    for (unsigned i = 0; i < count; ++i) {
        auto& device = state->devices[i];
        device = {};
        bt_addr_to_str(&results[i].addr, device.address, sizeof(device.address));
        device.rssi = results[i].rssi;
        std::memcpy(device.class_of_device, results[i].cod, sizeof(device.class_of_device));
        mpx_bt_scan_data_t scan{};
        mpx_bt_scan_parse(&scan, results[i].eir, sizeof(results[i].eir));
        std::memcpy(device.name, scan.name, sizeof(device.name));
        device.group = mpx_bt_scan_br_group(&scan, device.class_of_device);
    }
    status.device_count = count;
    status.state = MPX_BT_IDLE;
    ++status.revision;
}

void AddBond(const bt_br_bond_info* info, void* context) {
    auto& count = *static_cast<uint8_t*>(context);
    if (count < kBondCount) {
        auto& address = state->bonds[count++];
        bt_addr_to_str(info->addr, address.address, sizeof(address.address));
    }
}

void RefreshBonds() {
    Lock lock;
    uint8_t count = 0;
    bt_br_foreach_bond(AddBond, &count);
    if (count != state->status.bond_count) {
        state->status.bond_count = count;
        ++state->status.revision;
    }
}

int SetVisible(bool enabled, uint8_t role) {
    if (enabled) {
        if (const int error = bt_br_write_local_name(local_name)) return error;
        eir[0] = sizeof(local_name);
        eir[1] = BT_DATA_NAME_COMPLETE;
        std::memcpy(eir + 2, local_name, sizeof(local_name) - 1);
        const auto next = sizeof(local_name) + 1;
        eir[next] = 3;
        eir[next + 1] = 0x03;  // Complete list of 16-bit service UUIDs.
        eir[next + 2] = role == MPX_BT_SINK ? 0x0b : 0x0a;
        eir[next + 3] = 0x11;
        if (const int error = bt_br_write_eir(0, eir)) return error;
        {
            Lock lock;
            state->connectable = true;
        }
        int error = bt_br_set_connectable(true);
        if (!error || error == -EALREADY) error = bt_br_set_discoverable(true);
        if (error && error != -EALREADY) {
            (void)bt_br_set_connectable(false);
            Lock lock;
            state->connectable = false;
            return error;
        }
    } else {
        int error = bt_br_set_discoverable(false);
        if (error && error != -EALREADY) return error;
        error = bt_br_set_connectable(false);
        if (error && error != -EALREADY) {
            (void)bt_br_set_discoverable(true);
            return error;
        }
    }
    Lock lock;
    state->connectable = enabled;
    state->status.visible = enabled;
    return 0;
}

int Connect(const bt_addr_t& address) {
    {
        Lock lock;
        if (state->status.state != MPX_BT_IDLE || state->peer) return -EBUSY;
        state->outgoing = address;
        state->status.state = MPX_BT_CONNECTING;
        bt_addr_to_str(&address, state->status.peer_address, sizeof(state->status.peer_address));
        ++state->status.revision;
    }
    const bt_br_conn_param parameters{.allow_role_switch = true};
    bt_conn* conn = bt_conn_create_br(&address, &parameters);
    {
        Lock lock;
        if (state->status.state == MPX_BT_CONNECTING) {
            if (conn && !state->peer) {
                state->peer = conn;
                return 0;
            }
            if (!conn) SetIdle();
        }
    }
    if (!conn) return -ECONNREFUSED;
    bt_conn_unref(conn);
    return 0;
}

int StreamCommand(uint32_t method, bt_conn* peer, const micropixel_bluetooth_status_t& status) {
    switch (method) {
        case MICROPIXEL_BT_DISCONNECT:
            return bt_conn_disconnect(peer, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        case MICROPIXEL_BT_PAIR:
            return bt_conn_set_security(peer, BT_SECURITY_L2);
        case MICROPIXEL_BT_OPEN_STREAM: {
            {
                Lock lock;
                if (state->peer != peer) return -ENOTCONN;
                if (state->status.stream != MPX_BT_STREAM_IDLE) return -EALREADY;
                state->profile_started = xTaskGetTickCount();
                SetStream(MPX_BT_STREAM_CONNECTING);
            }
            if (bt_a2dp_connect(peer)) return 0;
            Lock lock;
            if (state->status.stream == MPX_BT_STREAM_CONNECTING) SetStream(MPX_BT_STREAM_IDLE);
            return -ECONNREFUSED;
        }
        case MICROPIXEL_BT_START_STREAM:
            if (status.stream != MPX_BT_STREAM_OPEN && status.stream != MPX_BT_STREAM_SUSPENDED) return -EINVAL;
            return status.role == MPX_BT_SOURCE ? bt_stream_resume(peer) : -ENOTSUP;
        case MICROPIXEL_BT_SUSPEND_STREAM:
            if (status.stream != MPX_BT_STREAM_PLAYING) return -EINVAL;
            return status.role == MPX_BT_SOURCE ? bt_stream_suspend(peer) : -ENOTSUP;
        default:
            return -ENOTSUP;
    }
}

int Execute(const Request& command) {
    const auto status = Status();
    switch (command.method) {
        case MICROPIXEL_BT_SCAN: {
            {
                Lock lock;
                if (state->status.state != MPX_BT_IDLE || state->connectable) return -EBUSY;
                state->status.state = MPX_BT_SCANNING;
                state->status.device_count = 0;
                ++state->status.scan_revision;
            }
            const bt_br_discovery_param params{.length = 6, .limited = false};
            const int error = bt_br_discovery_start(&params, state->results, kDeviceCount, ScanDone);
            if (error) ScanDone(nullptr, 0);
            return error;
        }
        case MICROPIXEL_BT_STOP_SCAN: {
            if (status.state != MPX_BT_SCANNING) return -EALREADY;
            const int error = bt_br_discovery_stop();
            if (!error) ScanDone(nullptr, 0);
            return error;
        }
        case MICROPIXEL_BT_SET_VISIBLE:
            if (status.state == MPX_BT_SCANNING || status.state == MPX_BT_CONNECTING) return -EBUSY;
            return SetVisible(command.value != 0, status.role);
        case MICROPIXEL_BT_SET_ROLE: {
            if (status.role == command.value) return 0;
            {
                Lock lock;
                if (state->status.state != MPX_BT_IDLE || state->connectable || state->sending) return -EBUSY;
            }
            const int error = mpx_classic_sdk_role(command.value);
            a2dp_cb_register(&profile_callbacks);
            if (error) return error;
            Lock lock;
            state->status.role = command.value;
            Flush();
            return 0;
        }
        case MICROPIXEL_BT_SET_AUDIO_ROUTE: {
            Lock lock;
            if (state->status.audio_route != command.value) {
                state->status.audio_route = command.value;
                state->source_clock = false;
                Flush();
            }
            return 0;
        }
        case MICROPIXEL_BT_CONNECT:
            return Connect(command.address);
        case MICROPIXEL_BT_FORGET:
            return mpx_classic_sdk_forget(&command.address);
        default:
            break;
    }
    bt_conn* peer = nullptr;
    {
        Lock lock;
        if (state->peer && (state->status.state == MPX_BT_CONNECTED ||
                            (command.method == MICROPIXEL_BT_DISCONNECT && state->status.state == MPX_BT_CONNECTING)))
            peer = bt_conn_ref(state->peer);
    }
    if (!peer) return -ENOTCONN;
    const int result = StreamCommand(command.method, peer, status);
    bt_conn_unref(peer);
    return result;
}

void PutDecoded(const micropixel_bluetooth_status_t& status, unsigned frames) {
    Lock lock;
    if (state->status.stream_revision != status.stream_revision) return;
    ++state->status.rx_sbc_frames;
    for (unsigned i = 0; i < frames; ++i) {
        const int16_t left = state->decoded[i * status.channels];
        const int16_t right = status.channels == 2 ? state->decoded[i * 2 + 1] : left;
        if (status.audio_route == MPX_BT_AUDIO_GUEST) {
            if (Free() >= status.channels) {
                Push(left);
                if (status.channels == 2) Push(right);
            } else {
                ++state->status.dropped_frames;
            }
            continue;
        }
        state->sum += (int32_t(left) + right) / 2;
        ++state->samples;
        state->phase += state->mix_rate;
        while (state->phase >= status.sample_rate) {
            state->phase -= status.sample_rate;
            if (Free())
                Push(state->sum / int32_t(state->samples));
            else
                ++state->status.dropped_frames;
        }
        if (state->phase < state->mix_rate) {
            state->sum = 0;
            state->samples = 0;
        }
    }
}

void Decode(const Packet& packet, const micropixel_bluetooth_status_t& status) {
    if (!status.channels || status.stream != MPX_BT_STREAM_PLAYING || status.role != MPX_BT_SINK ||
        packet.revision != status.stream_revision)
        return;
    bool error = false;
    if (state->decoder_revision != status.stream_revision) {
        error = OI_CODEC_SBC_DecoderReset(&state->decoder, state->decoder_storage.data,
                                          sizeof(state->decoder_storage.data), 2, status.channels, 0, 0) != OI_OK;
        if (!error) state->decoder_revision = status.stream_revision;
    }
    const OI_BYTE* cursor = packet.data;
    OI_UINT32 remaining = packet.size;
    while (!error && remaining) {
        const auto before = remaining;
        OI_UINT32 bytes = sizeof(state->decoded);
        error = OI_CODEC_SBC_DecodeFrame(&state->decoder, &cursor, &remaining, state->decoded, &bytes) != OI_OK ||
                remaining >= before || !bytes || bytes > sizeof(state->decoded);
        if (!error) PutDecoded(status, bytes / (sizeof(int16_t) * status.channels));
    }
    if (error) {
        Lock lock;
        if (state->status.stream_revision == status.stream_revision) ++state->status.decode_errors;
    }
}

device::PcmReadResult MixerRead(void* context, int16_t* samples, uint32_t capacity) {
    const auto& playback = *static_cast<State*>(context);
    Lock lock;
    if (!Playing(MPX_BT_SINK, MPX_BT_AUDIO_SYSTEM) || playback.voice_revision != playback.status.stream_revision)
        return {};
    const unsigned count = std::min<unsigned>(capacity, Available());
    for (unsigned i = 0; i < count; ++i) samples[i] = Pop();
    return {count, false};
}

void UpdateVoice(const micropixel_bluetooth_status_t& status) {
    const bool playing = status.role == MPX_BT_SINK && status.stream == MPX_BT_STREAM_PLAYING &&
                         status.audio_route == MPX_BT_AUDIO_SYSTEM;
    if (state->voice && (!playing || state->voice_revision != status.stream_revision)) {
        if (state->audio->StopPcm(state->voice) == MICROPIXEL_STATUS_RATE_LIMITED) return;
        state->voice = 0;
    }
    if (!playing) return;
    uint16_t gain;
    {
        Lock lock;
        gain = state->sink_gain;
        state->voice_revision = status.stream_revision;
    }
    if (!state->voice) {
        if (state->audio->StartPcm({MixerRead, state}, 0, gain, state->voice) == kOk) {
            state->voice_gain = gain;
            std::printf("MicroPixel A2DP Sink mixer opened gain=%u/1000\n", static_cast<unsigned>(gain));
        }
    } else if (gain != state->voice_gain && state->audio->SetPcmVolume(state->voice, gain) == kOk) {
        state->voice_gain = gain;
        std::printf("MicroPixel A2DP Sink gain=%u/1000\n", static_cast<unsigned>(gain));
    }
}

void SendSource(uint32_t& pace_revision, TickType_t& due, uint32_t& remainder) {
    const auto now = xTaskGetTickCount();
    uint32_t revision, rate;
    unsigned frames, bytes;
    auto* buffer = state->send[state->send_slot];
    {
        Lock lock;
        const auto& status = state->status;
        if (!Playing(MPX_BT_SOURCE, status.audio_route) || !state->frame_bytes) return;
        if (pace_revision != status.stream_revision ||
            static_cast<int32_t>(now - due) > static_cast<int32_t>(pdMS_TO_TICKS(30))) {
            pace_revision = status.stream_revision;
            due = now;
            remainder = 0;
        }
        if (static_cast<int32_t>(now - due) < 0) return;
        bytes = kPcmBytes / state->frame_bytes * state->frame_bytes;
        const unsigned count = bytes / sizeof(int16_t);
        if (Available() < count) {
            if (status.audio_route == MPX_BT_AUDIO_GUEST || !state->source_clock) {
                due = now;
                remainder = 0;
                return;
            }
        }
        const unsigned ready = std::min(count, Available());
        for (unsigned i = 0; i < ready; ++i) buffer[i] = Pop();
        std::fill(buffer + ready, buffer + count, 0);
        frames = count / status.channels;
        revision = status.stream_revision;
        rate = status.sample_rate;
        state->sending = true;
    }
    const auto result = bt_a2dp_send_media(buffer, bytes);
    if (result >= 0) state->send_slot ^= 1;
    const uint32_t ticks = frames * configTICK_RATE_HZ + remainder;
    due += ticks / rate;
    remainder = ticks % rate;
    Lock lock;
    state->sending = false;
    if (state->status.stream_revision == revision) {
        if (result >= 0) {
            state->status.tx_pcm_frames += frames;
        } else {
            state->status.dropped_frames += frames;
            state->status.error = result;
        }
    }
}

void Worker(void*) {
    uint32_t pace_revision = 0, remainder = 0;
    TickType_t due = 0;
    for (;;) {
        const auto status = Status();
        UpdateVoice(status);
        const TickType_t wait = status.stream == MPX_BT_STREAM_PLAYING ? 1 : pdMS_TO_TICKS(10);
        if (xQueueReceive(state->packets, &state->input, wait) == pdPASS) Decode(state->input, Status());
        SendSource(pace_revision, due, remainder);
    }
}
}  // namespace

bool Initialize(platform::audio::AudioEngine& audio, uint32_t sample_rate) {
    auto* memory = kmalloc(sizeof(State), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE);
    if (!memory) return false;
    state = new (memory) State{.audio = &audio, .mix_rate = sample_rate};
    state->packets = xQueueCreate(kPacketDepth, sizeof(Packet));
    if (state->packets &&
        mpx_create_task(Worker, "mpx_bt_audio", 6144, nullptr, task_policy::kAudioDecodePriority, nullptr) == pdPASS)
        return true;
    if (state->packets) vQueueDelete(state->packets);
    state->~State();
    kfree(state);
    state = nullptr;
    return false;
}

bool RouteOutput(const int32_t* stereo_pcm, uint32_t frames) {
    Lock lock;
    if (!state || !Playing(MPX_BT_SOURCE, MPX_BT_AUDIO_SYSTEM)) return false;
    auto& status = state->status;
    state->source_clock = true;
    for (unsigned i = 0; i < frames; ++i) {
        const int32_t left = stereo_pcm[2 * i] >> 16;
        const int32_t right = stereo_pcm[2 * i + 1] >> 16;
        state->phase += status.sample_rate;
        while (state->phase >= state->mix_rate) {
            state->phase -= state->mix_rate;
            if (Free() >= status.channels) {
                Push(status.channels == 2 ? left : (left + right) / 2);
                if (status.channels == 2) Push(right);
            } else {
                ++status.dropped_frames;
            }
        }
    }
    return true;
}

micropixel_bluetooth_status_t Status() {
    Lock lock;
    return state ? state->status : micropixel_bluetooth_status_t{};
}

#if defined(CONFIG_BT_AVRCP)
micropixel_bluetooth_remote_status_t RemoteStatus() {
    Lock lock;
    return state ? state->remote
                 : micropixel_bluetooth_remote_status_t{.position_ms = MPX_BT_REMOTE_UNKNOWN_TIME,
                                                        .duration_ms = MPX_BT_REMOTE_UNKNOWN_TIME};
}

micropixel_bluetooth_remote_track_t RemoteTrack() {
    Lock lock;
    return state ? state->remote_track : micropixel_bluetooth_remote_track_t{};
}

int32_t RemoteKey(const micropixel_bluetooth_remote_key_t& request) {
    if (!RemoteOperation(request.key)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    Lock lock;
    if (!state || state->remote.link != MPX_BT_REMOTE_READY) return MICROPIXEL_STATUS_CLOSED;
    if (request.session != state->remote.session) return MICROPIXEL_STATUS_STALE_STATE;
    if (state->remote.pending) return MICROPIXEL_STATUS_RATE_LIMITED;
    state->remote.pending = 1;
    state->remote.error = 0;
    state->remote_key = request.key;
    return kOk;
}
#endif

int32_t Device(const micropixel_bluetooth_selection_t& selection, micropixel_bluetooth_device_t& device) {
    Lock lock;
    if (!state) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (selection.scan_revision != state->status.scan_revision) return MICROPIXEL_STATUS_STALE_STATE;
    if (selection.index >= state->status.device_count) return MICROPIXEL_STATUS_NOT_FOUND;
    device = state->devices[selection.index];
    return kOk;
}

int32_t Bond(uint32_t index, micropixel_bluetooth_address_t& address) {
    Lock lock;
    if (!state) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (index >= state->status.bond_count) return MICROPIXEL_STATUS_NOT_FOUND;
    address = state->bonds[index];
    return kOk;
}

int32_t Command(uint32_t method, uint32_t value, const micropixel_bluetooth_address_t* address) {
    if ((method == MICROPIXEL_BT_SET_ROLE && value != MPX_BT_SINK && value != MPX_BT_SOURCE) ||
        (method == MICROPIXEL_BT_SET_VISIBLE && value > 1) ||
        (method == MICROPIXEL_BT_SET_AUDIO_ROUTE && value > MPX_BT_AUDIO_GUEST))
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    Request request{.method = method, .value = value};
    constexpr bt_addr_t any{};
    if (address && (address->address[17] || std::strlen(address->address) != 17 ||
                    bt_addr_from_str(address->address, &request.address) || !bt_addr_cmp(&request.address, &any)))
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    Lock lock;
    if (!state || state->status.state == MPX_BT_UNAVAILABLE) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (state->status.pending) return MICROPIXEL_STATUS_RATE_LIMITED;
    state->command = request;
    state->status.error = 0;
    state->status.pending = 1;
    ++state->status.revision;
    return kOk;
}

int32_t ReadPcm(uint32_t revision, uint8_t* output, uint32_t capacity, uint32_t& bytes) {
    Lock lock;
    if (!state) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (revision != state->status.stream_revision) return MICROPIXEL_STATUS_STALE_STATE;
    if (!Playing(MPX_BT_SINK, MPX_BT_AUDIO_GUEST)) return MICROPIXEL_STATUS_CLOSED;
    const unsigned channels = state->status.channels;
    const unsigned frame_size = channels * sizeof(int16_t);
    if (capacity < frame_size) {
        bytes = frame_size;
        return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    }
    const unsigned count =
        std::min<uint32_t>({capacity, kPcmBytes, Available() * sizeof(int16_t)}) / frame_size * channels;
    if (!count) return MICROPIXEL_STATUS_WOULD_BLOCK;
    for (unsigned i = 0; i < count; ++i) {
        const auto sample = Pop();
        std::memcpy(output + i * sizeof(sample), &sample, sizeof(sample));
    }
    bytes = count * sizeof(int16_t);
    return kOk;
}

int32_t WritePcm(uint32_t revision, const uint8_t* input, uint32_t bytes) {
    Lock lock;
    if (!state) return MICROPIXEL_STATUS_UNSUPPORTED;
    if (revision != state->status.stream_revision) return MICROPIXEL_STATUS_STALE_STATE;
    if (!Playing(MPX_BT_SOURCE, MPX_BT_AUDIO_GUEST)) return MICROPIXEL_STATUS_CLOSED;
    if (!bytes || bytes > kPcmBytes || bytes % (state->status.channels * sizeof(int16_t)))
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (bytes / sizeof(int16_t) > Free()) return MICROPIXEL_STATUS_WOULD_BLOCK;
    for (unsigned i = 0; i < bytes; i += sizeof(int16_t)) {
        int16_t sample;
        std::memcpy(&sample, input + i, sizeof(sample));
        Push(sample);
    }
    return kOk;
}

#if defined(CONFIG_BT_AVRCP)
extern "C" int __real_avrcp_recv(bt_l2cap_chan* channel, net_buf* packet);
extern "C" int __wrap_avrcp_recv(bt_l2cap_chan* channel, net_buf* packet) {
    // The pinned SDK's inbound AVCTP connected callback is a no-op. Its
    // receive callback still supplies the established channel and ACL peer.
    ObserveRemoteChannel(channel);
    if (state && packet) {
        const std::span<const uint8_t> frame{packet->data, packet->len};
        if (avrcp::IsTrackResponse(frame)) {
            ReceiveTrack(channel, frame);
            return 0;
        }
    }
    return __real_avrcp_recv(channel, packet);
}
#endif

extern "C" void mpx_classic_ready() {
    if (!state) return;
    bt_conn_cb_register(&connection_callbacks);
    a2dp_cb_register(&profile_callbacks);
#if defined(CONFIG_BT_AVRCP)
    avrcp_cb_register(&remote_callbacks);
#endif
    {
        Lock lock;
        state->status.state = MPX_BT_IDLE;
        ++state->status.revision;
    }
    std::puts("MicroPixel Bluetooth Classic ready (A2DP Sink)");
}

extern "C" void mpx_classic_poll() {
    if (!state) return;
    Request command{};
    bt_conn* rejected;
    {
        Lock lock;
        if (state->status.state == MPX_BT_UNAVAILABLE) return;
        if (state->status.stream == MPX_BT_STREAM_CONNECTING &&
            xTaskGetTickCount() - state->profile_started >= pdMS_TO_TICKS(40000)) {
            SetStream(MPX_BT_STREAM_IDLE);
            state->status.error = -ETIMEDOUT;
        }
        command = state->command;
        state->command = {};
        rejected = state->rejected;
        state->rejected = nullptr;
    }
    if (rejected) {
        (void)bt_conn_disconnect(rejected, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(rejected);
    }
    if (command.method) {
        const int error = Execute(command);
        Lock lock;
        if (error) state->status.error = error;
        state->status.pending = 0;
        ++state->status.revision;
    }
#if defined(CONFIG_BT_AVRCP)
    PollRemote();
#endif
    RefreshBonds();
}

// These strong hooks bridge the SDK's compressed SBC receiver to the mixer.
extern "C" void* sbc2pcm_player_open(int, int rate, int channels, int) {
    if (!state || (rate != 16000 && rate != 32000 && rate != 44100 && rate != 48000) ||
        (channels != 1 && channels != 2))
        return nullptr;
    Lock lock;
    if (state->status.role != MPX_BT_SINK || state->status.state != MPX_BT_CONNECTED) return nullptr;
    state->status.sample_rate = rate;
    state->status.channels = channels;
    return state;
}

extern "C" int sbc2pcm_player_start(void*) {
    Lock lock;
    if (state->status.state != MPX_BT_CONNECTED || state->status.role != MPX_BT_SINK || !state->status.sample_rate)
        return -ENOTCONN;
    SetStream(MPX_BT_STREAM_PLAYING);
    return 0;
}

extern "C" int sbc2pcm_player_stop(void*) {
    Suspend();
    return 0;
}

// The SDK also closes the decoder handle on suspend. Link teardown arrives via ProfileLink.
extern "C" void sbc2pcm_player_close(void*) { Suspend(); }

extern "C" int sbc2pcm_player_write(void*, const void* data, unsigned bytes) {
    {
        Lock lock;
        if (state->status.role != MPX_BT_SINK || state->status.stream != MPX_BT_STREAM_PLAYING || !bytes) return 0;
        state->encoded.revision = state->status.stream_revision;
    }
    if (bytes <= sizeof(state->encoded.data)) {
        state->encoded.size = bytes;
        std::memcpy(state->encoded.data, data, bytes);
        if (xQueueSend(state->packets, &state->encoded, 0) == pdPASS) return bytes;
    }
    Lock lock;
    if (state->status.stream_revision == state->encoded.revision) ++state->status.rx_dropped_packets;
    return -ENOBUFS;
}
}  // namespace micropixel::bouffalo::classic
