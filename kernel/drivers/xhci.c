/*
 * xHCI USB host controller ("eXtensible Host Controller Interface for
 * Universal Serial Bus", Intel, revision 1.2): the USB controller of current
 * PCs and laptops, and of Apple's Virtualization framework, whose keyboard
 * and pointer are USB devices.
 *
 * Devices on the root ports are enumerated at boot and when plugged in
 * (port status change events), including behind USB 2 hubs. Each configured
 * device is offered to the class drivers (HID). Events are handled from the
 * controller's MSI-X interrupt when there is one, and from the idle loop.
 */
#include "arch.h"
#include "console.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "mmio.h"
#include "pci.h"
#include "smp.h"
#include "string.h"
#include "usb.h"

#define PAGE 4096u
#define RING_TRBS 256 /* one page; the last entry is the link back */
#define MAX_SLOTS 32
#define MAX_PORTS 32

/* TRB types (section 6.4.6) */
enum {
    T_NORMAL = 1, T_SETUP = 2, T_DATA = 3, T_STATUS = 4, T_LINK = 6,
    T_ENABLE_SLOT = 9, T_DISABLE_SLOT = 10, T_ADDRESS_DEVICE = 11, T_CONFIGURE_EP = 12,
    T_EVALUATE_CTX = 13, T_RESET_EP = 14, T_SET_TR_DEQ = 16,
    T_TRANSFER_EVENT = 32, T_COMMAND_DONE = 33, T_PORT_CHANGE = 34,
};
#define TRB_CYCLE (1u << 0)
#define TRB_TC (1u << 1)  /* link: toggle cycle */
#define TRB_ISP (1u << 2)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_DIR_IN (1u << 16)
#define CC_SUCCESS 1
#define CC_SHORT 13

/* Operational registers (section 5.4) */
enum { O_USBCMD = 0x00, O_USBSTS = 0x04, O_CRCR = 0x18, O_DCBAAP = 0x30, O_CONFIG = 0x38,
       O_PORTSC = 0x400 };
#define CMD_RUN (1u << 0)
#define CMD_RESET (1u << 1)
#define CMD_INTE (1u << 2)
#define STS_HALTED (1u << 0)
#define STS_EINT (1u << 3)
#define STS_CNR (1u << 11)
/* PORTSC bits (section 5.4.8) */
#define PS_CCS (1u << 0)
#define PS_PED (1u << 1)
#define PS_PR (1u << 4)
#define PS_PP (1u << 9)
#define PS_SPEED(v) (((v) >> 10) & 0xf)
#define PS_CHANGES (0x7fu << 17) /* CSC PEC WRC OCC PRC PLC CEC: write 1 to clear */
#define PS_PRC (1u << 21)
/* writing PED or PR back would disable or reset the port */
#define PS_NEUTRAL(v) ((v) & ~(PS_PED | PS_PR | (1u << 16) | PS_CHANGES))

struct trb {
    u64 param;
    u32 status;
    u32 control;
};

struct ring {
    struct trb *trb;
    u32 enq;
    u32 cycle;
};

struct xhc;

struct ep {
    struct ring ring;
    usb_report_fn fn;
    void *ctx;
    u8 *buf;
    u32 len;
};

struct usb_dev {
    struct xhc *hc;
    int slot, port, speed;
    u8 *out_ctx, *in_ctx;
    struct ring ep0;
    u16 mps0;
    u8 *dma;        /* one page for control data */
    int max_dci;    /* highest endpoint configured */
    struct ep *eps[32];
    volatile bool ctl_done;
    volatile u32 ctl_code;
    char name[24];
    bool gone;
    /* topology */
    struct usb_dev *parent; /* hub, or NULL on a root port */
    int depth;              /* hubs between it and the root port */
    u32 route;              /* route string (section 8.9) */
    int tt_slot, tt_port;   /* transaction translator for full/low speed behind a USB 2 hub */
    /* hubs */
    int hub_ports;
    struct usb_dev *child[16];
    volatile u32 hub_pending; /* ports with changes, from the status endpoint */
};

