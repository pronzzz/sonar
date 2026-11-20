/*
 * Virtio transports, MMIO (virtio 1.x, section 4.2) and PCI (section 4.1),
 * and split virtqueues (section 2.7). The kernel identity-maps RAM, so buffer addresses given to
 * devices are the kernel pointers themselves.
 */
#include "virtio.h"
#include "arch.h"
#include "fdt.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "mmio.h"
#include "pci.h"
#include "string.h"

enum {
    R_MAGIC = 0x000,
    R_VERSION = 0x004,
    R_DEVICE_ID = 0x008,
    R_DEV_FEATURES = 0x010,
    R_DEV_FEATURES_SEL = 0x014,
    R_DRV_FEATURES = 0x020,
    R_DRV_FEATURES_SEL = 0x024,
    R_QUEUE_SEL = 0x030,
    R_QUEUE_NUM_MAX = 0x034,
    R_QUEUE_NUM = 0x038,
    R_QUEUE_READY = 0x044,
    R_QUEUE_NOTIFY = 0x050,
    R_INT_STATUS = 0x060,
    R_INT_ACK = 0x064,
    R_STATUS = 0x070,
    R_QUEUE_DESC = 0x080,
    R_QUEUE_DRIVER = 0x090,
    R_QUEUE_DEVICE = 0x0a0,
    R_CONFIG_GEN = 0x0fc,
    R_CONFIG = 0x100,
};

enum {
    S_ACKNOWLEDGE = 1,
    S_DRIVER = 2,
    S_DRIVER_OK = 4,
    S_FEATURES_OK = 8,
    S_FAILED = 128,
};

#define VIRTQ_DESC_F_NEXT 1
#define VIRTQ_DESC_F_WRITE 2
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1
#define VIRTQ_USED_F_NO_NOTIFY 1

static inline u32 rd(struct virtio_dev *d, u32 off) { return mmio_read32(d->base + off); }
static inline void wr(struct virtio_dev *d, u32 off, u32 v) { mmio_write32(d->base + off, v); }

/* virtio-pci common configuration structure (virtio 1.x 4.1.4.3) */
enum {
    P_DEV_FEATURE_SEL = 0x00,
    P_DEV_FEATURE = 0x04,
    P_DRV_FEATURE_SEL = 0x08,
    P_DRV_FEATURE = 0x0c,
    P_MSIX_CONFIG = 0x10,
    P_NUM_QUEUES = 0x12,
    P_STATUS = 0x14,
    P_CONFIG_GEN = 0x15,
    P_Q_SELECT = 0x16,
    P_Q_SIZE = 0x18,
    P_Q_MSIX = 0x1a,
    P_Q_ENABLE = 0x1c,
    P_Q_NOTIFY_OFF = 0x1e,
    P_Q_DESC = 0x20,
    P_Q_DRIVER = 0x28,
    P_Q_DEVICE = 0x30,
};
#define NO_VECTOR 0xffff

static uintptr_t cfg_base(struct virtio_dev *d) { return d->pci ? d->devcfg : d->base + R_CONFIG; }
static u32 status_get(struct virtio_dev *d)
{
    return d->pci ? mmio_read8(d->common + P_STATUS) : rd(d, R_STATUS);
}
static void status_set(struct virtio_dev *d, u32 v)
{
    if (d->pci)
        mmio_write8(d->common + P_STATUS, (u8)v);
    else
        wr(d, R_STATUS, v);
}
static u32 config_gen(struct virtio_dev *d)
{
    return d->pci ? mmio_read8(d->common + P_CONFIG_GEN) : rd(d, R_CONFIG_GEN);
}

u8 virtio_cfg8(struct virtio_dev *d, u32 off) { return mmio_read8(cfg_base(d) + off); }
u16 virtio_cfg16(struct virtio_dev *d, u32 off) { return mmio_read16(cfg_base(d) + off); }
u32 virtio_cfg32(struct virtio_dev *d, u32 off) { return mmio_read32(cfg_base(d) + off); }
void virtio_cfg_write8(struct virtio_dev *d, u32 off, u8 v) { mmio_write8(cfg_base(d) + off, v); }

