// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstring>
#include <span>

#include "abi/bluetooth.h"

namespace micropixel {

// Use from the Guest event loop. Control calls enqueue work. Read status to
// observe completion and platform errors before issuing another control call.
class Bluetooth final {
   public:
    int Open() {
        micropixel_service_info_t info{};
        const int result = micropixel_service_open(MICROPIXEL_SERVICE_BLUETOOTH, MICROPIXEL_BLUETOOTH_MIN_VERSION,
                                                   &info, sizeof(info));
        handle_ = result == MICROPIXEL_STATUS_OK ? info.service_handle : 0;
        return result;
    }
    [[nodiscard]] bool available() const { return handle_ != 0; }
    int Status(micropixel_bluetooth_status_t& value) const { return Read(MICROPIXEL_BT_STATUS, nullptr, 0, value); }
    int Device(micropixel_bluetooth_selection_t selection, micropixel_bluetooth_device_t& device) const {
        return Read(MICROPIXEL_BT_DEVICE, &selection, sizeof(selection), device);
    }
    int Bond(uint32_t index, micropixel_bluetooth_address_t& address) const {
        return Read(MICROPIXEL_BT_BOND, &index, sizeof(index), address);
    }
    int Scan() const { return Control(MICROPIXEL_BT_SCAN); }
    int StopScan() const { return Control(MICROPIXEL_BT_STOP_SCAN); }
    int SetVisible(bool visible) const { return Value(MICROPIXEL_BT_SET_VISIBLE, visible); }
    int Connect(const micropixel_bluetooth_address_t& address) const {
        return Control(MICROPIXEL_BT_CONNECT, &address, sizeof(address));
    }
    int Disconnect() const { return Control(MICROPIXEL_BT_DISCONNECT); }
    int Pair() const { return Control(MICROPIXEL_BT_PAIR); }
    int Forget(const micropixel_bluetooth_address_t& address) const {
        return Control(MICROPIXEL_BT_FORGET, &address, sizeof(address));
    }
    int SetRole(uint32_t role) const { return Value(MICROPIXEL_BT_SET_ROLE, role); }
    int OpenStream() const { return Control(MICROPIXEL_BT_OPEN_STREAM); }
    int StartStream() const { return Control(MICROPIXEL_BT_START_STREAM); }
    int SuspendStream() const { return Control(MICROPIXEL_BT_SUSPEND_STREAM); }
    int SetAudioRoute(uint32_t route) const { return Value(MICROPIXEL_BT_SET_AUDIO_ROUTE, route); }
    int RemoteStatus(micropixel_bluetooth_remote_status_t& status) const {
        return Read(MICROPIXEL_BT_REMOTE_STATUS, nullptr, 0, status);
    }
    int RemoteKey(uint32_t session, uint32_t key) const {
        const micropixel_bluetooth_remote_key_t request{session, key};
        return Control(MICROPIXEL_BT_REMOTE_KEY, &request, sizeof(request));
    }

    int ReadPcm(uint32_t revision, std::span<int16_t> output, uint32_t& samples) const {
        const micropixel_bluetooth_pcm_t request{revision};
        uint32_t bytes = 0;
        samples = 0;
        if (output.empty() || output.size_bytes() > MICROPIXEL_BLUETOOTH_PCM_BYTES)
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        const int result = Call(MICROPIXEL_BT_READ_PCM, &request, sizeof(request), output.data(),
                                static_cast<uint32_t>(output.size_bytes()), bytes);
        if (result == MICROPIXEL_STATUS_OK) samples = bytes / sizeof(int16_t);
        return result;
    }
    int WritePcm(uint32_t revision, std::span<const int16_t> input) const {
        if (input.empty() || input.size_bytes() > MICROPIXEL_BLUETOOTH_PCM_BYTES)
            return MICROPIXEL_STATUS_INVALID_ARGUMENT;
        const micropixel_bluetooth_pcm_t request{revision};
        std::array<uint8_t, sizeof(request) + MICROPIXEL_BLUETOOTH_PCM_BYTES> bytes;
        std::memcpy(bytes.data(), &request, sizeof(request));
        std::memcpy(bytes.data() + sizeof(request), input.data(), input.size_bytes());
        return Control(MICROPIXEL_BT_WRITE_PCM, bytes.data(),
                       static_cast<uint32_t>(sizeof(request) + input.size_bytes()));
    }

   private:
    int Call(uint32_t method, const void* request, uint32_t length, void* response, uint32_t capacity,
             uint32_t& size) const {
        if (!handle_) return MICROPIXEL_STATUS_UNSUPPORTED;
        return micropixel_service_call(handle_, method, static_cast<const uint8_t*>(request), length,
                                       static_cast<uint8_t*>(response), capacity, &size);
    }
    int Control(uint32_t method, const void* request = nullptr, uint32_t length = 0) const {
        uint32_t size = 0;
        return Call(method, request, length, nullptr, 0, size);
    }
    int Value(uint32_t method, uint32_t value) const { return Control(method, &value, sizeof(value)); }

    template <typename T>
    int Read(uint32_t method, const void* request, uint32_t length, T& response) const {
        uint32_t size = 0;
        const int result = Call(method, request, length, &response, sizeof(response), size);
        return result == MICROPIXEL_STATUS_OK && size != sizeof(response) ? MICROPIXEL_STATUS_INTERNAL : result;
    }

    micropixel_service_handle_t handle_{};
};
}  // namespace micropixel
