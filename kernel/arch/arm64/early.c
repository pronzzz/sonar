/*
 * First C code, running with the MMU off on the boot CPU.
 * Finds RAM and the console in the device tree, builds the identity map,
 * enables the MMU and caches, then enters kmain().
 */
#include "arch.h"
#include "efi.h"
#include "fdt.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"

struct boot_info boot_info;
extern char exception_vectors[];

/* Pre-MMU console: PL011 data register at the stdout-path address. */
static uintptr_t early_uart;

static void early_puts(const char *s)
{
    if (!early_uart)
        return;
    for (; *s; s++) {
        if (*s == '\n') {
            while (*(volatile u32 *)(early_uart + 0x18) & (1 << 5))
                ;
            *(volatile u32 *)early_uart = '\r';
        }
        while (*(volatile u32 *)(early_uart + 0x18) & (1 << 5))
            ;
        *(volatile u32 *)early_uart = (u8)*s;
    }
}

static __noreturn void early_halt(const char *msg)
{
    early_puts("\nearly boot failed: ");
    early_puts(msg);
    early_puts("\n");
    for (;;)
        wfe();
}

/* Locate the console UART (PL011) from /chosen/stdout-path or by compatible. */
int fdt_console_node(void)
{
    int chosen = fdt_path("/chosen");
    if (chosen >= 0) {
        const char *path = fdt_prop_str(chosen, "stdout-path");
        if (path) {
            char buf[128];
            size_t n = 0;
            while (path[n] && path[n] != ':' && n < sizeof(buf) - 1) {
                buf[n] = path[n];
                n++;
            }
            buf[n] = 0;
            int node = buf[0] == '/' ? fdt_path(buf) : -1;
            if (node < 0) {
                /* stdout-path may be an alias */
                int aliases = fdt_path("/aliases");
                const char *real = aliases >= 0 ? fdt_prop_str(aliases, buf) : NULL;
                if (real)
                    node = fdt_path(real);
            }
            if (node >= 0 && fdt_is_compatible(node, "arm,pl011"))
                return node;
        }
    }
    int n = -1;
    while ((n = fdt_find_compatible(n, "arm,pl011")) >= 0) {
        if (fdt_is_enabled(n))
            return n;
    }
    return -1;
}

static void map_ram(void)
{
    if (efi_booted()) {
        /* UEFI: the firmware memory map is authoritative (the device tree
         * may list RAM the firmware still uses, or none at all). */
        for (u32 i = 0; i < efi_handoff.nranges; i++)
            mmu_map(efi_handoff.ranges[i].base, efi_handoff.ranges[i].size, MAP_NORMAL);
        if (!efi_handoff.nranges)
            early_halt("UEFI memory map has no usable RAM");
        for (u32 i = 0; i < efi_handoff.nfiles; i++)
            mmu_map(efi_handoff.files[i].base, efi_handoff.files[i].size, MAP_NORMAL);
        return;
    }
    int depth = 0;
    int n = 0;
    bool any = false;
    while ((n = fdt_next_node(n, &depth)) >= 0) {
        const char *type = fdt_prop_str(n, "device_type");
        if (!type || strcmp(type, "memory") != 0 || !fdt_is_enabled(n))
            continue;
        for (int i = 0;; i++) {
            u64 base, size;
            if (!fdt_reg(n, i, &base, &size))
                break;
            if (!size)
                continue;
            mmu_map(base, size, MAP_NORMAL);
            any = true;
        }
    }
    if (!any)
        early_halt("no memory nodes in device tree");
}

__noreturn void early_main(uintptr_t dtb, uintptr_t base, u64 el)
{
    boot_info.image_base = base;
    boot_info.image_end = base + (uintptr_t)(__image_end - __image_start);
    boot_info.dtb = dtb;
    boot_info.boot_el = (int)el;

    write_sysreg(vbar_el1, (u64)(uintptr_t)exception_vectors);
    isb();

    if (!fdt_init((const void *)dtb))
        early_halt("no valid device tree in x0");
    boot_info.dtb_size = fdt_total_size((const void *)dtb);

    int uart = fdt_console_node();
    u64 uart_base = 0;
    if (uart >= 0 && fdt_reg(uart, 0, &uart_base, NULL))
        early_uart = (uintptr_t)uart_base;
    early_puts("sonar: early boot\n");

    mmu_early_init();
    map_ram();
    /* The image and DTB must lie in RAM; map them explicitly in case a
     * bootloader placed them in memory the device tree does not list. */
    mmu_map(boot_info.image_base, boot_info.image_end - boot_info.image_base, MAP_NORMAL);
    mmu_map(dtb, boot_info.dtb_size, MAP_NORMAL);
    if (uart_base)
        mmu_map(uart_base, 0x1000, MAP_DEVICE);
    mmu_enable();
    early_puts("sonar: mmu on\n");
    kmain();
}
