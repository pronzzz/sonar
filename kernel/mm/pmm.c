/*
 * Physical memory manager: a sorted list of free physical ranges with
 * first-fit allocation and coalescing on free. Ranges make very large
 * contiguous allocations (multi-GiB model weights) cheap and exact.
 */
#include "efi.h"
#include "fdt.h"
#include "kernel.h"
#include "mm.h"
#include "smp.h"
#include "string.h"

#define MAX_FREE 512
#define MAX_RAM 32

static struct mem_range freelist[MAX_FREE];
static int nfree;
static struct mem_range ram[MAX_RAM];
static int nram;
static u64 total_bytes;
static spinlock_t lock = SPINLOCK_INIT;

static void remove_at(int i)
{
    memmove(&freelist[i], &freelist[i + 1], (size_t)(nfree - i - 1) * sizeof(freelist[0]));
    nfree--;
}

static bool insert_at(int i, paddr_t base, u64 size)
{
    if (nfree >= MAX_FREE)
        return false;
    memmove(&freelist[i + 1], &freelist[i], (size_t)(nfree - i) * sizeof(freelist[0]));
    freelist[i].base = base;
    freelist[i].size = size;
    nfree++;
    return true;
}

/* Insert a free range, merging with neighbours. Caller holds the lock. */
static void free_range_locked(paddr_t base, u64 size)
{
    if (!size)
        return;
    int i = 0;
    while (i < nfree && freelist[i].base < base)
        i++;
    /* Merge with predecessor? */
    if (i > 0 && freelist[i - 1].base + freelist[i - 1].size == base) {
        freelist[i - 1].size += size;
        if (i < nfree && freelist[i - 1].base + freelist[i - 1].size == freelist[i].base) {
            freelist[i - 1].size += freelist[i].size;
            remove_at(i);
        }
        return;
    }
    if (i < nfree && base + size == freelist[i].base) {
        freelist[i].base = base;
        freelist[i].size += size;
        return;
    }
    if (!insert_at(i, base, size))
        printk("pmm: free list full, leaking %llu bytes\n", (unsigned long long)size);
}

/* Remove [base, base+size) from the free list (may split ranges). */
static void take_range_locked(paddr_t base, u64 size)
{
    paddr_t end = base + size;
    for (int i = 0; i < nfree; i++) {
        paddr_t rb = freelist[i].base, re = rb + freelist[i].size;
        if (re <= base || rb >= end)
            continue;
        if (base <= rb && end >= re) {
            remove_at(i);
            i--;
        } else if (base <= rb) {
            freelist[i].base = end;
            freelist[i].size = re - end;
        } else if (end >= re) {
            freelist[i].size = base - rb;
        } else {
            freelist[i].size = base - rb;
            if (!insert_at(i + 1, end, re - end))
                printk("pmm: free list full while splitting\n");
            i++;
        }
    }
}

void pmm_add(paddr_t base, u64 size)
{
    paddr_t b = ALIGN_UP(base, PAGE_SIZE);
    paddr_t e = ALIGN_DOWN(base + size, PAGE_SIZE);
    if (e <= b)
        return;
    u64 f = spin_lock_irqsave(&lock);
    free_range_locked(b, e - b);
    spin_unlock_irqrestore(&lock, f);
}

void pmm_reserve(paddr_t base, u64 size)
{
    paddr_t b = ALIGN_DOWN(base, PAGE_SIZE);
    paddr_t e = ALIGN_UP(base + size, PAGE_SIZE);
    u64 f = spin_lock_irqsave(&lock);
    take_range_locked(b, e - b);
    spin_unlock_irqrestore(&lock, f);
}

