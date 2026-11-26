// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace micropixel::device {
class HttpClient;
}
namespace micropixel::bouffalo {
bool InitializeNetwork();
device::HttpClient* NetworkBackend(uint32_t slot);
}  // namespace micropixel::bouffalo
