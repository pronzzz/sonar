/* AArch64 CPU helpers: system registers, barriers, interrupts. */
#pragma once

#include "types.h"

#define read_sysreg(r)                                                         \
    ({                                                                         \
        u64 __v;                                                               \
        __asm__ volatile("mrs %0, " #r : "=r"(__v));                           \
        __v;                                                                   \
    })

#define write_sysreg(r, v)                                                     \
    do {                                                                       \
        u64 __v = (u64)(v);                                                    \
        __asm__ volatile("msr " #r ", %0" ::"r"(__v) : "memory");              \
    } while (0)

#define dsb(opt) __asm__ volatile("dsb " #opt ::: "memory")
#define dmb(opt) __asm__ volatile("dmb " #opt ::: "memory")

static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline void wfi(void) { __asm__ volatile("wfi" ::: "memory"); }
static inline void wfe(void) { __asm__ volatile("wfe" ::: "memory"); }
static inline void sev(void) { __asm__ volatile("sev" ::: "memory"); }
static inline void cpu_relax(void) { __asm__ volatile("yield" ::: "memory"); }

static inline void irq_enable(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void irq_disable(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }

static inline u64 irq_save(void)
{
    u64 flags = read_sysreg(daif);
    irq_disable();
    return flags;
}

static inline void irq_restore(u64 flags) { write_sysreg(daif, flags); }

static inline u64 cpu_mpidr(void) { return read_sysreg(mpidr_el1) & 0xff00ffffffULL; }

/* Counter / timer */
static inline u64 timer_freq(void) { return read_sysreg(cntfrq_el0); }
static inline u64 timer_count(void)
{
    isb();
    return read_sysreg(cntvct_el0);
}

/* Data cache maintenance (by virtual address, to the point of coherency). */
void dcache_clean_range(uintptr_t start, uintptr_t end);
void dcache_inval_range(uintptr_t start, uintptr_t end);
void dcache_clean_inval_range(uintptr_t start, uintptr_t end);

/* Exception frame saved by the vector stubs (see vectors.S). */
struct trap_frame {
    u64 x[31];
    u64 elr;
    u64 spsr;
    u64 esr;
    u64 far;
    u64 sp_el0;
    __uint128_t q[32];
    u64 fpcr;
    u64 fpsr;
};

/* PSCI firmware interface */
int psci_init(void);
int psci_cpu_on(u64 mpidr, paddr_t entry, u64 context);
__attribute__((noreturn)) void psci_system_off(void);
__attribute__((noreturn)) void psci_system_reset(void);
