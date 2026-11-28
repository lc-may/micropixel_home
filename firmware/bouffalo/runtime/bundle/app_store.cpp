// SPDX-License-Identifier: Apache-2.0
#include "app_store.hpp"

#include <cstdio>
#include <cstring>

#include "abi/micropixel_abi.h"
#include "lfs.h"
#include "mm.h"
#include "platform/storage/app_store_flash.h"
#include "platform/storage/embedded_xz.hpp"
#include "platform/storage/file_store.h"
#include "runtime/bundle/memory_bundle_source.h"
#include "runtime/wamr/guest_runtime.hpp"

namespace micropixel::bouffalo {
namespace {

struct FileHeader {
    std::array<char, 4> magic{'M', 'P', 'X', 'Z'};
    uint32_t unpacked_size{};
};
static_assert(sizeof(FileHeader) == 8);
constexpr FileHeader kHeader{};

AppBytes Allocate(size_t size) {
    return AppBytes(static_cast<uint8_t*>(kmalloc(size, MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE)));
}

bool ValidSize(uint32_t packed, uint32_t unpacked) {
    return packed && packed <= kMaxPackedAppBytes && unpacked && unpacked <= kMaxBundleBytes &&
           unpacked % MICROPIXEL_BUNDLE_EXTENT_ALIGNMENT == 0;
}

AppBytes Decode(const PackedApp& file) {
    if (!file || !ValidSize(file.size, file.unpacked_size)) return {};
    auto bytes = Allocate(file.unpacked_size);
    if (!bytes || !DecodeEmbeddedXz({file.bytes.get(), file.size}, {bytes.get(), file.unpacked_size})) return {};
    return bytes;
}

bool MakeRecord(const micropixel_bundle_source_t& source, AppRecord& record) {
    const auto scratch = Allocate(sizeof(micropixel_bundle_metadata_t));
    if (!scratch) return false;
    auto* metadata = std::construct_at(reinterpret_cast<micropixel_bundle_metadata_t*>(scratch.get()));
    if (!micropixel_read_bundle_metadata_for_locale(&source, "zh-CN", metadata) ||
        metadata->package_type != MICROPIXEL_BUNDLE_PACKAGE_APP || !metadata->package_version[0] ||
        !metadata->app_id[0] || metadata->app_id[0] == '.')
        return false;
    const auto required_abi = metadata->requirements.core_abi;
    if (required_abi &&
        ((required_abi >> 16U) != (MICROPIXEL_ABI_VERSION >> 16U) || required_abi > MICROPIXEL_ABI_VERSION))
        return false;
    std::memcpy(record.id.data(), metadata->app_id, record.id.size());
    std::memcpy(record.title.data(), metadata->display_name, record.title.size());
    std::memcpy(record.version.data(), metadata->package_version, record.version.size());
    record.component_flags = metadata->requirements.component_flags;
    return true;
}

bool OpenBundle(const micropixel_bundle_source_t& source, micropixel_aot_package_t& bundle) {
    if (!micropixel_open_aot_package(&source, &bundle)) return false;
    if (bundle.aot_target == MICROPIXEL_BUNDLE_AOT_TARGET_MASK_RISCV32_ILP32D &&
        !(bundle.aot_flags & (MICROPIXEL_BUNDLE_AOT_FLAG_SHARED_MEMORY | MICROPIXEL_BUNDLE_AOT_FLAG_UNCHECKED_MEMORY)))
        return true;
    micropixel_close_aot_package(&bundle);
    return false;
}

StoreResult StorageResult(int status) {
    if (status == 0) return StoreResult::kOk;
    if (status == LFS_ERR_NOSPC) return StoreResult::kNoSpace;
    if (status == LFS_ERR_NOENT) return StoreResult::kNotFound;
    return StoreResult::kIo;
}

}  // namespace

void AppBytesFree::operator()(uint8_t* bytes) const { kfree(bytes); }

LoadedApp::~LoadedApp() { micropixel_close_aot_package(&package_); }

bool LoadedApp::Open(PackedApp& file) {
    const auto size = file.unpacked_size;
    bytes_ = Decode(file);
    file = {};
    micropixel_bundle_source_t source{};
    if (!bytes_ || !micropixel_memory_bundle_source(bytes_.get(), size, &source)) return false;
    return OpenBundle(source, package_);
}

AppBytes LoadedApp::TakeImage() {
    AppBytes image(const_cast<uint8_t*>(package_.payload));
    package_.payload = nullptr;
    // The default apps have only AOT and metadata. The code copy is independent.
    if (package_.section_count == 2) {
        micropixel_close_aot_package(&package_);
        bytes_.reset();
    }
    return image;
}

const char* StoreResultName(StoreResult result) {
    switch (result) {
        case StoreResult::kOk:
            return "ok";
        case StoreResult::kUnavailable:
            return "store_unavailable";
        case StoreResult::kInvalidPackage:
            return "invalid_bundle";
        case StoreResult::kNoSpace:
            return "no_space";
        case StoreResult::kIo:
            return "storage_error";
        case StoreResult::kBusy:
            return "busy";
        case StoreResult::kVersion:
            return "version_not_newer";
        case StoreResult::kFull:
            return "app_limit";
        case StoreResult::kNotFound:
            return "app_not_found";
    }
    return "unknown_error";
}

bool OrionAppStore::Mount() {
    available_ = mpx_app_store_mount();
    return available_;
}

bool OrionAppStore::Visit(bool (*visitor)(void*, const AppRecord&), void* context) const {
    struct Context {
        decltype(visitor) it;
        void* user;
    } visit{visitor, context};
    return available_ &&
           mpx_app_store_visit(
               [](void* raw, const char* id) {
                   auto& real = *static_cast<Context*>(raw);
                   auto file = Read(id);
                   auto data = Decode(file);
                   micropixel_bundle_source_t source{};
                   AppRecord record;
                   if (!data || !micropixel_memory_bundle_source(data.get(), file.unpacked_size, &source) ||
                       !MakeRecord(source, record) || std::strcmp(id, record.id.data()) != 0) {
                       std::printf("MPX STORE rejected file %s\n", id);
                       return true;
                   }
                   return real.it(real.user, record);
               },
               &visit);
}

size_t OrionAppStore::FreeBytes() const { return mpx_app_store_free_bytes(); }

PackedApp OrionAppStore::Read(const char* id) {
    const auto size = mpx_app_store_file_size(id);
    if (size <= sizeof(FileHeader) || size - sizeof(FileHeader) > kMaxPackedAppBytes) return {};
    const auto packed_size = size - sizeof(FileHeader);
    auto data = Allocate(packed_size);
    FileHeader header;
    size_t read = 0;
    if (!data || mpx_app_store_read(id, &header, sizeof(header), data.get(), packed_size, &read) != 0 ||
        header.magic != kHeader.magic || read != packed_size || !ValidSize(read, header.unpacked_size))
        return {};
    return {std::move(data), static_cast<uint32_t>(read), header.unpacked_size};
}

StoreResult OrionAppStore::Begin(uint32_t packed_bytes, uint32_t unpacked_bytes) {
    if (!available_) return StoreResult::kUnavailable;
    if (upload_) return StoreResult::kBusy;
    if (!ValidSize(packed_bytes, unpacked_bytes)) return StoreResult::kInvalidPackage;
    auto bytes = Allocate(packed_bytes);
    if (!bytes) return StoreResult::kNoSpace;
    upload_ = {std::move(bytes), packed_bytes, unpacked_bytes};
    return StoreResult::kOk;
}

StoreResult OrionAppStore::Append(uint32_t offset, std::span<const uint8_t> bytes) {
    if (!Receiving() || offset > received_ || bytes.empty() || bytes.size() > upload_.size - offset)
        return StoreResult::kInvalidPackage;
    // Retrying an acknowledged chunk cannot change already-received bytes.
    auto* destination = upload_.bytes.get() + offset;
    if (offset < received_)
        return bytes.size() <= received_ - offset && std::memcmp(destination, bytes.data(), bytes.size()) == 0
                   ? StoreResult::kOk
                   : StoreResult::kInvalidPackage;
    std::memcpy(destination, bytes.data(), bytes.size());
    received_ += bytes.size();
    return StoreResult::kOk;
}

StoreResult OrionAppStore::Prepare() {
    if (prepared_) return StoreResult::kOk;
    if (!Receiving() || received_ != upload_.size) return StoreResult::kInvalidPackage;
    auto bytes = Decode(upload_);
    micropixel_bundle_source_t source{};
    if (!bytes || !micropixel_memory_bundle_source(bytes.get(), upload_.unpacked_size, &source) ||
        !MakeRecord(source, candidate_) || !micropixel_validate_app_package(&source, nullptr))
        return StoreResult::kInvalidPackage;
    micropixel_aot_package_t bundle{};
    if (!OpenBundle(source, bundle)) return StoreResult::kInvalidPackage;
    const bool valid = ValidateGuestAot({const_cast<uint8_t*>(bundle.payload), bundle.payload_size});
    micropixel_close_aot_package(&bundle);
    if (!valid) return StoreResult::kInvalidPackage;
    prepared_ = true;
    return StoreResult::kOk;
}

StoreResult OrionAppStore::Commit() {
    if (!prepared_) return StoreResult::kInvalidPackage;
    const auto directory = StorageResult(mpx_app_data_ensure(candidate_.id.data()));
    if (directory != StoreResult::kOk) return directory;
    const FileHeader header{.unpacked_size = upload_.unpacked_size};
    const auto result = StorageResult(
        mpx_app_store_commit(candidate_.id.data(), &header, sizeof(header), upload_.bytes.get(), upload_.size));
    if (result == StoreResult::kOk) Cancel();
    return result;
}

StoreResult OrionAppStore::Remove(const char* id) { return StorageResult(mpx_app_store_remove(id)); }

void OrionAppStore::Cancel() {
    upload_ = {};
    candidate_ = {};
    received_ = 0;
    prepared_ = false;
}

}  // namespace micropixel::bouffalo
