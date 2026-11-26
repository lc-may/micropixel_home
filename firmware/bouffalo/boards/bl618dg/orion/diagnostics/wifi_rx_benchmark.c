// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bflb_mtimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/sockets.h"
#include "lwip/stats.h"
#include "lwip/tcpip.h"
#include "rtos_al.h"
#include "shell.h"
#include "wifi_mgmr_ext.h"

enum { kPort = 5208, kClients = 15, kRunMs = 300000, kBufferBytes = 8192, kProbeMs = 30 };

struct client {
    int fd;
    uint16_t port;
    uint8_t id;
    uint8_t reply_bytes;
    bool draining;
    uint32_t received;
    uint32_t last_read_ms;
};

struct flow {
    uint32_t rcv_nxt;
    uint32_t rcv_wnd;
    uint32_t gap;
    uint32_t ooseq_since_ms;
    uint8_t ooseq;
    uint8_t tcp_state;
};

static struct client clients[kClients];
static struct flow flows[kClients];
static uint8_t read_buffer[kBufferBytes] __attribute__((section(".psram_noinit")));
static TaskHandle_t worker;
static volatile bool stop;
static volatile unsigned empty_checks;
static volatile unsigned free_low;
static unsigned low_watermark;
static unsigned low_water_hits;
static unsigned ack_calls;
static unsigned ack_mem_errors;
static unsigned ack_other_errors;

// In the pinned BL618DG FHOST archive, the first member is the free RX queue.
// The benchmark reads this private symbol for diagnostics only.
extern rtos_queue fhost_rx_env[];
extern const uint16_t g_macsw_config_FHOST_RX_BUF_CNT;
extern const uint32_t fhost_rx_buf_mem_size;
extern bool __real_macif_rx_buf_check(void);
extern err_t __real_tcp_send_empty_ack(struct tcp_pcb* pcb);

unsigned orion_benchmark_ooseq_limit(unsigned local_port) {
    if (local_port == kPort && low_watermark && (unsigned)rtos_queue_cnt(fhost_rx_env[0]) <= low_watermark) {
        ++low_water_hits;
        return 1;
    }
    return TCP_OOSEQ_MAX_PBUFS;
}

bool __wrap_macif_rx_buf_check(void) {
    const bool available = __real_macif_rx_buf_check();
    if (worker != NULL) {
        const unsigned free = available ? (unsigned)rtos_queue_cnt(fhost_rx_env[0]) : 0;
        if (free < free_low) free_low = free;
        if (!available) ++empty_checks;
    }
    return available;
}

err_t __wrap_tcp_send_empty_ack(struct tcp_pcb* pcb) {
    const unsigned mem_errors = lwip_stats.mem.err;
    const err_t result = __real_tcp_send_empty_ack(pcb);
    if (pcb->local_port == kPort) {
        ++ack_calls;
        if (result != ERR_OK) {
            if (lwip_stats.mem.err != mem_errors)
                ++ack_mem_errors;
            else
                ++ack_other_errors;
        }
    }
    return result;
}

