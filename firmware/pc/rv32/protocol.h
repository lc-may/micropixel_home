// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

#define MP_RV_MAGIC 0x5652504dU
#define MP_RV_VERSION 1U
#define MP_RV_RAM_BASE 0x80000000U
#define MP_RV_RAM_SIZE (128U * 1024 * 1024)
#define MP_RV_BOOT_ADDRESS 0x84000000U
#define MP_RV_AOT_ADDRESS 0x84010000U
#define MP_RV_AOT_LIMIT (8U * 1024 * 1024)
#define MP_RV_MEMORY_LIMIT (16U * 1024 * 1024)
#define MP_RV_TRACE_LIMIT 512U
enum mp_rv_kind {
    MP_RV_HELLO = 1,
    MP_RV_TRACE,
    MP_RV_ABI,
    MP_RV_LOG,
    MP_RV_EVENT,
    MP_RV_CLOCK,
    MP_RV_OPEN,
    MP_RV_CALL,
    MP_RV_SUBMIT,
    MP_RV_DONE,
    MP_RV_REPLY
};
struct mp_rv_frame {
    uint32_t magic, version, kind, sequence;
    uint32_t memory_base, memory_size;
    uint32_t args[10];
    uint32_t result_lo, result_hi, payload_size, reserved;
};
struct mp_rv_boot {
    uint32_t magic, version, aot_size, flags;
};
#if defined(__cplusplus)
static_assert(sizeof(mp_rv_frame) == 80);
#else
_Static_assert(sizeof(struct mp_rv_frame) == 80, "RV32 protocol layout");
#endif
