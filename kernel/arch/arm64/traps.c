/* Exception dispatch: interrupts go to the GIC layer, everything else is a
 * kernel fault reported with a register dump and a frame-pointer backtrace. */
#include "arch.h"
#include "kernel.h"
#include "smp.h"

void gic_handle_irq(struct trap_frame *tf);

static const char *const kind_names[16] = {
    "sync (SP0)", "irq (SP0)", "fiq (SP0)", "serror (SP0)",
    "sync", "irq", "fiq", "serror",
    "sync (EL0)", "irq (EL0)", "fiq (EL0)", "serror (EL0)",
    "sync (A32)", "irq (A32)", "fiq (A32)", "serror (A32)",
};

static const char *esr_class(u32 ec)
{
    switch (ec) {
    case 0x00: return "unknown reason";
    case 0x01: return "WFI/WFE trap";
    case 0x07: return "FP/SIMD access trap";
    case 0x0e: return "illegal execution state";
    case 0x15: return "SVC (AArch64)";
    case 0x18: return "MSR/MRS trap";
    case 0x20: return "instruction abort (lower EL)";
    case 0x21: return "instruction abort";
    case 0x22: return "PC alignment fault";
    case 0x24: return "data abort (lower EL)";
    case 0x25: return "data abort";
    case 0x26: return "SP alignment fault";
    case 0x2c: return "FP exception";
    case 0x2f: return "SError";
    case 0x3c: return "BRK instruction";
    default: return "other";
    }
}

static bool addr_in_text(u64 a)
{
    return a >= (u64)(uintptr_t)__text_start && a < (u64)(uintptr_t)__text_end;
}

void backtrace_from(u64 fp)
{
    printk("backtrace (offsets from image base %#llx):\n", (unsigned long long)boot_info.image_base);
    for (int i = 0; i < 16 && fp && IS_ALIGNED(fp, 16); i++) {
        if (fp < boot_info.image_base || fp > boot_info.image_end + 64 * MiB)
            break;
        u64 lr = ((u64 *)(uintptr_t)fp)[1];
        if (!addr_in_text(lr))
            break;
        printk("  #%d  +%#llx\n", i, (unsigned long long)(lr - boot_info.image_base));
        fp = ((u64 *)(uintptr_t)fp)[0];
    }
}

static __noreturn void fatal(struct trap_frame *tf, int kind)
{
    u32 ec = (u32)(tf->esr >> 26) & 0x3f;
    printk("\n*** KERNEL EXCEPTION on cpu %d: %s - %s\n", smp_cpu_id(), kind_names[kind & 15],
           esr_class(ec));
    printk("ESR=%#llx FAR=%#llx ELR=%#llx (+%#llx) SPSR=%#llx\n", (unsigned long long)tf->esr,
           (unsigned long long)tf->far, (unsigned long long)tf->elr,
           (unsigned long long)(tf->elr - boot_info.image_base), (unsigned long long)tf->spsr);
    for (int i = 0; i < 30; i += 2)
        printk("x%-2d=%016llx  x%-2d=%016llx\n", i, (unsigned long long)tf->x[i], i + 1,
               (unsigned long long)tf->x[i + 1]);
    printk("x30=%016llx\n", (unsigned long long)tf->x[30]);
    backtrace_from(tf->x[29]);
    panic("unhandled exception");
}

void trap_dispatch(struct trap_frame *tf, u64 kind)
{
    switch (kind) {
    case 5:
    case 9:
        gic_handle_irq(tf);
        return;
    default:
        fatal(tf, (int)kind);
    }
}
