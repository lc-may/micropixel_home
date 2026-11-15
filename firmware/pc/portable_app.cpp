// SPDX-License-Identifier: Apache-2.0
#include "portable_app.hpp"

#include <cJSON.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <memory>

#include "runtime/bundle/memory_bundle_source.h"
#include "rv32/protocol.h"

namespace micropixel::pc {
namespace {
uint32_t U32(const uint8_t* p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t Fnv(const uint8_t* p, size_t size) {
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 16777619U;
    return h;
}
const char* String(const cJSON* json, const char* name) {
    const auto* v = cJSON_GetObjectItemCaseSensitive(json, name);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}
bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH) return false;
    for (auto c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.'))
            return false;
    return true;
}
}  // namespace
PortableApp::~PortableApp() {
    if (owns_package) micropixel_close_aot_package(&package);
}
bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& bytes, size_t maximum) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    auto size = file.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > maximum) return false;
    bytes.resize(static_cast<size_t>(size));
    file.seekg(0);
    return bytes.empty() || static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()));
}
bool PortableApp::Load(const std::filesystem::path& path, std::string& error) {
    auto fail = [&](const char* message) {
        error = message;
        return false;
    };
    auto wasm_path = path;
    std::filesystem::path resource_path;
    title = path.stem().string();
    app_id = "micropixel.pc." + title;
    if (path.extension() == ".json") {
        std::vector<uint8_t> descriptor;
        if (!ReadFile(path, descriptor, 16384)) return fail("Cannot read PC app descriptor");
        descriptor.push_back(0);
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_Parse(reinterpret_cast<char*>(descriptor.data())),
                                                             cJSON_Delete);
        const auto* schema = cJSON_GetObjectItemCaseSensitive(json.get(), "schema_version");
        const char* wasm_name = String(json.get(), "wasm");
        const char* id = String(json.get(), "app_id");
        if (!json || !cJSON_IsNumber(schema) || schema->valueint != 1 || !wasm_name || !id)
            return fail("Invalid PC app descriptor");
        app_id = id;
        if (const char* text = String(json.get(), "title")) title = text;
        wasm_path = path.parent_path() / wasm_name;
        if (const char* name = String(json.get(), "resources"); name && *name)
            resource_path = path.parent_path() / name;
    }
    if (!ValidId(app_id)) return fail("Invalid App ID");
    if (!ReadFile(wasm_path, wasm, 32U * 1024 * 1024) || wasm.size() < 8) return fail("Cannot read application module");
    if (std::memcmp(wasm.data(), "MPXBNDL\0", 8) == 0) {
        if (wasm.size() > MP_RV_AOT_LIMIT) return fail("RV32 Bundle exceeds 8 MiB limit");
        resources = std::move(wasm);
        micropixel_bundle_source_t source{};
        auto metadata = std::make_unique<micropixel_bundle_metadata_t>();
        if (!micropixel_memory_bundle_source(resources.data(), resources.size(), &source) ||
            !micropixel_validate_app_package(&source, metadata.get()) ||
            !micropixel_open_aot_package(&source, &package))
            return fail("Invalid app Bundle: metadata, ABI, TOC or content hash");
        owns_package = true;
        for (uint32_t i = 0; i < package.section_count; ++i) {
            const auto& section = package.sections[i];
            if (section.kind == MICROPIXEL_BUNDLE_SECTION_AOT &&
                section.reserved0 != MICROPIXEL_BUNDLE_AOT_TARGET_MASK_RISCV32_ILP32F)
                return fail("RV32 backend requires a riscv32-ilp32f Bundle; Xtensa is unsupported");
        }
        if (package.payload_size < 64 || package.payload_size > MP_RV_AOT_LIMIT ||
            std::memcmp(package.payload, "\0aot\6\0\0\0", 8) != 0 ||
            std::memcmp(package.payload + 48, "riscv32\0", 8) != 0)
            return fail("Expected WAMR v6 RV32 AOT, at most 8 MiB");
        if (package.aot_flags & MICROPIXEL_BUNDLE_AOT_FLAG_UNCHECKED_MEMORY)
            return fail("Unchecked AOT is unsupported");
        app_id = reinterpret_cast<const char*>(metadata->app_id);
        title = reinterpret_cast<const char*>(metadata->display_name);
        wasm.assign(package.payload, package.payload + package.payload_size);
        rv32 = true;
        return true;
    }
    if (std::memcmp(wasm.data(), "\0asm\1\0\0\0", 8) != 0) return fail("Expected portable Wasm or a RISC-V App Bundle");
    package = {};
    std::memcpy(package.app_id, app_id.c_str(), app_id.size() + 1);
    if (resource_path.empty()) return true;
    if (!ReadFile(resource_path, resources, 128U * 1024 * 1024) || resources.size() < 64)
        return fail("Cannot read resource pack");
    const auto* bytes = resources.data();
    const uint32_t count = U32(bytes + 20), toc = U32(bytes + 24), launch = U32(bytes + 28);
    if (std::memcmp(bytes, "MPXRPAK\0", 8) || U32(bytes + 8) != 1 || U32(bytes + 12) != 64 ||
        U32(bytes + 16) != resources.size() || resources.size() % 64 || toc != 64 || count > sections.size() ||
        uint64_t(toc) + uint64_t(count) * 48 > resources.size())
        return fail("Invalid resource pack header");
    static_assert(std::endian::native == std::endian::little);
    std::memcpy(sections.data(), bytes + toc, count * sizeof(sections[0]));
    const uint64_t payload_begin = (uint64_t(toc) + uint64_t(count) * 48 + 63) & ~uint64_t(63);
    std::array<uint32_t, MICROPIXEL_BUNDLE_MAX_SECTIONS> order{};
    for (uint32_t i = 0; i < count; ++i) {
        const auto& s = sections[i];
        const bool asset = s.kind == MICROPIXEL_BUNDLE_SECTION_ASSET;
        const bool font = s.kind == MICROPIXEL_BUNDLE_SECTION_FONT;
        const bool format_ok = asset ? (s.format >= 2 && s.format <= 5) || s.format == 9 || s.format == 10
                                     : s.format == 8 || s.format == 11;
        if ((!asset && !font) || !format_ok || !s.id || !s.size || s.flags || s.reserved0 || s.reserved1 ||
            s.offset < payload_begin || s.offset % 64 || uint64_t(s.offset) + s.size > resources.size() ||
            Fnv(bytes + s.offset, s.size) != s.hash || (font && (s.width || s.height || s.stride)))
            return fail("Invalid resource pack section");
        for (uint32_t j = 0; j < i; ++j) {
            const auto& previous = sections[j];
            if (s.id == previous.id ||
                (s.offset < uint64_t(previous.offset) + previous.size && previous.offset < uint64_t(s.offset) + s.size))
                return fail("Resource pack contains overlapping sections or duplicate IDs");
        }
        order[i] = i;
    }
    if (launch && std::none_of(sections.begin(), sections.begin() + count, [launch](const auto& s) {
            return s.id == launch && s.kind == MICROPIXEL_BUNDLE_SECTION_ASSET && (s.format == 3 || s.format == 4);
        }))
        return fail("Missing launch cover");
    std::sort(order.begin(), order.begin() + count, [this](auto a, auto b) { return sections[a].id < sections[b].id; });
    mbedtls_sha256_context digest;
    mbedtls_sha256_init(&digest);
    mbedtls_sha256_starts_ret(&digest, 0);
    constexpr char prefix[] = "MicroPixel resource pack catalog v1";
    mbedtls_sha256_update_ret(&digest, reinterpret_cast<const uint8_t*>(prefix), sizeof(prefix));
    mbedtls_sha256_update_ret(&digest, bytes + 28, 4);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& s = sections[order[i]];
        const uint32_t fields[] = {s.id, s.format, s.width, s.height, s.stride, s.size};
        uint8_t hash[32];
        mbedtls_sha256_ret(bytes + s.offset, s.size, hash, 0);
        mbedtls_sha256_update_ret(&digest, reinterpret_cast<const uint8_t*>(fields), sizeof(fields));
        mbedtls_sha256_update_ret(&digest, hash, sizeof(hash));
    }
    uint8_t hash[32];
    mbedtls_sha256_finish_ret(&digest, hash);
    mbedtls_sha256_free(&digest);
    if (std::memcmp(hash, bytes + 32, sizeof(hash))) return fail("Resource catalog SHA-256 mismatch");
    if (!micropixel_memory_bundle_source(bytes, resources.size(), &package.source)) return fail("Cannot map resources");
    package.sections = sections.data();
    package.section_count = count;
    package.launch_asset_id = launch;
    return true;
}
}  // namespace micropixel::pc
