// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <memory>
#include <span>

#include "runtime/bundle/bundle_reader.h"

namespace micropixel::bouffalo {

inline constexpr uint32_t kMaxInstalledApps = 15;
inline constexpr uint32_t kMaxPackedAppBytes = 768U * 1024U;
inline constexpr uint32_t kMaxBundleBytes = 2U * 1024U * 1024U;

struct AppRecord {
    std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U> id{};
    std::array<char, MICROPIXEL_BUNDLE_DISPLAY_NAME_MAX_LENGTH + 1U> title{};
    std::array<char, MICROPIXEL_BUNDLE_PACKAGE_VERSION_MAX_LENGTH + 1U> version{};
    uint16_t component_flags{};
};

struct AppBytesFree {
    void operator()(uint8_t* bytes) const;
};
using AppBytes = std::unique_ptr<uint8_t[], AppBytesFree>;

struct PackedApp {
    AppBytes bytes;
    uint32_t size{};
    uint32_t unpacked_size{};
    explicit operator bool() const { return bytes != nullptr; }
};

class LoadedApp final {
   public:
    LoadedApp() = default;
    ~LoadedApp();
    LoadedApp(const LoadedApp&) = delete;
    LoadedApp& operator=(const LoadedApp&) = delete;

    [[nodiscard]] bool Open(PackedApp& source);
    const micropixel_aot_package_t& Bundle() const { return package_; }
    AppBytes TakeImage();

   private:
    AppBytes bytes_;
    micropixel_aot_package_t package_{};
};

enum class StoreResult : uint8_t {
    kOk,
    kUnavailable,
    kInvalidPackage,
    kNoSpace,
    kIo,
    kBusy,
    kVersion,
    kFull,
    kNotFound
};
const char* StoreResultName(StoreResult result);

// All filesystem operations and upload state belong to the Host controller.
class OrionAppStore final {
   public:
    [[nodiscard]] bool Mount();
    [[nodiscard]] bool Visit(bool (*visitor)(void*, const AppRecord&), void* context) const;
    bool available() const { return available_; }
    size_t FreeBytes() const;
    static PackedApp Read(const char* id);

    StoreResult Begin(uint32_t packed_bytes, uint32_t unpacked_bytes);
    StoreResult Append(uint32_t offset, std::span<const uint8_t> bytes);
    StoreResult Prepare();
    StoreResult Commit();
    StoreResult Remove(const char* id);
    void Cancel();
    bool Receiving() const { return upload_ && !prepared_; }
    const AppRecord* Pending() const { return prepared_ ? &candidate_ : nullptr; }

   private:
    PackedApp upload_;
    AppRecord candidate_;
    uint32_t received_{};
    bool available_{}, prepared_{};
};

}  // namespace micropixel::bouffalo