u64 virtio_cfg64(struct virtio_dev *d, u32 off)
{
    /* 64-bit fields may tear; retry until the generation is stable. */
    for (;;) {
        u32 g = config_gen(d);
        u64 v = (u64)virtio_cfg32(d, off) | (u64)virtio_cfg32(d, off + 4) << 32;
        if (config_gen(d) == g)
            return v;
    }
}

static u32 features_get(struct virtio_dev *d, u32 sel)
{
    if (d->pci) {
        mmio_write32(d->common + P_DEV_FEATURE_SEL, sel);
        return mmio_read32(d->common + P_DEV_FEATURE);
    }
    wr(d, R_DEV_FEATURES_SEL, sel);
    return rd(d, R_DEV_FEATURES);
}

static void features_set(struct virtio_dev *d, u32 sel, u32 v)
{
    if (d->pci) {
        mmio_write32(d->common + P_DRV_FEATURE_SEL, sel);
        mmio_write32(d->common + P_DRV_FEATURE, v);
    } else {
        wr(d, R_DRV_FEATURES_SEL, sel);
        wr(d, R_DRV_FEATURES, v);
    }
}

bool virtio_negotiate(struct virtio_dev *d, u64 wanted)
{
    status_set(d, 0); /* reset */
    for (int i = 0; i < 1000 && status_get(d); i++)
        delay_us(10);
    status_set(d, S_ACKNOWLEDGE);
    status_set(d, S_ACKNOWLEDGE | S_DRIVER);
    u64 have = features_get(d, 0) | (u64)features_get(d, 1) << 32;
    if (!(have & VIRTIO_F_VERSION_1)) {
        virtio_fail(d);
        return false;
    }
    u64 use = (have & wanted) | VIRTIO_F_VERSION_1;
    features_set(d, 0, (u32)use);
    features_set(d, 1, (u32)(use >> 32));
    status_set(d, S_ACKNOWLEDGE | S_DRIVER | S_FEATURES_OK);
    if (!(status_get(d) & S_FEATURES_OK)) {
        virtio_fail(d);
        return false;
    }
    d->features = use;
    return true;
}

