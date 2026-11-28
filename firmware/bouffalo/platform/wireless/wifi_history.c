// SPDX-License-Identifier: Apache-2.0
#include "wifi_history.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "mm.h"
#include "platform/storage/file_store.h"

_Static_assert(sizeof(mpx_wifi_profile_t) == 124, "Wi-Fi history record size");

static const char kHistoryFile[] = "settings.wifi.v2";
static mpx_wifi_profile_t* profiles;
static size_t profile_count;
static size_t profile_capacity;
static bool writable;
static bool dirty;
static bool write_failed;

void mpx_wifi_clear_credentials(void* value, size_t size) {
    for (volatile unsigned char* p = value; size; --size) *p++ = 0;
}

bool mpx_wifi_unicast_bssid(const uint8_t bssid[6]) {
    static const uint8_t zero[6];
    return !(bssid[0] & 1) && memcmp(bssid, zero, sizeof(zero)) != 0;
}

bool mpx_wifi_profile_credentials_valid(const mpx_wifi_profile_t* profile) {
    const char* ssid_end = memchr(profile->ssid, 0, sizeof(profile->ssid));
    const char* password_end = memchr(profile->password, 0, sizeof(profile->password));
    return ssid_end && ssid_end != profile->ssid && password_end &&
           (password_end == profile->password ||
            (password_end - profile->password >= 8 && password_end - profile->password <= 63));
}

static bool profile_valid(const mpx_wifi_profile_t* profile) {
    return mpx_wifi_profile_credentials_valid(profile) && mpx_wifi_unicast_bssid(profile->bssid) &&
           (profile->ipv4.addr ? profile->ipv4.mask != 0
                               : !(profile->ipv4.mask || profile->ipv4.gateway || profile->ipv4.dns));
}

static bool reserve_profiles(size_t capacity) {
    if (capacity > SIZE_MAX / sizeof(*profiles)) return false;
    mpx_wifi_profile_t* storage = kmalloc(capacity * sizeof(*profiles), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE);
    if (!storage) return false;
    if (profiles) {
        const size_t used = profile_count * sizeof(*profiles);
        memcpy(storage, profiles, used);
        mpx_wifi_clear_credentials(profiles, used);
        kfree(profiles);
    }
    profiles = storage;
    profile_capacity = capacity;
    return true;
}

bool mpx_wifi_history_load(void) {
    size_t size = 0;
    // LittleFS reports the file size even when the supplied buffer is too small.
    const int result = mpx_file_read(kHistoryFile, NULL, 0, &size);
    if (result == -ENOENT) {
        writable = true;
        return false;
    }
    if (!size || size % sizeof(*profiles) || !reserve_profiles(size / sizeof(*profiles))) return false;
    size_t read = 0;
    if (mpx_file_read(kHistoryFile, profiles, size, &read) || read != size) {
        mpx_wifi_clear_credentials(profiles, size);
        return false;
    }
    for (size_t i = 0; i < size / sizeof(*profiles); ++i) {
        if (profile_valid(&profiles[i])) profiles[profile_count++] = profiles[i];
    }
    mpx_wifi_clear_credentials(profiles + profile_count, size - profile_count * sizeof(*profiles));
    writable = true;
    dirty = profile_count != size / sizeof(*profiles);
    return profile_count != 0;
}

bool mpx_wifi_history_remember(const mpx_wifi_profile_t* profile) {
    if (!profile_valid(profile)) return false;
    size_t index = 0;
    for (; index < profile_count; ++index) {
        if (!memcmp(profiles[index].bssid, profile->bssid, sizeof(profile->bssid)) &&
            !strcmp(profiles[index].ssid, profile->ssid))
            break;
    }
    if (index == 0 && profile_count && !memcmp(&profiles[0], profile, sizeof(*profile))) return true;
    if (index == profile_count) {
        if (profile_count == profile_capacity && !reserve_profiles(profile_capacity ? profile_capacity * 2 : 4))
            return false;
        ++profile_count;
    }
    memmove(profiles + 1, profiles, index * sizeof(*profiles));
    profiles[0] = *profile;
    dirty = true;
    return true;
}

void mpx_wifi_history_flush(void) {
    if (!dirty || !profile_count) return;
    if (writable && mpx_file_write(kHistoryFile, profiles, profile_count * sizeof(*profiles)) == 0) {
        dirty = false;
        write_failed = false;
    } else if (!write_failed) {
        puts("MicroPixel Wi-Fi history save failed");
        write_failed = true;
    }
}

const mpx_wifi_profile_t* mpx_wifi_history_profile(size_t index) {
    return index < profile_count ? &profiles[index] : NULL;
}

const mpx_wifi_network_t* mpx_wifi_profile_match(const mpx_wifi_profile_t* profile, const mpx_wifi_network_t* networks,
                                                 unsigned count) {
    const size_t length = strlen(profile->ssid);
    const bool known_bssid = mpx_wifi_unicast_bssid(profile->bssid);
    const bool open = profile->password[0] == 0;
    const mpx_wifi_network_t* best = NULL;
    for (unsigned i = 0; i < count; ++i) {
        const mpx_wifi_network_t* item = &networks[i];
        if (!item->supported || item->open != open || item->length != length ||
            memcmp(item->ssid, profile->ssid, length) ||
            (known_bssid && memcmp(item->bssid, profile->bssid, sizeof(profile->bssid))))
            continue;
        if (!best || item->rssi > best->rssi) best = item;
    }
    return best;
}
