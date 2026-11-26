// SPDX-License-Identifier: Apache-2.0
#pragma once

#include_next "lwipopts_user.h"

#define MEMP_NUM_TCP_PCB 15

// Accept sockets and the listener share this pool with UDP sockets.
#undef MEMP_NUM_NETCONN
#define MEMP_NUM_NETCONN 24

#undef MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG 96

// Leave room for ACK pbufs during concurrent downloads.
#undef LWIP_HEAP_SIZE
#define LWIP_HEAP_SIZE (96 * 1024)

// Limit per-flow RX bursts against the shared Wi-Fi queue.
#undef TCP_WND
#define TCP_WND (16 * TCP_MSS)

// Bound the shared Wi-Fi RX buffers each out-of-order queue can retain.
#define TCP_OOSEQ_MAX_PBUFS 3

#ifdef MICROPIXEL_ORION_RX_BENCHMARK
#ifdef __cplusplus
extern "C" {
#endif
unsigned orion_benchmark_ooseq_limit(unsigned local_port);
#ifdef __cplusplus
}
#endif
#define TCP_OOSEQ_PBUFS_LIMIT(pcb) orion_benchmark_ooseq_limit((pcb)->local_port)
#endif
