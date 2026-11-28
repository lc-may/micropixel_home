// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <span>

#include "runtime/abi/service_registry.hpp"
#include "runtime/event_queue.hpp"

namespace micropixel::runtime {
class Components;
}

namespace micropixel::bouffalo {

class TimerEndpoint;

struct GuestMemory final {
    const uint8_t* Resolve(uint32_t offset, uint32_t length) const;
    void* module{};
};

struct GuestContext final {
    runtime::EventQueue& events;
    const runtime::ServiceRegistry& services;
    int64_t clock_origin_us;
    TimerEndpoint* timers{};
    runtime::Components* components{};
    GuestMemory* memory{};
    std::atomic<bool> stopping{};
    bool instruction_budget_epoch{};
};

[[nodiscard]] bool InitializeGuestRuntime();
int32_t RunGuest(GuestContext& context, std::span<const uint8_t> binary, uint32_t unpacked_size = 0,
                 const char* launch_name = nullptr, uint32_t linear_growth_pages = 0);
// Takes ownership of the mutable image allocated from the WAMR heap.
int32_t RunGuestImage(GuestContext& context, std::span<uint8_t> image, const char* launch_name,
                      uint32_t linear_growth_pages, uint64_t started_us);
#if WASM_ENABLE_AOT && defined(MICROPIXEL_ORION_HOST)
[[nodiscard]] bool ValidateGuestAot(std::span<uint8_t> image);
#endif
void ShutdownGuestRuntime();

}  // namespace micropixel::bouffalo
