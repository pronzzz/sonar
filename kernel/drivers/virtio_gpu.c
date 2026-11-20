/*
 * Virtio GPU, 2D mode (virtio 1.x, section 5.7): one host resource backed
 * by a framebuffer in guest memory, shown on scanout 0. The framebuffer
 * console draws into it; flushing copies the changed rectangle to the host
 * (TRANSFER_TO_HOST_2D) and displays it (RESOURCE_FLUSH).
 *
 * Updates do not wait for the host: they go into a ring of command slots
 * and completed slots are reclaimed later. A host may complete a flush only
 * when it next presents a frame (Apple's Virtualization framework with no
 * window open seems to hold them), and waiting would stall all output.
 */
#include "console.h"
#include "fbcon.h"
#include "kernel.h"
#include "shell.h"
#include "mm.h"
#include "string.h"
#include "virtio.h"

enum {
    CMD_GET_DISPLAY_INFO = 0x0100,
    CMD_RESOURCE_CREATE_2D = 0x0101,
    CMD_SET_SCANOUT = 0x0103,
    CMD_RESOURCE_FLUSH = 0x0104,
    CMD_TRANSFER_TO_HOST_2D = 0x0105,
    CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    RESP_OK_NODATA = 0x1100,
    RESP_OK_DISPLAY_INFO = 0x1101,
    FORMAT_B8G8R8X8_UNORM = 2,
    RESOURCE_ID = 1,
};

struct hdr {
    u32 type, flags;
    u64 fence_id;
    u32 ctx_id;
    u8 ring_idx, pad[3];
};
struct rect {
    u32 x, y, w, h;
};
struct display_info {
    struct hdr h;
    struct {
        struct rect r;
        u32 enabled, flags;
    } pmodes[16];
};

struct vgpu {
    struct virtio_dev *vd;
    struct virtq *q;
    struct framebuffer fb;
    union {
        struct hdr h;
        u8 raw[64];
    } *cmd;
    struct hdr *resp;
    /* asynchronous update slots: one transfer + one flush each */
    struct upd {
        u8 xfer[64], flush[64];
        struct hdr r1, r2;
        bool busy;
    } *upd;
    int nupd, next_upd;
    bool dropped; /* an update was skipped: redo the whole screen next time */
};
#define NUPD 8

static bool command(struct vgpu *g, size_t len)
{
    struct vq_buf b[2] = { { g->cmd, (u32)len, false }, { g->resp, sizeof(*g->resp), true } };
    return virtq_run(g->q, b, 2, NULL) && g->resp->type == RESP_OK_NODATA;
}

static u64 st_flush, st_sent, st_done, st_drop, st_catch;

static void reclaim(struct vgpu *g)
{
    void *ck;
    while ((ck = virtq_poll(g->q, NULL)) != NULL) {
        /* The second request's cookie points at its slot's flush command. */
        for (int k = 0; k < g->nupd; k++)
            if (ck == (void *)g->upd[k].flush) {
                g->upd[k].busy = false; /* the flush (second request) finished */
                st_done++;
                break;
            }
    }
}

static void flush(void *ctx, u32 x, u32 y, u32 w, u32 h)
{
    struct vgpu *g = ctx;
    st_flush++;
    reclaim(g);
    struct upd *u = &g->upd[g->next_upd];
    if (u->busy) {
        st_drop++;
        g->dropped = true; /* host is behind: skip, catch up later */
        return;
    }
    if (g->dropped) {
        x = y = 0;
        w = g->fb.width;
        h = g->fb.height;
        g->dropped = false;
    }
    struct {
        struct hdr h;
        struct rect r;
        u64 offset;
        u32 resource_id, pad;
    } *t = (void *)u->xfer;
    memset(t, 0, sizeof(*t));
    t->h.type = CMD_TRANSFER_TO_HOST_2D;
    t->r = (struct rect){ x, y, w, h };
    t->offset = (u64)y * g->fb.stride + (u64)x * 4;
    t->resource_id = RESOURCE_ID;
    struct {
        struct hdr h;
        struct rect r;
        u32 resource_id, pad;
    } *f = (void *)u->flush;
    memset(f, 0, sizeof(*f));
    f->h.type = CMD_RESOURCE_FLUSH;
    f->r = (struct rect){ x, y, w, h };
    f->resource_id = RESOURCE_ID;
    struct vq_buf b1[2] = { { t, sizeof(*t), false }, { &u->r1, sizeof(u->r1), true } };
    struct vq_buf b2[2] = { { f, sizeof(*f), false }, { &u->r2, sizeof(u->r2), true } };
    /* The cookie of the first request is the slot itself (ignored); the
     * second's is u->flush, which frees the slot. */
    if (!virtq_submit(g->q, b1, 2, u)) {
        g->dropped = true;
        return;
    }
    if (!virtq_submit(g->q, b2, 2, u->flush)) {
        g->dropped = true;
        virtq_kick(g->q);
        return;
    }
    u->busy = true;
    st_sent++;
    g->next_upd = (g->next_upd + 1) % g->nupd;
    virtq_kick(g->q);
}

/* An update skipped while the host was busy is redone on the next flush;
 * if no flush follows (a still screen, e.g. the desktop's first frame),
 * this redraws the whole screen once the host has caught up. */
static struct vgpu *gpus[4];
static int ngpus;