void pmm_init(void)
{
    int depth = 0;
    int n = 0;
    if (efi_booted()) {
        /* UEFI firmware memory map (kernel/arch/arm64/efi.c) */
        for (u32 i = 0; i < efi_handoff.nranges; i++) {
            u64 base = efi_handoff.ranges[i].base, size = efi_handoff.ranges[i].size;
            if (nram < MAX_RAM) {
                ram[nram].base = base;
                ram[nram].size = size;
                nram++;
            }
            total_bytes += size;
            pmm_add(base, size);
        }
        n = -1; /* skip the device tree's memory nodes */
    }
    while (n >= 0 && (n = fdt_next_node(n, &depth)) >= 0) {
        const char *type = fdt_prop_str(n, "device_type");
        if (!type || strcmp(type, "memory") != 0 || !fdt_is_enabled(n))
            continue;
        for (int i = 0;; i++) {
            u64 base, size;
            if (!fdt_reg(n, i, &base, &size))
                break;
            if (!size)
                continue;
            if (nram < MAX_RAM) {
                ram[nram].base = base;
                ram[nram].size = size;
                nram++;
            }
            total_bytes += size;
            pmm_add(base, size);
        }
    }
    /* Firmware reservations. */
    int nrsv = fdt_num_mem_rsv();
    for (int i = 0; i < nrsv; i++) {
        u64 a, s;
        if (fdt_get_mem_rsv(i, &a, &s))
            pmm_reserve(a, s);
    }
    int rm = fdt_path("/reserved-memory");
    if (rm >= 0) {
        for (int c = fdt_first_child(rm); c >= 0; c = fdt_next_sibling(c)) {
            for (int i = 0;; i++) {
                u64 a, s;
                if (!fdt_reg(c, i, &a, &s))
                    break;
                pmm_reserve(a, s);
            }
        }
    }
    /* The kernel image and the device tree. */
    pmm_reserve(boot_info.image_base, boot_info.image_end - boot_info.image_base);
    pmm_reserve(boot_info.dtb, boot_info.dtb_size);
    /* Files the UEFI loader read into memory, and its framebuffer. */
    if (efi_booted()) {
        for (u32 i = 0; i < efi_handoff.nfiles; i++)
            pmm_reserve(efi_handoff.files[i].base, efi_handoff.files[i].size);
        if (efi_handoff.fb_base)
            pmm_reserve(efi_handoff.fb_base, efi_handoff.fb_size);
    }
}

void *page_alloc_aligned(u64 npages, u64 align)
{
    if (!npages)
        return NULL;
    if (align < PAGE_SIZE)
        align = PAGE_SIZE;
    u64 bytes = npages * PAGE_SIZE;
    void *result = NULL;
    u64 f = spin_lock_irqsave(&lock);
    for (int i = 0; i < nfree; i++) {
        paddr_t start = ALIGN_UP(freelist[i].base, align);
        paddr_t end = freelist[i].base + freelist[i].size;
        if (start >= end || end - start < bytes)
            continue;
        take_range_locked(start, bytes);
        result = (void *)(uintptr_t)start;
        break;
    }
    spin_unlock_irqrestore(&lock, f);
    return result;
}

void *page_alloc_nozero(u64 npages) { return page_alloc_aligned(npages, PAGE_SIZE); }

void *page_alloc(u64 npages)
{
    void *p = page_alloc_aligned(npages, PAGE_SIZE);
    if (p)
        memset(p, 0, npages * PAGE_SIZE);
    return p;
}

void page_free(void *p, u64 npages)
{
    if (!p || !npages)
        return;
    u64 f = spin_lock_irqsave(&lock);
    free_range_locked((paddr_t)(uintptr_t)p, npages * PAGE_SIZE);
    spin_unlock_irqrestore(&lock, f);
}

u64 pmm_total_bytes(void) { return total_bytes; }

u64 pmm_free_bytes(void)
{
    u64 sum = 0;
    u64 f = spin_lock_irqsave(&lock);
    for (int i = 0; i < nfree; i++)
        sum += freelist[i].size;
    spin_unlock_irqrestore(&lock, f);
    return sum;
}

u64 pmm_largest_free(void)
{
    u64 best = 0;
    u64 f = spin_lock_irqsave(&lock);
    for (int i = 0; i < nfree; i++)
        best = MAX(best, freelist[i].size);
    spin_unlock_irqrestore(&lock, f);
    return best;
}

int pmm_ram_ranges(struct mem_range *out, int max)
{
    int n = MIN(max, nram);
    for (int i = 0; i < n; i++)
        out[i] = ram[i];
    return n;
}

u64 pmm_take_chunk(u64 max_bytes, u64 align, paddr_t *pa)
{
    u64 got = 0;
    u64 f = spin_lock_irqsave(&lock);
    int best = -1;
    u64 best_len = 0;
    for (int i = 0; i < nfree; i++) {
        paddr_t start = ALIGN_UP(freelist[i].base, align);
        paddr_t end = ALIGN_DOWN(freelist[i].base + freelist[i].size, align);
        if (end > start && end - start > best_len) {
            best = i;
            best_len = end - start;
        }
    }
    if (best >= 0) {
        paddr_t start = ALIGN_UP(freelist[best].base, align);
        got = MIN(best_len, max_bytes);
        take_range_locked(start, got);
        *pa = start;
    }
    spin_unlock_irqrestore(&lock, f);
    return got;
}
