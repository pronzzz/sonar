/*
 * Build a device tree from ACPI tables, for firmware that describes the
 * machine with ACPI only (most UEFI arm64 machines, Apple's Virtualization
 * framework). Runs in the UEFI loader, before ExitBootServices.
 *
 * Only the static tables are read; ACPI's AML (DSDT/SSDT) is not
 * interpreted. That covers what the kernel needs to start:
 *   MADT  GICv3 distributor, redistributors, ITS / MSI frames, CPU MPIDRs
 *   GTDT  generic timer interrupts
 *   FADT  PSCI conduit (HVC or SMC)
 *   MCFG  PCI Express configuration space (ECAM)
 *   SPCR  serial console (PL011 / SBSA UART)
 * Table layouts: ACPI Specification 6.5, sections 5.2.12 (MADT), 5.2.25
 * (GTDT), 5.2.9 (FADT); MCFG: PCI Firmware Specification 3.3 section 4.1.2;
 * SPCR: Microsoft's Serial Port Console Redirection Table specification.
 */
#include "acpi_fdt.h"
#include "kernel.h"
#include "string.h"

/* ---- unaligned little-endian reads (ACPI fields are packed) ---- */
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64(const u8 *p) { return rd32(p) | (u64)rd32(p + 4) << 32; }

/* ---- a minimal flattened device tree writer (devicetree spec v0.4, ch. 5) ---- */
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_END 9

static u8 *st;       /* structure block */
static u32 st_len, st_cap;
static char *strs;   /* strings block */
static u32 str_len, str_cap;
static bool overflow;

static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static void emit(const void *data, u32 len)
{
    u32 padded = (len + 3) & ~3u;
    if (st_len + padded > st_cap) {
        overflow = true;
        return;
    }
    memcpy(st + st_len, data, len);
    memset(st + st_len + len, 0, padded - len);
    st_len += padded;
}

static void emit32(u32 v)
{
    u8 b[4];
    put_be32(b, v);
    emit(b, 4);
}

static u32 str_off(const char *name)
{
    for (u32 o = 0; o < str_len; o += (u32)strlen(strs + o) + 1)
        if (!strcmp(strs + o, name))
            return o;
    u32 n = (u32)strlen(name) + 1;
    if (str_len + n > str_cap) {
        overflow = true;
        return 0;
    }
    memcpy(strs + str_len, name, n);
    str_len += n;
    return str_len - n;
}

static void begin(const char *name)
{
    emit32(FDT_BEGIN_NODE);
    emit(name, (u32)strlen(name) + 1);
}

static void end(void) { emit32(FDT_END_NODE); }

static void prop(const char *name, const void *val, u32 len)
{
    emit32(FDT_PROP);
    emit32(len);
    emit32(str_off(name));
    if (len)
        emit(val, len);
}

static void prop_str(const char *name, const char *s) { prop(name, s, (u32)strlen(s) + 1); }

static void prop_cells(const char *name, const u32 *cells, int n)
{
    u8 b[32 * 4];
    if (n > 32)
        n = 32;
    for (int i = 0; i < n; i++)
        put_be32(b + 4 * i, cells[i]);
    prop(name, b, (u32)n * 4);
}

static void prop_u32(const char *name, u32 v) { prop_cells(name, &v, 1); }

/* reg with #address-cells = #size-cells = 2 */
static void prop_reg(const u64 *pairs, int npairs)
{
    u32 c[32];
    int n = 0;
    for (int i = 0; i < npairs && n + 4 <= 32; i++) {
        c[n++] = (u32)(pairs[2 * i] >> 32);
        c[n++] = (u32)pairs[2 * i];
        c[n++] = (u32)(pairs[2 * i + 1] >> 32);
        c[n++] = (u32)pairs[2 * i + 1];
    }
    prop_cells("reg", c, n);
}

static void node_name(char *out, size_t n, const char *base, u64 addr)
{
    snprintf(out, n, "%s@%llx", base, (unsigned long long)addr);
}

