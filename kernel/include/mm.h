/* Memory management: MMU mappings, physical page allocator, kernel heap. */
#pragma once

#include "types.h"

enum map_type {
    MAP_NORMAL,   /* cacheable RAM */
    MAP_DEVICE,   /* MMIO registers (Device-nGnRE) */
    MAP_NOCACHE,  /* normal non-cacheable (shared framebuffers) */
};

/* MMU (kernel/arch/arm64/mmu.c). The kernel uses an identity map: VA == PA. */
void mmu_early_init(void);
void mmu_enable(void);
bool mmu_map(paddr_t pa, u64 size, enum map_type type);
/* VA != PA mappings, for the virtual window above (kernel/mm/vmap.c). */
bool mmu_map_at(u64 va, paddr_t pa, u64 size, enum map_type type);
void mmu_unmap(u64 va, u64 size);
void *ioremap(paddr_t pa, u64 size);
u64 mmu_reg_mair(void);
u64 mmu_reg_tcr(void);
u64 mmu_reg_ttbr0(void);
u64 mmu_reg_sctlr(void);

/* Physical memory (kernel/mm/pmm.c). */
struct mem_range {
    paddr_t base;
    u64 size;
};
void pmm_init(void);
void pmm_add(paddr_t base, u64 size);
void pmm_reserve(paddr_t base, u64 size);
void *page_alloc(u64 npages);                  /* contiguous, zeroed */
void *page_alloc_aligned(u64 npages, u64 align);
void *page_alloc_nozero(u64 npages);
void page_free(void *p, u64 npages);
u64 pmm_total_bytes(void);
u64 pmm_free_bytes(void);
u64 pmm_largest_free(void);
int pmm_ram_ranges(struct mem_range *out, int max);
/* Take up to max_bytes (a multiple of align) from the largest free range,
 * aligned to align; returns the bytes taken (0 if none) and sets *pa. */
u64 pmm_take_chunk(u64 max_bytes, u64 align, paddr_t *pa);

/* Large buffers built from scattered physical chunks, mapped contiguously
 * in a virtual window above RAM (kernel/mm/vmap.c). Not usable for DMA. */
void *vmap_alloc(u64 bytes);
bool vmap_free(void *p);
bool vmap_contains(const void *p);

/* Kernel heap (kernel/mm/kmalloc.c). */
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *p, size_t size);
void kfree(void *p);
size_t kmalloc_usable(void *p);
u64 kmalloc_stats_used(void);