static void snapshot(bool include_flows) {
    const unsigned rx_free = (unsigned)rtos_queue_cnt(fhost_rx_env[0]);
    struct client sample[kClients];
    taskENTER_CRITICAL();
    memcpy(sample, clients, sizeof(sample));
    taskEXIT_CRITICAL();

    LOCK_TCPIP_CORE();
    const struct stats_mem mem = lwip_stats.mem;
    const struct stats_mem seg = *lwip_stats.memp[MEMP_TCP_SEG];
    const struct stats_mem pcb_pool = *lwip_stats.memp[MEMP_TCP_PCB];
    const struct stats_mem netconn = *lwip_stats.memp[MEMP_NETCONN];
    const unsigned ack = ack_calls;
    const unsigned ack_mem = ack_mem_errors;
    const unsigned ack_other = ack_other_errors;
    const unsigned low_hits = low_water_hits;
    const unsigned link_drops = lwip_stats.link.drop;
    const unsigned now = (unsigned)bflb_mtimer_get_time_ms();
    unsigned ooseq = 0;
    for (unsigned i = 0; i < kClients; ++i) {
        struct flow* f = &flows[i];
        f->tcp_state = 0;
        f->ooseq = 0;
        if (!sample[i].port) continue;
        for (const struct tcp_pcb* pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
            if (pcb->local_port != kPort || pcb->remote_port != sample[i].port) continue;
            f->tcp_state = pcb->state;
            f->rcv_nxt = pcb->rcv_nxt;
            f->rcv_wnd = pcb->rcv_wnd;
            if (pcb->ooseq) {
                if (!f->ooseq_since_ms) f->ooseq_since_ms = now;
                f->gap = pcb->ooseq->tcphdr->seqno - pcb->rcv_nxt;
                for (const struct tcp_seg* s = pcb->ooseq; s; s = s->next) {
                    f->ooseq += pbuf_clen(s->p);
                }
                ooseq += f->ooseq;
            }
            break;
        }
        if (!f->ooseq) f->ooseq_since_ms = 0;
    }
    UNLOCK_TCPIP_CORE();

    unsigned live = 0;
    unsigned received = 0;
    for (unsigned i = 0; i < kClients; ++i) {
        live += sample[i].fd >= 0 && sample[i].received > 0;
        received += sample[i].received;
    }
    printf(
        "LAN RX data_flows=%u received=%u rx_free=%u rx_free_low=%u empty_checks=%u "
        "mem_used=%u mem_peak=%u mem_err=%u seg_used=%u seg_peak=%u seg_err=%u pcb_used=%u "
        "netconn_used=%u ooseq_pbufs=%u ack_calls=%u ack_mem_err=%u ack_other_err=%u link_drop=%u low_hits=%u\n",
        live, received, rx_free, free_low, empty_checks, (unsigned)mem.used, (unsigned)mem.max, (unsigned)mem.err,
        (unsigned)seg.used, (unsigned)seg.max, (unsigned)seg.err, (unsigned)pcb_pool.used, (unsigned)netconn.used,
        ooseq, ack, ack_mem, ack_other, link_drops, low_hits);
    if (!include_flows) return;
    for (unsigned i = 0; i < kClients; ++i) {
        if (!sample[i].port) continue;
        const struct flow* f = &flows[i];
        printf(
            "LAN FLOW id=%u peer=%u recv=%lu last_read_ms=%lu draining=%u reply=%u state=%u nxt=%lu "
            "wnd=%lu ooseq=%u gap=%lu gap_sample_ms=%lu\n",
            sample[i].id, sample[i].port, (unsigned long)sample[i].received, (unsigned long)sample[i].last_read_ms,
            sample[i].draining, sample[i].reply_bytes, f->tcp_state, (unsigned long)f->rcv_nxt,
            (unsigned long)f->rcv_wnd, f->ooseq, (unsigned long)(f->ooseq ? f->gap : 0),
            (unsigned long)(f->ooseq_since_ms ? now - f->ooseq_since_ms : 0));
    }
    puts("LAN FLOWS end");
}

static void close_client(struct client* c) {
    lwip_close(c->fd);
    c->fd = -1;
}

static void accept_client(int listener) {
    struct sockaddr_in peer;
    socklen_t length = sizeof(peer);
    const int fd = lwip_accept(listener, (struct sockaddr*)&peer, &length);
    if (fd < 0) return;
    for (unsigned i = 0; i < kClients; ++i) {
        if (!clients[i].port) {
            clients[i] = (struct client){.fd = fd, .port = lwip_ntohs(peer.sin_port)};
            return;
        }
    }
    lwip_close(fd);
}

static void process_client(struct client* c) {
    if (!c->draining) {
        const int length = lwip_recv(c->fd, read_buffer, c->id ? sizeof(read_buffer) : 1, MSG_DONTWAIT);
        if (length > 0) {
            if (c->id) {
                c->received += length;
                c->last_read_ms = (uint32_t)bflb_mtimer_get_time_ms();
            } else if (read_buffer[0] && read_buffer[0] <= kClients) {
                c->id = read_buffer[0];
            } else {
                close_client(c);
            }
        } else if (!length) {
            c->draining = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            close_client(c);
        }
        return;
    }

    const uint32_t reply = lwip_htonl(c->received);
    const int length =
        lwip_send(c->fd, (const uint8_t*)&reply + c->reply_bytes, sizeof(reply) - c->reply_bytes, MSG_DONTWAIT);
    if (length > 0) {
        c->reply_bytes += length;
        if (c->reply_bytes == sizeof(reply)) close_client(c);
    } else if (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOMEM) {
        close_client(c);
    }
}

