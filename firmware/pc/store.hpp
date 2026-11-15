// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <filesystem>
#include <string>

namespace micropixel::pc {
struct StoreRelease {
    std::string id, app_id, version, sha256;
    uint32_t size{};
};
std::filesystem::path InstalledAppsDirectory();
bool InstallRelease(const std::string& base, const StoreRelease&, std::filesystem::path& output, std::string& error,
                    std::atomic<bool>* cancel = nullptr);
bool InstallStoreApp(const std::string& id, std::filesystem::path& output, std::string& error);
}  // namespace micropixel::pc
