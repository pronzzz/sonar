/*
 * NVM Express (NVMe 1.4, "NVM Express Base Specification" revision 1.4c):
 * the SSD interface of almost every current laptop, and a disk option in
 * Apple's Virtualization framework.
 *
 * One admin queue pair and one I/O queue pair per controller, polled (block
 * requests here are synchronous, like the virtio disk driver). Several
 * commands are kept in flight so large sequential reads run at device
 * speed. Each namespace becomes a disk: nvme0n1, nvme0n2, ...
 */
#include "arch.h"
#include "block.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "mmio.h"
#include "pci.h"
#include "smp.h"
#include "string.h"

/* Controller registers (section 3.1) */
enum {
    R_CAP = 0x00, R_VS = 0x08, R_INTMS = 0x0c, R_CC = 0x14, R_CSTS = 0x1c,
    R_AQA = 0x24, R_ASQ = 0x28, R_ACQ = 0x30, R_DOORBELL = 0x1000,
};
#define CC_EN (1u << 0)
#define CC_IOSQES (6u << 16) /* 64-byte submission entries */
#define CC_IOCQES (4u << 20) /* 16-byte completion entries */
#define CC_SHN_NORMAL (1u << 14)
#define CSTS_RDY (1u << 0)
#define CSTS_CFS (1u << 1)
#define CSTS_SHST_MASK (3u << 2)
#define CSTS_SHST_DONE (2u << 2)

/* Opcodes (sections 5 and 6) */
enum { A_CREATE_SQ = 0x01, A_CREATE_CQ = 0x05, A_IDENTIFY = 0x06, A_SET_FEATURES = 0x09 };
enum { IO_FLUSH = 0x00, IO_WRITE = 0x01, IO_READ = 0x02 };

#define PAGE 4096u
#define ADMIN_DEPTH 16
#define IO_DEPTH 64     /* entries per I/O queue */
#define INFLIGHT 32     /* commands kept in flight (< IO_DEPTH) */
#define CMD_TIMEOUT_MS 5000

