// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "abi/bluetooth.h"

#ifdef __cplusplus
extern "C" {
#endif
void mpx_classic_ready(void);
void mpx_classic_poll(void);
#ifdef __cplusplus
}

namespace micropixel::platform::audio {
class AudioEngine;
}
namespace micropixel::bouffalo::classic {
bool Initialize(platform::audio::AudioEngine& audio, uint32_t sample_rate);
bool RouteOutput(const int32_t* stereo_pcm, uint32_t frames);
micropixel_bluetooth_status_t Status();
int32_t Device(const micropixel_bluetooth_selection_t& selection, micropixel_bluetooth_device_t& device);
int32_t Bond(uint32_t index, micropixel_bluetooth_address_t& address);
int32_t Command(uint32_t method, uint32_t value = 0, const micropixel_bluetooth_address_t* address = nullptr);
int32_t ReadPcm(uint32_t revision, uint8_t* output, uint32_t capacity, uint32_t& bytes);
int32_t WritePcm(uint32_t revision, const uint8_t* input, uint32_t bytes);
#if defined(CONFIG_BT_AVRCP)
micropixel_bluetooth_remote_status_t RemoteStatus();
int32_t RemoteKey(const micropixel_bluetooth_remote_key_t& request);
#endif
}  // namespace micropixel::bouffalo::classic
#endif
