/* Kernel implementation of the AI engine platform layer (platform.h). */
#include "platform.h"
#include "arch.h"
#include "console.h"
#include "irq.h"
#include "mm.h"
#include "smp.h"

void *ai_alloc(size_t n) { return kmalloc(n ? n : 1); }
void *ai_calloc(size_t n, size_t s) { return kcalloc(n ? n : 1, s ? s : 1); }
void *ai_realloc(void *p, size_t n) { return krealloc(p, n); }
void ai_free(void *p) { kfree(p); }

/* Model weights and caches: physically contiguous pages (identity map)
 * when a run that large is free, else chunks mapped together (vmap.c). */
void *ai_alloc_large(size_t n)
{
    void *p = page_alloc_nozero(ALIGN_UP(n, PAGE_SIZE) / PAGE_SIZE);
    return p ? p : vmap_alloc(n);
}

void ai_free_large(void *p, size_t n)
{
    if (!p || !n || vmap_free(p))
        return;
    page_free(p, ALIGN_UP(n, PAGE_SIZE) / PAGE_SIZE);
}

bool ai_large_dma_ok(const void *p) { return !vmap_contains(p); }

void ai_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
}

void ai_parallel_for(int n, int grain, ai_par_fn fn, void *ctx) { parallel_for(n, grain, fn, ctx); }
int ai_num_threads(void) { return smp_get_workers(); }
u64 ai_time_ns(void) { return time_ns(); }
bool ai_should_abort(void) { return console_interrupted(); }

/* Reading a long prompt keeps the boot CPU busy for up to a minute before
 * any text is printed. Run the console's idle work (the desktop: pointer,
 * windows, taskbar) every 25 ms meanwhile, so the screen does not freeze. */
static u64 last_yield_ms, last_busy_ms;
void ai_yield(void)
{
    if (smp_cpu_id() != 0)
        return;
    u64 now = time_ms();
    last_busy_ms = now;
    if (now - last_yield_ms < 25)
        return;
    last_yield_ms = now;
    console_run_idle();
}

bool ai_busy(void) { return last_busy_ms && time_ms() - last_busy_ms < 400; }
void ai_done(void) { last_busy_ms = 0; }

bool ai_cpu_has_dotprod(void)
{
    /* ID_AA64ISAR0_EL1.DP, bits [47:44]: 1 = SDOT/UDOT implemented. */
    return ((read_sysreg(id_aa64isar0_el1) >> 44) & 0xf) >= 1;
}
