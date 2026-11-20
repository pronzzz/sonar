/* Symmetric multiprocessing: CPU bring-up, spinlocks, parallel work. */
#pragma once

#include "types.h"

#define MAX_CPUS 64

typedef struct {
    volatile u32 locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *l)
{
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
            __asm__ volatile("yield");
    }
}

static inline bool spin_trylock(spinlock_t *l)
{
    return __atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE) == 0;
}

static inline void spin_unlock(spinlock_t *l) { __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE); }

u64 spin_lock_irqsave(spinlock_t *l);
void spin_unlock_irqrestore(spinlock_t *l, u64 flags);

int smp_cpu_id(void);
int smp_num_cpus(void);   /* CPUs online */
int smp_num_present(void);
void smp_init(void);      /* start secondary CPUs */
u64 smp_cpu_mpidr(int cpu);

/*
 * Run fn(ctx, start, end) over [0, n) split into chunks across all online
 * CPUs (the caller participates). Blocks until every chunk has finished.
 * Must be called from the boot CPU only (not reentrant).
 */
typedef void (*par_fn)(void *ctx, int start, int end);
void parallel_for(int n, int grain, par_fn fn, void *ctx);
void smp_set_workers(int n); /* limit CPUs used by parallel_for */
int smp_get_workers(void);
