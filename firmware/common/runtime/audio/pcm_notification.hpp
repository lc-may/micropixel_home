// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>

namespace micropixel::runtime {
// The consumer may refill/rearm as soon as publish wakes it. Never clear its
// new arm after publication. A full queue leaves the notification eligible.
template <class Publish>
bool PublishPcmLowWater(std::atomic<bool>& armed, Publish&& publish) {
    if (!armed.exchange(false, std::memory_order_acq_rel)) return false;
    if (publish()) return true;
    armed.store(true, std::memory_order_release);
    return false;
}
}  // namespace micropixel::runtime