struct xhc {
    struct pci_dev *pci;
    uintptr_t cap, op, rt, db;
    int index, nports, nslots, csz;
    u64 *dcbaa;
    struct ring cmd;
    struct trb *evt;
    u32 evt_deq, evt_cycle;
    u64 *erst;
    volatile bool cmd_done;
    volatile u32 cmd_code, cmd_slot;
    u64 cmd_wait;
    struct usb_dev *slots[MAX_SLOTS + 1];
    struct usb_dev *ports[MAX_PORTS + 1];
    volatile u32 port_pending; /* bitmask: connection changed, enumerate */
    struct usb_dev *hubs[16];
    int nhubs;
    spinlock_t lock;
};

static struct xhc *hcs[4];
static int nhcs;

static u32 op_rd(struct xhc *h, u32 off) { return mmio_read32(h->op + off); }
static void op_wr(struct xhc *h, u32 off, u32 v) { mmio_write32(h->op + off, v); }
static u32 portsc_off(int port) { return O_PORTSC + 0x10 * (u32)(port - 1); }

static bool wait_bits(uintptr_t reg, u32 mask, u32 want, u64 ms)
{
    u64 end = time_ms() + ms;
    while ((mmio_read32(reg) & mask) != want) {
        if (time_ms() > end)
            return false;
        __asm__ volatile("yield");
    }
    return true;
}

/* --- rings ---------------------------------------------------------- */

static bool ring_init(struct ring *r)
{
    r->trb = page_alloc(1);
    if (!r->trb)
        return false;
    r->enq = 0;
    r->cycle = 1;
    struct trb *link = &r->trb[RING_TRBS - 1];
    link->param = (u64)(uintptr_t)r->trb;
    return true;
}

/* Queue one TRB; returns its address (events refer to TRBs by address). */
static u64 ring_push(struct ring *r, u64 param, u32 status, u32 control)
{
    struct trb *t = &r->trb[r->enq];
    t->param = param;
    t->status = status;
    dmb(ish);
    t->control = (control & ~TRB_CYCLE) | r->cycle;
    u64 addr = (u64)(uintptr_t)t;
    if (++r->enq == RING_TRBS - 1) {
        struct trb *link = &r->trb[RING_TRBS - 1];
        dmb(ish);
        link->control = (u32)T_LINK << 10 | TRB_TC | r->cycle;
        r->cycle ^= 1;
        r->enq = 0;
    }
    return addr;
}

static void doorbell(struct xhc *h, int slot, u32 target)
{
    dsb(sy);
    mmio_write32(h->db + 4 * (u32)slot, target);
}

/* --- events ----------------------------------------------------------- */

static void transfer_event(struct xhc *h, struct trb *e)
{
    int slot = (int)(e->control >> 24), dci = (int)((e->control >> 16) & 0x1f);
    u32 code = e->status >> 24;
    struct usb_dev *d = slot <= MAX_SLOTS ? h->slots[slot] : NULL;
    if (!d)
        return;
    if (dci == 1) {
        d->ctl_code = code;
        d->ctl_done = true;
        return;
    }
    struct ep *ep = d->eps[dci];
    if (!ep || d->gone)
        return;
    if (code == CC_SUCCESS || code == CC_SHORT) {
        u32 got = ep->len - (e->status & 0xffffff);
        ep->fn(ep->ctx, ep->buf, got);
    }
    if (code == CC_SUCCESS || code == CC_SHORT) {
        ring_push(&ep->ring, (u64)(uintptr_t)ep->buf, ep->len,
                  (u32)T_NORMAL << 10 | TRB_IOC | TRB_ISP);
        doorbell(h, slot, (u32)dci);
    }
    /* other codes (stall, babble, ...) halt the endpoint: stop polling it */
}

static void poll_events(struct xhc *h)
{
    u64 f = spin_lock_irqsave(&h->lock);
    bool any = false;
    for (;;) {
        struct trb *e = &h->evt[h->evt_deq];
        if ((e->control & TRB_CYCLE) != h->evt_cycle)
            break;
        dmb(ish);
        any = true;
        switch ((e->control >> 10) & 0x3f) {
        case T_COMMAND_DONE:
            if (e->param == h->cmd_wait) {
                h->cmd_code = e->status >> 24;
                h->cmd_slot = e->control >> 24;
                h->cmd_done = true;
            }
            break;
        case T_TRANSFER_EVENT:
            transfer_event(h, e);
            break;
        case T_PORT_CHANGE: {
            int port = (int)(e->param >> 24) & 0xff;
            if (port >= 1 && port <= h->nports)
                h->port_pending |= 1u << port;
            break;
        }
        }
        if (++h->evt_deq == RING_TRBS) {
            h->evt_deq = 0;
            h->evt_cycle ^= 1;
        }
    }
    if (any) /* dequeue pointer, clearing Event Handler Busy */
        mmio_write64(h->rt + 0x38, (u64)(uintptr_t)&h->evt[h->evt_deq] | (1u << 3));
    spin_unlock_irqrestore(&h->lock, f);
}

