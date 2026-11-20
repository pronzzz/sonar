/*
 * The Sonar desktop: a windowed user interface on the display.
 *
 * A compositor keeps a full-screen back buffer and redraws only damaged
 * rectangles: wallpaper, then windows from bottom to top, the top bar, the
 * dock and the mouse pointer, and copies the result to the screen.
 *
 * Windows draw into their own surfaces. The Terminal window is the system
 * console itself: the framebuffer console (fbcon.c) is pointed at the
 * window's surface, so the shell and DeepSeek (? question) run there and
 * keyboard input goes to the shell as before. The other windows are a live
 * system monitor, a file browser and an About page.
 *
 * Input arrives in interrupt handlers and is queued; everything else runs
 * on the boot CPU from the console's idle hook, or (to keep streaming output
 * smooth) from the terminal's flush, which holds the console lock.
 */
#include "ui.h"
#include "ai.h"
#include "block.h"
#include "config.h"
#include "console.h"
#include "fbcon.h"
#include "fs.h"
#include "gfx.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "net.h"
#include "shell.h"
#include "smp.h"
#include "string.h"

void epoch_to_date(u64 epoch, int *y, int *mo, int *d, int *h, int *mi, int *s);

/* ---------------------------------------------------------------- theme */
#define C_TEXT 0xe6edf3
#define C_MUTED 0x8b98a8
#define C_DIM 0x5c6672
#define C_ACCENT 0x3fc1d6
#define C_BODY 0x12171e
#define C_TERM_BG 0x0f1115 /* fbcon's background */
#define C_FRAME 0x2c3440
#define C_TITLE 0x1b222c
#define C_TITLE_ACTIVE 0x242e3b
#define C_CLOSE 0xe5534b
#define C_GOOD 0x5fd38d
#define C_WARN 0xe5b84b
#define C_TRACK 0x232b35
#define C_PANEL 0x070b10

static int S = 1; /* UI scale: 2 on very high resolution screens */
#define PANEL_H (26 * S)
#define TITLE_H (30 * S)
#define RADIUS (9 * S)
#define DOCK_ICON (46 * S)
#define DOCK_GAP (12 * S)
#define DOCK_PAD (10 * S)
#define DOCK_H (DOCK_ICON + 2 * DOCK_PAD + 6 * S)
#define DOCK_BOTTOM (10 * S)
#define SHADOW (16 * S)
#define MARGIN (20 * S)

static struct framebuffer *hw;
static struct surface back, wall;
static int W, H;
static bool started, active;

static spinlock_t dlock = SPINLOCK_INIT;
static struct rect dirty_r;

static void damage(struct rect r)
{
    u64 f = spin_lock_irqsave(&dlock);
    dirty_r = rect_union(dirty_r, r);
    spin_unlock_irqrestore(&dlock, f);
}

static struct rect screen_rect(void) { return rect_make(0, 0, W, H); }

/* ---------------------------------------------------------------- input */
enum { UEV_BUTTON, UEV_WHEEL, UEV_FKEY };
struct uev {
    u8 kind, button;
    bool down;
    int x, y, val;
};
#define NEV 64
static struct uev evq[NEV];
static u32 ev_head, ev_tail;
static spinlock_t evlock = SPINLOCK_INIT;

static volatile int ptr_x, ptr_y;
static volatile bool ptr_seen, ptr_abs;
static volatile bool button_down[3];
static int cur_x = -1000, cur_y = -1000; /* where the pointer is drawn */

static void push_event(struct uev e)
{
    u64 f = spin_lock_irqsave(&evlock);
    u32 next = (ev_head + 1) % NEV;
    if (next != ev_tail) {
        evq[ev_head] = e;
        ev_head = next;
    }
    spin_unlock_irqrestore(&evlock, f);
}

static bool pop_event(struct uev *e)
{
    bool got = false;
    u64 f = spin_lock_irqsave(&evlock);
    if (ev_tail != ev_head) {
        *e = evq[ev_tail];
        ev_tail = (ev_tail + 1) % NEV;
        got = true;
    }
    spin_unlock_irqrestore(&evlock, f);
    return got;
}

void ui_pointer_present(bool absolute)
{
    ptr_seen = true;
    ptr_abs = ptr_abs || absolute;
}

void ui_pointer_move_abs(int fx, int fy)
{
    if (W <= 0)
        return;
    ptr_x = (int)((i64)fx * (W - 1) / 65535);
    ptr_y = (int)((i64)fy * (H - 1) / 65535);
}

void ui_pointer_move_rel(int dx, int dy)
{
    if (W <= 0)
        return;
    ptr_x = CLAMP(ptr_x + dx, 0, W - 1);
    ptr_y = CLAMP(ptr_y + dy, 0, H - 1);
}

void ui_pointer_button(int button, bool down)
{
    if (button < 0 || button > 2)
        return;
    button_down[button] = down;
    push_event((struct uev){ UEV_BUTTON, (u8)button, down, ptr_x, ptr_y, 0 });
}

void ui_pointer_wheel(int delta) { push_event((struct uev){ UEV_WHEEL, 0, false, ptr_x, ptr_y, delta }); }

void ui_function_key(int n) { push_event((struct uev){ UEV_FKEY, 0, true, 0, 0, n }); }

bool ui_desktop_active(void) { return active; }

/* ---------------------------------------------------------------- windows */
struct window {
    const char *title;
    struct rect r; /* frame, in screen coordinates */
    struct surface body;
    bool visible;
    void (*paint)(struct window *w);
    void (*click)(struct window *w, int x, int y);
    void (*wheel)(struct window *w, int delta);
};
enum { W_TERM, W_MONITOR, W_FILES, W_ABOUT, NWIN };
static struct window win[NWIN];
static int zorder[NWIN] = { W_ABOUT, W_FILES, W_MONITOR, W_TERM }; /* last is on top */

