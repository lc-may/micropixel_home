// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "abi/micropixel_abi.h"
#include "runtime/bundle/bundle_reader.h"

namespace micropixel::pc {
struct PortableApp {
    ~PortableApp();
    PortableApp() = default;
    PortableApp(const PortableApp&) = delete;
    PortableApp& operator=(const PortableApp&) = delete;
    std::string title, app_id;
    bool rv32{}, owns_package{};
    micropixel_system_launch_arguments_response_t launch_arguments{};
    std::vector<uint8_t> wasm, resources;
    std::array<micropixel_bundle_section_t, MICROPIXEL_BUNDLE_MAX_SECTIONS> sections{};
    micropixel_aot_package_t package{};
    micropixel_app_requirements_t requirements{};
    bool Load(const std::filesystem::path&, std::string& error);
};
bool ReadFile(const std::filesystem::path&, std::vector<uint8_t>&, size_t maximum_bytes);
}  // namespace micropixel::pc
