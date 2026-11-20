/*
 * USB HID ("Device Class Definition for HID" 1.11, "HID Usage Tables" 1.4):
 * keyboards, mice, touchpads in mouse mode, and absolute pointers such as the
 * tablet of Apple's Virtualization framework.
 *
 * Reports are decoded from the device's report descriptor (keys from usage
 * page 7, X/Y/wheel from Generic Desktop, buttons from page 9); keyboards
 * that support the boot protocol are switched to it. Keys go through the
 * shared keyboard translation, pointer input to the desktop. Held keys
 * repeat (500 ms delay, 30 per second) since USB keyboards do not.
 */
#include "console.h"
#include "irq.h"
#include "kernel.h"
#include "keyboard.h"
#include "mm.h"
#include "string.h"
#include "ui.h"
#include "usb.h"

#define MAX_FIELDS 48
#define REPEAT_DELAY_MS 500
#define REPEAT_EVERY_MS 33

enum { PAGE_DESKTOP = 0x01, PAGE_KEYS = 0x07, PAGE_BUTTONS = 0x09 };
enum { U_X = 0x30, U_Y = 0x31, U_WHEEL = 0x38 };

struct field {
    u8 id;          /* report ID, 0 if the device has none */
    u16 off;        /* bit offset within the report (after the ID byte) */
    u8 size, count; /* bits per element; elements (arrays) */
    bool array, rel;
    u16 page, usage; /* variable fields: one usage; arrays: first usage */
    i32 lmin, lmax;
};

struct hid {
    struct usb_dev *dev;
    struct field f[MAX_FIELDS];
    int nf;
    bool ids;           /* reports start with an ID byte */
    bool boot_kbd;      /* boot protocol: fixed 8-byte keyboard reports */
    bool keyboard, pointer, absolute;
    struct kbd_state kbd;
    u8 keys[32];        /* keys down (HID usages; 0 = none) */
    u8 nkeys;
    u8 mods;
    u32 buttons;
    u16 repeat_code;    /* Linux code of the key being repeated */
    u64 repeat_at;
};

static struct hid *hids[8];
static int nhids;

/* --- report descriptor parser (HID 1.11 section 6.2.2) --------------- */

static i32 item_value(const u8 *p, int size, bool sign)
{
    u32 v = 0;
    for (int i = 0; i < size; i++)
        v |= (u32)p[i] << (8 * i);
    if (sign && size && size < 4 && (v & (1u << (8 * size - 1))))
        v |= ~0u << (8 * size);
    return (i32)v;
}

static void parse(struct hid *h, const u8 *d, u32 len)
{
    struct { u16 page; i32 lmin, lmax; u8 size, count, id; } g = { 0 }, stack[4];
    int sp = 0;
    u16 usages[16];
    int nu = 0;
    u32 umin = 0, umax = 0;
    bool have_range = false;
    static u16 offs[256];
    memset(offs, 0, sizeof(offs));
    for (u32 i = 0; i < len;) {
        u8 b = d[i];
        if (b == 0xfe) { /* long item */
            if (i + 1 >= len)
                break;
            i += 3 + d[i + 1];
            continue;
        }
        int size = (b & 3) == 3 ? 4 : (b & 3);
        int type = (b >> 2) & 3, tag = b >> 4;
        if (i + 1 + (u32)size > len)
            break;
        const u8 *p = d + i + 1;
        i32 sv = item_value(p, size, true);
        u32 uv = (u32)item_value(p, size, false);
        i += 1 + (u32)size;
        if (type == 1) { /* global */
            switch (tag) {
            case 0: g.page = (u16)uv; break;
            case 1: g.lmin = sv; break;
            case 2: g.lmax = sv; break;
            case 7: g.size = (u8)uv; break;
            case 8: g.id = (u8)uv; h->ids = true; break;
            case 9: g.count = (u8)uv; break;
            case 10: if (sp < 4) stack[sp++] = g; break;
            case 11: if (sp) g = stack[--sp]; break;
            }
            if (tag == 2 && g.lmax < g.lmin)
                g.lmax = (i32)uv; /* unsigned maximum, e.g. 0..255 */
            continue;
        }
        if (type == 2) { /* local */
            if (tag == 0 && nu < 16)
                usages[nu++] = (u16)uv; /* (32-bit usages carry a page; rare) */
            else if (tag == 1)
                umin = uv, have_range = true;
            else if (tag == 2)
                umax = uv, have_range = true;
            continue;
        }
        if (type == 0 && tag == 8) { /* Input */
            u16 *off = &offs[g.id];
            bool constant = uv & 1, variable = uv & 2, rel = uv & 4;
            if (!constant && !variable && h->nf < MAX_FIELDS) {
                struct field *f = &h->f[h->nf++];
                *f = (struct field){ g.id, *off, g.size, g.count, true, rel, g.page,
                                     (u16)(have_range ? umin : nu ? usages[0] : 0),
                                     g.lmin, g.lmax };
            } else if (!constant) {
                for (int k = 0; k < g.count && h->nf < MAX_FIELDS; k++) {
                    u16 u = have_range ? (u16)MIN(umin + (u32)k, umax)
                            : nu ? usages[MIN(k, nu - 1)] : 0;
                    struct field *f = &h->f[h->nf++];
                    *f = (struct field){ g.id, (u16)(*off + k * g.size), g.size, 1, false, rel,
                                         g.page, u, g.lmin, g.lmax };
                }
            }
            *off = (u16)(*off + g.size * g.count);
        }
        if (type == 0) { /* any main item ends the local state */
            nu = 0;
            have_range = false;
            umin = umax = 0;
        }
    }
}

