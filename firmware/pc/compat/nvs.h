#pragma once
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NVS_NS_NAME_MAX_SIZE 16
#define NVS_KEY_NAME_MAX_SIZE 16
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1103
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
enum nvs_type_t {
    NVS_TYPE_ANY,
    NVS_TYPE_U8,
    NVS_TYPE_I8,
    NVS_TYPE_U16,
    NVS_TYPE_I16,
    NVS_TYPE_U32,
    NVS_TYPE_I32,
    NVS_TYPE_U64,
    NVS_TYPE_I64,
    NVS_TYPE_BLOB,
    NVS_TYPE_STR
};
using nvs_handle_t = uint32_t;
struct PcNvsIterator;
using nvs_iterator_t = PcNvsIterator*;
struct nvs_entry_info_t {
    char key[16];
    nvs_type_t type;
};
struct nvs_stats_t {
    size_t used_entries, free_entries, available_entries, total_entries, namespace_count;
};
esp_err_t nvs_open_from_partition(const char*, const char*, int, nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*);
esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t);
esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*);
esp_err_t nvs_set_u8(nvs_handle_t, const char*, uint8_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*);
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t);
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char*);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_find_key(nvs_handle_t, const char*, nvs_type_t*);
esp_err_t nvs_entry_find_in_handle(nvs_handle_t, nvs_type_t, nvs_iterator_t*);
esp_err_t nvs_entry_info(nvs_iterator_t, nvs_entry_info_t*);
esp_err_t nvs_entry_next(nvs_iterator_t*);
void nvs_release_iterator(nvs_iterator_t);
esp_err_t nvs_get_stats(const char*, nvs_stats_t*);
