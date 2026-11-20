/*
 * Virtio block device (virtio 1.x, section 5.2). Requests are kept in
 * flight several at a time so large sequential reads (model loading) run
 * at device speed.
 */
#include "block.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"
#include "virtio.h"

#define VIRTIO_BLK_F_SIZE_MAX (1ULL << 1)
#define VIRTIO_BLK_F_SEG_MAX (1ULL << 2)
#define VIRTIO_BLK_F_RO (1ULL << 5)
#define VIRTIO_BLK_F_BLK_SIZE (1ULL << 6)
#define VIRTIO_BLK_F_FLUSH (1ULL << 9)

enum { T_IN = 0, T_OUT = 1, T_FLUSH = 4 };
enum { S_OK = 0, S_IOERR = 1, S_UNSUPP = 2 };

#define MAX_INFLIGHT 32
#define REQ_BYTES (1u << 20)

struct req {
    u32 type, reserved;
    u64 sector;
    u8 status;
    bool busy;
};

struct vblk {
    struct blkdev bd;
    struct virtio_dev *vd;
    struct virtq *q;
    struct req *reqs; /* MAX_INFLIGHT */
    u32 max_bytes;
    int inflight;
};

static bool wait_one(struct vblk *v, bool *ok)
{
    for (;;) {
        struct req *r = virtq_poll(v->q, NULL);
        if (r) {
            if (r->status != S_OK)
                *ok = false;
            r->busy = false;
            v->inflight--;
            return true;
        }
        __asm__ volatile("yield");
    }
}

static bool rw(struct vblk *v, int type, u64 lba, u32 count, void *buf)
{
    bool ok = true;
    u8 *p = buf;
    u64 left = (u64)count * BLK_SECTOR;
    while (left || v->inflight) {
        if (left && v->inflight < MAX_INFLIGHT) {
            struct req *r = NULL;
            for (int i = 0; i < MAX_INFLIGHT; i++) {
                if (!v->reqs[i].busy) {
                    r = &v->reqs[i];
                    break;
                }
            }
            u32 n = (u32)MIN(left, (u64)v->max_bytes);
            r->type = (u32)type;
            r->reserved = 0;
            r->sector = lba;
            r->status = 0xff;
            r->busy = true;
            struct vq_buf b[3] = {
                { r, 16, false },
                { p, n, type == T_IN },
                { &r->status, 1, true },
            };
            if (virtq_submit(v->q, b, 3, r)) {
                v->inflight++;
                p += n;
                lba += n / BLK_SECTOR;
                left -= n;
                /* batch submissions; kick when the window is full or done */
                if (!left || v->inflight == MAX_INFLIGHT)
                    virtq_kick(v->q);
                continue;
            }
            r->busy = false;
            virtq_kick(v->q);
        }
        wait_one(v, &ok);
    }
    return ok;
}

static bool vblk_read(struct blkdev *d, u64 lba, u32 count, void *buf)
{
    return rw(d->priv, T_IN, lba, count, buf);
}

static bool vblk_write(struct blkdev *d, u64 lba, u32 count, const void *buf)
{
    return rw(d->priv, T_OUT, lba, count, (void *)buf);
}

static bool vblk_flush(struct blkdev *d)
{
    struct vblk *v = d->priv;
    if (!(v->vd->features & VIRTIO_BLK_F_FLUSH))
        return true;
    struct req *r = &v->reqs[0];
    r->type = T_FLUSH;
    r->reserved = 0;
    r->sector = 0;
    r->status = 0xff;
    r->busy = true;
    struct vq_buf b[2] = { { r, 16, false }, { &r->status, 1, true } };
    if (!virtq_submit(v->q, b, 2, r))
        return false;
    virtq_kick(v->q);
    v->inflight++;
    bool ok = true;
    wait_one(v, &ok);
    return ok;
}

bool virtio_blk_probe(struct virtio_dev *d)
{
    if (!virtio_negotiate(d, VIRTIO_BLK_F_RO | VIRTIO_BLK_F_FLUSH | VIRTIO_BLK_F_SIZE_MAX |
                                 VIRTIO_BLK_F_SEG_MAX | VIRTIO_BLK_F_BLK_SIZE))
        return false;
    struct vblk *v = kzalloc(sizeof(*v));
    v->vd = d;
    v->q = virtio_setup_queue(d, 0, 128);
    v->reqs = kzalloc(sizeof(struct req) * MAX_INFLIGHT);
    if (!v->q || !v->reqs) {
        virtio_fail(d);
        return false;
    }
    v->max_bytes = REQ_BYTES;
    if (d->features & VIRTIO_BLK_F_SIZE_MAX) {
        u32 sm = virtio_cfg32(d, 8);
        if (sm >= BLK_SECTOR && sm < v->max_bytes)
            v->max_bytes = sm & ~(u32)(BLK_SECTOR - 1);
    }
    d->priv = v;
    virtio_driver_ok(d);
    struct blkdev *b = &v->bd;
    snprintf(b->name, sizeof(b->name), "vd%c", 'a' + d->index);
    b->nsectors = virtio_cfg64(d, 0);
    b->readonly = (d->features & VIRTIO_BLK_F_RO) != 0;
    b->read = vblk_read;
    b->write = vblk_write;
    b->flush = vblk_flush;
    b->priv = v;
    char sz[24];
    blk_format_size(b->nsectors * BLK_SECTOR, sz, sizeof(sz));
    printk("  %s: virtio disk, %s%s\n", b->name, sz, b->readonly ? " (read-only)" : "");
    blk_register(b);
    return true;
}
