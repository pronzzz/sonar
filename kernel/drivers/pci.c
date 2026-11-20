/*
 * PCI Express host support: configuration space through ECAM (the
 * "pci-host-ecam-generic" device tree binding, which the UEFI loader also
 * builds from ACPI's MCFG), enumeration through bridges, BAR sizing, and
 * MSI-X delivered through a GICv2m MSI frame.
 *
 * BARs are used as the firmware assigned them (UEFI's PCI bus driver
 * programs them before the OS starts); Sonar does not reassign resources.
 *
 * References: PCI Express Base Specification (configuration header, BARs,
 * capability list, MSI-X capability and table); Arm GICv2m ("Server Base
 * System Architecture" appendix): MSI_TYPER at 0x008, MSI_SETSPI_NS at 0x040.
 */
#include "pci.h"
#include "fdt.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "mmio.h"
#include "string.h"

#define MAX_PCI 64

static struct pci_dev devs[MAX_PCI];
static int ndevs;

struct ecam {
    u64 phys;
    uintptr_t virt;
    u32 bus0, bus1;
    /* memory windows from "ranges", for BARs the firmware left unassigned */
    u64 mem32, mem32_end, mem64, mem64_end;
};
static struct ecam hosts[4];
static int nhosts;

u8 pci_read8(struct pci_dev *d, u32 off) { return mmio_read8(d->cfg + off); }
u16 pci_read16(struct pci_dev *d, u32 off) { return mmio_read16(d->cfg + off); }
u32 pci_read32(struct pci_dev *d, u32 off) { return mmio_read32(d->cfg + off); }
void pci_write16(struct pci_dev *d, u32 off, u16 v) { mmio_write16(d->cfg + off, v); }
void pci_write32(struct pci_dev *d, u32 off, u32 v) { mmio_write32(d->cfg + off, v); }

int pci_count(void) { return ndevs; }
struct pci_dev *pci_get(int i) { return i >= 0 && i < ndevs ? &devs[i] : NULL; }

/* ---- MSI through GICv2m frames ---- */
struct v2m {
    u64 phys;
    u32 spi_base, spi_count, next;
};
static struct v2m frames[4];
static int nframes;

bool pci_have_msi(void) { return nframes > 0; }

static void v2m_init(void)
{
    for (int n = fdt_find_compatible(-1, "arm,gic-v2m-frame"); n >= 0 && nframes < 4;
         n = fdt_find_compatible(n, "arm,gic-v2m-frame")) {
        u64 addr, size;
        if (!fdt_reg(n, 0, &addr, &size))
            continue;
        uintptr_t va = (uintptr_t)ioremap(addr, 0x1000);
        u32 base = 0, count = 0;
        if (!fdt_prop_u32(n, "arm,msi-base-spi", &base) || !fdt_prop_u32(n, "arm,msi-num-spis", &count)) {
            u32 typer = mmio_read32(va + 0x008); /* MSI_TYPER */
            base = (typer >> 16) & 0x3ff;
            count = typer & 0x3ff;
        }
        if (!count)
            continue;
        frames[nframes++] = (struct v2m){ addr, base, count, 0 };
        printk("pci: MSI frame at %#llx, SPIs %u-%u\n", (unsigned long long)addr, base,
               base + count - 1);
    }
}

static int msi_alloc(u64 *doorbell)
{
    for (int i = 0; i < nframes; i++)
        if (frames[i].next < frames[i].spi_count) {
            *doorbell = frames[i].phys + 0x040; /* MSI_SETSPI_NS */
            return (int)(frames[i].spi_base + frames[i].next++);
        }
    return -1;
}

/* ---- capabilities ---- */
u8 pci_find_cap(struct pci_dev *d, u8 id, u8 after)
{
    if (!(pci_read16(d, 0x06) & 0x10)) /* status: capability list */
        return 0;
    u8 p = after ? pci_read8(d, after + 1) : (u8)(pci_read8(d, 0x34) & 0xfc);
    for (int guard = 0; p && guard < 48; guard++) {
        if (pci_read8(d, p) == id)
            return p;
        p = (u8)(pci_read8(d, p + 1) & 0xfc);
    }
    return 0;
}

void pci_enable(struct pci_dev *d)
{
    u16 cmd = pci_read16(d, 0x04);
    cmd |= 0x2 | 0x4;    /* memory space, bus master */
    cmd |= 0x400;        /* INTx disabled: Sonar uses MSI-X */
    pci_write16(d, 0x04, cmd);
}

uintptr_t pci_map_bar(struct pci_dev *d, int i)
{
    if (i < 0 || i > 5 || !d->bar[i] || !d->bar_size[i])
        return 0;
    return (uintptr_t)ioremap(d->bar[i], d->bar_size[i]);
}

