/* Virtio 1.x devices over MMIO or PCI (modern register layouts, split virtqueues). */
#pragma once
#include "types.h"
#include "smp.h"

enum {
    VIRTIO_ID_NET = 1,
    VIRTIO_ID_BLOCK = 2,
    VIRTIO_ID_CONSOLE = 3,
    VIRTIO_ID_RNG = 4,
    VIRTIO_ID_GPU = 16,
    VIRTIO_ID_INPUT = 18,
};

#define VIRTIO_F_VERSION_1 (1ULL << 32)

struct pci_dev;
struct virtio_dev {
    uintptr_t base; /* MMIO register window (virtio-mmio) */
    int irq;        /* virtio-mmio interrupt */
    /* virtio-pci: the structures its capabilities point to */
    struct pci_dev *pci;
    uintptr_t common, notify, isr, devcfg;
    u32 notify_mult;
    bool msix;      /* queues interrupt through MSI-X vector 0 */
    u32 device_id;
    u64 features; /* negotiated */
    int index;    /* nth device of this type */
    void *priv;
    /* Optional: called from the interrupt handler (e.g. keyboard events). */
    void (*on_interrupt)(struct virtio_dev *d);
};

struct vq_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

struct virtq {
    struct virtio_dev *dev;
    int index;
    u16 size;
    struct vq_desc *desc;
    volatile u16 *avail; /* flags, idx, ring[size], used_event */
    volatile u8 *used;   /* flags, idx, ring[size] of {u32 id, u32 len}, avail_event */
    u16 free_head, num_free;
    u16 last_used;
    u16 avail_idx;
    void **cookie;
    uintptr_t notify_addr; /* virtio-pci queue notify register */
    spinlock_t lock;
};

/* One buffer of a request chain. */
struct vq_buf {
    void *addr;
    u32 len;
    bool device_writes;
};

/* Feature negotiation: offers `wanted & device features` (plus VERSION_1,
 * which is required). Returns false if the device is unusable. */
bool virtio_negotiate(struct virtio_dev *d, u64 wanted);
struct virtq *virtio_setup_queue(struct virtio_dev *d, int index, int max_size);
void virtio_driver_ok(struct virtio_dev *d);
void virtio_fail(struct virtio_dev *d);

u8 virtio_cfg8(struct virtio_dev *d, u32 off);
u16 virtio_cfg16(struct virtio_dev *d, u32 off);
u32 virtio_cfg32(struct virtio_dev *d, u32 off);
u64 virtio_cfg64(struct virtio_dev *d, u32 off);
void virtio_cfg_write8(struct virtio_dev *d, u32 off, u8 v);
/* Run a request (driver-readable buffers then device-writable ones) and
 * busy-wait for its completion. For setup and rare commands. */
bool virtq_run(struct virtq *q, const struct vq_buf *bufs, int n, u32 *written);

/* Add a buffer chain; returns false when the ring is full. */
bool virtq_submit(struct virtq *q, const struct vq_buf *bufs, int n, void *cookie);
void virtq_kick(struct virtq *q);
/* Next completed chain's cookie (and bytes written by the device), or NULL. */
void *virtq_poll(struct virtq *q, u32 *len);
/* Ask the device to interrupt (or not) when it completes chains; a hint the
 * device may ignore, used to avoid an interrupt per packet while polling. */
void virtq_interrupts(struct virtq *q, bool on);
/* True if the device has completed chains not yet taken by virtq_poll. */
bool virtq_pending(struct virtq *q);

/* Probe all virtio-mmio devices in the device tree, and virtio-pci functions. */
void virtio_scan(void);
void virtio_pci_scan(void);

/* Device drivers */
bool virtio_blk_probe(struct virtio_dev *d);
bool virtio_net_probe(struct virtio_dev *d);
bool virtio_rng_probe(struct virtio_dev *d);
bool virtio_gpu_probe(struct virtio_dev *d);
bool virtio_input_probe(struct virtio_dev *d);
bool virtio_console_probe(struct virtio_dev *d);