static bool command(struct xhc *h, u64 param, u32 control, u32 *slot_out)
{
    u64 f = spin_lock_irqsave(&h->lock);
    h->cmd_done = false;
    h->cmd_wait = ring_push(&h->cmd, param, 0, control);
    spin_unlock_irqrestore(&h->lock, f);
    doorbell(h, 0, 0);
    u64 end = time_ms() + 1000;
    while (!h->cmd_done) {
        poll_events(h);
        if (time_ms() > end)
            return false;
    }
    if (slot_out)
        *slot_out = h->cmd_slot;
    return h->cmd_code == CC_SUCCESS;
}

/* --- contexts (section 6.2) ---------------------------------------- */

static u32 *in_ctx(struct usb_dev *d, int i) { return (u32 *)(d->in_ctx + (u32)i * d->hc->csz); }
static u32 *in_slot(struct usb_dev *d) { return in_ctx(d, 1); }
static u32 *in_ep(struct usb_dev *d, int dci) { return in_ctx(d, 1 + dci); }
static u32 *out_slot(struct usb_dev *d) { return (u32 *)d->out_ctx; }

static void ctx_clear(struct usb_dev *d)
{
    memset(d->in_ctx, 0, (size_t)33 * d->hc->csz);
}

/* --- control transfers --------------------------------------------- */

static bool reset_endpoint(struct usb_dev *d, int dci, struct ring *r)
{
    struct xhc *h = d->hc;
    u32 sd = (u32)d->slot << 24 | (u32)dci << 16;
    command(h, 0, (u32)T_RESET_EP << 10 | sd, NULL);
    return command(h, (u64)(uintptr_t)&r->trb[r->enq] | r->cycle, (u32)T_SET_TR_DEQ << 10 | sd,
                   NULL);
}

bool usb_control(struct usb_dev *d, u8 type, u8 req, u16 value, u16 index, void *data, u16 len)
{
    if (d->gone || len > PAGE)
        return false;
    bool in = (type & USB_DIR_IN) != 0;
    if (!in && len)
        memcpy(d->dma, data, len);
    u64 setup = (u64)type | (u64)req << 8 | (u64)value << 16 | (u64)index << 32 |
                (u64)len << 48;
    struct xhc *h = d->hc;
    u64 f = spin_lock_irqsave(&h->lock);
    d->ctl_done = false;
    u32 trt = !len ? 0 : in ? 3u : 2u;
    ring_push(&d->ep0, setup, 8, (u32)T_SETUP << 10 | TRB_IDT | trt << 16);
    if (len)
        ring_push(&d->ep0, (u64)(uintptr_t)d->dma, len,
                  (u32)T_DATA << 10 | (in ? TRB_DIR_IN : 0));
    ring_push(&d->ep0, 0, 0, (u32)T_STATUS << 10 | TRB_IOC | (in && len ? 0 : TRB_DIR_IN));
    spin_unlock_irqrestore(&h->lock, f);
    doorbell(h, d->slot, 1);
    u64 end = time_ms() + 1000;
    while (!d->ctl_done) {
        poll_events(h);
        if (time_ms() > end)
            return false;
    }
    if (d->ctl_code != CC_SUCCESS && d->ctl_code != CC_SHORT) {
        /* A stall (unsupported request) halts endpoint 0; recover it. */
        reset_endpoint(d, 1, &d->ep0);
        return false;
    }
    if (in && len)
        memcpy(data, d->dma, len);
    return true;
}

const char *usb_name(struct usb_dev *d) { return d->name; }

/* --- interrupt endpoints ------------------------------------------- */

/* Endpoint context interval: 2^n x 125 us (section 6.2.3.6). */
static u32 ep_interval(int speed, u8 binterval)
{
    if (speed >= 3) /* high speed and up: bInterval is already an exponent + 1 */
        return binterval ? (u32)MIN(binterval - 1, 15) : 0;
    u32 frames = binterval ? binterval : 1, n = 3; /* full/low speed: milliseconds */
    while ((1u << (n + 1)) <= frames * 8 && n < 10)
        n++;
    return n;
}