void virtio_driver_ok(struct virtio_dev *d)
{
    status_set(d, S_ACKNOWLEDGE | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
    /* Read the status back before using the queues. Apple's Virtualization
     * framework handles DRIVER_OK asynchronously and takes the avail index
     * it finds then as its starting point, so a buffer published in that
     * window is silently skipped (and its slot never comes back). The read
     * traps and returns only after the write has been handled. */
    (void)status_get(d);
}

void virtio_fail(struct virtio_dev *d) { status_set(d, status_get(d) | S_FAILED); }

struct virtq *virtio_setup_queue(struct virtio_dev *d, int index, int max_size)
{
    u32 n;
    if (d->pci) {
        if (index >= mmio_read16(d->common + P_NUM_QUEUES))
            return NULL;
        mmio_write16(d->common + P_Q_SELECT, (u16)index);
        if (mmio_read16(d->common + P_Q_ENABLE))
            return NULL;
        n = mmio_read16(d->common + P_Q_SIZE);
    } else {
        wr(d, R_QUEUE_SEL, (u32)index);
        if (rd(d, R_QUEUE_READY))
            return NULL;
        n = rd(d, R_QUEUE_NUM_MAX);
    }
    if (!n)
        return NULL;
    if (n > (u32)max_size)
        n = (u32)max_size;
    /* queue sizes must be powers of two for split rings */
    u32 size = 1;
    while (size * 2 <= n)
        size *= 2;
    if (d->pci) {
        /* The driver may shrink a queue (virtio 1.x 4.1.4.3); use whatever
         * size the device reports back, in case it refuses. */
        mmio_write16(d->common + P_Q_SIZE, (u16)size);
        u32 got = mmio_read16(d->common + P_Q_SIZE);
        if (got != size) {
            if (!got || (got & (got - 1)) || got > 32768)
                return NULL;
            size = got;
        }
    }
    size_t desc_sz = 16 * size, avail_sz = 6 + 2 * size, used_sz = 6 + 8 * size;
    size_t used_off = ALIGN_UP(desc_sz + avail_sz, 64);
    size_t total = used_off + used_sz;
    u8 *mem = page_alloc(ALIGN_UP(total, PAGE_SIZE) / PAGE_SIZE);
    struct virtq *q = kzalloc(sizeof(*q));
    if (!mem || !q)
        return NULL;
    q->cookie = kzalloc(sizeof(void *) * size);
    q->dev = d;
    q->index = index;
    q->size = (u16)size;
    q->desc = (struct vq_desc *)mem;
    q->avail = (volatile u16 *)(mem + desc_sz);
    q->used = mem + used_off;
    for (u32 i = 0; i < size; i++)
        q->desc[i].next = (u16)(i + 1);
    q->free_head = 0;
    q->num_free = (u16)size;
    u64 a = (u64)(uintptr_t)q->desc, b = (u64)(uintptr_t)q->avail, c = (u64)(uintptr_t)q->used;
    if (d->pci) {
        mmio_write32(d->common + P_Q_DESC, (u32)a);
        mmio_write32(d->common + P_Q_DESC + 4, (u32)(a >> 32));
        mmio_write32(d->common + P_Q_DRIVER, (u32)b);
        mmio_write32(d->common + P_Q_DRIVER + 4, (u32)(b >> 32));
        mmio_write32(d->common + P_Q_DEVICE, (u32)c);
        mmio_write32(d->common + P_Q_DEVICE + 4, (u32)(c >> 32));
        mmio_write16(d->common + P_Q_MSIX, d->msix ? 0 : NO_VECTOR);
        q->notify_addr = d->notify + (uintptr_t)mmio_read16(d->common + P_Q_NOTIFY_OFF) * d->notify_mult;
        dsb(sy);
        mmio_write16(d->common + P_Q_ENABLE, 1);
        return q;
    }
    wr(d, R_QUEUE_NUM, size);
    wr(d, R_QUEUE_DESC, (u32)a);
    wr(d, R_QUEUE_DESC + 4, (u32)(a >> 32));
    wr(d, R_QUEUE_DRIVER, (u32)b);
    wr(d, R_QUEUE_DRIVER + 4, (u32)(b >> 32));
    wr(d, R_QUEUE_DEVICE, (u32)c);
    wr(d, R_QUEUE_DEVICE + 4, (u32)(c >> 32));
    dsb(sy);
    wr(d, R_QUEUE_READY, 1);
    return q;
}

bool virtq_submit(struct virtq *q, const struct vq_buf *bufs, int n, void *cookie)
{
    u64 f = spin_lock_irqsave(&q->lock);
    if (n <= 0 || q->num_free < n) {
        spin_unlock_irqrestore(&q->lock, f);
        return false;
    }
    u16 head = q->free_head, i = head, last = head;
    for (int k = 0; k < n; k++) {
        struct vq_desc *dsc = &q->desc[i];
        dsc->addr = (u64)(uintptr_t)bufs[k].addr;
        dsc->len = bufs[k].len;
        dsc->flags = (u16)((bufs[k].device_writes ? VIRTQ_DESC_F_WRITE : 0) |
                           (k + 1 < n ? VIRTQ_DESC_F_NEXT : 0));
        last = i;
        i = dsc->next;
    }
    q->free_head = q->desc[last].next;
    q->num_free = (u16)(q->num_free - n);
    q->cookie[head] = cookie;
    q->avail[2 + q->avail_idx % q->size] = head;
    dmb(ish); /* descriptors and ring entry before the index */
    q->avail_idx++;
    q->avail[1] = q->avail_idx;
    spin_unlock_irqrestore(&q->lock, f);
    return true;
}

/* Notifications trap to the hypervisor, so skip them when the device says
 * it is already processing the queue (virtio 1.x 2.7.7.2, no EVENT_IDX). */
void virtq_kick(struct virtq *q)
{
    dsb(sy); /* ring updates before reading the flags */
    if (*(volatile u16 *)q->used & VIRTQ_USED_F_NO_NOTIFY)
        return;
    if (q->dev->pci)
        mmio_write16(q->notify_addr, (u16)q->index);
    else
        wr(q->dev, R_QUEUE_NOTIFY, (u32)q->index);
}

void virtq_interrupts(struct virtq *q, bool on)
{
    q->avail[0] = on ? 0 : VIRTQ_AVAIL_F_NO_INTERRUPT;
    dsb(sy);
}

void *virtq_poll(struct virtq *q, u32 *len)
{
    u64 f = spin_lock_irqsave(&q->lock);
    u16 used_idx = *(volatile u16 *)(q->used + 2);
    if (used_idx == q->last_used) {
        spin_unlock_irqrestore(&q->lock, f);
        return NULL;
    }
    dmb(ish); /* index before the entry */
    volatile u8 *e = q->used + 4 + 8 * (q->last_used % q->size);
    u16 head = (u16)*(volatile u32 *)e;
    if (len)
        *len = *(volatile u32 *)(e + 4);
    q->last_used++;
    void *cookie = q->cookie[head];
    /* return the chain to the free list */
    u16 i = head, n = 1;
    while (q->desc[i].flags & VIRTQ_DESC_F_NEXT) {
        i = q->desc[i].next;
        n++;
    }
    q->desc[i].next = q->free_head;
    q->free_head = head;
    q->num_free = (u16)(q->num_free + n);
    spin_unlock_irqrestore(&q->lock, f);
    return cookie;
}

bool virtq_pending(struct virtq *q)
{
    return *(volatile u16 *)(q->used + 2) != q->last_used;
}

/* Interrupts only wake the CPU (drivers poll their queues); acknowledge. */
static void virtio_irq(int irq, void *ctx)
{
    struct virtio_dev *d = ctx;
    u32 st = rd(d, R_INT_STATUS);
    if (st)
        wr(d, R_INT_ACK, st);
    if (d->on_interrupt)
        d->on_interrupt(d);
}

bool virtq_run(struct virtq *q, const struct vq_buf *bufs, int n, u32 *written)
{
    static int token;
    if (!virtq_submit(q, bufs, n, &token))
        return false;
    virtq_kick(q);
    u64 deadline = time_ms() + 5000;
    for (;;) {
        void *ck = virtq_poll(q, written);
        if (ck == &token)
            return true;
        if (time_ms() > deadline)
            return false; /* the device did not answer */
        __asm__ volatile("yield");
    }
}

static void probe_one(int node, int *counts)
{
    u64 addr, size;
    if (!fdt_is_enabled(node) || !fdt_reg(node, 0, &addr, &size))
        return;
    uintptr_t base = (uintptr_t)ioremap(addr, size);
    if (!base || mmio_read32(base + R_MAGIC) != 0x74726976)
        return;
    u32 ver = mmio_read32(base + R_VERSION), id = mmio_read32(base + R_DEVICE_ID);
    if (!id)
        return; /* empty slot */
    if (ver != 2) {
        printk("virtio: legacy device (id %u) at %#llx ignored; use "
               "-global virtio-mmio.force-legacy=false\n", id, (unsigned long long)addr);
        return;
    }
    struct virtio_dev *d = kzalloc(sizeof(*d));
    d->base = base;
    d->irq = fdt_irq(node, 0);
    d->device_id = id;
    d->index = id < 32 ? counts[id]++ : 0;
    bool ok;
    switch (id) {
    case VIRTIO_ID_BLOCK: ok = virtio_blk_probe(d); break;
    case VIRTIO_ID_NET: ok = virtio_net_probe(d); break;
    case VIRTIO_ID_RNG: ok = virtio_rng_probe(d); break;
    case VIRTIO_ID_GPU: ok = virtio_gpu_probe(d); break;
    case VIRTIO_ID_INPUT: ok = virtio_input_probe(d); break;
    default:
        printk("virtio: device type %u at %#llx not supported\n", id, (unsigned long long)addr);
        ok = false;
    }
    if (!ok) {
        if (id < 32)
            counts[id]--;
        kfree(d);
        return;
    }
    if (d->irq >= 0) {
        irq_register(d->irq, virtio_irq, d);
        irq_unmask(d->irq);
    }
}

void virtio_scan(void)
{
    /* QEMU's virt board hands out transports from the highest address
     * down, so probing in descending address order names devices in
     * command-line order (first -drive is vda). */
    enum { MAXT = 64 };
    int nodes[MAXT], n = 0;
    u64 addrs[MAXT];
    for (int node = fdt_find_compatible(-1, "virtio,mmio"); node >= 0 && n < MAXT;
         node = fdt_find_compatible(node, "virtio,mmio")) {
        u64 a, s;
        if (!fdt_reg(node, 0, &a, &s))
            continue;
        int i = n++;
        while (i > 0 && addrs[i - 1] < a) {
            addrs[i] = addrs[i - 1];
            nodes[i] = nodes[i - 1];
            i--;
        }
        addrs[i] = a;
        nodes[i] = node;
    }
    int counts[32] = { 0 };
    for (int i = 0; i < n; i++)
        probe_one(nodes[i], counts);
}

/* ---- virtio over PCI (virtio 1.x 4.1) ---- */

/* MSI-X vector 0 of every virtio-pci device: queue activity. */
static void virtio_msix_irq(int irq, void *ctx)
{
    struct virtio_dev *d = ctx;
    if (d->on_interrupt)
        d->on_interrupt(d);
}

/* Device-specific structures are found through vendor capabilities:
 * cfg_type (1 common, 2 notify, 3 ISR, 4 device), bar, offset, length;
 * the notify capability adds notify_off_multiplier. */
static bool pci_layout(struct virtio_dev *d)
{
    struct pci_dev *p = d->pci;
    for (u8 cap = pci_find_cap(p, PCI_CAP_VENDOR, 0); cap; cap = pci_find_cap(p, PCI_CAP_VENDOR, cap)) {
        u8 type = pci_read8(p, cap + 3), bar = pci_read8(p, cap + 4);
        u32 off = pci_read32(p, cap + 8);
        if (bar > 5)
            continue;
        uintptr_t base = pci_map_bar(p, bar);
        if (!base)
            continue;
        switch (type) {
        case 1: d->common = base + off; break;
        case 2:
            d->notify = base + off;
            d->notify_mult = pci_read32(p, cap + 16);
            break;
        case 3: d->isr = base + off; break;
        case 4: d->devcfg = base + off; break;
        }
    }
    return d->common && d->notify;
}

void virtio_pci_scan(void)
{
    int counts[32] = { 0 };
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *p = pci_get(i);
        if (p->vendor != 0x1af4 || p->device < 0x1000 || p->device > 0x107f)
            continue;
        /* 0x1040 + type for modern devices; transitional IDs 0x1000-0x103f
         * map through the subsystem ID. */
        u32 id = p->device >= 0x1040 ? p->device - 0x1040u : pci_read16(p, 0x2e);
        bool known = id == VIRTIO_ID_BLOCK || id == VIRTIO_ID_NET || id == VIRTIO_ID_RNG ||
                     id == VIRTIO_ID_GPU || id == VIRTIO_ID_INPUT || id == VIRTIO_ID_CONSOLE;
        if (!known)
            continue; /* sound, balloon, file system...: left alone */
        struct virtio_dev *d = kzalloc(sizeof(*d));
        d->pci = p;
        d->irq = -1;
        d->device_id = id;
        pci_enable(p);
        if (!pci_layout(d)) {
            printk("virtio: %s has no modern (virtio 1.x) interface\n", p->name);
            kfree(d);
            continue;
        }
        /* Only the first console is the serial console; further ones (e.g.
         * a clipboard channel) are not used. */
        if (id == VIRTIO_ID_CONSOLE && counts[id]) {
            status_set(d, 0);
            kfree(d);
            continue;
        }
        void *ctx = d;
        d->msix = pci_msix_setup(p, 1, virtio_msix_irq, &ctx) == 1;
        d->index = id < 32 ? counts[id]++ : 0;
        bool ok;
        switch (id) {
        case VIRTIO_ID_BLOCK: ok = virtio_blk_probe(d); break;
        case VIRTIO_ID_NET: ok = virtio_net_probe(d); break;
        case VIRTIO_ID_RNG: ok = virtio_rng_probe(d); break;
        case VIRTIO_ID_GPU: ok = virtio_gpu_probe(d); break;
        case VIRTIO_ID_INPUT: ok = virtio_input_probe(d); break;
        case VIRTIO_ID_CONSOLE: ok = virtio_console_probe(d); break;
        default: ok = false; break; /* sound, balloon, file system...: not used */
        }
        if (!ok) {
            /* The MSI handler may still fire with d: keep it, silenced. */
            if (id < 32)
                counts[id]--;
            d->on_interrupt = NULL;
            status_set(d, 0);
        }
    }
}
