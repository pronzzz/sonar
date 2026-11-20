/*
 * ARM Generic Interrupt Controller driver: GICv3 (system-register CPU
 * interface + redistributors) and GICv2 (memory-mapped CPU interface).
 * All interrupts are Group 1, level-triggered by default, routed to the
 * boot CPU.
 */
#include "arch.h"
#include "fdt.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "mmio.h"
#include "smp.h"

#define GICD_CTLR 0x000
#define GICD_TYPER 0x004
#define GICD_IGROUPR 0x080
#define GICD_ISENABLER 0x100
#define GICD_ICENABLER 0x180
#define GICD_ICPENDR 0x280
#define GICD_ICACTIVER 0x380
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR 0x800
#define GICD_ICFGR 0xc00
#define GICD_SGIR 0xf00
#define GICD_IROUTER 0x6000
#define GICD_CTLR_RWP (1U << 31)

#define GICR_TYPER 0x008
#define GICR_WAKER 0x014
#define GICR_SGI 0x10000
#define GICR_CTLR_RWP (1U << 3)

#define GICC_CTLR 0x00
#define GICC_PMR 0x04
#define GICC_BPR 0x08
#define GICC_IAR 0x0c
#define GICC_EOIR 0x10

#define ICC_PMR_EL1 S3_0_C4_C6_0
#define ICC_IAR1_EL1 S3_0_C12_C12_0
#define ICC_EOIR1_EL1 S3_0_C12_C12_1
#define ICC_BPR1_EL1 S3_0_C12_C12_3
#define ICC_CTLR_EL1 S3_0_C12_C12_4
#define ICC_SRE_EL1 S3_0_C12_C12_5
#define ICC_IGRPEN1_EL1 S3_0_C12_C12_7
#define ICC_SGI1R_EL1 S3_0_C12_C11_5

#define MAX_IRQS 1024
#define DEFAULT_PRIO 0xa0

static int version;
static uintptr_t gicd, gicc;
static uintptr_t gicr_base[4];
static u64 gicr_size[4];
static int gicr_regions;
static u64 gicr_stride;
static uintptr_t cpu_rd[MAX_CPUS];
static int nr_irqs;

static struct {
    irq_handler_t fn;
    void *ctx;
    u64 count;
} handlers[MAX_IRQS];

const char *gic_name(void) { return version == 3 ? "GICv3" : version == 2 ? "GICv2" : "none"; }

static void gicd_wait_rwp(void)
{
    if (version == 3)
        while (mmio_read32(gicd + GICD_CTLR) & GICD_CTLR_RWP)
            ;
}

static u32 mpidr_to_aff(u64 mpidr)
{
    return (u32)((mpidr >> 32 & 0xff) << 24 | (mpidr & 0xffffff));
}

static uintptr_t find_redistributor(u64 mpidr)
{
    u32 want = mpidr_to_aff(mpidr);
    for (int r = 0; r < gicr_regions; r++) {
        uintptr_t rd = gicr_base[r];
        uintptr_t end = rd + gicr_size[r];
        while (rd < end) {
            u64 typer = mmio_read64(rd + GICR_TYPER);
            if ((u32)(typer >> 32) == want)
                return rd;
            if (typer & (1 << 4)) /* Last */
                break;
            u64 stride = gicr_stride ? gicr_stride : ((typer & (1 << 1)) ? 0x40000 : 0x20000);
            rd += stride;
        }
    }
    return 0;
}