static struct rect body_rect(const struct window *w)
{
    return rect_make(w->r.x + S, w->r.y + TITLE_H, w->body.w, w->body.h);
}

static struct rect outer_rect(const struct window *w)
{
    return rect_make(w->r.x - SHADOW, w->r.y - SHADOW, w->r.w + 2 * SHADOW,
                     w->r.h + 2 * SHADOW + SHADOW / 3);
}

static void close_center(const struct window *w, int *cx, int *cy)
{
    *cx = w->r.x + w->r.w - 18 * S;
    *cy = w->r.y + TITLE_H / 2;
}

static int top_window(void)
{
    for (int z = NWIN - 1; z >= 0; z--)
        if (win[zorder[z]].visible)
            return zorder[z];
    return -1;
}

static int window_at(int x, int y)
{
    for (int z = NWIN - 1; z >= 0; z--) {
        struct window *w = &win[zorder[z]];
        if (w->visible && rect_contains(w->r, x, y))
            return zorder[z];
    }
    return -1;
}

static void damage_panel(void) { damage(rect_make(0, 0, W, PANEL_H)); }

static void repaint(int i)
{
    struct window *w = &win[i];
    if (w->paint) {
        surface_reset_clip(&w->body);
        w->paint(w);
    }
    if (w->visible)
        damage(body_rect(w));
}

static void raise_window(int i)
{
    int old = top_window();
    int z = 0;
    while (zorder[z] != i)
        z++;
    for (; z < NWIN - 1; z++)
        zorder[z] = zorder[z + 1];
    zorder[NWIN - 1] = i;
    if (old >= 0)
        damage(outer_rect(&win[old]));
    damage(outer_rect(&win[i]));
    damage_panel();
}

static void show_window(int i)
{
    if (!win[i].visible) {
        win[i].visible = true;
        repaint(i);
    }
    raise_window(i);
}

static void hide_window(int i)
{
    win[i].visible = false;
    damage(outer_rect(&win[i]));
    damage_panel();
    int t = top_window();
    if (t >= 0)
        damage(outer_rect(&win[t])); /* its title shows focus now */
}

static void toggle_window(int i)
{
    if (win[i].visible && top_window() == i)
        hide_window(i);
    else
        show_window(i);
}

/* ---------------------------------------------------------------- dock */
enum { D_TERM, D_ASK, D_MONITOR, D_FILES, D_ABOUT, D_CONSOLE, NDOCK };
static const struct {
    const char *label;
    u32 color;
    int window;
} dock[NDOCK] = {
    { "Terminal (F2)", 0x1f2a36, W_TERM },
    { "Ask DeepSeek", 0x4d6bfe, -1 },
    { "System Monitor (F3)", 0x0f8b8d, W_MONITOR },
    { "Files (F4)", 0xd99a2b, W_FILES },
    { "About Sonar (F5)", 0x8a63d2, W_ABOUT },
    { "Full-screen console (F1)", 0x39424e, -1 },
};
static int hover = -1;

static struct rect dock_rect(void)
{
    int w = NDOCK * DOCK_ICON + (NDOCK - 1) * DOCK_GAP + 2 * DOCK_PAD;
    return rect_make((W - w) / 2, H - DOCK_BOTTOM - DOCK_H, w, DOCK_H);
}

static struct rect dock_icon_rect(int i)
{
    struct rect d = dock_rect();
    return rect_make(d.x + DOCK_PAD + i * (DOCK_ICON + DOCK_GAP), d.y + DOCK_PAD, DOCK_ICON,
                     DOCK_ICON);
}

static struct rect tip_rect(int i)
{
    struct rect ic = dock_icon_rect(i);
    int tw = gfx_text_width(dock[i].label, S) + 20 * S;
    return rect_make(ic.x + ic.w / 2 - tw / 2, ic.y - 40 * S, tw, 24 * S);
}

static struct rect dock_area(void)
{
    struct rect d = dock_rect();
    return rect_make(d.x - 120 * S, d.y - 44 * S, d.w + 240 * S, d.h + 44 * S);
}

/* ---------------------------------------------------------------- drawing */
static void draw_x(struct surface *s, int cx, int cy, int r, u32 color)
{
    for (int i = -r; i <= r; i++) {
        gfx_fill(s, rect_make(cx + i, cy + i, S + 1, S + 1), color);
        gfx_fill(s, rect_make(cx + i, cy - i, S + 1, S + 1), color);
    }
}

static void draw_window(struct window *w, bool focused)
{
    struct rect r = w->r;
    gfx_shadow(&back, r, SHADOW, focused ? 160 : 110);
    gfx_round_rect(&back, rect_make(r.x, r.y, r.w, TITLE_H + 2 * RADIUS), RADIUS, C_FRAME, 256);
    gfx_fill(&back, rect_make(r.x, r.y + RADIUS, r.w, r.h - RADIUS), C_FRAME);
    u32 tc = focused ? C_TITLE_ACTIVE : C_TITLE;
    gfx_round_rect(&back, rect_make(r.x + S, r.y + S, r.w - 2 * S, TITLE_H + RADIUS), RADIUS - S,
                   tc, 256);
    gfx_blend(&back, rect_make(r.x + RADIUS, r.y + S, r.w - 2 * RADIUS, 1), 0xffffff, 28);
    gfx_hline(&back, r.x + S, r.y + TITLE_H - 1, r.w - 2 * S, 0x0b0e12);
    gfx_disc(&back, r.x + 16 * S, r.y + TITLE_H / 2, 4 * S, focused ? C_ACCENT : C_DIM);
    int tw = gfx_text_width(w->title, S);
    int tx = r.x + (r.w - tw) / 2, ty = r.y + (TITLE_H - 16 * S) / 2;
    if (focused)
        gfx_text_bold(&back, tx, ty, w->title, C_TEXT, S);
    else
        gfx_text(&back, tx, ty, w->title, C_MUTED, S);
    int cx, cy;
    close_center(w, &cx, &cy);
    gfx_disc(&back, cx, cy, 7 * S, focused ? C_CLOSE : 0x3a434f);
    draw_x(&back, cx - S / 2, cy - S / 2, 3 * S, focused ? 0x4a1210 : 0x1b2027);
    gfx_blit(&back, r.x + S, r.y + TITLE_H, &w->body);
}

