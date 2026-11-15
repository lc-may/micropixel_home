#pragma once
#include <stdint.h>
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_NVS 2
struct esp_partition_t {
    uint32_t size;
};
inline const esp_partition_t* esp_partition_find_first(int, int, const char*) {
    static const esp_partition_t partition{1024 * 1024};
    return &partition;
}