void gic_init(void)
{
    static const char *const v2_compat[] = { "arm,gic-400", "arm,cortex-a15-gic",
                                             "arm,cortex-a9-gic", "arm,cortex-a7-gic" };
    int node = fdt_find_compatible(-1, "arm,gic-v3");
    if (node >= 0) {
        version = 3;
    } else {
        for (size_t i = 0; i < ARRAY_SIZE(v2_compat) && node < 0; i++)
            node = fdt_find_compatible(-1, v2_compat[i]);
        if (node < 0)
            panic("gic: no supported interrupt controller in device tree");
        version = 2;
    }
    u64 addr, size;
    if (!fdt_reg(node, 0, &addr, &size))
        panic("gic: missing distributor registers");
    gicd = (uintptr_t)ioremap(addr, size ? size : 0x10000);

    if (version == 3) {
        u32 nregions = 1;
        fdt_prop_u32(node, "#redistributor-regions", &nregions);
        if (nregions > 4)
            nregions = 4;
        for (u32 i = 0; i < nregions; i++) {
            if (!fdt_reg(node, (int)i + 1, &addr, &size))
                break;
            gicr_base[i] = (uintptr_t)ioremap(addr, size);
            gicr_size[i] = size;
            gicr_regions++;
        }
        u32 stride32 = 0;
        if (fdt_prop_u32(node, "redistributor-stride", &stride32))
            gicr_stride = stride32;
        if (!gicr_regions)
            panic("gic: missing redistributor registers");
        /* Make sure EL1 uses the system-register interface. */
        write_sysreg(ICC_SRE_EL1, read_sysreg(ICC_SRE_EL1) | 1);
        isb();
        if (!(read_sysreg(ICC_SRE_EL1) & 1))
            panic("gic: cannot enable GICv3 system register interface");
    } else {
        if (!fdt_reg(node, 1, &addr, &size))
            panic("gic: missing CPU interface registers");
        gicc = (uintptr_t)ioremap(addr, size ? size : 0x2000);
    }

    nr_irqs = (int)(32 * ((mmio_read32(gicd + GICD_TYPER) & 0x1f) + 1));
    if (nr_irqs > MAX_IRQS)
        nr_irqs = MAX_IRQS;

    /* Distributor: disable, configure all SPIs, re-enable. */
    mmio_write32(gicd + GICD_CTLR, 0);
    gicd_wait_rwp();
    for (int i = 32; i < nr_irqs; i += 32) {
        mmio_write32(gicd + GICD_ICENABLER + i / 8, 0xffffffff);
        mmio_write32(gicd + GICD_ICPENDR + i / 8, 0xffffffff);
        mmio_write32(gicd + GICD_ICACTIVER + i / 8, 0xffffffff);
        /* GICv3: Group 1. GICv2: keep reset grouping so that CTLR bit 0
         * enables the right group in both secure and non-secure views. */
        if (version == 3)
            mmio_write32(gicd + GICD_IGROUPR + i / 8, 0xffffffff);
    }
    for (int i = 32; i < nr_irqs; i += 16)
        mmio_write32(gicd + GICD_ICFGR + i / 4, 0); /* level-triggered */
    for (int i = 32; i < nr_irqs; i += 4)
        mmio_write32(gicd + GICD_IPRIORITYR + (u32)i,
                     DEFAULT_PRIO * 0x01010101U);
    gicd_wait_rwp();
    if (version == 3) {
        u64 aff = cpu_mpidr();
        u64 route = (aff & 0xffffff) | ((aff >> 32 & 0xff) << 32);
        for (int i = 32; i < nr_irqs; i++)
            mmio_write64(gicd + GICD_IROUTER + 8 * (u32)i, route);
        mmio_write32(gicd + GICD_CTLR, (1 << 4) | (1 << 1) | (1 << 0)); /* ARE | G1A | G1 */
    } else {
        for (int i = 32; i < nr_irqs; i += 4)
            mmio_write32(gicd + GICD_ITARGETSR + (u32)i, 0x01010101); /* CPU0 */
        mmio_write32(gicd + GICD_CTLR, 1);
    }
    gicd_wait_rwp();
    gic_init_cpu();
}

void gic_init_cpu(void)
{
    int cpu = smp_cpu_id();
    if (version == 3) {
        write_sysreg(ICC_SRE_EL1, read_sysreg(ICC_SRE_EL1) | 1);
        isb();
        uintptr_t rd = find_redistributor(cpu_mpidr());
        if (!rd)
            panic("gic: no redistributor for cpu %d", cpu);
        cpu_rd[cpu] = rd;
        /* Wake the redistributor. */
        u32 waker = mmio_read32(rd + GICR_WAKER);
        mmio_write32(rd + GICR_WAKER, waker & ~(1U << 1));
        while (mmio_read32(rd + GICR_WAKER) & (1U << 2))
            ;
        uintptr_t sgi = rd + GICR_SGI;
        mmio_write32(sgi + GICD_ICENABLER, 0xffffffff);
        mmio_write32(sgi + GICD_ICPENDR, 0xffffffff);
        mmio_write32(sgi + GICD_IGROUPR, 0xffffffff);
        for (int i = 0; i < 32; i += 4)
            mmio_write32(sgi + GICD_IPRIORITYR + (u32)i, DEFAULT_PRIO * 0x01010101U);
        while (mmio_read32(rd) & GICR_CTLR_RWP)
            ;
        write_sysreg(ICC_PMR_EL1, 0xff);
        write_sysreg(ICC_BPR1_EL1, 0);
        write_sysreg(ICC_CTLR_EL1, 0); /* EOImode 0: EOI also deactivates */
        write_sysreg(ICC_IGRPEN1_EL1, 1);
        isb();
    } else {
        mmio_write32(gicd + GICD_ICENABLER, 0xffffffff);
        for (int i = 0; i < 32; i += 4)
            mmio_write32(gicd + GICD_IPRIORITYR + (u32)i, DEFAULT_PRIO * 0x01010101U);
        mmio_write32(gicc + GICC_PMR, 0xff);
        mmio_write32(gicc + GICC_BPR, 0);
        mmio_write32(gicc + GICC_CTLR, 1);
    }
}