bool usb_interrupt_in(struct usb_dev *d, u8 ep_addr, u16 mps, u8 interval, usb_report_fn fn,
                      void *ctx)
{
    struct xhc *h = d->hc;
    int dci = (ep_addr & 0xf) * 2 + 1;
    struct ep *ep = kzalloc(sizeof(*ep));
    if (!ep || !ring_init(&ep->ring))
        return false;
    ep->fn = fn;
    ep->ctx = ctx;
    ep->len = mps;
    ep->buf = page_alloc(1);
    if (!ep->buf)
        return false;
    if (dci > d->max_dci)
        d->max_dci = dci;
    ctx_clear(d);
    in_ctx(d, 0)[1] = 1u | 1u << dci; /* add: slot, this endpoint */
    memcpy(in_slot(d), out_slot(d), (size_t)h->csz);
    in_slot(d)[0] = (in_slot(d)[0] & ~(0x1fu << 27)) | (u32)d->max_dci << 27;
    if (d->hub_ports) { /* Hub flag and port count (section 6.2.2) */
        in_slot(d)[0] |= 1u << 26;
        in_slot(d)[1] = (in_slot(d)[1] & 0x00ffffffu) | (u32)d->hub_ports << 24;
    }
    u32 *e = in_ep(d, dci);
    e[0] = ep_interval(d->speed, interval) << 16;
    e[1] = 3u << 1 | 7u << 3 | (u32)mps << 16; /* 3 retries, interrupt IN */
    u64 deq = (u64)(uintptr_t)ep->ring.trb | 1;
    e[2] = (u32)deq;
    e[3] = (u32)(deq >> 32);
    e[4] = (u32)mps | (u32)mps << 16; /* average TRB length, max ESIT payload */
    if (!command(h, (u64)(uintptr_t)d->in_ctx, (u32)T_CONFIGURE_EP << 10 | (u32)d->slot << 24,
                 NULL))
        return false;
    d->eps[dci] = ep;
    u64 f = spin_lock_irqsave(&h->lock);
    ring_push(&ep->ring, (u64)(uintptr_t)ep->buf, ep->len, (u32)T_NORMAL << 10 | TRB_IOC | TRB_ISP);
    spin_unlock_irqrestore(&h->lock, f);
    doorbell(h, d->slot, (u32)dci);
    return true;
}

/* --- enumeration (USB 2.0 section 9.1.2; xHCI section 4.3) ------------ */

static const char *speed_name(int s)
{
    switch (s) {
    case 1: return "full speed";
    case 2: return "low speed";
    case 3: return "high speed";
    case 4: return "SuperSpeed";
    case 5: return "SuperSpeed+";
    }
    return "unknown speed";
}

static void detach(struct usb_dev *d)
{
    struct xhc *h = d->hc;
    for (int p = 1; p <= d->hub_ports; p++)
        if (d->child[p]) {
            detach(d->child[p]);
            d->child[p] = NULL;
        }
    d->gone = true;
    command(h, 0, (u32)T_DISABLE_SLOT << 10 | (u32)d->slot << 24, NULL);
    h->slots[d->slot] = NULL;
    for (int i = 0; i < h->nhubs; i++)
        if (h->hubs[i] == d)
            h->hubs[i] = h->hubs[--h->nhubs];
    printk("%s: disconnected\n", d->name);
    /* the device's memory is kept: a late event may still name it */
}

static bool port_reset(struct xhc *h, int port)
{
    uintptr_t reg = h->op + portsc_off(port);
    u32 v = mmio_read32(reg);
    if (!(v & PS_CCS))
        return false;
    if (!(v & PS_PED)) { /* USB 2 ports need a reset; USB 3 ones enable themselves */
        mmio_write32(reg, PS_NEUTRAL(v) | PS_PR);
        if (!wait_bits(reg, PS_PRC, PS_PRC, 500))
            return false;
        delay_ms(10); /* reset recovery */
    }
    v = mmio_read32(reg);
    mmio_write32(reg, PS_NEUTRAL(v) | (v & PS_CHANGES));
    return (mmio_read32(reg) & (PS_CCS | PS_PED)) == (PS_CCS | PS_PED);
}

static void hub_init(struct usb_dev *d, const u8 *cfg, u32 len);