static void fmt_size(u64 bytes, char *out, size_t n) { blk_format_size(bytes, out, n); }

static void draw_panel(void)
{
    struct rect p = rect_make(0, 0, W, PANEL_H);
    gfx_blend(&back, p, C_PANEL, 215);
    gfx_blend(&back, rect_make(0, PANEL_H - 1, W, 1), 0xffffff, 20);
    int cy = PANEL_H / 2, x = 14 * S;
    gfx_disc(&back, x + 6 * S, cy, 7 * S, C_ACCENT);
    gfx_disc(&back, x + 6 * S, cy, 5 * S, 0x0a1016);
    gfx_disc(&back, x + 6 * S, cy, 2 * S, C_ACCENT);
    int ty = (PANEL_H - 16 * S) / 2;
    x += 20 * S;
    x += gfx_text_bold(&back, x, ty, "Sonar", C_TEXT, S) + 16 * S;
    int t = top_window();
    if (t >= 0)
        gfx_text(&back, x, ty, win[t].title, C_MUTED, S);

    /* right side, drawn right to left */
    char buf[96];
    int rx = W - 14 * S;
    u64 e = rtc_epoch();
    if (e) {
        int y, mo, d, h, mi, s;
        epoch_to_date(e, &y, &mo, &d, &h, &mi, &s);
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d UTC", y, mo, d, h, mi);
    } else {
        u64 up = time_ms() / 1000;
        snprintf(buf, sizeof(buf), "up %llu:%02llu", (unsigned long long)(up / 3600),
                 (unsigned long long)(up / 60 % 60));
    }
    rx -= gfx_text_width(buf, S);
    gfx_text_bold(&back, rx, ty, buf, C_TEXT, S);

    u64 total = pmm_total_bytes(), freeb = pmm_free_bytes();
    snprintf(buf, sizeof(buf), "RAM %llu%%",
             (unsigned long long)(total ? (total - freeb) * 100 / total : 0));
    rx -= gfx_text_width(buf, S) + 22 * S;
    gfx_text(&back, rx, ty, buf, C_MUTED, S);

    struct netif *n = net_iface();
    if (n && n->ip) {
        char ip[20];
        ip4_format(n->ip, ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "%s %s", n->name, ip);
    } else {
        snprintf(buf, sizeof(buf), "%s", n ? "network: no address" : "no network");
    }
    rx -= gfx_text_width(buf, S) + 22 * S;
    gfx_text(&back, rx, ty, buf, C_MUTED, S);

    const char *model = ai_model_name();
    snprintf(buf, sizeof(buf), "DeepSeek: %s", model ? model : "no model");
    int bw = MIN(gfx_text_width(buf, S), 40 * 8 * S);
    rx -= bw + 22 * S + 10 * S;
    gfx_round_rect(&back, rect_make(rx - 8 * S, 4 * S, bw + 16 * S, PANEL_H - 8 * S), 8 * S,
                   model ? C_ACCENT : C_WARN, 48);
    gfx_text_fit(&back, rx, ty, buf, model ? C_TEXT : C_WARN, S, bw);
}

static void draw_icon_glyph(int i, struct rect r)
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    switch (i) {
    case D_TERM:
        gfx_text_bold(&back, r.x + 8 * S, cy - 16 * S, ">_", C_GOOD, 2 * S);
        break;
    case D_ASK:
        gfx_text_bold(&back, cx - 16 * S, cy - 16 * S, "AI", 0xffffff, 2 * S);
        break;
    case D_MONITOR: {
        static const int hs[3] = { 12, 24, 17 };
        for (int k = 0; k < 3; k++)
            gfx_round_rect(&back,
                           rect_make(cx - 14 * S + k * 10 * S, cy + 12 * S - hs[k] * S, 7 * S,
                                     hs[k] * S),
                           2 * S, 0xffffff, 235);
        break;
    }
    case D_FILES:
        gfx_round_rect(&back, rect_make(cx - 15 * S, cy - 11 * S, 13 * S, 8 * S), 2 * S, 0xfff3d6,
                       256);
        gfx_round_rect(&back, rect_make(cx - 15 * S, cy - 7 * S, 30 * S, 20 * S), 3 * S, 0xfff3d6,
                       256);
        break;
    case D_ABOUT:
        gfx_text_bold(&back, cx - 8 * S, cy - 16 * S, "i", 0xffffff, 2 * S);
        break;
    case D_CONSOLE:
        gfx_frame(&back, rect_make(cx - 15 * S, cy - 11 * S, 30 * S, 20 * S), 0xffffff, 200);
        gfx_text_bold(&back, cx - 8 * S, cy - 9 * S, "F1", 0xffffff, S);
        break;
    }
}

static void draw_dock(void)
{
    struct rect d = dock_rect();
    gfx_round_rect(&back, d, 16 * S, 0x0b1118, 175);
    gfx_frame(&back, rect_make(d.x + 8 * S, d.y, d.w - 16 * S, 1), 0xffffff, 0);
    gfx_blend(&back, rect_make(d.x + 14 * S, d.y, d.w - 28 * S, 1), 0xffffff, 40);
    for (int i = 0; i < NDOCK; i++) {
        struct rect r = dock_icon_rect(i);
        if (hover == i)
            r.y -= 4 * S;
        gfx_round_rect(&back, r, 11 * S, dock[i].color, 256);
        gfx_round_rect(&back, rect_make(r.x, r.y, r.w, r.h / 2), 11 * S, 0xffffff, 18);
        draw_icon_glyph(i, r);
        int wi = dock[i].window;
        if (wi >= 0 && win[wi].visible)
            gfx_disc(&back, r.x + r.w / 2, dock_icon_rect(i).y + DOCK_ICON + 6 * S, 2 * S, C_TEXT);
    }
    if (hover >= 0) {
        struct rect t = tip_rect(hover);
        gfx_round_rect(&back, t, 7 * S, 0x0b1118, 235);
        gfx_text(&back, t.x + 10 * S, t.y + 4 * S, dock[hover].label, C_TEXT, S);
    }
}

