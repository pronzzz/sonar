/*
 * Kernel heap. Small requests (<= 2032 bytes) come from power-of-two size
 * classes carved out of 64 KiB slabs; larger requests get whole pages.
 * Every block carries a 16-byte header just before the returned pointer.
 */
#include "kernel.h"
#include "mm.h"
#include "smp.h"
#include "string.h"

#define KH_MAGIC 0x5eaf00d5U
#define CLASS_LARGE 0xffU
#define NCLASSES 8   /* 16 .. 2048 bytes including header */
#define SLAB_PAGES 16
#define LARGE_OFFSET 64

struct khdr {
    u32 magic;
    u32 cls;
    u64 npages; /* large blocks only */
};

struct free_block {
    struct free_block *next;
};

static struct free_block *freelists[NCLASSES];
static spinlock_t lock = SPINLOCK_INIT;
static u64 used_bytes;

static size_t class_size(int c) { return (size_t)16 << c; }

static int size_to_class(size_t total)
{
    for (int c = 0; c < NCLASSES; c++)
        if (total <= class_size(c))
            return c;
    return -1;
}

static bool refill(int c)
{
    u8 *slab = page_alloc_nozero(SLAB_PAGES);
    if (!slab)
        return false;
    size_t sz = class_size(c);
    size_t count = SLAB_PAGES * PAGE_SIZE / sz;
    for (size_t i = 0; i < count; i++) {
        struct free_block *b = (struct free_block *)(slab + i * sz);
        b->next = freelists[c];
        freelists[c] = b;
    }
    return true;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        size = 1;
    int c = size_to_class(size + sizeof(struct khdr));
    if (c < 0) {
        u64 npages = (size + LARGE_OFFSET + PAGE_SIZE - 1) / PAGE_SIZE;
        u8 *base = page_alloc_nozero(npages);
        if (!base)
            return NULL;
        struct khdr *h = (struct khdr *)(base + LARGE_OFFSET - sizeof(struct khdr));
        h->magic = KH_MAGIC;
        h->cls = CLASS_LARGE;
        h->npages = npages;
        __atomic_fetch_add(&used_bytes, npages * PAGE_SIZE, __ATOMIC_RELAXED);
        return base + LARGE_OFFSET;
    }
    u64 f = spin_lock_irqsave(&lock);
    if (!freelists[c] && !refill(c)) {
        spin_unlock_irqrestore(&lock, f);
        return NULL;
    }
    struct free_block *b = freelists[c];
    freelists[c] = b->next;
    used_bytes += class_size(c);
    spin_unlock_irqrestore(&lock, f);
    struct khdr *h = (struct khdr *)b;
    h->magic = KH_MAGIC;
    h->cls = (u32)c;
    h->npages = 0;
    return h + 1;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void *kcalloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size)
        return NULL;
    return kzalloc(n * size);
}

static struct khdr *header_of(void *p)
{
    struct khdr *h = (struct khdr *)p - 1;
    if (h->magic != KH_MAGIC)
        panic("kfree/krealloc: bad pointer %p (heap corruption or double free)", p);
    return h;
}

size_t kmalloc_usable(void *p)
{
    struct khdr *h = header_of(p);
    if (h->cls == CLASS_LARGE)
        return h->npages * PAGE_SIZE - LARGE_OFFSET;
    return class_size((int)h->cls) - sizeof(struct khdr);
}

void kfree(void *p)
{
    if (!p)
        return;
    struct khdr *h = header_of(p);
    h->magic = 0;
    if (h->cls == CLASS_LARGE) {
        u64 npages = h->npages;
        __atomic_fetch_sub(&used_bytes, npages * PAGE_SIZE, __ATOMIC_RELAXED);
        page_free((u8 *)p - LARGE_OFFSET, npages);
        return;
    }
    int c = (int)h->cls;
    if (c >= NCLASSES)
        panic("kfree: corrupt size class %d", c);
    u64 f = spin_lock_irqsave(&lock);
    struct free_block *b = (struct free_block *)h;
    b->next = freelists[c];
    freelists[c] = b;
    used_bytes -= class_size(c);
    spin_unlock_irqrestore(&lock, f);
}

void *krealloc(void *p, size_t size)
{
    if (!p)
        return kmalloc(size);
    if (size == 0) {
        kfree(p);
        return NULL;
    }
    size_t old = kmalloc_usable(p);
    if (size <= old)
        return p;
    void *n = kmalloc(size);
    if (!n)
        return NULL;
    memcpy(n, p, old);
    kfree(p);
    return n;
}

u64 kmalloc_stats_used(void) { return used_bytes; }
