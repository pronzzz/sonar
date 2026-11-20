/*
 * Stage-1 EL1 translation tables: 4 KiB granule, 48-bit identity map.
 *
 * Early boot (MMU off) maps every RAM bank from the device tree as Normal
 * write-back memory and the console UART as Device memory, then turns on
 * the MMU and caches. Later, drivers map their MMIO with ioremap().
 *
 * Table pages come from a static pool until the page allocator is up.
 */
#include "arch.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"

#define DESC_VALID (1ULL << 0)
#define DESC_TABLE (1ULL << 1) /* at L0-L2: next-level table; at L3: page */
#define DESC_AF (1ULL << 10)
#define DESC_SH_INNER (3ULL << 8)
#define DESC_PXN (1ULL << 53)
#define DESC_UXN (1ULL << 54)
#define DESC_ATTR(i) ((u64)(i) << 2)
#define DESC_ADDR_MASK 0x0000fffffffff000ULL

/* MAIR indices */
#define MT_DEVICE 0  /* Device-nGnRE */
#define MT_NORMAL 1  /* Normal, inner/outer write-back, read/write allocate */
#define MT_NOCACHE 2 /* Normal, non-cacheable */
#define MAIR_VALUE (0x04ULL << (8 * MT_DEVICE) | 0xffULL << (8 * MT_NORMAL) | 0x44ULL << (8 * MT_NOCACHE))

#define POOL_PAGES 256
static u64 table_pool[POOL_PAGES][512] __aligned(4096);
static int pool_used;
static u64 *root_table;
static bool mmu_on;
static u64 reg_tcr, reg_sctlr;

static u64 *alloc_table(void)
{
    u64 *t;
    if (pool_used < POOL_PAGES) {
        t = table_pool[pool_used++];
    } else {
        t = page_alloc(1);
        if (!t)
            panic("mmu: out of memory for page tables");
    }
    memset(t, 0, PAGE_SIZE);
    return t;
}

static u64 leaf_attrs(enum map_type type)
{
    switch (type) {
    case MAP_DEVICE:
        return DESC_ATTR(MT_DEVICE) | DESC_AF | DESC_PXN | DESC_UXN;
    case MAP_NOCACHE:
        return DESC_ATTR(MT_NOCACHE) | DESC_AF | DESC_SH_INNER | DESC_PXN | DESC_UXN;
    case MAP_NORMAL:
    default:
        return DESC_ATTR(MT_NORMAL) | DESC_AF | DESC_SH_INNER | DESC_UXN;
    }
}

static void tlb_flush_all(void)
{
    if (mmu_on) {
        dsb(ishst);
        __asm__ volatile("tlbi vmalle1is" ::: "memory");
        dsb(ish);
        isb();
    }
}

/*
 * Map [va, va+size) to [pa, ...). Uses 1 GiB / 2 MiB blocks where both
 * addresses are aligned. Regions already covered by an existing mapping are
 * left alone.
 */
bool mmu_map_at(u64 va, paddr_t pa, u64 size, enum map_type type)
{
    if (!root_table)
        root_table = alloc_table();
    u64 end = ALIGN_UP(va + size, PAGE_SIZE);
    u64 addr = ALIGN_DOWN(va, PAGE_SIZE);
    u64 delta = ALIGN_DOWN(pa, PAGE_SIZE) - addr; /* pa = addr + delta (mod 2^64) */
    if (end > (1ULL << 48))
        return false;
    u64 attrs = leaf_attrs(type);
    while (addr < end) {
        u64 *table = root_table;
        int level;
        u64 phys = addr + delta;
        for (level = 0; level <= 3; level++) {
            int shift = 39 - 9 * level;
            u64 block = 1ULL << shift;
            u64 idx = (addr >> shift) & 511;
            u64 desc = table[idx];
            bool can_block = (level == 1 || level == 2) && IS_ALIGNED(addr, block) &&
                             IS_ALIGNED(phys, block) && end - addr >= block;
            if (level == 3) {
                if (!(desc & DESC_VALID))
                    table[idx] = phys | attrs | DESC_VALID | DESC_TABLE;
                addr += block;
                break;
            }
            if (desc & DESC_VALID) {
                if (!(desc & DESC_TABLE)) {
                    /* Existing block mapping covers this address already. */
                    addr = ALIGN_DOWN(addr, block) + block;
                    break;
                }
                table = (u64 *)(uintptr_t)(desc & DESC_ADDR_MASK);
                continue;
            }
            if (can_block) {
                table[idx] = phys | attrs | DESC_VALID;
                addr += block;
                break;
            }
            u64 *next = alloc_table();
            table[idx] = (u64)(uintptr_t)next | DESC_VALID | DESC_TABLE;
            table = next;
        }
    }
    tlb_flush_all();
    return true;
}

