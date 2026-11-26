// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <optional>

#include "audio/orion_audio_output.hpp"
#include "audio_guest_aot.h"
#include "bflb_mtimer.h"
#include "platform/audio/audio_engine.hpp"
#include "runtime/abi/audio_endpoint.hpp"
#include "runtime/wamr/guest_runtime.hpp"
#include "work/task_policy.hpp"
#include "work/task_stack.h"

namespace {

using namespace micropixel::bouffalo;
using micropixel::platform::audio::AudioEngine;
using micropixel::runtime::EventQueue;
using micropixel::runtime::PcmStreamService;
using micropixel::runtime::ServiceRegistry;

int32_t RunAudioTest() {
    static OrionAudioOutput output;
    static AudioEngine engine;
    static EventQueue events;
    if (!events.valid() || engine.Initialize(output, output.SampleRate()) != micropixel::platform::audio::kAudioOk)
        return -1;
    micropixel::device::AudioService audio(engine);
    const auto origin = static_cast<int64_t>(bflb_mtimer_get_time_us());
    static std::optional<PcmStreamService> pcm_storage;
    auto& pcm = pcm_storage.emplace(audio, events, origin);
    AudioEndpoint endpoint(engine, pcm);
    static std::optional<ServiceRegistry> registry;
    GuestContext guest{events, registry.emplace(endpoint), origin};
    int32_t status = -1;
    if (pcm.valid() && InitializeGuestRuntime()) {
        status = RunGuest(guest, audio_guest_aot);
        ShutdownGuestRuntime();
    }
    pcm.CloseAll();
    (void)engine.StopAll();
    registry.reset();
    pcm_storage.reset();
    (void)engine.SuspendAll();
    const uint32_t writes = output.NonzeroWrites();
    std::printf("Orion Audio Guest exit=%ld; nonzero DMA writes=%lu\n", static_cast<long>(status),
                static_cast<unsigned long>(writes));
    return status == 0 && writes == 0U ? -1 : status;
}

void AudioTest(void*) {
    const bool passed = RunAudioTest() == 0;
    std::printf("MicroPixel Orion audio Guest %s\nOrion audio Guest stack minimum free: %lu bytes\n",
                passed ? "PASS" : "FAIL", static_cast<unsigned long>(mpx_stack_free_bytes(nullptr)));
    vTaskDelete(nullptr);
}

}  // namespace

int main() {
    if (mpx_create_task(AudioTest, "mpx_audio_guest", 8192, nullptr, micropixel::task_policy::kGuestPriority,
                        nullptr) != pdPASS) {
        std::puts("Unable to start MicroPixel Orion audio Guest");
        return 1;
    }
    return 0;
}
