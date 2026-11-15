// SPDX-License-Identifier: Apache-2.0
#include "store.hpp"

#include <cJSON.h>
#include <fcntl.h>
#include <mbedtls/sha256.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "network.hpp"
#include "portable_app.hpp"
#include "rv32/protocol.h"

namespace micropixel::pc {
namespace {
bool Id(const std::string& value) {
    return !value.empty() && value.size() <= 64 &&
           value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") ==
               std::string::npos;
}
const char* Text(const cJSON* json, const char* key) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
bool Fail(std::string& error, const char* text) {
    error = text;
    return false;
}
struct Staging {
    std::string path;
    int fd = -1;
    ~Staging() {
        if (fd >= 0) close(fd);
        if (!path.empty()) unlink(path.c_str());
    }
};
}  // namespace
std::filesystem::path InstalledAppsDirectory() {
    const char* configured = std::getenv("MICROPIXEL_PC_DATA_DIR");
    return std::filesystem::path(configured ? configured : "build/pc-data") / "apps";
}
bool InstallRelease(const std::string& base, const StoreRelease& release, std::filesystem::path& output,
                    std::string& error, std::atomic<bool>* cancel) {
    if (!Id(release.id) || !Id(release.app_id) || release.sha256.size() != 64 || !release.size ||
        release.size > MP_RV_AOT_LIMIT)
        return Fail(error, "Invalid or unsupported store release");
    std::vector<uint8_t> bytes;
    if (!HttpGet(base + "/api/v1/store/releases/" + release.id + "/download", bytes, error, cancel, MP_RV_AOT_LIMIT))
        return false;
    if (bytes.size() != release.size) return Fail(error, "Store release size mismatch");
    std::array<uint8_t, 32> hash{};
    if (mbedtls_sha256_ret(bytes.data(), bytes.size(), hash.data(), 0)) return Fail(error, "SHA-256 failed");
    std::array<char, 65> hex{};
    for (size_t i = 0; i < hash.size(); ++i) std::snprintf(hex.data() + i * 2, 3, "%02x", hash[i]);
    if (release.sha256 != hex.data()) return Fail(error, "Store release SHA-256 mismatch");
    const auto directory = InstalledAppsDirectory();
    std::error_code status;
    std::filesystem::create_directories(directory, status);
    if (status) return Fail(error, "Cannot create installed-app directory");
    Staging stage;
    stage.path = (directory / ".install-XXXXXX").string();
    stage.fd = mkstemp(stage.path.data());
    if (stage.fd < 0) return Fail(error, "Cannot create installation staging file");
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = write(stage.fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return Fail(error, "Cannot write installation staging file");
        offset += count;
    }
    if (fsync(stage.fd)) return Fail(error, "Cannot sync installation staging file");
    PortableApp app;
    if (!app.Load(stage.path, error)) return false;
    auto metadata = std::make_unique<micropixel_bundle_metadata_t>();
    if (!app.rv32 || app.app_id != release.app_id ||
        !micropixel_read_bundle_metadata(&app.package.source, metadata.get()) ||
        release.version != reinterpret_cast<const char*>(metadata->package_version))
        return Fail(error, "Store release identity/version mismatch");
    if (cancel && cancel->load()) return Fail(error, "Installation cancelled");
    output = directory / (release.app_id + ".bundle.bin");
    if (rename(stage.path.c_str(), output.c_str())) return Fail(error, "Cannot commit installed app");
    stage.path.clear();
    int directory_fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd >= 0) {
        (void)fsync(directory_fd);
        close(directory_fd);
    }
    std::fprintf(stderr, "Market installed: %s v%s (RV32 AOT)\n", release.app_id.c_str(), release.version.c_str());
    return true;
}
bool InstallStoreApp(const std::string& id, std::filesystem::path& output, std::string& error) {
    if (!Id(id)) return Fail(error, "Invalid store App ID");
    std::string base = "https://micropixel.ai";
    if (const char* value = std::getenv("MICROPIXEL_PC_MARKET_URL")) base = value;
    while (base.ends_with('/')) base.pop_back();
    std::vector<uint8_t> bytes;
    if (!HttpGet(base + "/api/v1/store/apps/" + id, bytes, error, nullptr, 2U * 1024 * 1024)) return false;
    bytes.push_back(0);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
        cJSON_ParseWithLengthOpts(reinterpret_cast<const char*>(bytes.data()), bytes.size(), nullptr, true),
        cJSON_Delete);
    if (!json || id != Text(json.get(), "appId")) return Fail(error, "Invalid store app details");
    const auto* releases = cJSON_GetObjectItemCaseSensitive(json.get(), "releases");
    const cJSON* item;
    cJSON_ArrayForEach(item, releases) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "withdrawn")) ||
            std::string_view(Text(item, "target")) != "riscv32-ilp32f")
            continue;
        StoreRelease release{Text(item, "id"), id, Text(item, "version"), Text(item, "sha256")};
        const auto* size = cJSON_GetObjectItemCaseSensitive(item, "sizeBytes");
        if (cJSON_IsNumber(size) && size->valuedouble > 0 && size->valuedouble <= MP_RV_AOT_LIMIT)
            release.size = static_cast<uint32_t>(size->valuedouble);
        return InstallRelease(base, release, output, error);
    }
    return Fail(error, "This app has no available riscv32-ilp32f release");
}
}  // namespace micropixel::pc
