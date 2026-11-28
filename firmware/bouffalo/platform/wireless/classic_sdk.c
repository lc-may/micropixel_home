// SPDX-License-Identifier: Apache-2.0
#include <errno.h>

#include "abi/bluetooth.h"
#include "atomic.h"
#include "bluetooth.h"
#include "conn.h"
#include "hci_host.h"
#include "l2cap.h"
#include "zephyr.h"

// The SDK's private Host headers depend on the public transport declarations.
// clang-format off
#include "hci_core.h"
#include "conn_internal.h"
#include "keys.h"
#include "a2dp.h"
// clang-format on

// The pinned BL618DG profile archive keeps one local AVDTP endpoint and
// registers the SDP record for bt_dev.br.a2dp_role during initialization.
int mpx_classic_sdk_role(uint8_t role) {
    const uint8_t next = role == MPX_BT_SOURCE ? BT_A2DP_SOURCE_ROLE : BT_A2DP_SINK_ROLE;
    const uint8_t previous = bt_dev.br.a2dp_role;
    if (next == previous) return 0;
    if (bt_conn_get_remote_dev_info(NULL, BT_CONN_TYPE_BR)) return -EBUSY;
    bt_a2dp_deinit();
    bt_dev.br.a2dp_role = next;
    const int error = bt_a2dp_init();
    if (error) {
        bt_a2dp_deinit();
        bt_dev.br.a2dp_role = previous;
        (void)bt_a2dp_init();
    }
    return error;
}

int mpx_classic_sdk_forget(const bt_addr_t* address) {
    struct bt_conn* link = bt_conn_lookup_addr_br(address);
    if (link) {
        bt_conn_unref(link);
        return -EBUSY;
    }
    if (!bt_keys_find_link_key(address)) return -ENOENT;
    bt_keys_link_key_clear_addr(address);
    return 0;
}

void mpx_classic_sdk_source_format(uint32_t* rate, uint8_t* channels, uint16_t* frame_bytes) {
    extern struct bt_a2dp_config a2dp_config;
    static const uint32_t rates[] = {0, 48000, 44100, 0, 32000, 0, 0, 0, 16000};
    static const uint8_t blocks[] = {0, 16, 12, 0, 8, 0, 0, 0, 4};
    const uint8_t frequency = a2dp_config.samp_freq;
    const uint8_t block = a2dp_config.block_len;
    const uint8_t subbands = a2dp_config.sub_band == 2 ? 4 : 8;
    *rate = frequency < sizeof(rates) / sizeof(rates[0]) ? rates[frequency] : 0;
    *channels = a2dp_config.channel_mode == 8 ? 1 : 2;
    *frame_bytes = block < sizeof(blocks) ? blocks[block] * subbands * *channels * sizeof(int16_t) : 0;
}