int pci_msix_setup(struct pci_dev *d, int n, irq_handler_t fn, void **ctxs)
{
    u8 cap = pci_find_cap(d, PCI_CAP_MSIX, 0);
    if (!cap || !nframes)
        return 0;
    u16 ctl = pci_read16(d, cap + 2);
    int size = (ctl & 0x7ff) + 1;
    if (n > size)
        n = size;
    u32 tbl = pci_read32(d, cap + 4);
    int bir = tbl & 7;
    uintptr_t bar = pci_map_bar(d, bir);
    if (!bar)
        return 0;
    uintptr_t table = bar + (tbl & ~7u);
    /* Function mask while programming, then enable. */
    pci_write16(d, cap + 2, (u16)(ctl | 0x4000 | 0x8000));
    int done = 0;
    for (int v = 0; v < n; v++) {
        u64 doorbell;
        int spi = msi_alloc(&doorbell);
        if (spi < 0)
            break;
        uintptr_t e = table + 16 * (uintptr_t)v;
        mmio_write32(e + 0, (u32)doorbell);
        mmio_write32(e + 4, (u32)(doorbell >> 32));
        mmio_write32(e + 8, (u32)spi);
        mmio_write32(e + 12, 0); /* unmasked */
        irq_set_edge(spi, true);
        irq_register(spi, fn, ctxs ? ctxs[v] : NULL);
        irq_unmask(spi);
        done++;
    }
    pci_write16(d, cap + 2, (u16)((ctl | 0x8000) & ~0x4000));
    pci_write16(d, 0x04, pci_read16(d, 0x04) | 0x400); /* no INTx */
    return done;
}

/* ---- enumeration ---- */
static u64 alloc_window(u64 *next, u64 end, u64 size)
{
    u64 a = ALIGN_UP(*next, size);
    if (!*next || a + size > end)
        return 0;
    *next = a + size;
    return a;
}

static void size_bars(struct ecam *h, struct pci_dev *d)
{
    u8 type = pci_read8(d, 0x0e) & 0x7f;
    int nbars = type == 0 ? 6 : type == 1 ? 2 : 0;
    u16 cmd = pci_read16(d, 0x04);
    pci_write16(d, 0x04, cmd & ~0x3); /* no decoding while sizing */
    for (int i = 0; i < nbars; i++) {
        u32 off = 0x10 + 4 * (u32)i;
        u32 lo = pci_read32(d, off);
        if (lo & 1) /* I/O space: not used on arm64 */
            continue;
        bool is64 = ((lo >> 1) & 3) == 2;
        u64 addr = lo & ~0xfULL;
        pci_write32(d, off, 0xffffffff);
        u64 mask = pci_read32(d, off) & ~0xfULL;
        pci_write32(d, off, lo);
        if (is64 && i + 1 < nbars) {
            u32 hi = pci_read32(d, off + 4);
            addr |= (u64)hi << 32;
            pci_write32(d, off + 4, 0xffffffff);
            mask |= (u64)pci_read32(d, off + 4) << 32;
            pci_write32(d, off + 4, hi);
        } else {
            mask |= 0xffffffff00000000ULL;
        }
        if (mask && mask != ~0xfULL) {
            u64 size = ~mask + 1;
            if (!addr) { /* unassigned (no firmware): place it in a window */
                bool pref64 = is64;
                addr = pref64 ? alloc_window(&h->mem64, h->mem64_end, size) : 0;
                if (!addr)
                    addr = alloc_window(&h->mem32, h->mem32_end, size);
                if (addr) {
                    pci_write32(d, off, (u32)addr | (lo & 0xf));
                    if (is64)
                        pci_write32(d, off + 4, (u32)(addr >> 32));
                }
            }
            if (addr) {
                d->bar[i] = addr;
                d->bar_size[i] = size;
            }
        }
        if (is64)
            i++;
    }
    pci_write16(d, 0x04, cmd);
}