/* Arrow pointer: X outline, . fill. */
static const char *const arrow[] = {
    "X",           "XX",          "X.X",         "X..X",       "X...X",      "X....X",
    "X.....X",     "X......X",    "X.......X",   "X........X", "X.....XXXXX", "X..X..X",
    "X.X X..X",    "XX  X..X",    "X    X..X",   "     X..X",  "      XX",
};
#define ARROW_W 11
#define ARROW_H 17

static struct rect pointer_rect(int x, int y) { return rect_make(x, y, ARROW_W * S, ARROW_H * S); }

static void draw_pointer(int x, int y)
{
    for (int j = 0; j < ARROW_H; j++)
        for (int i = 0; arrow[j][i]; i++) {
            char c = arrow[j][i];
            if (c == ' ')
                continue;
            gfx_fill(&back, rect_make(x + i * S, y + j * S, S, S), c == 'X' ? 0x0b0e12 : 0xffffff);
        }
}

static void compose(struct rect c)
{
    back.clip = c;
    for (int y = c.y; y < c.y + c.h; y++)
        memcpy(back.px + (size_t)y * back.stride + c.x, wall.px + (size_t)y * wall.stride + c.x,
               (size_t)c.w * 4);
    int top = top_window();
    for (int z = 0; z < NWIN; z++) {
        struct window *w = &win[zorder[z]];
        if (w->visible && !rect_empty(rect_intersect(outer_rect(w), c)))
            draw_window(w, zorder[z] == top);
    }
    if (c.y < PANEL_H)
        draw_panel();
    if (!rect_empty(rect_intersect(dock_area(), c)))
        draw_dock();
    if (ptr_seen)
        draw_pointer(cur_x, cur_y);
    surface_reset_clip(&back);
}

/* Compose the damaged area and show it. Caller holds the console lock. */
static void present_locked(void)
{
    int px = ptr_x, py = ptr_y;
    if (ptr_seen && (px != cur_x || py != cur_y)) {
        damage(pointer_rect(cur_x, cur_y));
        cur_x = px, cur_y = py;
        damage(pointer_rect(cur_x, cur_y));
    }
    u64 f = spin_lock_irqsave(&dlock);
    struct rect r = rect_intersect(dirty_r, screen_rect());
    dirty_r = rect_make(0, 0, 0, 0);
    spin_unlock_irqrestore(&dlock, f);
    if (rect_empty(r))
        return;
    compose(r);
    for (int y = r.y; y < r.y + r.h; y++) {
        const u32 *src = back.px + (size_t)y * back.stride + r.x;
        u32 *dst = (u32 *)(hw->base + (size_t)y * hw->stride) + r.x;
        if (!hw->bgr) {
            memcpy(dst, src, (size_t)r.w * 4);
            continue;
        }
        for (int x = 0; x < r.w; x++) {
            u32 v = src[x];
            dst[x] = (v & 0xff) << 16 | (v & 0xff00) | (v >> 16 & 0xff);
        }
    }
    if (hw->flush)
        hw->flush(hw->ctx, (u32)r.x, (u32)r.y, (u32)r.w, (u32)r.h);
}

static void present(void)
{
    u64 f = console_lock();
    if (active)
        present_locked();
    console_unlock(f);
}

/* ---------------------------------------------------------------- wallpaper */
static void build_wallpaper(void)
{
    int cx = W * 3 / 4, cy = H * 3 / 5;
    int spacing16 = 72 * S * 16, maxr = W * 3 / 5;
    for (int y = 0; y < H; y++) {
        u32 base_row = rgb_mix(0x0a1520, 0x0b2232, y * 256 / H);
        u32 *p = wall.px + (size_t)y * wall.stride;
        for (int x = 0; x < W; x++) {
            int dx = x - cx, dy = y - cy;
            u32 d16 = isqrt32((u32)(dx * dx + dy * dy) * 256u);
            int dist = (int)(d16 / 16);
            u32 c = base_row;
            if (dist < H) /* soft glow around the sonar's centre */
                c = rgb_mix(c, 0x1b5a70, (H - dist) * 70 / H);
            if (dist < maxr) {
                int ph = (int)(d16 % (u32)spacing16);
                int near = MIN(ph, spacing16 - ph); /* to the nearest ring, 1/16 px */
                if (near < 20) {
                    int a = (20 - near) * 7 * (maxr - dist) / maxr;
                    c = rgb_mix(c, 0x58d6ea, MIN(a, 140));
                }
            }
            p[x] = c;
        }
    }
    /* a faint sweep line from the centre towards the upper left */
    for (int t = 0; t < maxr; t++) {
        int x = cx - t * 87 / 100, y = cy - t / 2;
        if (x < 0 || y < 0)
            break;
        int a = 90 * (maxr - t) / maxr;
        for (int k = 0; k < 2 * S; k++)
            if (y + k < H)
                wall.px[(size_t)(y + k) * wall.stride + x] =
                    rgb_mix(wall.px[(size_t)(y + k) * wall.stride + x], 0x7fe9f5, a);
    }
}

/* ---------------------------------------------------------------- terminal */
static struct framebuffer term_fb;
static char replay[24 * 1024];

