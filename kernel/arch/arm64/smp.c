/*
 * SMP bring-up via PSCI CPU_ON and a simple fork-join work pool.
 *
 * Secondary CPUs run with interrupts masked and wait for parallel_for()
 * jobs: they spin for about a millisecond after each job (so a run of
 * jobs, like a model's layers, starts at once), then sleep in WFI until
 * the boot CPU sends them a wake-up SGI. WFE alone does not sleep under
 * every hypervisor (Apple's returns at once), so idle CPUs would burn host
 * time. The boot CPU owns all devices and interrupts.
 *
 * Each CPU gets a contiguous rank (0 = boot CPU) from an atomic counter
 * when it actually comes online. A job is published as one atomic word
 * (generation << 8 | nworkers); ranks 1..nworkers-1 participate, so the
 * set of participants is fixed by a single load and the completion count
 * is exact even if a CPU comes online late.
 */
#include "arch.h"
#include "fdt.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "smp.h"
#include "string.h"

struct cpu_boot {
    u64 stack_top; /* offsets used by secondary_entry in boot.S */
    u64 mair;
    u64 tcr;
    u64 ttbr0;
    u64 sctlr;
    u64 entry;
    u64 mpidr;
};

extern char secondary_entry[];
extern char exception_vectors[];

static u64 cpu_mpidrs[MAX_CPUS];
static int present = 1;
static int online = 1;
static int workers_limit = MAX_CPUS;

static struct {
    par_fn fn;
    void *ctx;
    int n;
    int grain;
    int next;    /* next index to hand out (atomic) */
    int pending; /* helper CPUs still working (atomic) */
} job;
static u64 job_word; /* generation << 8 | nworkers (atomic) */
static u64 job_gen;
static int sleeping[MAX_CPUS]; /* rank is in (or about to enter) WFI */
#define WAKE_SGI 1

u64 spin_lock_irqsave(spinlock_t *l)
{
    u64 f = irq_save();
    spin_lock(l);
    return f;
}

void spin_unlock_irqrestore(spinlock_t *l, u64 flags)
{
    spin_unlock(l);
    irq_restore(flags);
}

int smp_cpu_id(void) { return (int)read_sysreg(tpidr_el1); }
int smp_num_cpus(void) { return __atomic_load_n(&online, __ATOMIC_ACQUIRE); }
int smp_num_present(void) { return present; }
u64 smp_cpu_mpidr(int cpu) { return cpu_mpidrs[cpu]; }

void smp_set_workers(int n) { workers_limit = CLAMP(n, 1, MAX_CPUS); }
int smp_get_workers(void) { return MIN(workers_limit, smp_num_cpus()); }

static void run_chunks(void)
{
    for (;;) {
        int start = __atomic_fetch_add(&job.next, job.grain, __ATOMIC_RELAXED);
        if (start >= job.n)
            break;
        int end = start + job.grain;
        if (end > job.n)
            end = job.n;
        job.fn(job.ctx, start, end);
    }
}

static __noreturn void worker_loop(int rank)
{
    u64 seen = 0;
    const u64 spin_ticks = timer_freq() / 1000; /* about 1 ms */
    for (;;) {
        u64 w;
        u64 t0 = timer_count();
        while ((w = __atomic_load_n(&job_word, __ATOMIC_ACQUIRE)) == seen) {
            if (timer_count() - t0 < spin_ticks) {
                cpu_relax();
                continue;
            }
            /* Dekker-style handshake with parallel_for: publish "sleeping",
             * then look at the job word again; the SGI stays pending (and
             * wakes WFI with interrupts masked) if it is sent in between. */
            __atomic_store_n(&sleeping[rank], 1, __ATOMIC_SEQ_CST);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (__atomic_load_n(&job_word, __ATOMIC_SEQ_CST) == seen)
                wfi();
            __atomic_store_n(&sleeping[rank], 0, __ATOMIC_SEQ_CST);
            gic_ack_pending();
            t0 = timer_count();
        }
        seen = w;
        if (rank < (int)(w & 0xff)) {
            run_chunks();
            __atomic_fetch_sub(&job.pending, 1, __ATOMIC_RELEASE);
            dsb(ish);
            sev();
        }
    }
}