/* ---- ACPI tables ---- */
#define MAX_TABLES 64
static const u8 *tables[MAX_TABLES];
static int ntables;

static bool checksum_ok(const u8 *p, u32 len)
{
    u8 s = 0;
    for (u32 i = 0; i < len; i++)
        s = (u8)(s + p[i]);
    return s == 0;
}

static const u8 *find_table(const char *sig)
{
    for (int i = 0; i < ntables; i++)
        if (!memcmp(tables[i], sig, 4))
            return tables[i];
    return NULL;
}

/* GTDT flags -> devicetree interrupt flags (1 = edge rising, 4 = level high,
 * 8 = level low); bit 0 of the ACPI flags is "edge", bit 1 "active low". */
static u32 dt_irq_flags(u32 acpi_flags)
{
    bool edge = acpi_flags & 1, low = acpi_flags & 2;
    if (edge)
        return low ? 2 : 1;
    return low ? 8 : 4;
}

static void ppi_cells(u32 *c, u32 gsiv, u32 flags)
{
    c[0] = 1; /* PPI */
    c[1] = gsiv >= 16 ? gsiv - 16 : 0;
    c[2] = dt_irq_flags(flags);
}

u64 acpi_build_fdt(u64 rsdp_addr, void *(*alloc)(u64 size), void (*log)(const char *))
{
    char line[160];
    const u8 *rsdp = (const u8 *)(uintptr_t)rsdp_addr;
    if (!rsdp || memcmp(rsdp, "RSD PTR ", 8)) {
        log("  ACPI: no valid RSDP\n");
        return 0;
    }
    const u8 *xsdt = rsdp[15] >= 2 ? (const u8 *)(uintptr_t)rd64(rsdp + 24) : NULL;
    const u8 *rsdt = (const u8 *)(uintptr_t)rd32(rsdp + 16);
    ntables = 0;
    if (xsdt && !memcmp(xsdt, "XSDT", 4)) {
        u32 n = (rd32(xsdt + 4) - 36) / 8;
        for (u32 i = 0; i < n && ntables < MAX_TABLES; i++)
            tables[ntables++] = (const u8 *)(uintptr_t)rd64(xsdt + 36 + 8 * i);
    } else if (rsdt && !memcmp(rsdt, "RSDT", 4)) {
        u32 n = (rd32(rsdt + 4) - 36) / 4;
        for (u32 i = 0; i < n && ntables < MAX_TABLES; i++)
            tables[ntables++] = (const u8 *)(uintptr_t)rd32(rsdt + 36 + 4 * i);
    } else {
        log("  ACPI: no XSDT or RSDT\n");
        return 0;
    }
    /* Drop tables with a bad checksum; list the rest. */
    int keep = 0;
    char list[4 * MAX_TABLES + MAX_TABLES + 1];
    size_t ll = 0;
    for (int i = 0; i < ntables; i++) {
        const u8 *t = tables[i];
        if (!t || !checksum_ok(t, rd32(t + 4)))
            continue;
        tables[keep++] = t;
        memcpy(list + ll, t, 4);
        ll += 4;
        list[ll++] = ' ';
    }
    ntables = keep;
    list[ll] = 0;
    snprintf(line, sizeof(line), "  ACPI tables: %s\n", list);
    log(line);

    const u8 *madt = find_table("APIC");
    if (!madt) {
        log("  ACPI: no MADT (interrupt controller table)\n");
        return 0;
    }

    st_cap = 64 << 10;
    str_cap = 4 << 10;
    st = alloc(st_cap);
    strs = alloc(str_cap);
    if (!st || !strs)
        return 0;
    st_len = str_len = 0;
    overflow = false;

    /* ---- root ---- */
    begin("");
    prop_u32("#address-cells", 2);
    prop_u32("#size-cells", 2);
    prop_u32("interrupt-parent", 1);
    prop_str("compatible", "sonar,acpi-machine");
    char model[64], oem[7], oemt[9];
    memcpy(oem, madt + 10, 6);
    oem[6] = 0;
    memcpy(oemt, madt + 16, 8);
    oemt[8] = 0;
    for (int i = 5; i >= 0 && oem[i] == ' '; i--)
        oem[i] = 0;
    for (int i = 7; i >= 0 && oemt[i] == ' '; i--)
        oemt[i] = 0;
    snprintf(model, sizeof(model), "%s %s (ACPI)", oem, oemt);
    prop_str("model", model);

    /* ---- PSCI (FADT ARM_BOOT_ARCH, offset 129) ---- */
    const u8 *fadt = find_table("FACP");
    if (fadt && rd32(fadt + 4) >= 131) {
        u16 arch = rd16(fadt + 129);
        if (arch & 1) {
            begin("psci");
            prop_str("compatible", "arm,psci-1.0");
            prop_str("method", arch & 2 ? "hvc" : "smc");
            end();
            snprintf(line, sizeof(line), "  PSCI via %s\n", arch & 2 ? "HVC" : "SMC");
            log(line);
        }
    }

    /* ---- MADT ---- */
    u32 madt_len = rd32(madt + 4);
    u64 gicd = 0, gicr[8], gicr_len[8], its[4], v2m[4];
    u32 v2m_flags[4], v2m_count[4], v2m_base[4];
    int ngicr = 0, nits = 0, nv2m = 0, ncpu = 0, gic_version = 0;
    u64 mpidr[256], gicc_rd[256];
    for (u32 o = 44; o + 2 <= madt_len;) {
        const u8 *e = madt + o;
        u8 type = e[0], len = e[1];
        if (len < 2 || o + len > madt_len)
            break;
        if (type == 0x0b && len >= 76) { /* GICC */
            u32 flags = rd32(e + 12);
            if ((flags & 1) && ncpu < 256) { /* enabled */
                mpidr[ncpu] = rd64(e + 68) & 0xff00ffffffULL;
                gicc_rd[ncpu] = rd64(e + 60);
                ncpu++;
            }
        } else if (type == 0x0c && len >= 24) { /* GICD */
            gicd = rd64(e + 8);
            gic_version = e[20];
        } else if (type == 0x0e && len >= 16 && ngicr < 8) { /* GICR range */
            gicr[ngicr] = rd64(e + 4);
            gicr_len[ngicr] = rd32(e + 12);
            ngicr++;
        } else if (type == 0x0f && len >= 20 && nits < 4) { /* ITS */
            its[nits++] = rd64(e + 8);
        } else if (type == 0x0d && len >= 24 && nv2m < 4) { /* GICv2m MSI frame */
            v2m[nv2m] = rd64(e + 8);
            v2m_flags[nv2m] = rd32(e + 16);
            v2m_count[nv2m] = rd16(e + 20);
            v2m_base[nv2m] = rd16(e + 22);
            nv2m++;
        }
        o += len;
    }
    snprintf(line, sizeof(line), "  MADT: GIC version %d, distributor %#llx, %d CPUs, %d redistributor ranges, %d ITS, %d MSI frames\n",
             gic_version, (unsigned long long)gicd, ncpu, ngicr, nits, nv2m);
    log(line);
    for (int i = 0; i < nits; i++) {
        snprintf(line, sizeof(line), "    ITS at %#llx\n", (unsigned long long)its[i]);
        log(line);
    }
    for (int i = 0; i < nv2m; i++) {
        snprintf(line, sizeof(line), "    MSI frame at %#llx: SPIs %u+%u (flags %#x)\n",
                 (unsigned long long)v2m[i], v2m_base[i], v2m_count[i], v2m_flags[i]);
        log(line);
    }
    if (!gicd || !ncpu) {
        log("  ACPI: MADT has no GIC distributor or no CPUs\n");
        return 0;
    }

    begin("cpus");
    prop_u32("#address-cells", 2);
    prop_u32("#size-cells", 0);
    for (int i = 0; i < ncpu; i++) {
        char nm[32];
        node_name(nm, sizeof(nm), "cpu", mpidr[i]);
        begin(nm);
        prop_str("device_type", "cpu");
        prop_str("compatible", "arm,armv8");
        u32 r[2] = { (u32)(mpidr[i] >> 32), (u32)mpidr[i] };
        prop_cells("reg", r, 2);
        prop_str("enable-method", "psci");
        end();
    }
    end();

    {
        char nm[32];
        node_name(nm, sizeof(nm), "interrupt-controller", gicd);
        begin(nm);
        bool v3 = gic_version >= 3 || (gic_version == 0 && (ngicr || gicc_rd[0]));
        prop_str("compatible", v3 ? "arm,gic-v3" : "arm,gic-400");
        prop_u32("#interrupt-cells", 3);
        prop("interrupt-controller", NULL, 0);
        prop_u32("phandle", 1);
        prop_u32("#address-cells", 2);
        prop_u32("#size-cells", 2);
        prop("ranges", NULL, 0);
        u64 regs[2 * 9];
        int nr = 0;
        regs[2 * nr] = gicd;
        regs[2 * nr + 1] = 0x10000;
        nr++;
        if (ngicr) {
            for (int i = 0; i < ngicr; i++, nr++) {
                regs[2 * nr] = gicr[i];
                regs[2 * nr + 1] = gicr_len[i];
            }
            prop_u32("#redistributor-regions", (u32)ngicr);
        } else if (v3) {
            /* Per-CPU redistributor addresses (GICC); assume they are
             * contiguous 128 KiB frames (GICv3 without VLPIs). */
            u64 lo = gicc_rd[0], hi = gicc_rd[0];
            for (int i = 1; i < ncpu; i++) {
                if (gicc_rd[i] < lo)
                    lo = gicc_rd[i];
                if (gicc_rd[i] > hi)
                    hi = gicc_rd[i];
            }
            regs[2 * nr] = lo;
            regs[2 * nr + 1] = hi - lo + 0x20000;
            nr++;
        }
        prop_reg(regs, nr);
        for (int i = 0; i < nits; i++) {
            char n2[40];
            node_name(n2, sizeof(n2), "msi-controller", its[i]);
            begin(n2);
            prop_str("compatible", "arm,gic-v3-its");
            prop("msi-controller", NULL, 0);
            prop_u32("#msi-cells", 1);
            u64 r[2] = { its[i], 0x20000 };
            prop_reg(r, 1);
            end();
        }
        for (int i = 0; i < nv2m; i++) {
            char n2[40];
            node_name(n2, sizeof(n2), "v2m", v2m[i]);
            begin(n2);
            prop_str("compatible", "arm,gic-v2m-frame");
            prop("msi-controller", NULL, 0);
            u64 r[2] = { v2m[i], 0x1000 };
            prop_reg(r, 1);
            if (v2m_flags[i] & 1) {
                prop_u32("arm,msi-base-spi", v2m_base[i]);
                prop_u32("arm,msi-num-spis", v2m_count[i]);
            }
            end();
        }
        end();
    }

    /* ---- generic timer (GTDT) ---- */
    const u8 *gtdt = find_table("GTDT");
    if (gtdt && rd32(gtdt + 4) >= 80) {
        u32 c[12];
        ppi_cells(c + 0, rd32(gtdt + 48), rd32(gtdt + 52)); /* secure EL1 */
        ppi_cells(c + 3, rd32(gtdt + 56), rd32(gtdt + 60)); /* non-secure EL1 */
        ppi_cells(c + 6, rd32(gtdt + 64), rd32(gtdt + 68)); /* virtual */
        ppi_cells(c + 9, rd32(gtdt + 72), rd32(gtdt + 76)); /* EL2 */
        begin("timer");
        prop_str("compatible", "arm,armv8-timer");
        prop_cells("interrupts", c, 12);
        end();
        snprintf(line, sizeof(line), "  GTDT: virtual timer INTID %u\n", rd32(gtdt + 64));
        log(line);
    } else {
        /* Architected defaults (SBSA): secure 29, non-secure 30, virtual 27, EL2 26. */
        u32 c[12] = { 1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4 };
        begin("timer");
        prop_str("compatible", "arm,armv8-timer");
        prop_cells("interrupts", c, 12);
        end();
    }

    /* ---- PCI Express (MCFG) ---- */
    const u8 *mcfg = find_table("MCFG");
    if (mcfg) {
        u32 n = (rd32(mcfg + 4) - 44) / 16;
        for (u32 i = 0; i < n && i < 8; i++) {
            const u8 *e = mcfg + 44 + 16 * i;
            u64 base = rd64(e);
            u16 seg = rd16(e + 8);
            u8 b0 = e[10], b1 = e[11];
            u64 start = base + ((u64)b0 << 20);
            char nm[32];
            node_name(nm, sizeof(nm), "pcie", start);
            begin(nm);
            prop_str("compatible", "pci-host-ecam-generic");
            prop_str("device_type", "pci");
            u64 r[2] = { start, (u64)(b1 - b0 + 1) << 20 };
            prop_reg(r, 1);
            u32 br[2] = { b0, b1 };
            prop_cells("bus-range", br, 2);
            prop_u32("linux,pci-domain", seg);
            end();
            snprintf(line, sizeof(line), "  MCFG: PCIe segment %u, buses %u-%u, ECAM at %#llx\n", seg,
                     b0, b1, (unsigned long long)base);
            log(line);
        }
    }

    /* ---- serial console (SPCR) ---- */
    const u8 *spcr = find_table("SPCR");
    if (spcr && rd32(spcr + 4) >= 58) {
        u8 itype = spcr[36], space = spcr[40];
        u64 addr = rd64(spcr + 44);
        u32 gsiv = rd32(spcr + 54);
        snprintf(line, sizeof(line), "  SPCR: interface type %u at %#llx, GSIV %u\n", itype,
                 (unsigned long long)addr, gsiv);
        log(line);
        /* 3 = ARM PL011, 0xe = ARM SBSA generic UART (PL011 subset) */
        if ((itype == 3 || itype == 0x0e) && space == 0 && addr) {
            char nm[32];
            node_name(nm, sizeof(nm), "serial", addr);
            begin("chosen");
            char path[40];
            snprintf(path, sizeof(path), "/%s", nm);
            prop_str("stdout-path", path);
            end();
            begin(nm);
            static const char compat[] = "arm,pl011\0arm,primecell";
            prop("compatible", compat, sizeof(compat));
            u64 r[2] = { addr, 0x1000 };
            prop_reg(r, 1);
            if (gsiv >= 32) {
                u32 c[3] = { 0, gsiv - 32, 4 };
                prop_cells("interrupts", c, 3);
            }
            end();
        }
    }

    end(); /* root */
    emit32(FDT_END);
    if (overflow) {
        log("  ACPI: device tree too large\n");
        return 0;
    }

    /* ---- assemble: header, empty memory reservation map, structure, strings ---- */
    u32 off_rsv = 40, off_struct = off_rsv + 16, off_strings = off_struct + st_len;
    u32 total = off_strings + str_len;
    u8 *blob = alloc(total);
    if (!blob)
        return 0;
    memset(blob, 0, off_struct);
    put_be32(blob + 0, 0xd00dfeed);
    put_be32(blob + 4, total);
    put_be32(blob + 8, off_struct);
    put_be32(blob + 12, off_strings);
    put_be32(blob + 16, off_rsv);
    put_be32(blob + 20, 17); /* version */
    put_be32(blob + 24, 16); /* last compatible version */
    put_be32(blob + 28, 0);  /* boot CPU */
    put_be32(blob + 32, str_len);
    put_be32(blob + 36, st_len);
    memcpy(blob + off_struct, st, st_len);
    memcpy(blob + off_strings, strs, str_len);
    return (u64)(uintptr_t)blob;
}