static i32 get_bits(const u8 *r, u32 len, u32 off, u32 size, bool sign)
{
    u32 v = 0;
    for (u32 i = 0; i < size && i < 32; i++) {
        u32 bit = off + i;
        if (bit / 8 < len && (r[bit / 8] >> (bit % 8) & 1))
            v |= 1u << i;
    }
    if (sign && size < 32 && (v & (1u << (size - 1))))
        v |= ~0u << size;
    return (i32)v;
}

/* --- keys ------------------------------------------------------------ */

static void key_event(struct hid *h, u8 usage, bool down)
{
    u16 code = kbd_hid_to_code(usage);
    if (!code)
        return;
    kbd_key(&h->kbd, code, down ? 1 : 0);
    if (down && !kbd_is_modifier(code)) {
        h->repeat_code = code;
        h->repeat_at = time_ms() + REPEAT_DELAY_MS;
    } else if (!down && code == h->repeat_code) {
        h->repeat_code = 0;
    }
}

/* New set of keys down: report releases, then presses. */
static void keys_update(struct hid *h, u8 mods, const u8 *keys, int n)
{
    for (int b = 0; b < 8; b++)
        if ((h->mods ^ mods) & (1u << b))
            key_event(h, (u8)(0xe0 + b), mods & (1u << b));
    h->mods = mods;
    for (int i = 0; i < h->nkeys; i++) {
        bool still = false;
        for (int j = 0; j < n; j++)
            still |= keys[j] == h->keys[i];
        if (!still)
            key_event(h, h->keys[i], false);
    }
    for (int j = 0; j < n; j++) {
        if (keys[j] < 4) /* 0 none, 1-3 rollover / errors */
            continue;
        bool was = false;
        for (int i = 0; i < h->nkeys; i++)
            was |= keys[j] == h->keys[i];
        if (!was)
            key_event(h, keys[j], true);
    }
    h->nkeys = 0;
    for (int j = 0; j < n && h->nkeys < sizeof(h->keys); j++)
        if (keys[j] >= 4)
            h->keys[h->nkeys++] = keys[j];
}

/* --- reports --------------------------------------------------------- */

static void report(void *ctx, const u8 *r, u32 len)
{
    struct hid *h = ctx;
    if (h->boot_kbd) {
        if (len >= 8 && r[2] != 1) /* 1 = phantom state (too many keys) */
            keys_update(h, r[0], r + 2, 6);
        return;
    }
    u8 id = 0;
    if (h->ids) {
        if (!len)
            return;
        id = *r++;
        len--;
    }
    u8 mods = 0, keys[32];
    int nk = 0;
    bool saw_keys = false;
    i32 dx = 0, dy = 0, wheel = 0, ax = -1, ay = -1;
    u32 buttons = 0;
    bool saw_buttons = false, saw_motion = false;
    for (int i = 0; i < h->nf; i++) {
        struct field *f = &h->f[i];
        if (f->id != id)
            continue;
        bool sign = f->lmin < 0;
        if (f->page == PAGE_KEYS) {
            saw_keys = true;
            if (f->array) {
                for (int k = 0; k < f->count && nk < 32; k++) {
                    i32 v = get_bits(r, len, f->off + (u32)k * f->size, f->size, false);
                    if (v >= f->lmin && v <= f->lmax)
                        keys[nk++] = (u8)(f->usage + (v - f->lmin));
                }
            } else if (get_bits(r, len, f->off, f->size, false)) {
                if (f->usage >= 0xe0 && f->usage <= 0xe7)
                    mods |= (u8)(1u << (f->usage - 0xe0));
                else if (nk < 32)
                    keys[nk++] = (u8)f->usage;
            }
            continue;
        }
        if (f->page == PAGE_BUTTONS && !f->array && f->usage >= 1 && f->usage <= 3) {
            saw_buttons = true;
            if (get_bits(r, len, f->off, f->size, false))
                buttons |= 1u << (f->usage - 1);
            continue;
        }
        if (f->page != PAGE_DESKTOP || f->array)
            continue;
        i32 v = get_bits(r, len, f->off, f->size, sign);
        if (f->usage == U_WHEEL) {
            wheel += v;
        } else if (f->usage == U_X || f->usage == U_Y) {
            saw_motion = true;
            if (f->rel) {
                *(f->usage == U_X ? &dx : &dy) += v;
            } else if (f->lmax > f->lmin) {
                i32 c = MAX(f->lmin, MIN(v, f->lmax));
                i32 norm = (i32)(((i64)(c - f->lmin) * 65535) / (f->lmax - f->lmin));
                *(f->usage == U_X ? &ax : &ay) = norm;
            }
        }
    }
    if (saw_keys)
        keys_update(h, mods, keys, nk);
    if (saw_motion) {
        if (dx || dy)
            ui_pointer_move_rel(dx, dy);
        if (ax >= 0 && ay >= 0)
            ui_pointer_move_abs(ax, ay);
    }
    if (saw_buttons && buttons != h->buttons) {
        for (int b = 0; b < 3; b++)
            if ((buttons ^ h->buttons) & (1u << b))
                ui_pointer_button(b, buttons & (1u << b));
        h->buttons = buttons;
    }
    if (wheel)
        ui_pointer_wheel(wheel);
}

