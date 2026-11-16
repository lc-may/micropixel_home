// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <span>

#include "protocol.h"
namespace micropixel::pc {
class Rv32Memory final {
   public:
    explicit Rv32Memory(std::span<uint8_t> ram) : ram_(ram) {}
    bool Set(const mp_rv_frame& frame) {
        const auto base = frame.memory_base, size = frame.memory_size;
        if (base < MP_RV_RAM_BASE || base >= MP_RV_BOOT_ADDRESS || !size || size > MP_RV_MEMORY_LIMIT ||
            size > MP_RV_BOOT_ADDRESS - base || uint64_t(base - MP_RV_RAM_BASE) + size > ram_.size())
            return false;
        base_ = base;
        size_ = size;
        return true;
    }
    uint8_t* Address(uint32_t physical, uint32_t length) const {
        if (!size_ || physical < base_ || physical - base_ > size_ || length > size_ - (physical - base_))
            return nullptr;
        return ram_.data() + physical - MP_RV_RAM_BASE;
    }
    static bool Resolve(void* raw, uint32_t offset, uint32_t length, uint8_t** out) {
        auto& self = *static_cast<Rv32Memory*>(raw);
        if (!out || !length || offset >= self.size_ || length > self.size_ - offset) return false;
        *out = self.Address(self.base_ + offset, length);
        return *out != nullptr;
    }

   private:
    std::span<uint8_t> ram_;
    uint32_t base_{}, size_{};
};
}  // namespace micropixel::pc