static void scan_bus(struct ecam *h, u32 bus, int depth)
{
    if (bus < h->bus0 || bus > h->bus1 || depth > 8)
        return;
    for (u32 dev = 0; dev < 32; dev++) {
        for (u32 fn = 0; fn < 8; fn++) {
            uintptr_t cfg = h->virt + ((bus - h->bus0) << 20 | dev << 15 | fn << 12);
            u16 vendor = mmio_read16(cfg);
            if (vendor == 0xffff) {
                if (fn == 0)
                    break;
                continue;
            }
            u8 hdr = mmio_read8(cfg + 0x0e);
            if (ndevs < MAX_PCI) {
                struct pci_dev *d = &devs[ndevs++];
                memset(d, 0, sizeof(*d));
                d->bus = (u8)bus;
                d->dev = (u8)dev;
                d->fn = (u8)fn;
                d->cfg = cfg;
                d->vendor = vendor;
                d->device = mmio_read16(cfg + 2);
                d->revision = mmio_read8(cfg + 8);
                d->prog_if = mmio_read8(cfg + 9);
                d->subclass = mmio_read8(cfg + 10);
                d->class_code = mmio_read8(cfg + 11);
                snprintf(d->name, sizeof(d->name), "%02x:%02x.%u", bus, dev, fn);
                size_bars(h, d);
            }
            if ((hdr & 0x7f) == 1) { /* bridge: follow its secondary bus */
                u32 sec = mmio_read8(cfg + 0x19);
                if (sec > bus)
                    scan_bus(h, sec, depth + 1);
            }
            if (fn == 0 && !(hdr & 0x80))
                break; /* single-function device */
        }
    }
}

static const char *class_name(const struct pci_dev *d)
{
    switch (d->class_code << 8 | d->subclass) {
    case 0x0100: return "SCSI";
    case 0x0106: return "SATA";
    case 0x0108: return "NVMe";
    case 0x0200: return "Ethernet";
    case 0x0280: return "network";
    case 0x0300: return "display";
    case 0x0380: return "display";
    case 0x0403: return "audio";
    case 0x0600: return "host bridge";
    case 0x0604: return "PCI bridge";
    case 0x0780: return "communication";
    case 0x0c03: return d->prog_if == 0x30 ? "USB (XHCI)" : "USB";
    case 0x00ff:
    case 0xff00: return "other";
    }
    return d->vendor == 0x1af4 ? "virtio" : "device";
}

void pci_init(void)
{
    for (int n = fdt_find_compatible(-1, "pci-host-ecam-generic"); n >= 0 && nhosts < 4;
         n = fdt_find_compatible(n, "pci-host-ecam-generic")) {
        u64 addr, size;
        if (!fdt_is_enabled(n) || !fdt_reg(n, 0, &addr, &size))
            continue;
        u32 b0 = 0, b1 = (u32)(size >> 20) - 1;
        int len;
        const u8 *br = fdt_prop(n, "bus-range", &len);
        if (br && len >= 8) {
            b0 = be32(br);
            b1 = be32(br + 4);
        }
        struct ecam *h = &hosts[nhosts];
        h->phys = addr;
        h->virt = (uintptr_t)ioremap(addr, size);
        h->bus0 = b0;
        h->bus1 = b1;
        if (!h->virt)
            continue;
        /* ranges: <pci-addr (3 cells)> <cpu-addr (2)> <size (2)>; the space
         * code in bits 24-25 of the first cell: 2 = 32-bit memory, 3 = 64-bit. */
        const u8 *rg = fdt_prop(n, "ranges", &len);
        for (int o = 0; rg && o + 28 <= len; o += 28) {
            u32 sp = (be32(rg + o) >> 24) & 3;
            u64 cpu = be64(rg + o + 12), sz = be64(rg + o + 20);
            if (sp == 2 && !h->mem32) {
                h->mem32 = cpu;
                h->mem32_end = cpu + sz;
            } else if (sp == 3 && !h->mem64) {
                h->mem64 = cpu;
                h->mem64_end = cpu + sz;
            }
        }
        nhosts++;
        printk("pci: ECAM at %#llx, buses %u-%u\n", (unsigned long long)addr, b0, b1);
        scan_bus(h, b0, 0);
    }
    if (!nhosts)
        return;
    v2m_init();
    for (int i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i];
        printk("  %s %04x:%04x %s\n", d->name, d->vendor, d->device, class_name(d));
    }
}

/* lspci */
#include "shell.h"
static int cmd_lspci(int argc, char **argv)
{
    if (!ndevs) {
        printk("No PCI devices.\n");
        return 0;
    }
    for (int i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i];
        printk("%s  %04x:%04x  class %02x%02x%02x  %s\n", d->name, d->vendor, d->device,
               d->class_code, d->subclass, d->prog_if, class_name(d));
        for (int b = 0; b < 6; b++)
            if (d->bar[b])
                printk("        BAR%d %#llx (%llu %s)\n", b, (unsigned long long)d->bar[b],
                       (unsigned long long)(d->bar_size[b] >= 1024 ? d->bar_size[b] >> 10
                                                                    : d->bar_size[b]),
                       d->bar_size[b] >= 1024 ? "KiB" : "bytes");
    }
    return 0;
}
SHELL_COMMAND(lspci, "lspci", "lspci", "list PCI devices", cmd_lspci);