/* fbcon's flush for the terminal surface: runs with the console lock held. */
static void term_flush(void *ctx, u32 x, u32 y, u32 w, u32 h)
{
    struct window *t = &win[W_TERM];
    if (t->visible) {
        struct rect b = body_rect(t);
        damage(rect_make(b.x + (int)x, b.y + (int)y, (int)w, (int)h));
    }
    if (active && smp_cpu_id() == 0)
        present_locked();
}

/* ---------------------------------------------------------------- monitor */
#define HIST 60
static u32 net_hist[HIST];
static u64 net_last;
static bool net_primed;

static void sample_net(void)
{
    struct netif *n = net_iface();
    u64 b = n ? n->rx_bytes + n->tx_bytes : 0;
    u32 rate = net_primed && b >= net_last ? (u32)MIN(b - net_last, 0xffffffffULL) : 0;
    net_last = b;
    net_primed = true;
    memmove(net_hist, net_hist + 1, sizeof(u32) * (HIST - 1));
    net_hist[HIST - 1] = rate;
}

static void section(struct surface *s, int x, int *y, const char *title)
{
    gfx_text_bold(s, x, *y, title, C_ACCENT, S);
    *y += 20 * S;
}

static void line(struct surface *s, int x, int *y, const char *text, u32 color)
{
    gfx_text_fit(s, x, *y, text, color, S, s->w - x - 12 * S);
    *y += 18 * S;
}

static void bar(struct surface *s, int x, int *y, int w, u64 part, u64 whole, u32 color)
{
    gfx_round_rect(s, rect_make(x, *y + 2 * S, w, 8 * S), 4 * S, C_TRACK, 256);
    int fw = whole ? (int)((u64)w * MIN(part, whole) / whole) : 0;
    if (fw > 0)
        gfx_round_rect(s, rect_make(x, *y + 2 * S, MAX(fw, 8 * S), 8 * S), 4 * S, color, 256);
    *y += 16 * S;
}

static void paint_monitor(struct window *w)
{
    struct surface *s = &w->body;
    char buf[128], a[24], b[24];
    gfx_fill(s, rect_make(0, 0, s->w, s->h), C_BODY);
    int x = 16 * S, y = 12 * S, bw = s->w - 32 * S;

    section(s, x, &y, "SYSTEM");
    snprintf(buf, sizeof(buf), "%s %s on aarch64, %d of %d CPUs online", OS_NAME, OS_VERSION,
             smp_num_cpus(), smp_num_present());
    line(s, x, &y, buf, C_TEXT);
    u64 up = time_ms() / 1000;
    snprintf(buf, sizeof(buf), "Up %llud %02lluh %02llum %02llus", (unsigned long long)(up / 86400),
             (unsigned long long)(up / 3600 % 24), (unsigned long long)(up / 60 % 60),
             (unsigned long long)(up % 60));
    line(s, x, &y, buf, C_MUTED);
    y += 6 * S;

    section(s, x, &y, "MEMORY");
    u64 total = pmm_total_bytes(), freeb = pmm_free_bytes();
    fmt_size(total - freeb, a, sizeof(a));
    fmt_size(total, b, sizeof(b));
    snprintf(buf, sizeof(buf), "%s used of %s", a, b);
    line(s, x, &y, buf, C_TEXT);
    bar(s, x, &y, bw, total - freeb, total, C_ACCENT);
    y += 4 * S;

    section(s, x, &y, "DISK");
    u64 dt, df;
    char label[40];
    fs_usage(&dt, &df, label, sizeof(label));
    if (dt) {
        fmt_size(dt - df, a, sizeof(a));
        fmt_size(dt, b, sizeof(b));
        snprintf(buf, sizeof(buf), "%s (\"%s\"): %s used of %s%s", fs_device(), label, a, b,
                 fs_is_ram() ? ", RAM" : "");
        line(s, x, &y, buf, C_TEXT);
        bar(s, x, &y, bw, dt - df, dt, 0x8a63d2);
    } else {
        line(s, x, &y, "No file system mounted", C_WARN);
        y += 16 * S;
    }
    y += 4 * S;

    section(s, x, &y, "NETWORK");
    struct netif *n = net_iface();
    if (n) {
        char ip[20];
        ip4_format(n->ip, ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "%s  %s%s", n->name, n->ip ? ip : "no address",
                 n->dhcp ? "  (DHCP)" : "");
        line(s, x, &y, buf, C_TEXT);
        fmt_size(n->rx_bytes, a, sizeof(a));
        fmt_size(n->tx_bytes, b, sizeof(b));
        snprintf(buf, sizeof(buf), "received %s, sent %s", a, b);
        line(s, x, &y, buf, C_MUTED);
        u32 peak = 1;
        for (int i = 0; i < HIST; i++)
            peak = MAX(peak, net_hist[i]);
        int gh = 26 * S, gx = x, colw = MAX(bw / HIST, 1);
        gfx_round_rect(s, rect_make(gx, y, bw, gh + 4 * S), 4 * S, C_TRACK, 120);
        for (int i = 0; i < HIST; i++) {
            int hgt = (int)((u64)net_hist[i] * gh / peak);
            if (net_hist[i] && hgt < S)
                hgt = S;
            gfx_fill(s, rect_make(gx + i * colw, y + 2 * S + gh - hgt, MAX(colw - 1, 1), hgt),
                     C_GOOD);
        }
        y += gh + 6 * S;
    } else {
        line(s, x, &y, "No network interface", C_MUTED);
        y += 32 * S;
    }
    y += 4 * S;

    section(s, x, &y, "DEEPSEEK");
    const char *model = ai_model_name();
    if (model) {
        line(s, x, &y, model, C_TEXT);
        u32 sp = ai_last_speed_x10();
        if (sp)
            snprintf(buf, sizeof(buf), "Last answer: %u.%u tokens/s", sp / 10, sp % 10);
        else
            snprintf(buf, sizeof(buf), "Ready. Type ? and a question in the Terminal.");
        line(s, x, &y, buf, C_MUTED);
    } else {
        line(s, x, &y, "No model loaded", C_WARN);
        line(s, x, &y, "Run setup in the Terminal to choose one.", C_MUTED);
    }
}

