// SPDX-License-Identifier: Apache-2.0
#include <sys/stat.h>

#include "platform_api_vmcore.h"
#include "protocol.h"

static volatile uint8_t* const uart = (volatile uint8_t*)0x10000000;
static void init_uart(void) {
    static bool ready;
    if (ready) return;
    /* Set FIFO mode before the first outgoing frame: changing it after the
     * Host replies can discard an already received byte. */
    uart[2] = 1;
    uart[1] = 1;
    *(volatile uint32_t*)0x0c000028 = 1;
    *(volatile uint32_t*)0x0c002000 = 1U << 10;
    *(volatile uint32_t*)0x0c200000 = 0;
    __asm__ volatile("csrs mie, %0" ::"r"(1U << 11));
    ready = true;
}
void mp_rv_uart_write(const void* bytes, uint32_t size) {
    init_uart();
    const uint8_t* p = bytes;
    for (uint32_t i = 0; i < size; ++i) {
        while (!(uart[5] & 0x20)) {
        }
        uart[0] = p[i];
    }
}
void mp_rv_uart_read(void* bytes, uint32_t size) {
    init_uart();
    volatile uint32_t* claim = (volatile uint32_t*)0x0c200004;
    uint8_t* p = bytes;
    for (uint32_t i = 0; i < size; ++i) {
        while (!(uart[5] & 1)) {
            /* MIE stays clear: WFI wakes for locally enabled pending IRQs,
             * without entering a trap handler or spinning while Host waits. */
            __asm__ volatile("wfi");
            uint32_t irq = *claim;
            if (irq) *claim = irq;
        }
        p[i] = uart[0];
    }
    __asm__ volatile("fence rw,rw" ::: "memory");
}
void mp_rv_exit(uint32_t status) {
    struct mp_rv_frame message = {
        .magic = MP_RV_MAGIC, .version = MP_RV_VERSION, .kind = MP_RV_DONE, .result_lo = status};
    mp_rv_uart_write(&message, sizeof(message));
    *(volatile uint32_t*)0x100000 = status ? ((status & 0xffff) << 16) | 0x3333 : 0x5555;
    for (;;) __asm__ volatile("wfi");
}
void mp_rv_trace(const void* bytes, uint32_t size) {
    if (size > MP_RV_TRACE_LIMIT) size = MP_RV_TRACE_LIMIT;
    const struct mp_rv_frame message = {
        .magic = MP_RV_MAGIC, .version = MP_RV_VERSION, .kind = MP_RV_TRACE, .payload_size = size};
    mp_rv_uart_write(&message, sizeof(message));
    mp_rv_uart_write(bytes, size);
}
void mp_rv_trap_report(uint32_t cause, uint32_t pc, uint32_t value) {
    os_printf("RV32 trap: cause=%u pc=%08x value=%08x\n", cause, pc, value);
    mp_rv_exit(1);
}
int bh_platform_init(void) { return 0; }
void bh_platform_destroy(void) {}
void* os_malloc(unsigned size) { return malloc(size); }
void* os_realloc(void* p, unsigned size) { return realloc(p, size); }
void os_free(void* p) { free(p); }
int os_vprintf(const char* format, va_list args) {
    char text[MP_RV_TRACE_LIMIT];
    int size = vsnprintf(text, sizeof(text), format, args);
    if (size > 0) mp_rv_trace(text, (uint32_t)size < sizeof(text) ? (uint32_t)size : sizeof(text) - 1);
    return size;
}
int os_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = os_vprintf(format, args);
    va_end(args);
    return result;
}
uint64 os_time_get_boot_us(void) {
    volatile uint32_t* clock = (volatile uint32_t*)0x200bff8;
    uint32_t hi, lo;
    do {
        hi = clock[1];
        lo = clock[0];
    } while (hi != clock[1]);
    return (((uint64)hi << 32) | lo) / 10;
}
uint64 os_time_thread_cputime_us(void) { return os_time_get_boot_us(); }
korp_tid os_self_thread(void) { return 1; }
extern uint8_t __stack_bottom[];
uint8* os_thread_get_stack_boundary(void) { return __stack_bottom; }
void os_thread_jit_write_protect_np(bool enabled) { (void)enabled; }
int os_mutex_init(korp_mutex* p) {
    *p = 0;
    return 0;
}
int os_mutex_destroy(korp_mutex* p) {
    (void)p;
    return 0;
}
int os_mutex_lock(korp_mutex* p) {
    (void)p;
    return 0;
}
int os_mutex_unlock(korp_mutex* p) {
    (void)p;
    return 0;
}
void* os_mmap(void* hint, size_t size, int prot, int flags, os_file_handle file) {
    (void)hint;
    (void)prot;
    (void)file;
    if (flags & MMAP_MAP_FIXED) return NULL;
    return calloc(1, size);
}
void os_munmap(void* address, size_t size) {
    (void)size;
    free(address);
}
int os_mprotect(void* address, size_t size, int prot) {
    (void)address;
    (void)size;
    (void)prot;
    return 0;
}
void* os_mremap(void* address, size_t old_size, size_t new_size) { return os_mremap_slow(address, old_size, new_size); }
void os_dcache_flush(void) { __asm__ volatile("fence rw,rw" ::: "memory"); }
void os_icache_flush(void* start, size_t size) {
    (void)start;
    (void)size;
    __asm__ volatile("fence.i" ::: "memory");
}
extern uint8_t __heap_start[], __heap_end[];
void* _sbrk(ptrdiff_t increment) {
    static uint8_t* end;
    if (!end) end = __heap_start;
    if (increment < 0 || (size_t)increment > (size_t)(__heap_end - end)) {
        errno = ENOMEM;
        return (void*)-1;
    }
    uint8_t* old = end;
    end += increment;
    return old;
}
int _write(int fd, const void* bytes, size_t size) {
    (void)fd;
    mp_rv_trace(bytes, size);
    return size;
}
int _read(int fd, void* bytes, size_t size) {
    (void)fd;
    (void)bytes;
    (void)size;
    return 0;
}
int _close(int fd) {
    (void)fd;
    return -1;
}
int _fstat(int fd, struct stat* info) {
    (void)fd;
    info->st_mode = S_IFCHR;
    return 0;
}
int _isatty(int fd) {
    (void)fd;
    return 1;
}
int _lseek(int fd, int off, int whence) {
    (void)fd;
    (void)off;
    (void)whence;
    return -1;
}
int _getpid(void) { return 1; }
int _kill(int pid, int sig) {
    (void)pid;
    (void)sig;
    return -1;
}
void _exit(int status) { mp_rv_exit(status); }