struct sqe {
    u32 cdw0;  /* opcode 7:0, command id 31:16 */
    u32 nsid;
    u64 rsvd;
    u64 mptr;
    u64 prp1, prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
struct cqe {
    u32 dw0, dw1;
    u16 sqhd, sqid;
    u16 cid;
    u16 status; /* phase bit 0, status field 15:1 */
};
_Static_assert(sizeof(struct sqe) == 64, "sqe");
_Static_assert(sizeof(struct cqe) == 16, "cqe");

struct queue {
    struct sqe *sq;
    volatile struct cqe *cq;
    u16 depth, sq_tail, cq_head;
    u8 phase;
    uintptr_t sq_db, cq_db;
};

struct ctrl {
    struct pci_dev *pci;
    uintptr_t regs;
    u32 dstrd;
    u32 max_bytes;      /* per command (MDTS, capped to one PRP list page) */
    struct queue admin, io;
    u64 *prp[INFLIGHT]; /* one PRP list page per in-flight slot */
    bool busy[INFLIGHT];
    u16 status[INFLIGHT];
    int inflight;
    u8 *bounce;         /* one page, for sub-block access and identify */
    int index;
    bool down;
    spinlock_t lock;
};

struct ns {
    struct blkdev bd;
    struct ctrl *c;
    u32 nsid;
    u32 lbshift;        /* log2 of the logical block size */
};

static int nctrls;

static u32 rd32(struct ctrl *c, u32 off) { return mmio_read32(c->regs + off); }
static void wr32(struct ctrl *c, u32 off, u32 v) { mmio_write32(c->regs + off, v); }
static u64 rd64(struct ctrl *c, u32 off)
{
    return (u64)rd32(c, off) | (u64)rd32(c, off + 4) << 32;
}
static void wr64(struct ctrl *c, u32 off, u64 v)
{
    wr32(c, off, (u32)v);
    wr32(c, off + 4, (u32)(v >> 32));
}

static bool wait_csts(struct ctrl *c, u32 mask, u32 want, u64 ms)
{
    u64 end = time_ms() + ms;
    while ((rd32(c, R_CSTS) & mask) != want) {
        if (rd32(c, R_CSTS) == 0xffffffff || time_ms() > end)
            return false;
        __asm__ volatile("yield");
    }
    return true;
}

static bool queue_init(struct ctrl *c, struct queue *q, int qid, u16 depth)
{
    q->depth = depth;
    q->sq = page_alloc(ALIGN_UP(depth * sizeof(struct sqe), PAGE) / PAGE);
    q->cq = page_alloc(ALIGN_UP(depth * sizeof(struct cqe), PAGE) / PAGE);
    if (!q->sq || !q->cq)
        return false;
    q->sq_tail = q->cq_head = 0;
    q->phase = 1;
    u32 stride = 4u << c->dstrd;
    q->sq_db = c->regs + R_DOORBELL + (2 * qid) * stride;
    q->cq_db = c->regs + R_DOORBELL + (2 * qid + 1) * stride;
    return true;
}

static void submit(struct queue *q, struct sqe *e)
{
    q->sq[q->sq_tail] = *e;
    q->sq_tail = (u16)((q->sq_tail + 1) % q->depth);
    dsb(sy); /* entry before the doorbell */
    mmio_write32(q->sq_db, q->sq_tail);
}

/* Next completion, if any: returns its command id, status in *st. */
static int reap(struct queue *q, u16 *st)
{
    volatile struct cqe *e = &q->cq[q->cq_head];
    if ((e->status & 1) != q->phase)
        return -1;
    dmb(ish);
    int cid = e->cid;
    *st = (u16)(e->status >> 1);
    if (++q->cq_head == q->depth) {
        q->cq_head = 0;
        q->phase ^= 1;
    }
    mmio_write32(q->cq_db, q->cq_head);
    return cid;
}

/* Admin commands run one at a time. */
static bool admin_cmd(struct ctrl *c, struct sqe *e)
{
    static u16 next_cid;
    u16 cid = next_cid++;
    e->cdw0 = (e->cdw0 & 0xffff) | (u32)cid << 16;
    submit(&c->admin, e);
    u64 end = time_ms() + CMD_TIMEOUT_MS;
    for (;;) {
        u16 st;
        int got = reap(&c->admin, &st);
        if (got == cid)
            return st == 0;
        if (got < 0 && time_ms() > end)
            return false;
        __asm__ volatile("yield");
    }
}

static bool identify(struct ctrl *c, u32 cns, u32 nsid, void *out)
{
    struct sqe e = { 0 };
    e.cdw0 = A_IDENTIFY;
    e.nsid = nsid;
    e.prp1 = (u64)(uintptr_t)c->bounce;
    e.cdw10 = cns;
    if (!admin_cmd(c, &e))
        return false;
    memcpy(out, c->bounce, PAGE);
    return true;
}

/* PRP entries for a physically contiguous buffer (identity-mapped memory):
 * the first may start mid-page, the rest are whole pages (section 4.3). */
static void build_prps(struct ctrl *c, int slot, u64 addr, u32 len, struct sqe *e)
{
    e->prp1 = addr;
    u64 first_end = (addr & ~(u64)(PAGE - 1)) + PAGE;
    if (addr + len <= first_end) {
        e->prp2 = 0;
        return;
    }
    if (addr + len <= first_end + PAGE) {
        e->prp2 = first_end;
        return;
    }
    u64 *list = c->prp[slot];
    int n = 0;
    for (u64 p = first_end; p < addr + len; p += PAGE)
        list[n++] = p;
    e->prp2 = (u64)(uintptr_t)list;
}

/* Wait for one I/O completion; false on timeout or controller failure. */
static bool io_wait_one(struct ctrl *c)
{
    u64 end = time_ms() + CMD_TIMEOUT_MS;
    for (;;) {
        u16 st;
        int cid = reap(&c->io, &st);
        if (cid >= 0 && cid < INFLIGHT && c->busy[cid]) {
            c->busy[cid] = false;
            c->status[cid] = st;
            c->inflight--;
            return true;
        }
        if (cid < 0 && (time_ms() > end || (rd32(c, R_CSTS) & CSTS_CFS)))
            return false;
        __asm__ volatile("yield");
    }
}

/* Read or write whole logical blocks of a namespace. */
static bool io(struct ns *n, int op, u64 lba, u64 nblocks, void *buf)
{
    struct ctrl *c = n->c;
    if (c->down)
        return false;
    u64 f = spin_lock_irqsave(&c->lock);
    bool ok = true;
    u8 *p = buf;
    u32 per = c->max_bytes >> n->lbshift; /* blocks per command */
    for (int i = 0; i < INFLIGHT; i++)
        c->status[i] = 0;
    while (nblocks || c->inflight) {
        if (nblocks && c->inflight < INFLIGHT) {
            int slot = 0;
            while (c->busy[slot])
                slot++;
            u32 cnt = (u32)MIN(nblocks, (u64)per);
            u32 bytes = cnt << n->lbshift;
            struct sqe e = { 0 };
            e.cdw0 = (u32)op | (u32)slot << 16;
            e.nsid = n->nsid;
            build_prps(c, slot, (u64)(uintptr_t)p, bytes, &e);
            e.cdw10 = (u32)lba;
            e.cdw11 = (u32)(lba >> 32);
            e.cdw12 = cnt - 1;
            c->busy[slot] = true;
            c->inflight++;
            submit(&c->io, &e);
            p += bytes;
            lba += cnt;
            nblocks -= cnt;
            continue;
        }
        if (!io_wait_one(c)) {
            printk("nvme%d: command timed out; disabling the controller\n", c->index);
            c->down = true;
            ok = false;
            break;
        }
    }
    for (int i = 0; i < INFLIGHT; i++)
        if (c->status[i]) {
            printk("nvme%d: %s error, status %#x\n", c->index, op == IO_READ ? "read" : "write",
                   c->status[i]);
            ok = false;
        }
    spin_unlock_irqrestore(&c->lock, f);
    return ok;
}

/* The block layer counts 512-byte sectors; namespaces may use larger blocks
 * (4096 is common). Aligned requests go straight through; the unaligned
 * head and tail go through a bounce block (read-modify-write for writes). */
static bool ns_rw(struct blkdev *d, int op, u64 sec, u32 count, void *buf)
{
    struct ns *n = d->priv;
    u32 spb = 1u << (n->lbshift - 9); /* sectors per block */
    u8 *p = buf;
    while (count) {
        u64 blk = sec / spb;
        u32 off = (u32)(sec % spb);
        if (off == 0 && count >= spb) {
            u32 whole = count / spb;
            if (!io(n, op, blk, whole, p))
                return false;
            u32 s = whole * spb;
            p += (u64)s * BLK_SECTOR;
            sec += s;
            count -= s;
            continue;
        }
        u32 take = MIN(count, spb - off);
        u8 *bb = n->c->bounce;
        if (!io(n, IO_READ, blk, 1, bb))
            return false;
        if (op == IO_READ) {
            memcpy(p, bb + off * BLK_SECTOR, take * BLK_SECTOR);
        } else {
            memcpy(bb + off * BLK_SECTOR, p, take * BLK_SECTOR);
            if (!io(n, IO_WRITE, blk, 1, bb))
                return false;
        }
        p += (u64)take * BLK_SECTOR;
        sec += take;
        count -= take;
    }
    return true;
}

static bool ns_read(struct blkdev *d, u64 lba, u32 count, void *buf)
{
    return ns_rw(d, IO_READ, lba, count, buf);
}

static bool ns_write(struct blkdev *d, u64 lba, u32 count, const void *buf)
{
    return ns_rw(d, IO_WRITE, lba, count, (void *)buf);
}

static bool ns_flush(struct blkdev *d)
{
    struct ns *n = d->priv;
    struct ctrl *c = n->c;
    if (c->down)
        return false;
    u64 f = spin_lock_irqsave(&c->lock);
    struct sqe e = { 0 };
    e.cdw0 = IO_FLUSH; /* slot 0, command id 0 */
    e.nsid = n->nsid;
    c->busy[0] = true;
    c->inflight++;
    c->status[0] = 0;
    submit(&c->io, &e);
    bool ok = io_wait_one(c) && c->status[0] == 0;
    spin_unlock_irqrestore(&c->lock, f);
    return ok;
}

/* Normal shutdown (section 7.6.2): the controller commits its caches. */
static void ns_shutdown(struct blkdev *d)
{
    struct ns *n = d->priv;
    struct ctrl *c = n->c;
    if (c->down)
        return;
    ns_flush(d);
    wr32(c, R_CC, (rd32(c, R_CC) & ~(3u << 14)) | CC_SHN_NORMAL);
    wait_csts(c, CSTS_SHST_MASK, CSTS_SHST_DONE, 2000);
    c->down = true;
}

static void copy_trim(char *dst, const u8 *src, int n)
{
    memcpy(dst, src, (size_t)n);
    dst[n] = 0;
    for (int i = n - 1; i >= 0 && (dst[i] == ' ' || dst[i] == 0); i--)
        dst[i] = 0;
}

static void add_namespace(struct ctrl *c, u32 nsid, u8 *idbuf)
{
    if (!identify(c, 0, nsid, idbuf))
        return;
    u64 nsze = *(u64 *)idbuf;
    if (!nsze)
        return; /* inactive */
    u8 flbas = idbuf[26] & 0xf;
    u32 lbaf = *(u32 *)(idbuf + 128 + 4 * flbas);
    u32 lbads = (lbaf >> 16) & 0xff;
    u16 ms = (u16)lbaf;
    if (lbads < 9 || lbads > 12 || ms) {
        printk("  nvme%dn%u: unsupported format (%u-byte blocks, %u metadata bytes)\n",
               c->index, nsid, 1u << lbads, ms);
        return;
    }
    struct ns *n = kzalloc(sizeof(*n));
    n->c = c;
    n->nsid = nsid;
    n->lbshift = lbads;
    struct blkdev *b = &n->bd;
    snprintf(b->name, sizeof(b->name), "nvme%dn%u", c->index, nsid);
    b->nsectors = nsze << (lbads - 9);
    b->read = ns_read;
    b->write = ns_write;
    b->flush = ns_flush;
    b->shutdown = ns_shutdown;
    b->priv = n;
    char sz[24];
    blk_format_size(b->nsectors * BLK_SECTOR, sz, sizeof(sz));
    printk("  %s: NVMe disk, %s (%u-byte blocks)\n", b->name, sz, 1u << lbads);
    blk_register(b);
}

static bool ctrl_init(struct pci_dev *p)
{
    struct ctrl *c = kzalloc(sizeof(*c));
    c->pci = p;
    c->index = nctrls;
    pci_enable(p);
    c->regs = pci_map_bar(p, 0);
    if (!c->regs)
        return false;
    u64 cap = rd64(c, R_CAP);
    if (cap == ~0ull)
        return false;
    c->dstrd = (u32)(cap >> 32) & 0xf;
    u32 timeout = (u32)((cap >> 24) & 0xff) * 500 + 500;
    u32 mqes = (u32)(cap & 0xffff) + 1;
    u32 mpsmin = (u32)(cap >> 48) & 0xf;
    if (!(cap & (1ull << 37)) || mpsmin != 0) {
        printk("  nvme%d: no NVM command set or 4 KiB pages; skipped\n", c->index);
        return false;
    }
    /* Reset: disable, wait until not ready. */
    if (rd32(c, R_CC) & CC_EN) {
        wr32(c, R_CC, rd32(c, R_CC) & ~CC_EN);
        if (!wait_csts(c, CSTS_RDY, 0, timeout))
            return false;
    }
    c->bounce = page_alloc(1);
    if (!c->bounce || !queue_init(c, &c->admin, 0, ADMIN_DEPTH))
        return false;
    u16 iodepth = (u16)MIN((u32)IO_DEPTH, mqes);
    if (iodepth <= INFLIGHT)
        return false;
    wr32(c, R_INTMS, 0xffffffff); /* polled */
    wr32(c, R_AQA, (u32)(ADMIN_DEPTH - 1) << 16 | (ADMIN_DEPTH - 1));
    wr64(c, R_ASQ, (u64)(uintptr_t)c->admin.sq);
    wr64(c, R_ACQ, (u64)(uintptr_t)c->admin.cq);
    wr32(c, R_CC, CC_IOSQES | CC_IOCQES | CC_EN); /* NVM set, 4 KiB pages, RR */
    if (!wait_csts(c, CSTS_RDY | CSTS_CFS, CSTS_RDY, timeout)) {
        printk("  nvme%d: controller did not become ready\n", c->index);
        return false;
    }
    u8 *id = page_alloc(1);
    if (!id || !identify(c, 1, 0, id))
        return false;
    char model[41], serial[21];
    copy_trim(model, id + 24, 40);
    copy_trim(serial, id + 4, 20);
    u32 nn = *(u32 *)(id + 516);
    u8 mdts = id[77];
    c->max_bytes = 512 * PAGE; /* one PRP list page of entries */
    if (mdts && mdts < 20 && (PAGE << mdts) < c->max_bytes)
        c->max_bytes = PAGE << mdts;
    u32 vs = rd32(c, R_VS);
    printk("  nvme%d: %s (NVMe %u.%u) at %s\n", c->index, model[0] ? model : "controller",
           vs >> 16, (vs >> 8) & 0xff, p->name);
    (void)serial;

    /* One I/O queue pair. */
    struct sqe e = { 0 };
    e.cdw0 = A_SET_FEATURES;
    e.cdw10 = 0x07; /* number of queues */
    e.cdw11 = 0;    /* one SQ, one CQ (0-based) */
    admin_cmd(c, &e);
    if (!queue_init(c, &c->io, 1, iodepth))
        return false;
    memset(&e, 0, sizeof(e));
    e.cdw0 = A_CREATE_CQ;
    e.prp1 = (u64)(uintptr_t)c->io.cq;
    e.cdw10 = (u32)(iodepth - 1) << 16 | 1;
    e.cdw11 = 1; /* physically contiguous, no interrupts */
    if (!admin_cmd(c, &e))
        return false;
    memset(&e, 0, sizeof(e));
    e.cdw0 = A_CREATE_SQ;
    e.prp1 = (u64)(uintptr_t)c->io.sq;
    e.cdw10 = (u32)(iodepth - 1) << 16 | 1;
    e.cdw11 = 1u << 16 | 1; /* completions to CQ 1, contiguous */
    if (!admin_cmd(c, &e))
        return false;
    for (int i = 0; i < INFLIGHT; i++) {
        c->prp[i] = page_alloc(1);
        if (!c->prp[i])
            return false;
    }
    nctrls++;
    /* Namespaces: the active list (CNS 2), else 1..NN. */
    u32 *list = page_alloc(1);
    bool have_list = list && identify(c, 2, 0, list) && list[0];
    for (u32 i = 0; have_list ? i < 1024 && list[i] : i < nn && i < 16; i++)
        add_namespace(c, have_list ? list[i] : i + 1, id);
    return true;
}

void nvme_scan(void)
{
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *p = pci_get(i);
        if (p->class_code == 0x01 && p->subclass == 0x08 && p->prog_if == 0x02)
            if (!ctrl_init(p))
                printk("  %s: NVMe controller failed to start\n", p->name);
    }
}
