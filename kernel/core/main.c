/* Kernel main: bring up subsystems in dependency order, then run the shell. */
#include "ai.h"
#include "arch.h"
#include "config.h"
#include "console.h"
#include "efi.h"
#include "fdt.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "net.h"
#include "shell.h"
#include "smp.h"
#include "ui.h"
#include "update.h"

void devices_init(void);
void fs_boot(void);

static void banner(void)
{
    printk("\n\x1b[1;36m"
           "   ____\n"
           "  / __/__  ___  ___ _____\n"
           " _\\ \\/ _ \\/ _ \\/ _ `/ __/\n"
           "/___/\\___/_//_/\\_,_/_/   \x1b[0m %s %s (aarch64)\n"
           "        an operating system with DeepSeek built in\n\n",
           OS_NAME, OS_VERSION);
}

void kmain(void)
{
    write_sysreg(tpidr_el1, 0); /* boot CPU is cpu 0 */
    pl011_init();
    banner();
    printk("boot: loaded at %#llx, entered at EL%d, device tree at %#llx (%llu bytes)\n",
           (unsigned long long)boot_info.image_base, boot_info.boot_el,
           (unsigned long long)boot_info.dtb, (unsigned long long)boot_info.dtb_size);
    if (efi_booted()) {
        printk("boot: UEFI firmware: %s, UEFI %u.%u%s\n", efi_handoff.firmware,
               efi_handoff.uefi_rev >> 16, (efi_handoff.uefi_rev & 0xffff) / 10,
               efi_handoff.dtb_from_file ? "; device tree from \\EFI\\sonar\\board.dtb" : "");
        if (efi_handoff.dropped_ranges)
            printk("boot: %u memory ranges beyond the first %d were not used\n",
                   efi_handoff.dropped_ranges, EFI_MAX_RANGES);
    }
    const char *model = fdt_prop_str(0, "model");
    if (model)
        printk("boot: machine: %s\n", model);

    pmm_init();
    printk("mm: %llu MiB RAM, %llu MiB free\n", (unsigned long long)(pmm_total_bytes() / MiB),
           (unsigned long long)(pmm_free_bytes() / MiB));

    gic_init();
    timer_init();
    rtc_init();
    pl011_init_irq();
    irq_enable();
    printk("irq: %s, timer %llu Hz\n", gic_name(), (unsigned long long)timer_freq());

    int pv = psci_init();
    if (pv >= 0) {
        printk("psci: version %d.%d\n", pv >> 16, pv & 0xffff);
        smp_init();
    }
    printk("smp: %d of %d CPUs online\n", smp_num_cpus(), smp_num_present());

    devices_init();
    fs_boot();
    config_load();
    net_boot();
    update_boot(); /* confirms a newly installed kernel; update.auto checks */
    ui_boot(); /* the desktop first, so first-time setup runs in its Terminal */
    ai_boot();
    shell_run();
}
