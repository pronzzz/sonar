/* ARM generic timer (virtual timer, CNTV) providing a periodic tick and a
 * monotonic clock, plus the PL031 real-time clock for wall-clock time. */
#include "arch.h"
#include "fdt.h"
#include "efi.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "mmio.h"

static u64 freq;
static u64 tick_interval;
static u64 next_deadline;
static volatile u64 ticks;
static int timer_irq = 27;

static void (*tick_hooks[8])(void);
static int ntick_hooks;

void timer_add_tick_hook(void (*fn)(void))
{
    if (ntick_hooks < 8)
        tick_hooks[ntick_hooks++] = fn;
}

static void timer_handler(int irq, void *ctx)
{
    u64 now = read_sysreg(cntvct_el0);
    /* Catch up without drifting if ticks were missed. */
    do {
        next_deadline += tick_interval;
        ticks++;
    } while (next_deadline <= now);
    write_sysreg(cntv_cval_el0, next_deadline);
    isb();
    for (int i = 0; i < ntick_hooks; i++)
        tick_hooks[i]();
}

void timer_init(void)
{
    freq = read_sysreg(cntfrq_el0);
    if (!freq)
        panic("timer: CNTFRQ_EL0 is zero (firmware did not program it)");
    int node = fdt_find_compatible(-1, "arm,armv8-timer");
    if (node >= 0) {
        int irq = fdt_irq(node, 2); /* virtual timer */
        if (irq > 0)
            timer_irq = irq;
    }
    tick_interval = freq / TIMER_HZ;
    irq_register(timer_irq, timer_handler, NULL);
    timer_init_cpu();
}

void timer_init_cpu(void)
{
    next_deadline = read_sysreg(cntvct_el0) + tick_interval;
    write_sysreg(cntv_cval_el0, next_deadline);
    write_sysreg(cntv_ctl_el0, 1); /* enable, unmasked */
    isb();
    irq_unmask(timer_irq);
}

u64 time_ns(void)
{
    u64 c = timer_count();
    u64 sec = c / freq, rem = c % freq;
    return sec * 1000000000ULL + rem * 1000000000ULL / freq;
}

u64 time_ms(void) { return time_ns() / 1000000ULL; }
u64 time_ticks(void) { return ticks; }

void delay_us(u64 us)
{
    u64 end = timer_count() + us * freq / 1000000ULL;
    while (timer_count() < end)
        cpu_relax();
}

void delay_ms(u64 ms) { delay_us(ms * 1000); }

/* ---- PL031 RTC ---- */
static uintptr_t rtc_base;
static u64 rtc_at_boot;
static u64 ns_at_boot;

static bool rtc_ok;

void rtc_init(void)
{
    int node = fdt_find_compatible(-1, "arm,pl031");
    u64 addr;
    if (node >= 0 && fdt_reg(node, 0, &addr, NULL)) {
        rtc_base = (uintptr_t)ioremap(addr, 0x1000);
        rtc_at_boot = mmio_read32(rtc_base + 0x00); /* RTCDR */
        ns_at_boot = time_ns();
        rtc_ok = true;
    } else if (efi_booted() && efi_handoff.boot_epoch) {
        /* No RTC device: use the time UEFI reported just before boot,
         * advanced by the generic counter since then. */
        u64 now = read_sysreg(cntvct_el0), hz = read_sysreg(cntfrq_el0);
        rtc_at_boot = efi_handoff.boot_epoch + (now - efi_handoff.boot_counter) / (hz ? hz : 1);
        ns_at_boot = time_ns();
        rtc_ok = true;
    }
}

u64 rtc_epoch(void)
{
    if (!rtc_ok)
        return 0;
    return rtc_at_boot + (time_ns() - ns_at_boot) / 1000000000ULL;
}
