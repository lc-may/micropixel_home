// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "portable_app.hpp"
#include "rv32/protocol.h"
namespace micropixel::pc {
inline constexpr uint32_t kNativeStartTimeoutMs = 10000, kRv32StartTimeoutMs = 30000;
inline constexpr uint64_t kSessionMemoryBudget = 4ULL * (MP_RV_RAM_SIZE + 2ULL * MP_RV_AOT_LIMIT);
inline uint32_t StartupTimeout(const PortableApp& app) {
    return app.rv32 ? kRv32StartTimeoutMs : kNativeStartTimeoutMs;
}
// Admission reservations include the VM's ceiling and its immutable package lease.
// Shared SDL/LVGL buffers and OS/QEMU executable overhead are outside this budget.
inline uint64_t MemoryReservation(const PortableApp& app) {
    return (app.rv32 ? MP_RV_RAM_SIZE : MP_RV_MEMORY_LIMIT) + uint64_t(app.wasm.size()) + app.resources.size();
}
}  // namespace micropixel::pc
