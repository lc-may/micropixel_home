// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cassert>
#include <cstdio>

#include "rv32/host_memory.hpp"
int main() {
    std::array<uint8_t, 256> first{}, second{};
    micropixel::pc::Rv32Memory a(first), b(second);
    mp_rv_frame frame{};
    frame.memory_base = MP_RV_RAM_BASE;
    frame.memory_size = 128;
    assert(a.Set(frame) && b.Set(frame));
    uint8_t* p = nullptr;
    assert(a.Resolve(&a, 8, 4, &p) && p == first.data() + 8);
    *p = 77;
    assert(b.Resolve(&b, 8, 4, &p) && *p == 0);
    assert(!a.Resolve(&a, 127, 2, &p) && !a.Resolve(&a, 128, 1, &p) && !a.Resolve(&a, 0, 0, &p));
    assert(!a.Address(MP_RV_RAM_BASE - 1, 1) && !a.Address(MP_RV_RAM_BASE + 127, 2));
    assert(!a.Address(UINT32_MAX, UINT32_MAX));
    frame.memory_base = MP_RV_BOOT_ADDRESS;
    assert(!a.Set(frame));
    assert(a.Resolve(&a, 8, 1, &p) && *p == 77);
    frame.memory_base = MP_RV_RAM_BASE;
    frame.memory_size = 257;
    assert(!a.Set(frame));
    frame.memory_size = MP_RV_MEMORY_LIMIT + 1;
    assert(!a.Set(frame));
    std::puts("PASS RV32 private memory windows, invalid frame preservation and overflowing/foreign ranges");
}