bool mmu_map(paddr_t pa, u64 size, enum map_type type) { return mmu_map_at(pa, pa, size, type); }

/* Remove the mappings of [va, va+size), which must cover whole blocks.
 * Table pages stay allocated (they are reused by the next mapping). */
void mmu_unmap(u64 va, u64 size)
{
    u64 end = ALIGN_UP(va + size, PAGE_SIZE);
    u64 addr = ALIGN_DOWN(va, PAGE_SIZE);
    while (root_table && addr < end) {
        u64 *table = root_table;
        for (int level = 0; level <= 3; level++) {
            int shift = 39 - 9 * level;
            u64 block = 1ULL << shift;
            u64 idx = (addr >> shift) & 511;
            u64 desc = table[idx];
            if (!(desc & DESC_VALID)) {
                addr = ALIGN_DOWN(addr, block) + block;
                break;
            }
            if (level == 3 || !(desc & DESC_TABLE)) {
                table[idx] = 0;
                addr = ALIGN_DOWN(addr, block) + block;
                break;
            }
            table = (u64 *)(uintptr_t)(desc & DESC_ADDR_MASK);
        }
    }
    tlb_flush_all();
}

void *ioremap(paddr_t pa, u64 size)
{
    if (!mmu_map(pa, size, MAP_DEVICE))
        return NULL;
    return (void *)(uintptr_t)pa;
}

void mmu_early_init(void)
{
    u64 mmfr0 = read_sysreg(id_aa64mmfr0_el1);
    u64 parange = mmfr0 & 0xf;
    if (parange > 5)
        parange = 5; /* 48-bit output addresses (no LPA2) */
    if ((mmfr0 >> 28 & 0xf) == 0xf)
        panic("mmu: CPU does not support the 4 KiB translation granule");
    reg_tcr = 16                 /* T0SZ: 48-bit VA */
              | (1ULL << 8)      /* IRGN0: write-back, write-allocate */
              | (1ULL << 10)     /* ORGN0: write-back, write-allocate */
              | (3ULL << 12)     /* SH0: inner shareable */
              | (0ULL << 14)     /* TG0: 4 KiB */
              | (1ULL << 23)     /* EPD1: no TTBR1 walks */
              | (parange << 32); /* IPS */
    reg_sctlr = (1ULL << 0)      /* M: MMU */
                | (1ULL << 2)    /* C: data cache */
                | (1ULL << 12)   /* I: instruction cache */
                | (1ULL << 11) | (1ULL << 20) | (1ULL << 22) | (1ULL << 23) |
                (1ULL << 28) | (1ULL << 29); /* RES1 */
    if (!root_table)
        root_table = alloc_table();
}

void mmu_enable(void)
{
    /* Tables were written with caches off; drop any stale lines first. */
    dsb(sy);
    dcache_inval_range((uintptr_t)table_pool, (uintptr_t)table_pool + sizeof(table_pool));
    write_sysreg(mair_el1, MAIR_VALUE);
    write_sysreg(tcr_el1, reg_tcr);
    write_sysreg(ttbr0_el1, (u64)(uintptr_t)root_table);
    isb();
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    dsb(nsh);
    isb();
    write_sysreg(sctlr_el1, reg_sctlr);
    isb();
    mmu_on = true;
}

u64 mmu_reg_mair(void) { return MAIR_VALUE; }
u64 mmu_reg_tcr(void) { return reg_tcr; }
u64 mmu_reg_ttbr0(void) { return (u64)(uintptr_t)root_table; }
u64 mmu_reg_sctlr(void) { return reg_sctlr; }

/* ---- cache maintenance ---- */

static u64 dcache_line(void)
{
    u64 ctr = read_sysreg(ctr_el0);
    return 4ULL << ((ctr >> 16) & 0xf);
}

void dcache_clean_range(uintptr_t start, uintptr_t end)
{
    u64 line = dcache_line();
    for (uintptr_t a = start & ~(line - 1); a < end; a += line)
        __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
    dsb(sy);
}

void dcache_inval_range(uintptr_t start, uintptr_t end)
{
    u64 line = dcache_line();
    for (uintptr_t a = start & ~(line - 1); a < end; a += line)
        __asm__ volatile("dc ivac, %0" ::"r"(a) : "memory");
    dsb(sy);
}

void dcache_clean_inval_range(uintptr_t start, uintptr_t end)
{
    u64 line = dcache_line();
    for (uintptr_t a = start & ~(line - 1); a < end; a += line)
        __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
    dsb(sy);
}
