// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace micropixel::runtime {
struct InstanceKey final {
    uint64_t boot{};
    uint32_t slot{};
    uint32_t epoch{};
    [[nodiscard]] constexpr bool valid() const { return boot != 0 && epoch != 0; }
    constexpr bool operator==(const InstanceKey&) const = default;
};

enum class ResourceKind : uint8_t { kTask, kRequest, kStream, kSurface, kFont, kCount };

// A scope is never rebound. A reused slot receives a new scope and generation.
class OwnerScope final {
   public:
    explicit OwnerScope(InstanceKey key) : key_(key) {}
    [[nodiscard]] InstanceKey key() const { return key_; }
    [[nodiscard]] bool accepts(InstanceKey key) const { return key == key_ && !closed_.load(); }
    void Close() { closed_.store(true); }
    [[nodiscard]] bool Acquire(ResourceKind kind, uint32_t limit) {
        if (closed_.load()) return false;
        auto& count = counts_[static_cast<size_t>(kind)];
        auto value = count.load();
        do {
            if (value >= limit) return false;
        } while (!count.compare_exchange_weak(value, value + 1));
        if (closed_.load()) {
            count.fetch_sub(1);
            return false;
        }
        return true;
    }
    void Release(ResourceKind kind) {
        auto& count = counts_[static_cast<size_t>(kind)];
        auto value = count.load();
        while (value && !count.compare_exchange_weak(value, value - 1)) {
        }
    }
    [[nodiscard]] uint32_t Count(ResourceKind kind) const { return counts_[static_cast<size_t>(kind)].load(); }

   private:
    const InstanceKey key_;
    std::atomic<bool> closed_{};
    std::array<std::atomic<uint32_t>, static_cast<size_t>(ResourceKind::kCount)> counts_{};
};
}  // namespace micropixel::runtime