/* Address a device that has just been reset, read its descriptors, set its
 * configuration and hand it to a driver. hub/hub_port: where it is plugged
 * in (NULL for a root port). */
static struct usb_dev *enumerate(struct xhc *h, struct usb_dev *hub, int hub_port, int root_port,
                                 int speed)
{
    u32 slot;
    if (!command(h, 0, (u32)T_ENABLE_SLOT << 10, &slot) || !slot || slot > (u32)h->nslots)
        return NULL;
    struct usb_dev *d = kzalloc(sizeof(*d));
    d->hc = h;
    d->slot = (int)slot;
    d->port = root_port;
    d->speed = speed;
    d->parent = hub;
    if (hub) {
        d->depth = hub->depth + 1;
        d->route = hub->route | (u32)MIN(hub_port, 15) << (4 * hub->depth);
        if (hub->speed == 3 && speed < 3) /* this hub translates for it */
            d->tt_slot = hub->slot, d->tt_port = hub_port;
        else
            d->tt_slot = hub->tt_slot, d->tt_port = hub->tt_port;
        snprintf(d->name, sizeof(d->name), "%s.%d", hub->name, hub_port);
    } else {
        snprintf(d->name, sizeof(d->name), "usb%d.%d", h->index, root_port);
    }
    d->out_ctx = page_alloc(1);
    d->in_ctx = page_alloc(2);
    d->dma = page_alloc(1);
    if (!d->out_ctx || !d->in_ctx || !d->dma || !ring_init(&d->ep0))
        return NULL;
    h->dcbaa[slot] = (u64)(uintptr_t)d->out_ctx;
    h->slots[slot] = d;
    d->mps0 = speed >= 4 ? 512 : speed == 3 ? 64 : 8;
    ctx_clear(d);
    in_ctx(d, 0)[1] = 1u | 2u; /* add slot and endpoint 0 */
    in_slot(d)[0] = d->route | (u32)speed << 20 | 1u << 27;
    in_slot(d)[1] = (u32)root_port << 16;
    in_slot(d)[2] = (u32)d->tt_slot | (u32)d->tt_port << 8;
    u32 *e = in_ep(d, 1);
    e[1] = 3u << 1 | 4u << 3 | (u32)d->mps0 << 16; /* control */
    u64 deq = (u64)(uintptr_t)d->ep0.trb | 1;
    e[2] = (u32)deq;
    e[3] = (u32)(deq >> 32);
    e[4] = 8;
    if (!command(h, (u64)(uintptr_t)d->in_ctx, (u32)T_ADDRESS_DEVICE << 10 | slot << 24, NULL)) {
        printk("%s: device did not take an address\n", d->name);
        h->slots[slot] = NULL;
        return NULL;
    }
    delay_ms(2);
    u8 dev[18];
    if (!usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, 0x0100, 0, dev, 8)) {
        printk("%s: no device descriptor\n", d->name);
        return d;
    }
    u16 mps = dev[7];
    if (speed >= 4)
        mps = (u16)(1u << dev[7]);
    if (mps && mps != d->mps0) {
        d->mps0 = mps;
        ctx_clear(d);
        in_ctx(d, 0)[1] = 2u;
        in_ep(d, 1)[1] = 3u << 1 | 4u << 3 | (u32)mps << 16;
        command(h, (u64)(uintptr_t)d->in_ctx, (u32)T_EVALUATE_CTX << 10 | slot << 24, NULL);
    }
    if (!usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, 0x0100, 0, dev, 18))
        return d;
    u16 vid = (u16)(dev[8] | dev[9] << 8), pid = (u16)(dev[10] | dev[11] << 8);
    u8 cfg9[9];
    if (!usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, 0x0200, 0, cfg9, 9))
        return d;
    u16 total = (u16)(cfg9[2] | cfg9[3] << 8);
    if (total < 9 || total > PAGE)
        return d;
    u8 *cfg = kmalloc(total);
    if (!cfg || !usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, 0x0200, 0, cfg, total))
        return d;
    if (!usb_control(d, 0, USB_REQ_SET_CONFIGURATION, cfg[5], 0, NULL, 0))
        return d;
    printk("%s: device %04x:%04x, %s, class %02x\n", d->name, vid, pid, speed_name(speed),
           dev[4]);
    if (dev[4] == 9)
        hub_init(d, cfg, total);
    else if (!usb_hid_probe(d, cfg, total))
        printk("%s: no driver for this device\n", d->name);
    return d;
}