void irq_register(int irq, irq_handler_t fn, void *ctx)
{
    if (irq < 0 || irq >= MAX_IRQS)
        return;
    handlers[irq].ctx = ctx;
    handlers[irq].fn = fn;
}

u64 irq_count(int irq) { return (irq >= 0 && irq < MAX_IRQS) ? handlers[irq].count : 0; }

static uintptr_t enable_base(int irq)
{
    if (version == 3 && irq < 32)
        return cpu_rd[smp_cpu_id()] + GICR_SGI;
    return gicd;
}

void irq_unmask(int irq)
{
    if (irq < 0 || irq >= nr_irqs)
        return;
    uintptr_t b = enable_base(irq);
    mmio_write32(b + GICD_ISENABLER + (u32)(irq / 32) * 4 * (irq >= 32),
                 1U << (irq % 32));
}

void irq_mask(int irq)
{
    if (irq < 0 || irq >= nr_irqs)
        return;
    uintptr_t b = enable_base(irq);
    mmio_write32(b + GICD_ICENABLER + (u32)(irq / 32) * 4 * (irq >= 32),
                 1U << (irq % 32));
    gicd_wait_rwp();
}

void irq_set_edge(int irq, bool edge)
{
    if (irq < 16 || irq >= nr_irqs)
        return;
    uintptr_t b = enable_base(irq);
    uintptr_t reg = b + GICD_ICFGR + (u32)(irq / 16) * 4 * (irq >= 32) + 4 * (irq >= 16 && irq < 32);
    u32 v = mmio_read32(reg);
    u32 bit = 2U * (u32)(irq % 16) + 1;
    v = edge ? (v | (1U << bit)) : (v & ~(1U << bit));
    mmio_write32(reg, v);
}

void gic_send_sgi(int sgi, u64 mpidr)
{
    if (version == 3) {
        u64 v = (u64)(1U << (mpidr & 0xf)) | ((mpidr >> 8 & 0xff) << 16) |
                ((u64)(sgi & 0xf) << 24) | ((mpidr >> 16 & 0xff) << 32) |
                ((mpidr >> 32 & 0xff) << 48) | ((u64)((mpidr & 0xff) >> 4) << 44);
        dsb(ishst);
        write_sysreg(ICC_SGI1R_EL1, v);
        isb();
    } else {
        dsb(ishst);
        mmio_write32(gicd + GICD_SGIR, (1U << (16 + (mpidr & 7))) | (u32)(sgi & 0xf));
    }
}

/* Acknowledge and end every pending interrupt without running handlers
 * (secondary CPUs, which only take the wake-up SGI, with IRQs masked). */
void gic_ack_pending(void)
{
    for (int n = 0; n < 64; n++) {
        u32 iar = version == 3 ? (u32)read_sysreg(ICC_IAR1_EL1) : mmio_read32(gicc + GICC_IAR);
        u32 irq = version == 3 ? (iar & 0xffffff) : (iar & 0x3ff);
        if (irq >= 1020)
            break;
        if (version == 3) {
            write_sysreg(ICC_EOIR1_EL1, iar);
            isb();
        } else {
            mmio_write32(gicc + GICC_EOIR, iar);
        }
    }
}

void gic_handle_irq(struct trap_frame *tf)
{
    (void)tf;
    for (;;) {
        u32 iar = version == 3 ? (u32)read_sysreg(ICC_IAR1_EL1) : mmio_read32(gicc + GICC_IAR);
        u32 irq = iar & 0xffffff;
        if (version == 2)
            irq = iar & 0x3ff;
        if (irq >= 1020)
            break;
        if (irq < MAX_IRQS) {
            handlers[irq].count++;
            if (handlers[irq].fn)
                handlers[irq].fn((int)irq, handlers[irq].ctx);
        }
        if (version == 3) {
            write_sysreg(ICC_EOIR1_EL1, iar);
            isb();
        } else {
            mmio_write32(gicc + GICC_EOIR, iar);
        }
    }
}
