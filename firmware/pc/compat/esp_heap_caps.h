#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
static inline void* heap_caps_malloc(size_t size, unsigned caps) {
    (void)caps;
    return malloc(size);
}
static inline void* heap_caps_calloc(size_t n, size_t size, unsigned caps) {
    (void)caps;
    return calloc(n, size);
}
static inline void* heap_caps_realloc(void* p, size_t size, unsigned caps) {
    (void)caps;
    return realloc(p, size);
}
static inline void heap_caps_free(void* p) { free(p); }
static inline void* heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps) {
    (void)caps;
    void* p = NULL;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    return posix_memalign(&p, alignment, size) == 0 ? p : NULL;
}
static inline void* heap_caps_aligned_calloc(size_t a, size_t n, size_t s, unsigned c) {
    if (s && n > (size_t)-1 / s) return NULL;
    void* p = heap_caps_aligned_alloc(a, n * s, c);
    if (p) memset(p, 0, n * s);
    return p;
}
/* Capacity telemetry is unavailable on Linux; never invent an MCU heap size. */
static inline size_t heap_caps_get_free_size(unsigned caps) {
    (void)caps;
    return 0;
}
static inline size_t heap_caps_get_largest_free_block(unsigned caps) {
    (void)caps;
    return 0;
}
static inline size_t heap_caps_get_minimum_free_size(unsigned caps) {
    (void)caps;
    return 0;
}
static inline size_t heap_caps_get_total_size(unsigned caps) {
    (void)caps;
    return 0;
}