/* ---------------------------------------------------------------- files */
struct fentry {
    char name[FS_NAME_MAX + 1];
    bool dir;
    u64 size, mtime;
};
static char files_path[FS_PATH_MAX] = "/";
static char files_next[FS_PATH_MAX]; /* directory to load at the next safe moment */
static struct fentry *files;
static int nfiles, files_scroll, files_sel = -1;
static char files_status[128];
#define FROW (22 * S)
#define FHEAD (30 * S)
#define FFOOT (24 * S)

static bool files_has_up(void) { return strcmp(files_path, "/") != 0; }

static void path_join(char *out, size_t n, const char *dir, const char *name)
{
    snprintf(out, n, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

static void path_parent(char *out, size_t n, const char *path)
{
    strlcpy(out, path, n);
    char *slash = strrchr(out, '/');
    if (slash && slash != out)
        *slash = 0;
    else
        strlcpy(out, "/", n);
}

/* Reads the directory: only called when no command is using the disk. */
static void files_load(const char *path)
{
    struct fs_dirent *ents;
    int n;
    int e = fs_list(path, &ents, &n);
    if (e < 0) {
        snprintf(files_status, sizeof(files_status), "%s: %s", path, fs_strerror(e));
        repaint(W_FILES);
        return;
    }
    struct fentry *list = n ? kcalloc((size_t)n, sizeof(*list)) : NULL;
    if (n && !list) {
        kfree(ents);
        return;
    }
    char full[FS_PATH_MAX];
    for (int i = 0; i < n; i++) {
        strlcpy(list[i].name, ents[i].name, sizeof(list[i].name));
        list[i].dir = ents[i].type == FT_DIR;
        struct fs_stat st;
        path_join(full, sizeof(full), path, ents[i].name);
        if (fs_stat(full, &st) == 0)
            list[i].size = st.size, list[i].mtime = st.mtime;
    }
    kfree(ents);
    /* folders first, then by name */
    for (int i = 1; i < n; i++) {
        struct fentry t = list[i];
        int j = i - 1;
        while (j >= 0 && (list[j].dir < t.dir ||
                          (list[j].dir == t.dir && strcmp(list[j].name, t.name) > 0))) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = t;
    }
    kfree(files);
    files = list;
    nfiles = n;
    strlcpy(files_path, path, sizeof(files_path));
    files_scroll = 0;
    files_sel = -1;
    snprintf(files_status, sizeof(files_status), "%d item%s", n, n == 1 ? "" : "s");
    repaint(W_FILES);
}

static int files_rows(const struct window *w) { return (w->body.h - FHEAD - FFOOT) / FROW; }

static void paint_files(struct window *w)
{
    struct surface *s = &w->body;
    char buf[96];
    gfx_fill(s, rect_make(0, 0, s->w, s->h), C_BODY);
    gfx_fill(s, rect_make(0, 0, s->w, FHEAD), 0x171d25);
    gfx_hline(s, 0, FHEAD - 1, s->w, 0x0b0e12);
    int ty = (FHEAD - 16 * S) / 2;
    if (files_has_up()) {
        gfx_round_rect(s, rect_make(8 * S, 5 * S, 34 * S, FHEAD - 10 * S), 5 * S, 0x2c3440, 256);
        gfx_text_bold(s, 13 * S, ty, "Up", C_TEXT, S);
    }
    gfx_text_fit(s, 52 * S, ty, files_path, C_TEXT, S, s->w - 64 * S);

    int rows = files_rows(w), y = FHEAD;
    for (int k = 0; k < rows && files_scroll + k < nfiles; k++) {
        int i = files_scroll + k;
        const struct fentry *f = &files[i];
        if (i == files_sel)
            gfx_fill(s, rect_make(0, y, s->w, FROW), 0x1d3a4a);
        int cy = y + FROW / 2;
        if (f->dir) {
            gfx_round_rect(s, rect_make(12 * S, cy - 6 * S, 7 * S, 4 * S), S, 0xe8b04a, 256);
            gfx_round_rect(s, rect_make(12 * S, cy - 4 * S, 16 * S, 11 * S), 2 * S, 0xe8b04a, 256);
        } else {
            gfx_round_rect(s, rect_make(14 * S, cy - 7 * S, 12 * S, 15 * S), 2 * S, 0xaab4c0, 256);
            gfx_hline(s, 17 * S, cy - 2 * S, 6 * S, 0x5c6672);
            gfx_hline(s, 17 * S, cy + 2 * S, 6 * S, 0x5c6672);
        }
        int sizew = 0;
        if (!f->dir) {
            fmt_size(f->size, buf, sizeof(buf));
            sizew = gfx_text_width(buf, S);
            gfx_text(s, s->w - 12 * S - sizew, cy - 8 * S, buf, C_MUTED, S);
        }
        gfx_text_fit(s, 38 * S, cy - 8 * S, f->name, f->dir ? C_TEXT : 0xc8ccd4, S,
                     s->w - 38 * S - sizew - 24 * S);
        y += FROW;
    }
    if (!nfiles)
        gfx_text(s, 16 * S, FHEAD + 10 * S, "(empty)", C_DIM, S);
    if (nfiles > rows) {
        /* scroll indicator */
        int track = rows * FROW, th = MAX(track * rows / nfiles, 12 * S);
        int tyy = FHEAD + (track - th) * files_scroll / MAX(nfiles - rows, 1);
        gfx_round_rect(s, rect_make(s->w - 5 * S, tyy, 3 * S, th), S, 0x4a5563, 256);
    }
    gfx_fill(s, rect_make(0, s->h - FFOOT, s->w, FFOOT), 0x171d25);
    gfx_text_fit(s, 12 * S, s->h - FFOOT + 4 * S, files_status, C_MUTED, S, s->w - 24 * S);
}

static void files_click(struct window *w, int x, int y)
{
    if (y < FHEAD) {
        if (files_has_up() && x < 46 * S)
            path_parent(files_next, sizeof(files_next), files_path);
        return;
    }
    int k = (y - FHEAD) / FROW;
    if (y >= w->body.h - FFOOT || k >= files_rows(w))
        return;
    int i = files_scroll + k;
    if (i >= nfiles)
        return;
    const struct fentry *f = &files[i];
    if (f->dir) {
        path_join(files_next, sizeof(files_next), files_path, f->name);
        return;
    }
    files_sel = i;
    char sz[24];
    fmt_size(f->size, sz, sizeof(sz));
    int yy, mo, d, h, mi, s;
    epoch_to_date(f->mtime, &yy, &mo, &d, &h, &mi, &s);
    snprintf(files_status, sizeof(files_status), "%s  %s  modified %04d-%02d-%02d %02d:%02d",
             f->name, sz, yy, mo, d, h, mi);
    repaint(W_FILES);
}

static void files_wheel(struct window *w, int delta)
{
    int max = MAX(nfiles - files_rows(w), 0);
    int ns = CLAMP(files_scroll - delta * 3, 0, max);
    if (ns != files_scroll) {
        files_scroll = ns;
        repaint(W_FILES);
    }
}

/* ---------------------------------------------------------------- about */
static void paint_about(struct window *w)
{
    struct surface *s = &w->body;
    gfx_fill(s, rect_make(0, 0, s->w, s->h), C_BODY);
    int x = 20 * S, y = 16 * S;
    gfx_disc(s, x + 14 * S, y + 16 * S, 15 * S, C_ACCENT);
    gfx_disc(s, x + 14 * S, y + 16 * S, 11 * S, C_BODY);
    gfx_disc(s, x + 14 * S, y + 16 * S, 5 * S, C_ACCENT);
    gfx_text_bold(s, x + 40 * S, y, OS_NAME " " OS_VERSION, C_TEXT, 2 * S);
    y += 40 * S;
    static const char *const text[] = {
        "An operating system with DeepSeek built in.",
        "Kernel, drivers, file system, network and AI",
        "engine are all part of Sonar itself.",
        "",
        "Ask DeepSeek: type ? and a question in the",
        "Terminal. It can run commands to find answers;",
        "anything that changes the system asks first.",
        "",
        "F1  full-screen console / desktop",
        "F2  Terminal       F3  System Monitor",
        "F4  Files          F5  About",
        "Typing always goes to the Terminal.",
    };
    for (size_t i = 0; i < ARRAY_SIZE(text); i++) {
        u32 c = text[i][0] == 'F' && text[i][1] >= '1' && text[i][1] <= '9' ? C_TEXT : C_MUTED;
        gfx_text(s, x, y, text[i], c, S);
        y += 18 * S;
    }
}

/* ---------------------------------------------------------------- control */
static void desktop_enter(void)
{
    if (active || !started)
        return;
    u64 f = console_lock();
    size_t n = console_log_copy_locked(replay, sizeof(replay));
    active = true;
    fbcon_retarget_locked(&term_fb, replay, n);
    damage(screen_rect());
    present_locked();
    console_unlock(f);
}

static void desktop_leave(void)
{
    if (!active)
        return;
    u64 f = console_lock();
    active = false;
    size_t n = console_log_copy_locked(replay, sizeof(replay));
    fbcon_retarget_locked(NULL, replay, n);
    console_unlock(f);
}

static int drag_win = -1, drag_dx, drag_dy;

static void dock_activate(int i)
{
    switch (i) {
    case D_TERM: show_window(W_TERM); break;
    case D_ASK:
        show_window(W_TERM);
        console_input("? ", 2);
        break;
    case D_CONSOLE: desktop_leave(); break;
    default: toggle_window(dock[i].window); break;
    }
}

static void press(int x, int y)
{
    for (int i = 0; i < NDOCK; i++)
        if (rect_contains(dock_icon_rect(i), x, y)) {
            dock_activate(i);
            return;
        }
    int i = window_at(x, y);
    if (i < 0)
        return;
    struct window *w = &win[i];
    if (top_window() != i)
        raise_window(i);
    int cx, cy;
    close_center(w, &cx, &cy);
    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= 81 * S * S) {
        hide_window(i);
        return;
    }
    if (y < w->r.y + TITLE_H) {
        drag_win = i;
        drag_dx = x - w->r.x;
        drag_dy = y - w->r.y;
        return;
    }
    struct rect b = body_rect(w);
    if (w->click && rect_contains(b, x, y))
        w->click(w, x - b.x, y - b.y);
}

static void function_key(int n)
{
    if (n == 1) {
        if (active)
            desktop_leave();
        else
            desktop_enter();
        return;
    }
    if (!active)
        return;
    switch (n) {
    case 2: show_window(W_TERM); break;
    case 3: toggle_window(W_MONITOR); break;
    case 4: toggle_window(W_FILES); break;
    case 5: toggle_window(W_ABOUT); break;
    }
}

static void handle(const struct uev *e)
{
    if (e->kind == UEV_FKEY) {
        function_key(e->val);
        return;
    }
    if (!active)
        return;
    if (e->kind == UEV_WHEEL) {
        int i = window_at(e->x, e->y);
        if (i >= 0 && win[i].wheel)
            win[i].wheel(&win[i], e->val);
        return;
    }
    if (e->button != 0)
        return;
    if (e->down)
        press(e->x, e->y);
    else
        drag_win = -1;
}

static void track_pointer(void)
{
    int px = ptr_x, py = ptr_y;
    if (drag_win >= 0) {
        if (!button_down[0]) {
            drag_win = -1;
        } else {
            struct window *w = &win[drag_win];
            int nx = CLAMP(px - drag_dx, 60 * S - w->r.w, W - 60 * S);
            int ny = CLAMP(py - drag_dy, PANEL_H, H - TITLE_H);
            if (nx != w->r.x || ny != w->r.y) {
                damage(outer_rect(w));
                w->r.x = nx, w->r.y = ny;
                damage(outer_rect(w));
            }
        }
    }
    int h = -1;
    for (int i = 0; i < NDOCK; i++)
        if (rect_contains(dock_icon_rect(i), px, py))
            h = i;
    if (h != hover) {
        hover = h;
        damage(dock_area());
    }
}

static u64 last_second;

static void tick(void)
{
    if (!started || smp_cpu_id() != 0)
        return;
    struct uev e;
    while (pop_event(&e))
        handle(&e);
    if (!active)
        return;
    track_pointer();
    if (files_next[0] && console_waiting_input()) {
        char path[FS_PATH_MAX];
        strlcpy(path, files_next, sizeof(path));
        files_next[0] = 0;
        files_load(path);
    }
    u64 now = time_ms();
    if (now - last_second >= 1000) {
        last_second = now;
        sample_net();
        if (win[W_MONITOR].visible)
            repaint(W_MONITOR);
        damage_panel();
    }
    present();
}

/* ---------------------------------------------------------------- setup */
static bool make_window(int i, const char *title, int x, int y, int bw, int bh)
{
    struct window *w = &win[i];
    w->title = title;
    if (!surface_alloc(&w->body, bw, bh))
        return false;
    w->r = rect_make(x, y, bw + 2 * S, TITLE_H + bh + S);
    return true;
}

static bool desktop_setup(void)
{
    W = (int)hw->width, H = (int)hw->height;
    if (W < 640 || H < 480) {
        printk("ui: the screen (%dx%d) is too small for the desktop\n", W, H);
        return false;
    }
    S = W >= 2400 ? 2 : 1;
    if (!surface_alloc(&back, W, H) || !surface_alloc(&wall, W, H)) {
        printk("ui: not enough memory for the desktop\n");
        return false;
    }
    build_wallpaper();

    /* Terminal on the left; monitor and files stacked on the right when
     * there is room, otherwise they open on top when asked for. */
    int top = PANEL_H + MARGIN, bottom = H - DOCK_BOTTOM - DOCK_H - MARGIN;
    int side_w = 380 * S;
    bool wide = W >= 1100 * S;
    int cw = 8 * S, chh = 16 * S;
    int avail_w = wide ? W - 3 * MARGIN - side_w - 2 * S : W - 2 * MARGIN - 2 * S;
    int cols = MIN(avail_w / cw, 110), rows = MIN((bottom - top - TITLE_H - S) / chh, 36);
    if (!make_window(W_TERM, "Terminal", MARGIN, top, cols * cw, rows * chh))
        return false;
    int sx = W - MARGIN - side_w - 2 * S;
    int mon_h = 354 * S;
    if (!make_window(W_MONITOR, "System Monitor", wide ? sx : W - side_w - 2 * MARGIN,
                     wide ? top : top + 40 * S, side_w, mon_h))
        return false;
    int files_y = top + TITLE_H + mon_h + S + MARGIN;
    int files_h = MAX(bottom - files_y - TITLE_H - S, 170 * S);
    if (!wide || files_y + TITLE_H + files_h > bottom)
        files_y = top + 70 * S, files_h = 260 * S;
    if (!make_window(W_FILES, "Files", wide ? sx : W - side_w - 3 * MARGIN, files_y, side_w,
                     files_h))
        return false;
    if (!make_window(W_ABOUT, "About Sonar", (W - 420 * S) / 2, top + 60 * S, 420 * S, 300 * S))
        return false;
    win[W_MONITOR].paint = paint_monitor;
    win[W_FILES].paint = paint_files;
    win[W_FILES].click = files_click;
    win[W_FILES].wheel = files_wheel;
    win[W_ABOUT].paint = paint_about;

    struct window *t = &win[W_TERM];
    term_fb = (struct framebuffer){ "terminal", (u8 *)t->body.px,  (u32)t->body.w,
                                    (u32)t->body.h, (u32)t->body.w * 4, false,
                                    term_flush, NULL };
    t->visible = true;
    win[W_MONITOR].visible = wide;
    win[W_FILES].visible = wide;
    files_load("/");
    for (int i = 0; i < NWIN; i++)
        repaint(i);
    ptr_x = W / 2, ptr_y = H / 2;
    return true;
}

void ui_boot(void)
{
    hw = fbcon_screen();
    if (!hw)
        return;
    if (!desktop_setup())
        return;
    console_add_idle_hook(tick);
    started = true;
    if (strcmp(config_get("ui.desktop", "on"), "off") == 0) {
        printk("ui: desktop is off (config ui.desktop); F1 or 'desktop on' starts it\n");
        return;
    }
    printk("ui: desktop on %dx%d%s; F1 switches to the full-screen console\n", W, H,
           ptr_seen ? (ptr_abs ? ", absolute pointer" : ", mouse") : ", no pointer device");
    desktop_enter();
}

static int cmd_desktop(int argc, char **argv)
{
    if (!started) {
        printk("desktop: no display%s\n", hw ? " (screen too small or out of memory)" : "");
        return 1;
    }
    if (argc < 2) {
        printk("desktop is %s (%dx%d). Use: desktop on | off\n", active ? "on" : "off", W, H);
        return 0;
    }
    bool on = !strcmp(argv[1], "on");
    if (!on && strcmp(argv[1], "off")) {
        printk("usage: desktop [on|off]\n");
        return 1;
    }
    config_set("ui.desktop", on ? "on" : "off");
    if (!config_save() && !fs_is_ram())
        printk("desktop: could not save the setting\n");
    if (on)
        desktop_enter();
    else
        desktop_leave();
    return 0;
}
SHELL_COMMAND(desktop, "desktop", "desktop [on|off]",
              "show the windowed desktop, or only the full-screen console", cmd_desktop);
