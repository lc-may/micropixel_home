// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

typedef uintptr_t korp_tid;
typedef uint32_t korp_mutex;
typedef uint32_t korp_cond;
typedef uint32_t korp_thread;
typedef uint32_t korp_rwlock;
typedef uint32_t korp_sem;
typedef int os_file_handle;
typedef void* os_dir_stream;
typedef int os_raw_file_handle;
typedef int os_poll_file_handle;
typedef unsigned os_nfds_t;
typedef struct timespec os_timespec;
#define OS_THREAD_MUTEX_INITIALIZER 0
#define BH_APPLET_PRESERVED_STACK_SIZE (16 * 1024)
#define BH_THREAD_DEFAULT_PRIORITY 0
#define os_thread_local_attribute
static inline os_file_handle os_get_invalid_handle(void) { return -1; }
static inline int os_getpagesize(void) { return 4096; }
void mp_rv_trace(const void* bytes, uint32_t size);
void mp_rv_exit(uint32_t status) __attribute__((noreturn));
void mp_rv_uart_write(const void* bytes, uint32_t size);
void mp_rv_uart_read(void* bytes, uint32_t size);