void parallel_for(int n, int grain, par_fn fn, void *ctx)
{
    if (n <= 0)
        return;
    if (grain < 1)
        grain = 1;
    int nw = smp_get_workers();
    if (nw <= 1 || n <= grain) {
        fn(ctx, 0, n);
        return;
    }
    job.fn = fn;
    job.ctx = ctx;
    job.n = n;
    job.grain = grain;
    job.next = 0;
    job.pending = nw - 1;
    job_gen++;
    __atomic_store_n(&job_word, job_gen << 8 | (u64)nw, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (int r = 1; r < nw; r++)
        if (__atomic_load_n(&sleeping[r], __ATOMIC_SEQ_CST))
            gic_send_sgi(WAKE_SGI, cpu_mpidrs[r]);
    dsb(ish);
    sev();
    run_chunks();
    while (__atomic_load_n(&job.pending, __ATOMIC_ACQUIRE) != 0)
        wfe();
}

static __noreturn void secondary_main(struct cpu_boot *b)
{
    write_sysreg(vbar_el1, (u64)(uintptr_t)exception_vectors);
    isb();
    int rank = __atomic_fetch_add(&online, 1, __ATOMIC_ACQ_REL);
    if (rank >= MAX_CPUS) {
        for (;;)
            wfe();
    }
    write_sysreg(tpidr_el1, (u64)rank);
    cpu_mpidrs[rank] = b->mpidr;
    /* Interrupts stay masked here: the GIC interface is set up only so the
     * wake-up SGI can end WFI (a pending interrupt wakes it while masked). */
    gic_init_cpu();
    irq_unmask(WAKE_SGI);
    dsb(ish);
    sev();
    worker_loop(rank);
}

void smp_init(void)
{
    cpu_mpidrs[0] = cpu_mpidr();
    int cpus = fdt_path("/cpus");
    if (cpus < 0)
        return;
    for (int n = fdt_first_child(cpus); n >= 0; n = fdt_next_sibling(n)) {
        const char *type = fdt_prop_str(n, "device_type");
        if (!type || strcmp(type, "cpu") != 0 || !fdt_is_enabled(n))
            continue;
        u64 mpidr;
        if (!fdt_reg(n, 0, &mpidr, NULL))
            continue;
        if (mpidr == cpu_mpidrs[0])
            continue;
        if (present >= MAX_CPUS)
            break;
        const char *em = fdt_prop_str(n, "enable-method");
        if (em && strcmp(em, "psci") != 0) {
            printk("smp: cpu %#llx uses enable-method '%s' (not supported yet)\n",
                   (unsigned long long)mpidr, em);
            continue;
        }
        present++;
        struct cpu_boot *b = kzalloc(sizeof(*b));
        void *stack = page_alloc(16); /* 64 KiB */
        if (!b || !stack)
            panic("smp: out of memory");
        b->stack_top = (u64)(uintptr_t)stack + 16 * PAGE_SIZE;
        b->mair = mmu_reg_mair();
        b->tcr = mmu_reg_tcr();
        b->ttbr0 = mmu_reg_ttbr0();
        b->sctlr = mmu_reg_sctlr();
        b->entry = (u64)(uintptr_t)secondary_main;
        b->mpidr = mpidr;
        /* The secondary reads this with its MMU and caches off. */
        dcache_clean_range((uintptr_t)b, (uintptr_t)b + sizeof(*b));
        int want = smp_num_cpus() + 1;
        int r = psci_cpu_on(mpidr, (paddr_t)(uintptr_t)secondary_entry, (u64)(uintptr_t)b);
        if (r != 0) {
            printk("smp: PSCI CPU_ON for cpu %#llx failed (%d)\n", (unsigned long long)mpidr, r);
            continue;
        }
        u64 deadline = time_ms() + 1000;
        while (smp_num_cpus() < want && time_ms() < deadline)
            cpu_relax();
        if (smp_num_cpus() < want)
            printk("smp: cpu %#llx is slow to come online\n", (unsigned long long)mpidr);
    }
}