static void port_attach(struct xhc *h, int port)
{
    if (!port_reset(h, port))
        return;
    int speed = (int)PS_SPEED(mmio_read32(h->op + portsc_off(port)));
    h->ports[port] = enumerate(h, NULL, 0, port, speed);
}

/* --- hubs (USB 2.0 chapter 11) ---------------------------------------- */

enum { HUB_GET_STATUS = 0, HUB_CLEAR_FEATURE = 1, HUB_SET_FEATURE = 3 };
enum { F_PORT_RESET = 4, F_PORT_POWER = 8, F_C_CONNECTION = 16, F_C_ENABLE = 17,
       F_C_RESET = 20 };
#define HUB_PORT_TYPE (USB_TYPE_CLASS | 0x03) /* class request to "other" (a port) */

static bool hub_status(struct usb_dev *hub, int port, u16 *status, u16 *change)
{
    u8 st[4];
    if (!usb_control(hub, USB_DIR_IN | HUB_PORT_TYPE, HUB_GET_STATUS, 0, (u16)port, st, 4))
        return false;
    *status = (u16)(st[0] | st[1] << 8);
    *change = (u16)(st[2] | st[3] << 8);
    return true;
}

static void hub_port(struct usb_dev *hub, int port)
{
    u16 st, ch;
    if (!hub_status(hub, port, &st, &ch))
        return;
    if (ch & 1)
        usb_control(hub, HUB_PORT_TYPE, HUB_CLEAR_FEATURE, F_C_CONNECTION, (u16)port, NULL, 0);
    if (ch & 2)
        usb_control(hub, HUB_PORT_TYPE, HUB_CLEAR_FEATURE, F_C_ENABLE, (u16)port, NULL, 0);
    if (ch & 0x10)
        usb_control(hub, HUB_PORT_TYPE, HUB_CLEAR_FEATURE, F_C_RESET, (u16)port, NULL, 0);
    bool connected = st & 1;
    if (!connected && hub->child[port]) {
        detach(hub->child[port]);
        hub->child[port] = NULL;
        return;
    }
    if (!connected || hub->child[port])
        return;
    usb_control(hub, HUB_PORT_TYPE, HUB_SET_FEATURE, F_PORT_RESET, (u16)port, NULL, 0);
    u64 end = time_ms() + 500;
    do {
        delay_ms(10);
        if (!hub_status(hub, port, &st, &ch))
            return;
    } while (!(ch & 0x10) && time_ms() < end);
    usb_control(hub, HUB_PORT_TYPE, HUB_CLEAR_FEATURE, F_C_RESET, (u16)port, NULL, 0);
    if (!(st & 2)) /* not enabled after the reset */
        return;
    delay_ms(10);
    int speed = (st & 0x200) ? 2 : (st & 0x400) ? 3 : 1;
    hub->child[port] = enumerate(hub->hc, hub, port, hub->port, speed);
}

static void hub_report(void *ctx, const u8 *data, u32 len)
{
    struct usb_dev *hub = ctx;
    u32 bits = 0;
    for (u32 i = 0; i < len && i < 4; i++)
        bits |= (u32)data[i] << (8 * i);
    hub->hub_pending |= bits & ~1u; /* bit 0 is the hub itself */
}

static void hub_init(struct usb_dev *d, const u8 *cfg, u32 len)
{
    struct xhc *h = d->hc;
    if (d->speed >= 4) {
        printk("%s: USB 3 hub (not supported yet; its USB 2 side works)\n", d->name);
        return;
    }
    if (d->depth >= 5 || h->nhubs >= 16) {
        printk("%s: hub nested too deep\n", d->name);
        return;
    }
    u8 desc[16];
    if (!usb_control(d, USB_DIR_IN | USB_TYPE_CLASS, USB_REQ_GET_DESCRIPTOR, 0x2900, 0, desc, 9))
        return;
    d->hub_ports = MIN(desc[2], 15);
    /* the status-change endpoint: the first interrupt IN endpoint */
    u8 ep = 0, interval = 12;
    u16 mps = 1;
    for (u32 i = 0; i + 2 <= len && cfg[i] >= 2 && i + cfg[i] <= len; i += cfg[i])
        if (cfg[i + 1] == 5 && cfg[i] >= 7 && (cfg[i + 3] & 3) == 3 && (cfg[i + 2] & 0x80)) {
            ep = cfg[i + 2];
            mps = (u16)((cfg[i + 4] | cfg[i + 5] << 8) & 0x7ff);
            interval = cfg[i + 6];
            break;
        }
    printk("%s: USB hub, %d ports\n", d->name, d->hub_ports);
    if (!ep || !usb_interrupt_in(d, ep, mps, interval, hub_report, d))
        return;
    h->hubs[h->nhubs++] = d;
    for (int p = 1; p <= d->hub_ports; p++)
        usb_control(d, HUB_PORT_TYPE, HUB_SET_FEATURE, F_PORT_POWER, (u16)p, NULL, 0);
    delay_ms(MAX(2u * desc[5], 20u) + 50);
    for (int p = 1; p <= d->hub_ports; p++)
        hub_port(d, p);
    d->hub_pending = 0;
}