static void hid_idle(void)
{
    u64 now = time_ms();
    for (int i = 0; i < nhids; i++) {
        struct hid *h = hids[i];
        if (h->repeat_code && now >= h->repeat_at) {
            kbd_key(&h->kbd, h->repeat_code, 2);
            h->repeat_at = now + REPEAT_EVERY_MS;
        }
    }
}

/* --- probe ------------------------------------------------------------ */

#define HID_SET_IDLE 0x0a
#define HID_SET_PROTOCOL 0x0b

static bool attach(struct usb_dev *d, u8 iface, u8 subclass, u8 protocol, u16 rdlen, u8 ep,
                   u16 mps, u8 interval)
{
    struct hid *h = kzalloc(sizeof(*h));
    h->dev = d;
    u8 type_out = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    bool boot_kbd = subclass == 1 && protocol == 1;
    if (!boot_kbd && rdlen) {
        u8 *rd = kmalloc(rdlen);
        if (rd && usb_control(d, USB_DIR_IN | USB_RECIP_INTERFACE, 6, 0x2200, iface, rd, rdlen))
            parse(h, rd, rdlen);
    }
    if (!h->nf && subclass == 1 && protocol == 1)
        boot_kbd = true;
    if (boot_kbd) {
        usb_control(d, type_out, HID_SET_PROTOCOL, 0, iface, NULL, 0); /* boot */
        h->boot_kbd = h->keyboard = true;
    }
    usb_control(d, type_out, HID_SET_IDLE, 0, iface, NULL, 0); /* report changes only */
    for (int i = 0; i < h->nf; i++) {
        struct field *f = &h->f[i];
        if (f->page == PAGE_KEYS)
            h->keyboard = true;
        if (f->page == PAGE_DESKTOP && (f->usage == U_X || f->usage == U_Y)) {
            h->pointer = true;
            h->absolute |= !f->rel;
        }
    }
    if (!h->keyboard && !h->pointer) {
        kfree(h);
        return false;
    }
    if (!usb_interrupt_in(d, ep, mps, interval, report, h)) {
        printk("%s: could not start the interrupt endpoint\n", usb_name(d));
        return false;
    }
    if (nhids < 8)
        hids[nhids++] = h;
    if (nhids == 1)
        console_add_idle_hook(hid_idle);
    printk("%s: USB %s%s%s\n", usb_name(d), h->keyboard ? "keyboard" : "",
           h->keyboard && h->pointer ? " and " : "",
           h->pointer ? (h->absolute ? "absolute pointer" : "mouse") : "");
    if (h->pointer)
        ui_pointer_present(h->absolute);
    return true;
}

bool usb_hid_probe(struct usb_dev *d, const u8 *cfg, u32 len)
{
    bool any = false;
    int iface = -1, alt = 0, cls = 0, sub = 0, proto = 0;
    u16 rdlen = 0;
    for (u32 i = 0; i + 2 <= len && cfg[i] >= 2; i += cfg[i]) {
        const u8 *x = cfg + i;
        if (i + x[0] > len)
            break;
        switch (x[1]) {
        case 4: /* interface */
            if (x[0] < 9)
                break;
            iface = x[2], alt = x[3], cls = x[5], sub = x[6], proto = x[7];
            rdlen = 0;
            break;
        case 0x21: /* HID descriptor: report descriptor length */
            if (x[0] >= 9 && x[6] == 0x22)
                rdlen = (u16)(x[7] | x[8] << 8);
            break;
        case 5: /* endpoint: the first interrupt IN of a HID interface */
            if (x[0] < 7 || cls != 3 || alt != 0 || iface < 0)
                break;
            if ((x[3] & 3) == 3 && (x[2] & 0x80)) {
                u16 mps = (u16)((x[4] | x[5] << 8) & 0x7ff);
                if (attach(d, (u8)iface, (u8)sub, (u8)proto, rdlen, x[2], mps, x[6]))
                    any = true;
                iface = -1; /* one endpoint per interface */
            }
            break;
        }
    }
    return any;
}
