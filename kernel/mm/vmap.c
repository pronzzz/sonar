/*
 * Large buffers from scattered physical memory.
 *
 * Firmware memory maps can split RAM into pieces (Apple's Virtualization
 * framework keeps 128 KiB at the 4 GiB line, leaving 2.3 + 4.3 GiB in a
 * 6.5 GiB guest), so a model's weights may not fit in one physical run.
 * vmap_alloc takes 2 MiB-aligned chunks, largest first, and maps them one
 * after another in a virtual window far above RAM. The kernel otherwise
 * uses VA == PA, so these buffers must not be handed to DMA.
 */
#include "kernel.h"
#include "mm.h"
#include "smp.h"

#define VMAP_BASE (1ULL << 47)
#define VMAP_END (VMAP_BASE + (1ULL << 46))
#define CHUNK_ALIGN (2ULL << 20)
#define MAX_REGIONS 16
#define MAX_CHUNKS 64

struct region {
    u64 va, size;
    int nchunks;
    struct mem_range chunks[MAX_CHUNKS];
};

static struct region regions[MAX_REGIONS];
static u64 next_va = VMAP_BASE;
static spinlock_t lock;

static void give_back(struct region *r)
{
    for (int i = 0; i < r->nchunks; i++)
        page_free((void *)(uintptr_t)r->chunks[i].base, r->chunks[i].size / PAGE_SIZE);
    r->nchunks = 0;
    r->size = 0;
}

void *vmap_alloc(u64 bytes)
{
    bytes = ALIGN_UP(bytes, CHUNK_ALIGN);
    if (!bytes || bytes > pmm_free_bytes())
        return NULL;
    u64 f = spin_lock_irqsave(&lock);
    struct region *r = NULL;
    for (int i = 0; i < MAX_REGIONS && !r; i++)
        if (!regions[i].size)
            r = &regions[i];
    if (!r || next_va + bytes > VMAP_END) {
        spin_unlock_irqrestore(&lock, f);
        return NULL;
    }
    r->va = next_va;
    r->size = bytes; /* claims the slot */
    next_va += bytes + CHUNK_ALIGN; /* an unmapped guard gap between regions */
    spin_unlock_irqrestore(&lock, f);

    r->nchunks = 0;
    u64 done = 0;
    while (done < bytes) {
        paddr_t pa;
        u64 got = r->nchunks < MAX_CHUNKS ? pmm_take_chunk(bytes - done, CHUNK_ALIGN, &pa) : 0;
        if (!got || !mmu_map_at(r->va + done, pa, got, MAP_NORMAL)) {
            if (got)
                page_free((void *)(uintptr_t)pa, got / PAGE_SIZE);
            mmu_unmap(r->va, done);
            give_back(r);
            return NULL;
        }
        r->chunks[r->nchunks].base = pa;
        r->chunks[r->nchunks].size = got;
        r->nchunks++;
        done += got;
    }
    return (void *)(uintptr_t)r->va;
}

bool vmap_contains(const void *p)
{
    u64 a = (u64)(uintptr_t)p;
    return a >= VMAP_BASE && a < VMAP_END;
}

bool vmap_free(void *p)
{
    u64 va = (u64)(uintptr_t)p;
    if (!vmap_contains(p))
        return false;
    for (int i = 0; i < MAX_REGIONS; i++) {
        struct region *r = &regions[i];
        if (r->size && r->va == va) {
            mmu_unmap(r->va, r->size);
            give_back(r);
            return true;
        }
    }
    return false;
}