static void catch_up(void)
{
    for (int i = 0; i < ngpus; i++) {
        struct vgpu *g = gpus[i];
        if (!g->dropped)
            continue;
        u64 f = console_lock();
        reclaim(g);
        if (g->dropped && !g->upd[g->next_upd].busy) {
            st_catch++;
            flush(g, 0, 0, 0, 0);
        }
        console_unlock(f);
    }
}

bool virtio_gpu_probe(struct virtio_dev *d)
{
    if (!virtio_negotiate(d, 0))
        return false;
    struct vgpu *g = kzalloc(sizeof(*g));
    struct display_info *di = kzalloc(sizeof(*di));
    if (!g || !di)
        goto fail;
    g->vd = d;
    g->q = virtio_setup_queue(d, 0, 64);
    g->nupd = NUPD;
    g->upd = page_alloc(ALIGN_UP(sizeof(struct upd) * NUPD, PAGE_SIZE) / PAGE_SIZE);
    g->cmd = kzalloc(sizeof(*g->cmd));
    g->resp = kzalloc(sizeof(*g->resp));
    if (!g->q || !g->cmd || !g->resp || !g->upd)
        goto fail;
    virtq_interrupts(g->q, false); /* completions are polled */
    virtio_driver_ok(d);

    /* which display size does the host want? */
    memset(g->cmd, 0, sizeof(*g->cmd));
    g->cmd->h.type = CMD_GET_DISPLAY_INFO;
    struct vq_buf b[2] = { { g->cmd, sizeof(struct hdr), false }, { di, sizeof(*di), true } };
    u32 w = 1280, h = 800;
    if (virtq_run(g->q, b, 2, NULL) && di->h.type == RESP_OK_DISPLAY_INFO && di->pmodes[0].enabled &&
        di->pmodes[0].r.w >= 320 && di->pmodes[0].r.h >= 200) {
        w = MIN(di->pmodes[0].r.w, 3840u);
        h = MIN(di->pmodes[0].r.h, 2400u);
    }
    kfree(di);
    di = NULL;

    u64 bytes = (u64)w * h * 4;
    u8 *pixels = page_alloc(ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE);
    if (!pixels)
        goto fail;

    struct {
        struct hdr h;
        u32 resource_id, format, width, height;
    } *cr = (void *)g->cmd;
    memset(cr, 0, sizeof(*cr));
    cr->h.type = CMD_RESOURCE_CREATE_2D;
    cr->resource_id = RESOURCE_ID;
    cr->format = FORMAT_B8G8R8X8_UNORM;
    cr->width = w;
    cr->height = h;
    if (!command(g, sizeof(*cr)))
        goto fail_cmd;

    struct {
        struct hdr h;
        u32 resource_id, nr_entries;
        u64 addr;
        u32 length, pad;
    } *ab = (void *)g->cmd;
    memset(ab, 0, sizeof(*ab));
    ab->h.type = CMD_RESOURCE_ATTACH_BACKING;
    ab->resource_id = RESOURCE_ID;
    ab->nr_entries = 1;
    ab->addr = (u64)(uintptr_t)pixels; /* identity mapped, physically contiguous */
    ab->length = (u32)bytes;
    if (!command(g, sizeof(*ab)))
        goto fail_cmd;

    struct {
        struct hdr h;
        struct rect r;
        u32 scanout_id, resource_id;
    } *ss = (void *)g->cmd;
    memset(ss, 0, sizeof(*ss));
    ss->h.type = CMD_SET_SCANOUT;
    ss->r = (struct rect){ 0, 0, w, h };
    ss->scanout_id = 0;
    ss->resource_id = RESOURCE_ID;
    if (!command(g, sizeof(*ss)))
        goto fail_cmd;

    d->priv = g;
    g->fb = (struct framebuffer){ "gpu0", pixels, w, h, w * 4, false, flush, g };
    fbcon_attach(&g->fb);
    if (ngpus < 4) {
        gpus[ngpus++] = g;
        if (ngpus == 1)
            console_add_idle_hook(catch_up);
    }
    return true;

fail_cmd:
    printk("  virtio-gpu: the device rejected a setup command (response %#x)\n",
           g->resp ? g->resp->type : 0);
    return true; /* the queue is live: keep the device claimed, without a display */
fail:
    kfree(di);
    virtio_fail(d);
    return false;
}

static int cmd_gpustat(int argc, char **argv)
{
    (void)argc, (void)argv;
    for (int i = 0; i < ngpus; i++) {
        struct vgpu *g = gpus[i];
        int busy = 0;
        for (int k = 0; k < g->nupd; k++)
            busy += g->upd[k].busy;
        u32 sum = 0;
        for (u32 y = 0; y < 40 && y < g->fb.height; y++)
            for (u32 x = 0; x < g->fb.width; x++)
                sum = sum * 31 + ((u32 *)(g->fb.base + y * g->fb.stride))[x];
        printk("%s: %ux%u flush calls %llu, sent %llu, done %llu, dropped %llu, catch-ups %llu, "
               "busy slots %d/%d, pending redo %d, top rows %#x, pixel(5,5) %#x\n",
               g->fb.name, g->fb.width, g->fb.height, (unsigned long long)st_flush,
               (unsigned long long)st_sent, (unsigned long long)st_done,
               (unsigned long long)st_drop, (unsigned long long)st_catch, busy, g->nupd,
               g->dropped, sum, ((u32 *)(g->fb.base + 5 * g->fb.stride))[5]);
    }
    return 0;
}
SHELL_COMMAND(gpustat, "gpustat", "gpustat", "show virtio-gpu update counters", cmd_gpustat);
