/* Memory-mapped I/O accessors. Each access is a single instruction of the
 * exact width, which device registers require. */
#pragma once

#include "types.h"

static inline u8 mmio_read8(uintptr_t a)
{
    u8 v;
    __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
    return v;
}

static inline u16 mmio_read16(uintptr_t a)
{
    u16 v;
    __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
    return v;
}

static inline u32 mmio_read32(uintptr_t a)
{
    u32 v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
    return v;
}

static inline u64 mmio_read64(uintptr_t a)
{
    u64 v;
    __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(a) : "memory");
    return v;
}

static inline void mmio_write8(uintptr_t a, u8 v)
{
    __asm__ volatile("strb %w0, [%1]" ::"r"(v), "r"(a) : "memory");
}

static inline void mmio_write16(uintptr_t a, u16 v)
{
    __asm__ volatile("strh %w0, [%1]" ::"r"(v), "r"(a) : "memory");
}

static inline void mmio_write32(uintptr_t a, u32 v)
{
    __asm__ volatile("str %w0, [%1]" ::"r"(v), "r"(a) : "memory");
}

static inline void mmio_write64(uintptr_t a, u64 v)
{
    __asm__ volatile("str %0, [%1]" ::"r"(v), "r"(a) : "memory");
}