/* Connection changes found by the event handler, handled outside it. */
static void xhci_idle(void)
{
    for (int i = 0; i < nhcs; i++) {
        struct xhc *h = hcs[i];
        poll_events(h);
        while (h->port_pending) {
            int port = __builtin_ctz(h->port_pending);
            h->port_pending &= ~(1u << port);
            uintptr_t reg = h->op + portsc_off(port);
            u32 v = mmio_read32(reg);
            mmio_write32(reg, PS_NEUTRAL(v) | (v & PS_CHANGES & ~PS_PRC));
            if (!(v & PS_CCS) && h->ports[port]) {
                detach(h->ports[port]);
                h->ports[port] = NULL;
            } else if ((v & PS_CCS) && !h->ports[port] && (v & (1u << 17))) {
                port_attach(h, port);
            }
        }
        for (int k = 0; k < h->nhubs; k++) {
            struct usb_dev *hub = h->hubs[k];
            while (hub->hub_pending && !hub->gone) {
                int port = __builtin_ctz(hub->hub_pending);
                hub->hub_pending &= ~(1u << port);
                if (port <= hub->hub_ports)
                    hub_port(hub, port);
            }
        }
    }
}

static void xhci_irq(int irq, void *ctx)
{
    struct xhc *h = ctx;
    op_wr(h, O_USBSTS, STS_EINT);
    mmio_write32(h->rt + 0x20, mmio_read32(h->rt + 0x20) | 1); /* IMAN: clear pending */
    poll_events(h);
}

/* --- controller ----------------------------------------------------- */

/* Take the controller from the firmware (section 7.1.1). */
static void bios_handoff(struct xhc *h)
{
    u32 off = (mmio_read32(h->cap + 0x10) >> 16) << 2;
    for (int n = 0; off && n < 64; n++) {
        uintptr_t c = h->cap + off;
        u32 v = mmio_read32(c);
        if ((v & 0xff) == 1) {
            mmio_write32(c, v | 1u << 24);
            if (!wait_bits(c, 1u << 16, 0, 1000))
                printk("  usb%d: firmware kept the controller; taking it anyway\n", h->index);
            mmio_write32(c + 4, mmio_read32(c + 4) & 0xffff1fee); /* no SMIs */
            return;
        }
        u32 next = (v >> 8) & 0xff;
        if (!next)
            return;
        off += next << 2;
    }
}