static void receive_task(void* unused) {
    (void)unused;
    for (unsigned i = 0; i < kClients; ++i) clients[i] = (struct client){.fd = -1};
    memset(flows, 0, sizeof(flows));
    lwip_socket_thread_init();
    ip4_addr_t ip = {0};
    wifi_sta_ip4_addr_get(&ip.addr, NULL, NULL, NULL);
    const int listener = ip.addr ? lwip_socket(AF_INET, SOCK_STREAM, 0) : -1;
    if (listener >= 0) {
        const int enabled = 1;
        struct sockaddr_in address = {.sin_len = sizeof(address),
                                      .sin_family = AF_INET,
                                      .sin_port = PP_HTONS(kPort),
                                      .sin_addr = {.s_addr = ip.addr}};
        lwip_setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (lwip_bind(listener, (struct sockaddr*)&address, sizeof(address)) == 0 &&
            lwip_listen(listener, kClients) == 0) {
            char address_text[16];
            ip4addr_ntoa_r(&ip, address_text, sizeof(address_text));
            printf("LAN READY address=%s port=%u window=%u pool_count=%u pool_bytes=%lu pcb=%u ids=1\n", address_text,
                   kPort, (unsigned)TCP_WND, g_macsw_config_FHOST_RX_BUF_CNT, (unsigned long)fhost_rx_buf_mem_size,
                   (unsigned)MEMP_NUM_TCP_PCB);
            const uint64_t deadline = bflb_mtimer_get_time_ms() + kRunMs;
            uint64_t next_probe = 0;
            unsigned next_client = 0;
            while (!stop && bflb_mtimer_get_time_ms() < deadline) {
                struct pollfd polls[kClients + 1] = {{.fd = listener, .events = POLLIN}};
                for (unsigned i = 0; i < kClients; ++i) {
                    polls[i + 1].fd = clients[i].fd;
                    polls[i + 1].events = clients[i].draining ? POLLOUT : POLLIN;
                }
                if (lwip_poll(polls, kClients + 1, kProbeMs) < 0) continue;
                const uint64_t now = bflb_mtimer_get_time_ms();
                const bool probe = now >= next_probe;
                if (probe) next_probe = now + kProbeMs;
                if (polls[0].revents & POLLIN) accept_client(listener);
                for (unsigned j = 0; j < kClients; ++j) {
                    const unsigned i = (next_client + j) % kClients;
                    // lwIP can defer a FIN after returning data without a second poll event.
                    if ((polls[i + 1].revents || probe) && clients[i].fd >= 0) process_client(&clients[i]);
                }
                next_client = (next_client + 1) % kClients;
            }
        }
        lwip_close(listener);
    }
    for (unsigned i = 0; i < kClients; ++i) {
        if (clients[i].fd >= 0) close_client(&clients[i]);
    }
    lwip_socket_thread_cleanup();
    const int error = errno;
    worker = NULL;
    printf("LAN CLOSED errno=%d stack_free=%lu\n", error,
           (unsigned long)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    vTaskDelete(NULL);
}

static void lan_command(int argc, char** argv) {
    if (argc < 2 || argc > 3) return;
    if (strcmp(argv[1], "start") == 0 && !worker) {
        if (!fhost_rx_env[0]) return;
        char* end = NULL;
        const unsigned long watermark = argc == 3 ? strtoul(argv[2], &end, 10) : 0;
        if (argc == 3 && (end == argv[2] || *end || watermark >= g_macsw_config_FHOST_RX_BUF_CNT)) return;
        stop = false;
        empty_checks = 0;
        free_low = g_macsw_config_FHOST_RX_BUF_CNT;
        LOCK_TCPIP_CORE();
        low_watermark = (unsigned)watermark;
        low_water_hits = 0;
        ack_calls = ack_mem_errors = ack_other_errors = 0;
        lwip_stats.mem.max = lwip_stats.mem.used;
        lwip_stats.memp[MEMP_TCP_SEG]->max = lwip_stats.memp[MEMP_TCP_SEG]->used;
        UNLOCK_TCPIP_CORE();
        if (xTaskCreate(receive_task, "lan_rx", 4096 / sizeof(StackType_t), NULL, 5, &worker) != pdPASS) {
            puts("LAN NOT STARTED");
        }
    } else if (worker && strcmp(argv[1], "stats") == 0) {
        snapshot(false);
    } else if (worker && strcmp(argv[1], "flows") == 0) {
        snapshot(true);
    } else if (worker && strcmp(argv[1], "stop") == 0) {
        stop = true;
    }
}

SHELL_CMD_EXPORT_ALIAS(lan_command, lanrx, LAN receive benchmark);
