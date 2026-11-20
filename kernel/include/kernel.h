/* Core kernel helpers: logging, panics, common macros. */
#pragma once

#include "types.h"

#define OS_NAME "Sonar"
#define OS_VERSION "0.1.0"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define __noreturn __attribute__((noreturn))
#define __unused __attribute__((unused))
#define __printf(a, b) __attribute__((format(printf, a, b)))
#define READ_ONCE(x) (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))

#define KiB (1024ULL)
#define MiB (1024ULL * 1024)
#define GiB (1024ULL * 1024 * 1024)
#define PAGE_SIZE 4096UL
#define PAGE_SHIFT 12

/* Console / logging */
int printk(const char *fmt, ...) __printf(1, 2);
int vprintk(const char *fmt, va_list ap);
void console_write(const char *s, size_t len);
void console_puts(const char *s);
__noreturn void panic(const char *fmt, ...) __printf(1, 2);

int snprintf(char *buf, size_t size, const char *fmt, ...) __printf(3, 4);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#define assert(c)                                                              \
    do {                                                                       \
        if (unlikely(!(c)))                                                    \
            panic("assertion failed: %s (%s:%d)", #c, __FILE__, __LINE__);     \
    } while (0)

/* Linker-provided symbols */
extern char __image_start[], __image_end[];
extern char __text_start[], __text_end[];
extern char __bss_start[], __bss_end[];

/* Boot information gathered during early boot. */
struct boot_info {
    paddr_t image_base;  /* where the bootloader placed us */
    paddr_t image_end;
    paddr_t dtb;
    u64 dtb_size;
    int boot_el;         /* exception level we were entered at */
};
extern struct boot_info boot_info;

void kmain(void) __noreturn;