static bool hc_init(struct pci_dev *p)
{
    struct xhc *h = kzalloc(sizeof(*h));
    h->pci = p;
    h->index = nhcs + 1;
    pci_enable(p);
    h->cap = pci_map_bar(p, 0);
    if (!h->cap)
        return false;
    u32 caplen = mmio_read8(h->cap);
    u32 hcs1 = mmio_read32(h->cap + 4), hcs2 = mmio_read32(h->cap + 8);
    u32 hcc1 = mmio_read32(h->cap + 0x10);
    h->op = h->cap + caplen;
    h->db = h->cap + (mmio_read32(h->cap + 0x14) & ~3u);
    h->rt = h->cap + (mmio_read32(h->cap + 0x18) & ~0x1fu);
    h->nslots = (int)MIN(hcs1 & 0xff, (u32)MAX_SLOTS);
    h->nports = (int)MIN((hcs1 >> 24) & 0xff, (u32)MAX_PORTS);
    h->csz = (hcc1 & (1u << 2)) ? 64 : 32;
    if (!(hcc1 & 1) && (u64)(uintptr_t)h > 0xffffffffull) {
        printk("  usb%d: controller is limited to 32-bit addresses; skipped\n", h->index);
        return false;
    }
    bios_handoff(h);
    /* Stop, then reset. */
    op_wr(h, O_USBCMD, op_rd(h, O_USBCMD) & ~CMD_RUN);
    if (!wait_bits(h->op + O_USBSTS, STS_HALTED, STS_HALTED, 100))
        return false;
    op_wr(h, O_USBCMD, CMD_RESET);
    if (!wait_bits(h->op + O_USBCMD, CMD_RESET, 0, 1000) ||
        !wait_bits(h->op + O_USBSTS, STS_CNR, 0, 1000)) {
        printk("  usb%d: controller reset failed\n", h->index);
        return false;
    }
    op_wr(h, O_CONFIG, (u32)h->nslots);
    h->dcbaa = page_alloc(1);
    u32 nscratch = ((hcs2 >> 21) & 0x1f) << 5 | (hcs2 >> 27);
    if (!h->dcbaa || !ring_init(&h->cmd))
        return false;
    if (nscratch) {
        u64 *arr = page_alloc(ALIGN_UP(nscratch * 8, PAGE) / PAGE);
        if (!arr)
            return false;
        for (u32 i = 0; i < nscratch; i++) {
            void *pg = page_alloc(1);
            if (!pg)
                return false;
            arr[i] = (u64)(uintptr_t)pg;
        }
        h->dcbaa[0] = (u64)(uintptr_t)arr;
    }
    mmio_write64(h->op + O_DCBAAP, (u64)(uintptr_t)h->dcbaa);
    mmio_write64(h->op + O_CRCR, (u64)(uintptr_t)h->cmd.trb | 1);
    /* Event ring: one segment for interrupter 0. */
    h->evt = page_alloc(1);
    h->erst = page_alloc(1);
    if (!h->evt || !h->erst)
        return false;
    h->evt_cycle = 1;
    h->erst[0] = (u64)(uintptr_t)h->evt;
    h->erst[1] = RING_TRBS;
    mmio_write32(h->rt + 0x28, 1);                              /* ERSTSZ */
    mmio_write64(h->rt + 0x38, (u64)(uintptr_t)h->evt);         /* ERDP */
    mmio_write64(h->rt + 0x30, (u64)(uintptr_t)h->erst);        /* ERSTBA */
    void *ctx = h;
    bool msi = pci_msix_setup(p, 1, xhci_irq, &ctx) == 1;
    if (msi) {
        mmio_write32(h->rt + 0x24, 4000);                       /* IMOD: 1 ms */
        mmio_write32(h->rt + 0x20, 3);                          /* IMAN: enable */
    }
    op_wr(h, O_USBCMD, CMD_RUN | (msi ? CMD_INTE : 0));
    if (!wait_bits(h->op + O_USBSTS, STS_HALTED, 0, 100))
        return false;
    if (hcc1 & (1u << 3)) /* port power control */
        for (int port = 1; port <= h->nports; port++) {
            uintptr_t reg = h->op + portsc_off(port);
            u32 v = mmio_read32(reg);
            if (!(v & PS_PP))
                mmio_write32(reg, PS_NEUTRAL(v) | PS_PP);
        }
    u16 ver = (u16)(mmio_read32(h->cap) >> 16); /* some controllers allow only 32-bit reads */
    printk("  usb%d: xHCI %x.%x controller at %s, %d ports%s\n", h->index, ver >> 8,
           (ver >> 4) & 0xf, p->name, h->nports, msi ? "" : " (polled)");
    hcs[nhcs++] = h;
    delay_ms(50); /* let connected devices show up */
    for (int port = 1; port <= h->nports; port++)
        if (mmio_read32(h->op + portsc_off(port)) & PS_CCS)
            port_attach(h, port);
    h->port_pending = 0;
    return true;
}

void xhci_scan(void)
{
    for (int i = 0; i < pci_count() && nhcs < 4; i++) {
        struct pci_dev *p = pci_get(i);
        if (p->class_code == 0x0c && p->subclass == 0x03 && p->prog_if == 0x30)
            if (!hc_init(p))
                printk("  %s: USB controller failed to start\n", p->name);
    }
    if (nhcs)
        console_add_idle_hook(xhci_idle);
}
