// SPDX-License-Identifier: Apache-2.0
#include <sqlite3.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

#include "nvs.h"

namespace {
std::recursive_mutex mutex;
struct Database {
    sqlite3* db{};
    std::array<std::string, 32> namespaces;
    ~Database() {
        if (db) sqlite3_close(db);
    }
    bool Open() {
        if (db) return true;
        const char* configured = std::getenv("MICROPIXEL_PC_DATA_DIR");
        std::filesystem::path directory = configured ? configured : "build/pc-data";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error || sqlite3_open((directory / "storage.sqlite3").c_str(), &db) != SQLITE_OK) return false;
        return sqlite3_exec(db,
                            "CREATE TABLE IF NOT EXISTS kv(ns TEXT NOT NULL,key TEXT NOT NULL,type INTEGER NOT "
                            "NULL,value BLOB NOT NULL,PRIMARY KEY(ns,key))",
                            nullptr, nullptr, nullptr) == SQLITE_OK;
    }
};
Database storage;
struct Statement {
    sqlite3_stmt* value{};
    explicit Statement(const char* sql) { sqlite3_prepare_v2(storage.db, sql, -1, &value, nullptr); }
    ~Statement() {
        if (value) sqlite3_finalize(value);
    }
    bool Bind(nvs_handle_t h, const char* key = nullptr) {
        if (!value || !h || h > storage.namespaces.size() || storage.namespaces[h - 1].empty()) return false;
        sqlite3_bind_text(value, 1, storage.namespaces[h - 1].c_str(), -1, SQLITE_TRANSIENT);
        if (key) sqlite3_bind_text(value, 2, key, -1, SQLITE_TRANSIENT);
        return true;
    }
};
esp_err_t Read(nvs_handle_t h, const char* key, nvs_type_t type, void* out, size_t* length) {
    std::lock_guard lock(mutex);
    if (!key || !length || !storage.Open()) return ESP_ERR_INVALID_ARG;
    Statement s("SELECT type,value FROM kv WHERE ns=? AND key=?");
    if (!s.Bind(h, key)) return ESP_ERR_INVALID_ARG;
    if (sqlite3_step(s.value) != SQLITE_ROW) return ESP_ERR_NVS_NOT_FOUND;
    if (sqlite3_column_int(s.value, 0) != type) return ESP_ERR_NVS_TYPE_MISMATCH;
    auto size = static_cast<size_t>(sqlite3_column_bytes(s.value, 1));
    const bool enough = !out || *length >= size;
    *length = size;
    if (!enough) return ESP_ERR_INVALID_SIZE;
    if (out && size) std::memcpy(out, sqlite3_column_blob(s.value, 1), size);
    return ESP_OK;
}
esp_err_t Write(nvs_handle_t h, const char* key, nvs_type_t type, const void* bytes, size_t size) {
    std::lock_guard lock(mutex);
    if (!key || (!bytes && size) || size > 65536 || !storage.Open()) return ESP_ERR_INVALID_ARG;
    Statement s(
        "INSERT INTO kv(ns,key,type,value) VALUES(?,?,?,?) ON CONFLICT(ns,key) DO UPDATE SET "
        "type=excluded.type,value=excluded.value");
    if (!s.Bind(h, key)) return ESP_ERR_INVALID_ARG;
    sqlite3_bind_int(s.value, 3, type);
    if (size)
        sqlite3_bind_blob(s.value, 4, bytes, size, SQLITE_TRANSIENT);
    else
        sqlite3_bind_zeroblob(s.value, 4, 0);
    return sqlite3_step(s.value) == SQLITE_DONE ? ESP_OK : ESP_FAIL;
}
}  // namespace
esp_err_t nvs_open_from_partition(const char*, const char* ns, int, nvs_handle_t* out) {
    std::lock_guard lock(mutex);
    if (!ns || !out || std::strlen(ns) >= NVS_NS_NAME_MAX_SIZE || !storage.Open()) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < storage.namespaces.size(); ++i) {
        if (storage.namespaces[i].empty()) {
            storage.namespaces[i] = ns;
            *out = i + 1;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}
void nvs_close(nvs_handle_t h) {
    std::lock_guard lock(mutex);
    if (h && h <= storage.namespaces.size()) storage.namespaces[h - 1].clear();
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char* k, uint32_t* out) {
    size_t n = sizeof(*out);
    return Read(h, k, NVS_TYPE_U32, out, &n);
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char* k, uint32_t v) { return Write(h, k, NVS_TYPE_U32, &v, sizeof(v)); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char* k, uint8_t* out) {
    size_t n = sizeof(*out);
    return Read(h, k, NVS_TYPE_U8, out, &n);
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char* k, uint8_t v) { return Write(h, k, NVS_TYPE_U8, &v, sizeof(v)); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char* k, void* out, size_t* n) {
    return Read(h, k, NVS_TYPE_BLOB, out, n);
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char* k, const void* in, size_t n) {
    return Write(h, k, NVS_TYPE_BLOB, in, n);
}
esp_err_t nvs_get_str(nvs_handle_t h, const char* k, char* out, size_t* n) { return Read(h, k, NVS_TYPE_STR, out, n); }
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }  // Each write is its own SQLite atomic transaction.
esp_err_t nvs_erase_key(nvs_handle_t h, const char* k) {
    std::lock_guard lock(mutex);
    Statement s("DELETE FROM kv WHERE ns=? AND key=?");
    if (!s.Bind(h, k)) return ESP_ERR_INVALID_ARG;
    return sqlite3_step(s.value) == SQLITE_DONE ? ESP_OK : ESP_FAIL;
}
esp_err_t nvs_erase_all(nvs_handle_t h) {
    std::lock_guard lock(mutex);
    Statement s("DELETE FROM kv WHERE ns=?");
    return s.Bind(h) && sqlite3_step(s.value) == SQLITE_DONE ? ESP_OK : ESP_FAIL;
}
esp_err_t nvs_find_key(nvs_handle_t h, const char* k, nvs_type_t* type) {
    std::lock_guard lock(mutex);
    Statement s("SELECT type FROM kv WHERE ns=? AND key=?");
    if (!s.Bind(h, k)) return ESP_ERR_INVALID_ARG;
    if (sqlite3_step(s.value) != SQLITE_ROW) return ESP_ERR_NVS_NOT_FOUND;
    if (type) *type = static_cast<nvs_type_t>(sqlite3_column_int(s.value, 0));
    return ESP_OK;
}
struct PcNvsIterator {
    std::array<nvs_entry_info_t, 128> entries{};
    size_t count{}, index{};
};
esp_err_t nvs_entry_find_in_handle(nvs_handle_t h, nvs_type_t, nvs_iterator_t* out) {
    std::lock_guard lock(mutex);
    Statement s("SELECT key,type FROM kv WHERE ns=? ORDER BY key");
    if (!out || !s.Bind(h)) return ESP_ERR_INVALID_ARG;
    auto i = std::make_unique<PcNvsIterator>();
    while (sqlite3_step(s.value) == SQLITE_ROW) {
        if (i->count == i->entries.size()) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        auto& entry = i->entries[i->count++];
        std::snprintf(entry.key, sizeof(entry.key), "%s", sqlite3_column_text(s.value, 0));
        entry.type = static_cast<nvs_type_t>(sqlite3_column_int(s.value, 1));
    }
    if (!i->count) {
        *out = nullptr;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out = i.release();
    return ESP_OK;
}
esp_err_t nvs_entry_info(nvs_iterator_t i, nvs_entry_info_t* out) {
    if (!i || !out) return ESP_ERR_INVALID_ARG;
    *out = i->entries[i->index];
    return ESP_OK;
}
esp_err_t nvs_entry_next(nvs_iterator_t* i) {
    if (!i || !*i) return ESP_ERR_NVS_NOT_FOUND;
    if (++(*i)->index < (*i)->count) return ESP_OK;
    delete *i;
    *i = nullptr;
    return ESP_ERR_NVS_NOT_FOUND;
}
void nvs_release_iterator(nvs_iterator_t i) { delete i; }
esp_err_t nvs_get_stats(const char*, nvs_stats_t* out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = {.total_entries = 32768};
    return ESP_OK;
}
