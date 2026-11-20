/*
 * Virtio input (virtio 1.x, section 5.8): keyboards, mice and tablets.
 * Events use Linux input codes (include/uapi/linux/input-event-codes.h);
 * keys are translated by kernel/drivers/keyboard.c.
 * Pointer motion (relative for a mouse, absolute for a tablet), buttons and
 * the wheel go to the desktop (kernel/ui/desktop.c).
 */
#include "console.h"
#include "keyboard.h"
#include "ui.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"
#include "virtio.h"

#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define REL_X 0
#define REL_Y 1
#define REL_WHEEL 8
#define ABS_X 0
#define ABS_Y 1
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
/* configuration selectors (virtio 1.x, 5.8.4) */
#define CFG_ID_NAME 0x01
#define CFG_EV_BITS 0x11
#define CFG_ABS_INFO 0x12
#define NBUF 64

struct event {
    u16 type, code;
    u32 value;
};

struct vinput {
    struct virtq *q;
    struct event *ev; /* NBUF */
    struct kbd_state kbd;
    bool pointer;
    u32 abs_min[2], abs_max[2]; /* tablet axis ranges */
    int rel_x, rel_y;          /* motion since the last EV_SYN */
    int abs_x, abs_y;          /* last absolute position, -1 if none yet */
    bool moved;
};

static void post(struct vinput *in, int i)
{
    struct vq_buf b = { &in->ev[i], sizeof(struct event), true };
    virtq_submit(in->q, &b, 1, (void *)(uintptr_t)(i + 1));
}

static void key(struct vinput *in, u16 code, u32 value)
{
    if (code >= BTN_LEFT && code <= BTN_MIDDLE) {
        ui_pointer_button(code - BTN_LEFT, value != 0);
        return;
    }
    kbd_key(&in->kbd, code, (int)value);
}

/* Scale a tablet coordinate to 0..65535. */
static int abs_norm(struct vinput *in, int axis, u32 v)
{
    u32 lo = in->abs_min[axis], hi = in->abs_max[axis];
    if (hi <= lo)
        return 0;
    if ((int)v < (int)lo)
        v = lo;
    if (v > hi)
        v = hi;
    return (int)(((u64)(v - lo) * 65535) / (hi - lo));
}

static void event(struct vinput *in, const struct event *e)
{
    switch (e->type) {
    case EV_KEY:
        key(in, e->code, e->value);
        break;
    case EV_REL:
        if (e->code == REL_X)
            in->rel_x += (int)e->value, in->moved = true;
        else if (e->code == REL_Y)
            in->rel_y += (int)e->value, in->moved = true;
        else if (e->code == REL_WHEEL)
            ui_pointer_wheel((int)e->value);
        break;
    case EV_ABS:
        if (e->code == ABS_X)
            in->abs_x = abs_norm(in, 0, e->value), in->moved = true;
        else if (e->code == ABS_Y)
            in->abs_y = abs_norm(in, 1, e->value), in->moved = true;
        break;
    case EV_SYN:
        if (!in->moved)
            break;
        in->moved = false;
        if (in->rel_x || in->rel_y) {
            ui_pointer_move_rel(in->rel_x, in->rel_y);
            in->rel_x = in->rel_y = 0;
        }
        if (in->abs_x >= 0 && in->abs_y >= 0)
            ui_pointer_move_abs(in->abs_x, in->abs_y);
        break;
    }
}

static void on_interrupt(struct virtio_dev *d)
{
    struct vinput *in = d->priv;
    void *ck;
    int n = 0;
    while ((ck = virtq_poll(in->q, NULL)) != NULL) {
        int i = (int)(uintptr_t)ck - 1;
        event(in, &in->ev[i]);
        post(in, i);
        n++;
    }
    if (n)
        virtq_kick(in->q);
}

bool virtio_input_probe(struct virtio_dev *d)
{
    if (!virtio_negotiate(d, 0))
        return false;
    struct vinput *in = kzalloc(sizeof(*in));
    if (!in) {
        virtio_fail(d);
        return false;
    }
    in->q = virtio_setup_queue(d, 0, NBUF);
    in->ev = kzalloc(sizeof(struct event) * NBUF);
    if (!in->q || !in->ev) {
        virtio_fail(d);
        return false;
    }
    /* device name: select VIRTIO_INPUT_CFG_ID_NAME */
    char name[64];
    virtio_cfg_write8(d, 0, CFG_ID_NAME);
    virtio_cfg_write8(d, 1, 0);
    u8 len = virtio_cfg8(d, 2);
    if (len >= sizeof(name))
        len = sizeof(name) - 1;
    for (u8 i = 0; i < len; i++)
        name[i] = (char)virtio_cfg8(d, 8 + i);
    name[len] = 0;
    /* A device that reports relative or absolute axes is a pointer. */
    in->abs_x = in->abs_y = -1;
    virtio_cfg_write8(d, 0, CFG_EV_BITS);
    virtio_cfg_write8(d, 1, EV_REL);
    bool rel = virtio_cfg8(d, 2) != 0;
    virtio_cfg_write8(d, 1, EV_ABS);
    bool abs = virtio_cfg8(d, 2) != 0;
    in->pointer = rel || abs;
    for (int axis = 0; abs && axis < 2; axis++) {
        /* struct virtio_input_absinfo { le32 min, max, fuzz, flat, res; } */
        virtio_cfg_write8(d, 0, CFG_ABS_INFO);
        virtio_cfg_write8(d, 1, (u8)axis);
        in->abs_min[axis] = virtio_cfg32(d, 8);
        in->abs_max[axis] = virtio_cfg32(d, 12);
    }
    d->priv = in;
    d->on_interrupt = on_interrupt;
    for (int i = 0; i < NBUF; i++)
        post(in, i);
    virtio_driver_ok(d);
    virtq_kick(in->q);
    printk("  input%d: %s%s\n", d->index, len ? name : "virtio input device",
           !in->pointer ? "" : abs ? " (absolute pointer)" : " (pointer)");
    if (in->pointer)
        ui_pointer_present(abs);
    return true;
}
