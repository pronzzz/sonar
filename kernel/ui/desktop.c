/*
 * The Sonar desktop: a windowed user interface on the display.
 *
 * A compositor keeps a full-screen back buffer and redraws only damaged
 * rectangles: wallpaper and desktop launchers, windows from bottom to top,
 * the taskbar, any open popup, a tooltip and the mouse pointer.
 *
 * The taskbar is always on screen. From left to right: the Sonar menu (all
 * applications and power), the DeepSeek button, a search field, one button
 * per open window, and the tray (DeepSeek status, network, memory, clock).
 * The search field is universal: it finds applications, shell commands,
 * files, does arithmetic, and passes anything else to DeepSeek. The Super
 * key opens it from anywhere; F6 opens the DeepSeek box.
 *
 * Windows draw into their own surfaces. The Terminal window is the system
 * console itself: the framebuffer console (fbcon.c) is pointed at the
 * window's surface, so the shell and DeepSeek run there. Questions asked
 * from the taskbar, the menu, the search field or the file browser are sent
 * to the shell as "? question", so they use the same harness, permissions
 * and session as typing them.
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
/* Deep violet with lavender and crimson accents. */
#define C_TEXT 0xece8f6
#define C_MUTED 0xa9a1c2
#define C_DIM 0x6f688a
#define C_ACCENT 0x9484e6   /* lavender */
#define C_ACCENT_D 0x5b4ea3
#define C_RED 0xd8215a      /* crimson */
#define C_DS 0x4d6bfe       /* DeepSeek blue */
#define C_BODY 0x1c1828
#define C_BODY_ALT 0x221d31
#define C_TERM_BG 0x0f1115 /* fbcon's background */
#define C_FRAME 0x3b3352
#define C_TITLE 0x231e33
#define C_TITLE_ACTIVE 0x2d2644
#define C_GOOD 0x6fd99a
#define C_WARN 0xf0c05a
#define C_TRACK 0x2d2742
#define C_BAR 0x15111f
#define C_POP 0x1f1a2d
#define C_HOVER 0x332b4b
#define C_SEL 0x463a80
#define C_FIELD 0x0f0c17

static int S = 1; /* UI scale: 2 on very high resolution screens */
#define BAR_H (44 * S)
#define TITLE_H (32 * S)
#define RADIUS (8 * S)
#define SHADOW (18 * S)
#define MARGIN (16 * S)
#define BTN_W (34 * S) /* title bar buttons */
#define ICON_COL (104 * S)
#define TEXT_Y(h) (((h) - 16 * S) / 2)

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
static int work_h(void) { return H - BAR_H; }

/* ---------------------------------------------------------------- text */
static int text(struct surface *s, int x, int y, const char *str, u32 c)
{
    return gfx_ui_text(s, x, y, str, c, S, false);
}
static int textb(struct surface *s, int x, int y, const char *str, u32 c)
{
    return gfx_ui_text(s, x, y, str, c, S, true);
}
static int fit(struct surface *s, int x, int y, const char *str, u32 c, int max_w)
{
    return gfx_ui_fit(s, x, y, str, c, S, false, max_w);
}
static int fitb(struct surface *s, int x, int y, const char *str, u32 c, int max_w)
{
    return gfx_ui_fit(s, x, y, str, c, S, true, max_w);
}
static int tw(const char *str) { return gfx_ui_width(str, S, false); }
static int twb(const char *str) { return gfx_ui_width(str, S, true); }

static bool ieq_prefix(const char *s, const char *p)
{
    for (; *p; s++, p++)
        if (!*s || tolower((u8)*s) != tolower((u8)*p))
            return false;
    return true;
}

static bool icontains(const char *hay, const char *needle)
{
    if (!*needle)
        return true;
    for (; *hay; hay++)
        if (ieq_prefix(hay, needle))
            return true;
    return false;
}

/* ---------------------------------------------------------------- input */
enum { UEV_BUTTON, UEV_WHEEL, UEV_FKEY, UEV_KEY };
struct uev {
    u8 kind, button;
    bool down;
    int x, y, val;
};
#define NEV 128
static struct uev evq[NEV];
static u32 ev_head, ev_tail;
static spinlock_t evlock = SPINLOCK_INIT;

static volatile int ptr_x, ptr_y;
static volatile bool ptr_seen, ptr_abs;
static volatile bool button_down[3];
static int cur_x = -1000, cur_y = -1000; /* where the pointer is drawn */
static volatile bool grab;               /* a desktop text field has the keyboard */

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

void ui_function_key(int n)
{
    if (active && (n == UI_FKEY_SUPER || n == 6))
        grab = true; /* typing right after the key belongs to the field */
    push_event((struct uev){ UEV_FKEY, 0, true, 0, 0, n });
}

bool ui_keyboard_grabbed(void) { return active && grab; }
void ui_key(int key) { push_event((struct uev){ UEV_KEY, 0, true, 0, 0, key }); }
bool ui_desktop_active(void) { return active; }

/* ---------------------------------------------------------------- icons */
enum {
    IC_TERM, IC_FILES, IC_MONITOR, IC_ABOUT, IC_DEEPSEEK, IC_CONSOLE, IC_POWER, IC_RESTART,
    IC_CMD, IC_FILE, IC_FOLDER, IC_CALC, IC_HOME, IC_SETUP,
};

static void ring(struct surface *s, int cx, int cy, int r, int t, u32 c, u32 bg)
{
    gfx_disc(s, cx, cy, r, c);
    gfx_disc(s, cx, cy, r - t, bg);
}

/* The Sonar mark: a dot and two arcs of a ping. */
static void sonar_mark(struct surface *s, int cx, int cy, int r, u32 c, u32 bg)
{
    ring(s, cx, cy, r, MAX(r / 5, S), c, bg);
    ring(s, cx, cy, r * 6 / 10, MAX(r / 5, S), c, bg);
    gfx_disc(s, cx, cy, MAX(r / 4, S), c);
    /* cut the rings on the lower left so they read as a signal */
    gfx_fill(s, rect_make(cx - r - S, cy + r / 6, r - r / 6, r), bg);
}

/* A whale: DeepSeek's mascot animal, drawn from simple shapes. */
static void whale(struct surface *s, int cx, int cy, int sz, u32 c, u32 eye)
{
    int bw = sz * 62 / 100, bh = sz * 34 / 100;
    gfx_round_rect(s, rect_make(cx - bw / 2 + sz / 14, cy - bh / 2 + sz / 16, bw, bh), bh / 2, c, 256);
    /* tail: two lobes up and to the left */
    int tx = cx - bw / 2 + sz / 14, ty = cy - bh / 6;
    gfx_disc(s, tx - sz / 9, ty - sz / 7, MAX(sz / 10, S), c);
    gfx_disc(s, tx - sz / 20, ty - sz / 5, MAX(sz / 12, S), c);
    gfx_round_rect(s, rect_make(tx - sz / 10, ty - sz / 8, sz / 7, sz / 5), sz / 14, c, 256);
    gfx_disc(s, cx + bw / 4 + sz / 14, cy - sz / 30, MAX(sz / 22, S), eye);
}

static void draw_icon(struct surface *s, int kind, struct rect r)
{
    int sz = MIN(r.w, r.h), cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    int rad = MAX(sz / 5, 2 * S);
    int ts = sz >= 40 * S ? 2 * S : S; /* glyph text scale */
    switch (kind) {
    case IC_TERM:
    case IC_CMD:
        gfx_round_rect(s, r, rad, kind == IC_TERM ? 0x13101c : 0x2b2540, 256);
        gfx_frame(s, rect_make(r.x, r.y, r.w, r.h), 0xffffff, 26);
        gfx_text_bold(s, r.x + sz / 7, cy - 8 * ts, kind == IC_TERM ? ">_" : ">", C_GOOD, ts);
        break;
    case IC_FILES:
    case IC_FOLDER:
    case IC_HOME: {
        u32 c = kind == IC_HOME ? 0xb7a8ff : 0xf0b54d;
        gfx_round_rect(s, rect_make(r.x + sz / 12, r.y + sz / 5, sz * 4 / 10, sz / 5), rad / 2,
                       rgb_mix(c, 0, 50), 256);
        gfx_round_rect(s, rect_make(r.x + sz / 12, r.y + sz * 3 / 10, sz - sz / 6, sz * 6 / 10), rad / 2,
                       c, 256);
        if (kind == IC_HOME) { /* a little house */
            int hx = cx, hy = cy + sz / 8;
            for (int k = 0; k < sz / 6; k++)
                gfx_fill(s, rect_make(hx - k, hy - sz / 6 + k, 2 * k + 1, 1), 0x3b2f73);
            gfx_fill(s, rect_make(hx - sz / 9, hy, 2 * sz / 9, sz / 7), 0x3b2f73);
        }
        break;
    }
    case IC_MONITOR: {
        gfx_round_rect(s, r, rad, 0x138a8c, 256);
        static const int hs[3] = { 40, 75, 55 };
        int bw = MAX(sz / 6, S);
        for (int k = 0; k < 3; k++) {
            int bh = sz * hs[k] / 120;
            gfx_round_rect(s, rect_make(cx - bw * 2 + k * bw * 3 / 2 - bw / 4, cy + sz / 4 - bh, bw, bh),
                           MAX(bw / 3, 1), 0xffffff, 235);
        }
        break;
    }
    case IC_ABOUT:
        gfx_disc(s, cx, cy, sz / 2, C_ACCENT_D);
        gfx_text_bold(s, cx - 4 * ts, cy - 8 * ts, "i", 0xffffff, ts);
        break;
    case IC_DEEPSEEK:
        gfx_disc(s, cx, cy, sz / 2, C_DS);
        whale(s, cx, cy, sz, 0xffffff, C_DS);
        break;
    case IC_CONSOLE:
        gfx_round_rect(s, r, rad, 0x2b2540, 256);
        gfx_frame(s, rect_make(r.x + sz / 6, r.y + sz / 4, sz * 2 / 3, sz / 2), 0xffffff, 200);
        break;
    case IC_POWER:
    case IC_RESTART: {
        u32 bg = kind == IC_POWER ? C_RED : 0x3d6fd1;
        gfx_disc(s, cx, cy, sz / 2, bg);
        ring(s, cx, cy, sz * 3 / 10, MAX(sz / 12, S), 0xffffff, bg);
        if (kind == IC_POWER) {
            gfx_fill(s, rect_make(cx - sz / 8, cy - sz * 4 / 10, sz / 4, sz * 3 / 10), bg);
            gfx_fill(s, rect_make(cx - MAX(sz / 24, 1), cy - sz * 4 / 10, MAX(sz / 12, S), sz * 3 / 10),
                     0xffffff);
        } else {
            gfx_fill(s, rect_make(cx, cy - sz * 4 / 10, sz / 4, sz / 4), bg);
            for (int k = 0; k < sz / 8; k++)
                gfx_fill(s, rect_make(cx + k / 2, cy - sz * 3 / 10 - sz / 8 + k, sz / 8 - k, 1), 0xffffff);
        }
        break;
    }
    case IC_FILE:
        gfx_round_rect(s, rect_make(cx - sz * 3 / 10, r.y + sz / 10, sz * 6 / 10, sz * 8 / 10), rad / 2,
                       0xcfc8e6, 256);
        for (int k = 0; k < 3; k++)
            gfx_hline(s, cx - sz / 6, cy - sz / 8 + k * sz / 6, sz / 3, 0x6f688a);
        break;
    case IC_CALC:
        gfx_round_rect(s, r, rad, 0x8a4fc9, 256);
        gfx_text_bold(s, cx - 4 * ts, cy - 8 * ts, "=", 0xffffff, ts);
        break;
    case IC_SETUP:
        gfx_disc(s, cx, cy, sz / 2, 0x5a5374);
        ring(s, cx, cy, sz / 4, MAX(sz / 10, S), 0xffffff, 0x5a5374);
        break;
    }
}

/* ---------------------------------------------------------------- windows */
struct window {
    const char *title;
    int icon;
    struct rect r;    /* frame, in screen coordinates */
    struct rect saved; /* frame before maximizing */
    struct surface body;
    bool open, minimized, maximized;
    void (*paint)(struct window *w);
    void (*click)(struct window *w, int x, int y);
    void (*wheel)(struct window *w, int delta);
};
enum { W_TERM, W_MONITOR, W_FILES, W_ABOUT, NWIN };
static struct window win[NWIN];
static int zorder[NWIN] = { W_ABOUT, W_FILES, W_MONITOR, W_TERM }; /* last is on top */

static bool visible(int i) { return win[i].open && !win[i].minimized; }

static struct rect body_rect(const struct window *w)
{
    return rect_make(w->r.x + S, w->r.y + TITLE_H, w->body.w, w->body.h);
}

static struct rect outer_rect(const struct window *w)
{
    return rect_make(w->r.x - SHADOW, w->r.y - SHADOW, w->r.w + 2 * SHADOW,
                     w->r.h + 2 * SHADOW + SHADOW / 3);
}

enum { TB_MIN, TB_MAX, TB_CLOSE };
static struct rect title_button(const struct window *w, int b)
{
    return rect_make(w->r.x + w->r.w - S - (3 - b) * BTN_W, w->r.y + S, BTN_W, TITLE_H - 2 * S);
}

static int top_window(void)
{
    for (int z = NWIN - 1; z >= 0; z--)
        if (visible(zorder[z]))
            return zorder[z];
    return -1;
}

static int window_at(int x, int y)
{
    for (int z = NWIN - 1; z >= 0; z--) {
        struct window *w = &win[zorder[z]];
        if (visible(zorder[z]) && rect_contains(w->r, x, y))
            return zorder[z];
    }
    return -1;
}

static void damage_bar(void) { damage(rect_make(0, H - BAR_H - 4 * S, W, BAR_H + 4 * S)); }

static void repaint(int i)
{
    struct window *w = &win[i];
    if (w->paint) {
        surface_reset_clip(&w->body);
        w->paint(w);
    }
    if (visible(i))
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
    damage_bar();
}

static void show_window(int i)
{
    struct window *w = &win[i];
    if (!visible(i)) {
        w->open = true;
        w->minimized = false;
        repaint(i);
    }
    raise_window(i);
}

static void after_hide(int i)
{
    damage(outer_rect(&win[i]));
    damage_bar();
    int t = top_window();
    if (t >= 0)
        damage(outer_rect(&win[t])); /* its title shows focus now */
}

static void close_window(int i)
{
    win[i].open = false;
    win[i].minimized = false;
    after_hide(i);
}

static void minimize_window(int i)
{
    win[i].minimized = true;
    after_hide(i);
}

static void toggle_window(int i)
{
    if (visible(i) && top_window() == i)
        minimize_window(i);
    else
        show_window(i);
}

/* ---------------------------------------------------------------- terminal */
static struct framebuffer term_fb;
static char replay[24 * 1024];

/* fbcon's flush for the terminal surface: runs with the console lock held. */
static void present_locked(void);
static void term_flush(void *ctx, u32 x, u32 y, u32 w, u32 h)
{
    (void)ctx;
    struct window *t = &win[W_TERM];
    if (visible(W_TERM)) {
        struct rect b = body_rect(t);
        damage(rect_make(b.x + (int)x, b.y + (int)y, (int)w, (int)h));
    }
    if (active && smp_cpu_id() == 0)
        present_locked();
}

/* Give a window a new frame (and body size). The Terminal's console is
 * re-wrapped for its new size. */
static bool resize_window(int i, struct rect frame)
{
    struct window *w = &win[i];
    int bw = frame.w - 2 * S, bh = frame.h - TITLE_H - S;
    if (i == W_TERM) { /* whole character cells */
        bw -= bw % (8 * S);
        bh -= bh % (16 * S);
        frame.w = bw + 2 * S;
        frame.h = TITLE_H + bh + S;
    }
    struct surface nb;
    if (bw < 120 * S || bh < 60 * S || !surface_alloc(&nb, bw, bh))
        return false;
    damage(outer_rect(w));
    struct surface old = w->body;
    if (i == W_TERM) {
        u64 f = console_lock();
        size_t n = console_log_copy_locked(replay, sizeof(replay));
        w->body = nb;
        w->r = frame;
        term_fb = (struct framebuffer){ "terminal", (u8 *)nb.px, (u32)nb.w, (u32)nb.h,
                                        (u32)nb.w * 4, false, term_flush, NULL };
        if (active)
            fbcon_retarget_locked(&term_fb, replay, n);
        console_unlock(f);
    } else {
        w->body = nb;
        w->r = frame;
    }
    surface_free(&old);
    repaint(i);
    damage(outer_rect(w));
    damage_bar();
    return true;
}

static void toggle_maximize(int i)
{
    struct window *w = &win[i];
    if (w->maximized) {
        if (resize_window(i, w->saved))
            w->maximized = false;
    } else {
        struct rect saved = w->r;
        if (resize_window(i, rect_make(0, 0, W, work_h()))) {
            w->saved = saved;
            w->maximized = true;
        }
    }
    raise_window(i);
}

/* ---------------------------------------------------------------- actions */
static void desktop_leave(void);
static void ask_deepseek(const char *q)
{
    show_window(W_TERM);
    console_input("? ", 2);
    console_input(q, strlen(q));
    console_input("\r", 1);
}

static void run_command(const char *cmd)
{
    show_window(W_TERM);
    console_input(cmd, strlen(cmd));
    console_input("\r", 1);
}

static char files_next[FS_PATH_MAX];      /* directory to load at the next safe moment */
static char files_pick[FS_NAME_MAX + 1];  /* entry to select after loading */

static void open_path(const char *path, bool dir)
{
    if (dir) {
        strlcpy(files_next, path, sizeof(files_next));
        files_pick[0] = 0;
    } else {
        strlcpy(files_next, path, sizeof(files_next));
        char *slash = strrchr(files_next, '/');
        strlcpy(files_pick, slash ? slash + 1 : path, sizeof(files_pick));
        if (slash && slash != files_next)
            *slash = 0;
        else
            strlcpy(files_next, "/", sizeof(files_next));
    }
    show_window(W_FILES);
}

/* ---------------------------------------------------------------- applications */
enum { A_DEEPSEEK, A_TERM, A_FILES, A_MONITOR, A_ABOUT, A_SETUP, A_CONSOLE, A_RESTART, A_POWEROFF, NAPP };
static const struct app {
    const char *name, *detail, *keywords;
    int icon;
} apps[NAPP] = {
    { "DeepSeek Assistant", "Ask anything, or let it use the system", "ai assistant chat ask question help",
      IC_DEEPSEEK },
    { "Terminal", "Sonar shell", "shell console command prompt cli", IC_TERM },
    { "Files", "Browse the file system", "folder browse explorer home documents disk", IC_FILES },
    { "System Monitor", "CPU, memory, disk and network", "cpu memory ram disk network usage task",
      IC_MONITOR },
    { "About Sonar", "Version and keyboard shortcuts", "version info help keys shortcuts", IC_ABOUT },
    { "DeepSeek Setup", "Choose the model and its settings", "model settings configure setup",
      IC_SETUP },
    { "Full-screen Console", "Leave the desktop (F1 returns)", "tty text fullscreen exit desktop",
      IC_CONSOLE },
    { "Restart", "Restart the computer", "reboot restart", IC_RESTART },
    { "Power Off", "Shut down the computer", "shutdown poweroff halt power off", IC_POWER },
};

static void open_deepseek_box(void);
static void launch(int a)
{
    switch (a) {
    case A_DEEPSEEK: open_deepseek_box(); break;
    case A_TERM: show_window(W_TERM); break;
    case A_FILES: show_window(W_FILES); break;
    case A_MONITOR: show_window(W_MONITOR); break;
    case A_ABOUT: show_window(W_ABOUT); break;
    case A_SETUP: run_command("setup"); break;
    case A_CONSOLE: desktop_leave(); break;
    case A_RESTART: run_command("reboot"); break;
    case A_POWEROFF: run_command("poweroff"); break;
    }
}

/* ---------------------------------------------------------------- taskbar layout */
enum { HIT_NONE, HIT_MENU, HIT_DS, HIT_SEARCH, HIT_TASK, HIT_TRAY_AI, HIT_TRAY_NET, HIT_TRAY_MEM,
       HIT_CLOCK, HIT_ICON, HIT_POP };
static struct rect r_menu, r_ds, r_search, r_task[NWIN], r_ai, r_net, r_mem, r_clock;
static int task_win[NWIN], ntask;

static void layout_bar(void)
{
    int y = H - BAR_H, h = BAR_H;
    int x = 6 * S;
    r_menu = rect_make(x, y + 5 * S, 34 * S + twb("Sonar") + 12 * S, h - 10 * S);
    x += r_menu.w + 6 * S;
    r_ds = rect_make(x, y + 5 * S, h - 10 * S, h - 10 * S);
    x += r_ds.w + 8 * S;
    int sw = W >= 1200 * S ? 340 * S : 240 * S;
    r_search = rect_make(x, y + 7 * S, sw, h - 14 * S);
    x += sw + 12 * S;
    /* tray, right to left */
    int rx = W - 8 * S;
    r_clock = rect_make(rx - 96 * S, y + 4 * S, 96 * S, h - 8 * S);
    rx = r_clock.x - 6 * S;
    r_mem = rect_make(rx - 72 * S, y + 6 * S, 72 * S, h - 12 * S);
    rx = r_mem.x - 4 * S;
    r_net = rect_make(rx - 30 * S, y + 6 * S, 30 * S, h - 12 * S);
    rx = r_net.x - 6 * S;
    const char *model = ai_model_name();
    int aw = 30 * S + tw(model ? "DeepSeek ready" : "No model");
    r_ai = rect_make(rx - aw, y + 7 * S, aw, h - 14 * S);
    rx = r_ai.x - 12 * S;
    /* window buttons in the middle */
    ntask = 0;
    for (int i = 0; i < NWIN; i++)
        if (win[i].open)
            task_win[ntask++] = i;
    int avail = rx - x, bw = ntask ? MIN(190 * S, avail / ntask - 4 * S) : 0;
    for (int k = 0; k < ntask; k++)
        r_task[k] = rect_make(x + k * (bw + 4 * S), y + 5 * S, bw, h - 10 * S);
}

/* ---------------------------------------------------------------- desktop launchers */
enum { L_HOME, L_TERM, L_DEEPSEEK, L_MONITOR, NLAUNCH };
static const struct {
    const char *label;
    int icon;
} launchers[NLAUNCH] = {
    { "Home", IC_HOME }, { "Terminal", IC_TERM }, { "DeepSeek", IC_DEEPSEEK }, { "Monitor", IC_MONITOR },
};

static struct rect launcher_rect(int i) { return rect_make(8 * S, 12 * S + i * 92 * S, 88 * S, 84 * S); }

static void launcher_activate(int i)
{
    switch (i) {
    case L_HOME: open_path("/home", true); break;
    case L_TERM: show_window(W_TERM); break;
    case L_DEEPSEEK: open_deepseek_box(); break;
    case L_MONITOR: show_window(W_MONITOR); break;
    }
}

/* ---------------------------------------------------------------- text fields */
struct field {
    char s[200];
    int len;
};
static struct field search_f, ask_f;

static bool field_key(struct field *f, int key)
{
    if (key == 0x7f || key == 8) {
        if (f->len > 0)
            f->s[--f->len] = 0;
        return true;
    }
    if (key >= 32 && key < 127 && f->len < (int)sizeof(f->s) - 1) {
        f->s[f->len++] = (char)key;
        f->s[f->len] = 0;
        return true;
    }
    return false;
}

static void draw_field(struct surface *s, struct rect r, const struct field *f, const char *placeholder,
                       bool focused, bool icon)
{
    gfx_round_rect(s, r, r.h / 2, focused ? C_FRAME : 0x2a2340, 256);
    gfx_round_rect(s, rect_make(r.x + S, r.y + S, r.w - 2 * S, r.h - 2 * S), r.h / 2 - S, C_FIELD, 256);
    int x = r.x + 12 * S, ty = r.y + TEXT_Y(r.h);
    if (icon) { /* magnifier */
        int cx = r.x + 16 * S, cy = r.y + r.h / 2 - S;
        ring(s, cx, cy, 6 * S, 2 * S, focused ? C_ACCENT : C_MUTED, C_FIELD);
        for (int k = 0; k < 4 * S; k++)
            gfx_fill(s, rect_make(cx + 4 * S + k, cy + 4 * S + k, 2 * S, 2 * S), focused ? C_ACCENT : C_MUTED);
        x = r.x + 30 * S;
    }
    int maxw = r.x + r.w - 12 * S - x;
    if (!f->len) {
        fit(s, x, ty, placeholder, C_DIM, maxw);
        if (focused)
            gfx_fill(s, rect_make(x - S, r.y + 7 * S, 2 * S, r.h - 14 * S), C_ACCENT);
        return;
    }
    /* show the end of long text */
    const char *p = f->s;
    while (*p && tw(p) > maxw - 4 * S)
        p++;
    int w = text(s, x, ty, p, C_TEXT);
    if (focused)
        gfx_fill(s, rect_make(x + w + S, r.y + 7 * S, 2 * S, r.h - 14 * S), C_ACCENT);
}

/* ---------------------------------------------------------------- universal search */
enum { R_ASK, R_APP, R_CMD, R_FILE, R_DIR, R_CALC };
struct result {
    int kind, app;
    char label[120], detail[120], arg[FS_PATH_MAX];
};
#define MAXRES 9
static struct result res[MAXRES];
static int nres, sel;

/* Paths on the system disk, for file search; built when search opens. */
#define IDX_MAX 3000
static char (*idx)[FS_PATH_MAX];
static u8 *idx_dir;
static int nidx;
static u64 idx_time;
static bool idx_wanted;

static void index_files(void)
{
    if (!idx) {
        idx = kmalloc(sizeof(*idx) * IDX_MAX);
        idx_dir = kmalloc(IDX_MAX);
        if (!idx || !idx_dir)
            return;
    }
    nidx = 0;
    strlcpy(idx[nidx], "/", FS_PATH_MAX);
    idx_dir[nidx++] = 1;
    for (int d = 0; d < nidx && nidx < IDX_MAX; d++) {
        if (!idx_dir[d])
            continue;
        struct fs_dirent *ents;
        int n;
        if (fs_list(idx[d], &ents, &n) < 0)
            continue;
        for (int i = 0; i < n && nidx < IDX_MAX; i++) {
            const char *base = idx[d];
            snprintf(idx[nidx], FS_PATH_MAX, "%s%s%s", base, strcmp(base, "/") ? "/" : "", ents[i].name);
            idx_dir[nidx++] = ents[i].type == FT_DIR;
        }
        kfree(ents);
    }
    idx_time = time_ms();
}

/* Arithmetic: + - * / % and parentheses, in fixed point (6 decimals). */
#define FX 1000000LL
static const char *cp;
static bool calc_ok;
static i64 c_expr(void);
static void c_space(void)
{
    while (*cp == ' ')
        cp++;
}
static i64 c_atom(void)
{
    c_space();
    if (*cp == '(') {
        cp++;
        i64 v = c_expr();
        c_space();
        if (*cp == ')')
            cp++;
        else
            calc_ok = false;
        return v;
    }
    if (*cp == '-') {
        cp++;
        return -c_atom();
    }
    if (!isdigit((u8)*cp) && *cp != '.') {
        calc_ok = false;
        return 0;
    }
    i64 v = 0, frac = 0, scale = 1;
    while (isdigit((u8)*cp))
        v = v * 10 + (*cp++ - '0');
    if (*cp == '.') {
        cp++;
        while (isdigit((u8)*cp)) {
            if (scale < FX)
                frac = frac * 10 + (*cp - '0'), scale *= 10;
            cp++;
        }
    }
    return v * FX + frac * (FX / scale);
}
static i64 c_term(void)
{
    i64 v = c_atom();
    for (;;) {
        c_space();
        char op = *cp;
        if (op != '*' && op != '/' && op != '%' && op != 'x')
            return v;
        cp++;
        i64 b = c_atom();
        if (op == '*' || op == 'x')
            v = v / 1000 * (b / 1000);
        else if (!b)
            calc_ok = false;
        else if (op == '/')
            v = v * 1000 / b * 1000;
        else
            v = v % b;
    }
}
static i64 c_expr(void)
{
    i64 v = c_term();
    for (;;) {
        c_space();
        if (*cp != '+' && *cp != '-')
            return v;
        char op = *cp++;
        i64 b = c_term();
        v = op == '+' ? v + b : v - b;
    }
}

static bool calculate(const char *q, char *out, size_t n)
{
    bool op = false;
    for (const char *p = q; *p; p++) {
        if (strchr("+-*/%x", *p) && p != q)
            op = true;
        if (!strchr("0123456789.+-*/%x() ", *p))
            return false;
    }
    if (!op)
        return false;
    cp = q;
    calc_ok = true;
    i64 v = c_expr();
    c_space();
    if (!calc_ok || *cp)
        return false;
    bool neg = v < 0;
    u64 a = (u64)(neg ? -v : v);
    u64 ip = a / FX, fp = a % FX;
    char frac[12] = "";
    if (fp) {
        snprintf(frac, sizeof(frac), ".%06llu", (unsigned long long)fp);
        for (int k = (int)strlen(frac) - 1; k > 0 && frac[k] == '0'; k--)
            frac[k] = 0;
    }
    snprintf(out, n, "%s%llu%s", neg ? "-" : "", (unsigned long long)ip, frac);
    return true;
}

static bool looks_like_question(const char *q)
{
    static const char *const starts[] = { "what", "how", "why", "who", "when", "where", "which", "is ",
                                          "are ", "can ", "does ", "do ", "explain", "tell", "write",
                                          "summar", "help", "show me", "find out", "should", "could" };
    int words = 1;
    for (const char *p = q; *p; p++)
        if (*p == ' ' && p[1] && p[1] != ' ')
            words++;
    if (strchr(q, '?') || words >= 4)
        return true;
    for (size_t i = 0; i < ARRAY_SIZE(starts); i++)
        if (ieq_prefix(q, starts[i]))
            return true;
    return false;
}

static const char *const hidden_cmds[] = { "fstest", "smptest", "jcrash", "gpustat", "?", "chat" };

static void add_result(int kind, int app, const char *label, const char *detail, const char *arg)
{
    if (nres >= MAXRES)
        return;
    struct result *r = &res[nres++];
    r->kind = kind;
    r->app = app;
    strlcpy(r->label, label, sizeof(r->label));
    strlcpy(r->detail, detail ? detail : "", sizeof(r->detail));
    strlcpy(r->arg, arg ? arg : "", sizeof(r->arg));
}

static void search_update(void)
{
    nres = 0;
    sel = 0;
    char q[200];
    const char *s = search_f.s;
    while (*s == ' ')
        s++;
    strlcpy(q, s, sizeof(q));
    for (int n = (int)strlen(q); n > 0 && q[n - 1] == ' '; n--)
        q[n - 1] = 0;
    char buf[160];
    if (!q[0]) {
        static const int sugg[] = { A_DEEPSEEK, A_TERM, A_FILES, A_MONITOR, A_ABOUT };
        for (size_t i = 0; i < ARRAY_SIZE(sugg); i++)
            add_result(R_APP, sugg[i], apps[sugg[i]].name, apps[sugg[i]].detail, NULL);
        return;
    }
    bool question = looks_like_question(q);
    snprintf(buf, sizeof(buf), "Ask DeepSeek: %s", q);
    if (question)
        add_result(R_ASK, 0, buf, "The answer appears in the Terminal", q);
    char val[48];
    if (calculate(q, val, sizeof(val))) {
        snprintf(buf, sizeof(buf), "= %s", val);
        add_result(R_CALC, 0, buf, q, val);
    }
    for (int a = 0; a < NAPP; a++)
        if (icontains(apps[a].name, q) || icontains(apps[a].keywords, q))
            add_result(R_APP, a, apps[a].name, apps[a].detail, NULL);
    /* commands: the first word names a command */
    char first[40];
    size_t k = 0;
    while (q[k] && q[k] != ' ' && k < sizeof(first) - 1)
        first[k] = q[k], k++;
    first[k] = 0;
    const struct shell_cmd *exact = shell_find(first);
    bool hidden = false;
    for (size_t i = 0; i < ARRAY_SIZE(hidden_cmds); i++)
        if (!strcmp(first, hidden_cmds[i]))
            hidden = true;
    if (exact && !hidden) {
        snprintf(buf, sizeof(buf), "Run: %s", q);
        add_result(R_CMD, 0, buf, exact->help, q);
    } else if (!strchr(q, ' ')) {
        char *all = kmalloc(16384);
        if (all) {
            shell_list_commands(all, 16384);
            int found = 0;
            for (char *ln = all; *ln && found < 3;) {
                char *nl = strchr(ln, '\n');
                if (nl)
                    *nl = 0;
                char name[40];
                size_t j = 0;
                while (ln[j] && ln[j] != ' ' && j < sizeof(name) - 1)
                    name[j] = ln[j], j++;
                name[j] = 0;
                bool hid = false;
                for (size_t i = 0; i < ARRAY_SIZE(hidden_cmds); i++)
                    if (!strcmp(name, hidden_cmds[i]))
                        hid = true;
                const struct shell_cmd *c = hid ? NULL : shell_find(name);
                if (c && ieq_prefix(name, q)) {
                    snprintf(buf, sizeof(buf), "Run: %s", c->usage[0] ? c->usage : name);
                    add_result(R_CMD, 0, buf, c->help, name);
                    found++;
                }
                if (!nl)
                    break;
                ln = nl + 1;
            }
            kfree(all);
        }
    }
    /* files and folders whose name matches */
    int files = 0;
    for (int i = 1; i < nidx && files < 4 && nres < MAXRES - !question; i++) {
        const char *base = strrchr(idx[i], '/');
        base = base ? base + 1 : idx[i];
        if (icontains(base, q)) {
            add_result(idx_dir[i] ? R_DIR : R_FILE, 0, base, idx[i], idx[i]);
            files++;
        }
    }
    if (!question) {
        if (nres >= MAXRES)
            nres = MAXRES - 1;
        snprintf(buf, sizeof(buf), "Ask DeepSeek: %s", q);
        add_result(R_ASK, 0, buf, "The answer appears in the Terminal", q);
    }
}

/* ---------------------------------------------------------------- popups */
enum { POP_NONE, POP_MENU, POP_SEARCH, POP_DEEPSEEK };
static int popup = POP_NONE;
static struct rect pop_r;
static int pop_hover = -1;
#define ROW_H (34 * S)
#define MENU_W (290 * S)
#define SEARCH_W (600 * S)
#define DS_W (480 * S)

static const int menu_items[] = { A_DEEPSEEK, A_TERM, A_FILES, A_MONITOR, A_ABOUT, A_SETUP, -1,
                                  A_CONSOLE, A_RESTART, A_POWEROFF };
#define NMENU ((int)ARRAY_SIZE(menu_items))
#define MENU_HEAD (56 * S)

static const char *const suggestions[] = {
    "How much free space is on the disk?",
    "What is using memory right now?",
    "What files are in /home?",
    "Is the network connected?",
};
#define NSUGG ((int)ARRAY_SIZE(suggestions))
#define DS_HEAD (64 * S)

static struct rect popup_rect(int p)
{
    int bottom = H - BAR_H - 6 * S;
    switch (p) {
    case POP_MENU: {
        int h = MENU_HEAD + NMENU * ROW_H + 10 * S;
        return rect_make(r_menu.x, bottom - h, MENU_W, h);
    }
    case POP_SEARCH: {
        int h = 40 * S + MAX(nres, 1) * ROW_H + 30 * S;
        return rect_make(MIN(r_search.x, W - SEARCH_W - 8 * S), bottom - h, SEARCH_W, h);
    }
    case POP_DEEPSEEK: {
        int h = DS_HEAD + 52 * S + 26 * S + NSUGG * ROW_H + 36 * S;
        return rect_make(MIN(r_ds.x, W - DS_W - 8 * S), bottom - h, DS_W, h);
    }
    }
    return rect_make(0, 0, 0, 0);
}

static struct rect pop_outer(void)
{
    return rect_make(pop_r.x - SHADOW, pop_r.y - SHADOW, pop_r.w + 2 * SHADOW, pop_r.h + 2 * SHADOW);
}

static void close_popup(void)
{
    if (popup == POP_NONE)
        return;
    damage(pop_outer());
    popup = POP_NONE;
    pop_hover = -1;
    grab = false;
    search_f.len = 0;
    search_f.s[0] = 0;
    damage_bar();
}

static void open_popup(int p)
{
    close_popup();
    popup = p;
    pop_hover = -1;
    grab = p == POP_SEARCH || p == POP_DEEPSEEK;
    if (p == POP_SEARCH) {
        search_f.len = 0;
        search_f.s[0] = 0;
        if (!idx || time_ms() - idx_time > 30000)
            idx_wanted = true;
        search_update();
    }
    if (p == POP_DEEPSEEK) {
        ask_f.len = 0;
        ask_f.s[0] = 0;
    }
    pop_r = popup_rect(p);
    damage(pop_outer());
    damage_bar();
}

static void refresh_popup(void)
{
    damage(pop_outer());
    pop_r = popup_rect(popup);
    damage(pop_outer());
    damage_bar();
}

static void open_deepseek_box(void) { open_popup(POP_DEEPSEEK); }

/* Row index under (x, y) in the open popup, or -1. */
static int popup_row(int x, int y)
{
    if (!rect_contains(pop_r, x, y))
        return -1;
    int ry;
    switch (popup) {
    case POP_MENU:
        ry = y - pop_r.y - MENU_HEAD;
        if (ry < 0 || ry / ROW_H >= NMENU || menu_items[ry / ROW_H] < 0)
            return -1;
        return ry / ROW_H;
    case POP_SEARCH:
        ry = y - pop_r.y - 40 * S;
        return ry >= 0 && ry / ROW_H < nres ? ry / ROW_H : -1;
    case POP_DEEPSEEK:
        ry = y - pop_r.y - DS_HEAD - 52 * S - 26 * S;
        return ry >= 0 && ry / ROW_H < NSUGG ? ry / ROW_H : -1;
    }
    return -1;
}

static void activate_result(int i)
{
    if (i < 0 || i >= nres)
        return;
    struct result r = res[i];
    close_popup();
    switch (r.kind) {
    case R_ASK: ask_deepseek(r.arg); break;
    case R_APP: launch(r.app); break;
    case R_CMD: run_command(r.arg); break;
    case R_FILE: open_path(r.arg, false); break;
    case R_DIR: open_path(r.arg, true); break;
    case R_CALC: break;
    }
}

static void popup_activate(int row)
{
    if (popup == POP_MENU) {
        int a = menu_items[row];
        close_popup();
        if (a >= 0)
            launch(a);
    } else if (popup == POP_SEARCH) {
        activate_result(row);
    } else if (popup == POP_DEEPSEEK) {
        const char *q = suggestions[row];
        close_popup();
        ask_deepseek(q);
    }
}

static void popup_key(int key)
{
    if (key == 27) {
        close_popup();
        return;
    }
    if (popup == POP_SEARCH) {
        if (key == '\r') {
            activate_result(sel);
        } else if (key == UI_KEY_DOWN || key == '\t') {
            sel = nres ? (sel + 1) % nres : 0;
        } else if (key == UI_KEY_UP) {
            sel = nres ? (sel + nres - 1) % nres : 0;
        } else if (field_key(&search_f, key)) {
            search_update();
        }
        refresh_popup();
    } else if (popup == POP_DEEPSEEK) {
        if (key == '\r') {
            if (ask_f.len) {
                char q[200];
                strlcpy(q, ask_f.s, sizeof(q));
                close_popup();
                ask_deepseek(q);
            }
            return;
        }
        if (field_key(&ask_f, key))
            refresh_popup();
    }
}

/* ---------------------------------------------------------------- drawing */
static void draw_x(struct surface *s, int cx, int cy, int r, u32 color)
{
    for (int i = -r; i <= r; i++) {
        gfx_fill(s, rect_make(cx + i, cy + i, S + 1, S + 1), color);
        gfx_fill(s, rect_make(cx + i, cy - i, S + 1, S + 1), color);
    }
}

static int hover_win = -1, hover_btn = -1; /* title bar button under the pointer */

static void draw_window(int i, bool focused)
{
    struct window *w = &win[i];
    struct rect r = w->r;
    if (!w->maximized)
        gfx_shadow(&back, r, SHADOW, focused ? 170 : 110);
    int rad = w->maximized ? 0 : RADIUS;
    gfx_round_rect(&back, rect_make(r.x, r.y, r.w, TITLE_H + 2 * RADIUS), rad, focused ? C_ACCENT_D : C_FRAME,
                   256);
    gfx_fill(&back, rect_make(r.x, r.y + RADIUS, r.w, r.h - RADIUS), focused ? 0x3e3566 : C_FRAME);
    u32 tc = focused ? C_TITLE_ACTIVE : C_TITLE;
    gfx_round_rect(&back, rect_make(r.x + S, r.y + S, r.w - 2 * S, TITLE_H + RADIUS), MAX(rad - S, 0), tc, 256);
    if (focused) /* accent line along the top */
        gfx_fill(&back, rect_make(r.x + rad, r.y + S, r.w - 2 * rad, 2 * S), C_ACCENT);
    gfx_hline(&back, r.x + S, r.y + TITLE_H - 1, r.w - 2 * S, 0x0d0a14);
    draw_icon(&back, w->icon, rect_make(r.x + 10 * S, r.y + (TITLE_H - 18 * S) / 2, 18 * S, 18 * S));
    int ty = r.y + TEXT_Y(TITLE_H);
    int maxw = r.w - 3 * BTN_W - 50 * S;
    if (focused)
        fitb(&back, r.x + 36 * S, ty, w->title, C_TEXT, maxw);
    else
        fit(&back, r.x + 36 * S, ty, w->title, C_MUTED, maxw);
    for (int b = 0; b < 3; b++) {
        struct rect br = title_button(w, b);
        bool hov = hover_win == i && hover_btn == b;
        if (hov)
            gfx_fill(&back, br, b == TB_CLOSE ? C_RED : C_HOVER);
        int cx = br.x + br.w / 2, cy = br.y + br.h / 2;
        u32 gc = hov ? 0xffffff : focused ? C_TEXT : C_DIM;
        if (b == TB_MIN)
            gfx_fill(&back, rect_make(cx - 5 * S, cy + 3 * S, 11 * S, 2 * S), gc);
        else if (b == TB_MAX) {
            gfx_frame(&back, rect_make(cx - 5 * S, cy - 5 * S, 11 * S, 11 * S), gc, 256);
            gfx_fill(&back, rect_make(cx - 5 * S, cy - 5 * S, 11 * S, 2 * S), gc);
            if (w->maximized)
                gfx_frame(&back, rect_make(cx - 2 * S, cy - 8 * S, 11 * S, 11 * S), gc, 160);
        } else
            draw_x(&back, cx - S / 2, cy - S / 2, 4 * S, gc);
    }
    gfx_blit(&back, r.x + S, r.y + TITLE_H, &w->body);
}

static int hover_hit = HIT_NONE, hover_idx = -1;

static void tooltip_text(char *buf, size_t n)
{
    buf[0] = 0;
    switch (hover_hit) {
    case HIT_MENU: snprintf(buf, n, "Applications and power"); break;
    case HIT_DS: snprintf(buf, n, "Ask DeepSeek (F6)"); break;
    case HIT_SEARCH: snprintf(buf, n, "Search everything, or ask (Super key)"); break;
    case HIT_TRAY_AI: {
        const char *m = ai_model_name();
        u32 sp = ai_last_speed_x10();
        if (m && sp)
            snprintf(buf, n, "%s, last answer %u.%u tokens/s", m, sp / 10, sp % 10);
        else
            snprintf(buf, n, "%s", m ? m : "No DeepSeek model loaded: open DeepSeek Setup");
        break;
    }
    case HIT_TRAY_NET: {
        struct netif *ni = net_iface();
        if (ni && ni->ip) {
            char ip[20];
            ip4_format(ni->ip, ip, sizeof(ip));
            snprintf(buf, n, "%s %s%s", ni->name, ip, ni->dhcp ? " (DHCP)" : "");
        } else {
            snprintf(buf, n, "%s", ni ? "Network: no address" : "No network");
        }
        break;
    }
    case HIT_TRAY_MEM: {
        char a[24], b[24];
        u64 total = pmm_total_bytes(), freeb = pmm_free_bytes();
        blk_format_size(total - freeb, a, sizeof(a));
        blk_format_size(total, b, sizeof(b));
        snprintf(buf, n, "Memory: %s used of %s", a, b);
        break;
    }
    case HIT_CLOCK: snprintf(buf, n, "Time is UTC"); break;
    case HIT_ICON:
        if (hover_idx >= 0)
            snprintf(buf, n, "%s", launchers[hover_idx].label);
        buf[0] = 0; /* labels are shown already */
        break;
    }
}

static struct rect tip_rect(void)
{
    char t[128];
    tooltip_text(t, sizeof(t));
    if (!t[0] || popup != POP_NONE)
        return rect_make(0, 0, 0, 0);
    struct rect a = hover_hit == HIT_MENU ? r_menu : hover_hit == HIT_DS ? r_ds : hover_hit == HIT_SEARCH ? r_search
                    : hover_hit == HIT_TRAY_AI ? r_ai : hover_hit == HIT_TRAY_NET ? r_net
                    : hover_hit == HIT_TRAY_MEM ? r_mem : r_clock;
    int w = tw(t) + 20 * S;
    int x = CLAMP(a.x + a.w / 2 - w / 2, 4 * S, W - w - 4 * S);
    return rect_make(x, H - BAR_H - 34 * S, w, 26 * S);
}

static void draw_tip(void)
{
    char t[128];
    tooltip_text(t, sizeof(t));
    struct rect r = tip_rect();
    if (rect_empty(r))
        return;
    gfx_shadow(&back, r, 8 * S, 120);
    gfx_round_rect(&back, r, 6 * S, 0x2a2340, 256);
    text(&back, r.x + 10 * S, r.y + TEXT_Y(r.h), t, C_TEXT);
}

static void bar_button(struct rect r, bool hot, bool on)
{
    if (on)
        gfx_round_rect(&back, r, 6 * S, C_SEL, 200);
    else if (hot)
        gfx_round_rect(&back, r, 6 * S, C_HOVER, 256);
}

static void draw_bar(void)
{
    struct rect b = rect_make(0, H - BAR_H, W, BAR_H);
    gfx_vgradient(&back, b, 0x1d1729, 0x120e1b);
    gfx_fill(&back, rect_make(0, b.y, W, S), 0x3d3455);
    gfx_blend(&back, rect_make(0, b.y + S, W, S), 0xffffff, 10);

    /* menu */
    bar_button(r_menu, hover_hit == HIT_MENU, popup == POP_MENU);
    sonar_mark(&back, r_menu.x + 17 * S, r_menu.y + r_menu.h / 2, 10 * S, C_ACCENT,
               popup == POP_MENU ? rgb_mix(0x16121f, C_SEL, 200) : hover_hit == HIT_MENU ? C_HOVER : 0x18131f);
    textb(&back, r_menu.x + 34 * S, r_menu.y + TEXT_Y(r_menu.h), "Sonar", C_TEXT);

    /* DeepSeek */
    bar_button(r_ds, hover_hit == HIT_DS, popup == POP_DEEPSEEK);
    draw_icon(&back, IC_DEEPSEEK, rect_make(r_ds.x + 4 * S, r_ds.y + 4 * S, r_ds.w - 8 * S, r_ds.h - 8 * S));

    /* search */
    draw_field(&back, r_search, &search_f, "Search apps, files, commands, or ask...", popup == POP_SEARCH,
               true);

    /* window buttons */
    int top = top_window();
    for (int k = 0; k < ntask; k++) {
        int i = task_win[k];
        struct rect r = r_task[k];
        bool focused = i == top && popup == POP_NONE;
        bool hot = hover_hit == HIT_TASK && hover_idx == k;
        gfx_round_rect(&back, r, 6 * S, focused ? C_SEL : hot ? C_HOVER : 0x221c31, focused ? 220 : 256);
        if (visible(i))
            gfx_fill(&back, rect_make(r.x + 10 * S, r.y + r.h - 3 * S, r.w - 20 * S, 2 * S),
                     focused ? C_ACCENT : C_DIM);
        draw_icon(&back, win[i].icon, rect_make(r.x + 8 * S, r.y + (r.h - 18 * S) / 2, 18 * S, 18 * S));
        fit(&back, r.x + 32 * S, r.y + TEXT_Y(r.h), win[i].title, win[i].minimized ? C_MUTED : C_TEXT,
            r.w - 40 * S);
    }

    /* tray: DeepSeek status */
    const char *model = ai_model_name();
    bar_button(r_ai, hover_hit == HIT_TRAY_AI, false);
    gfx_round_rect(&back, r_ai, r_ai.h / 2, model ? 0x1d2a5a : 0x3a2b1a, 200);
    gfx_disc(&back, r_ai.x + 14 * S, r_ai.y + r_ai.h / 2, 4 * S, model ? C_GOOD : C_WARN);
    text(&back, r_ai.x + 24 * S, r_ai.y + TEXT_Y(r_ai.h), model ? "DeepSeek ready" : "No model",
         model ? C_TEXT : C_WARN);

    /* network: signal bars */
    struct netif *ni = net_iface();
    bool up = ni && ni->ip;
    bar_button(r_net, hover_hit == HIT_TRAY_NET, false);
    for (int k = 0; k < 4; k++) {
        int bh = (4 + k * 4) * S;
        gfx_fill(&back, rect_make(r_net.x + 6 * S + k * 5 * S, r_net.y + r_net.h / 2 + 8 * S - bh, 3 * S, bh),
                 up ? (k < 4 ? C_TEXT : C_DIM) : C_DIM);
    }
    if (!up)
        draw_x(&back, r_net.x + r_net.w - 7 * S, r_net.y + 9 * S, 3 * S, C_RED);

    /* memory meter */
    bar_button(r_mem, hover_hit == HIT_TRAY_MEM, false);
    u64 total = pmm_total_bytes(), freeb = pmm_free_bytes();
    int pct = total ? (int)((total - freeb) * 100 / total) : 0;
    char buf[48];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    text(&back, r_mem.x + 6 * S, r_mem.y + 4 * S, "RAM", C_DIM);
    gfx_round_rect(&back, rect_make(r_mem.x + 6 * S, r_mem.y + r_mem.h - 9 * S, r_mem.w - 12 * S, 4 * S), 2 * S,
                   C_TRACK, 256);
    int fw = (r_mem.w - 12 * S) * pct / 100;
    if (fw > 0)
        gfx_round_rect(&back, rect_make(r_mem.x + 6 * S, r_mem.y + r_mem.h - 9 * S, MAX(fw, 4 * S), 4 * S), 2 * S,
                       pct > 85 ? C_RED : C_ACCENT, 256);
    text(&back, r_mem.x + r_mem.w - 6 * S - tw(buf), r_mem.y + 4 * S, buf, C_MUTED);

    /* clock */
    bar_button(r_clock, hover_hit == HIT_CLOCK, false);
    u64 e = rtc_epoch();
    if (e) {
        int y, mo, d, h, mi, s;
        epoch_to_date(e, &y, &mo, &d, &h, &mi, &s);
        static const char *const wd[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
        static const char *const mn[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        snprintf(buf, sizeof(buf), "%02d:%02d", h, mi);
        textb(&back, r_clock.x + (r_clock.w - twb(buf)) / 2, r_clock.y + S, buf, C_TEXT);
        snprintf(buf, sizeof(buf), "%s %d %s", wd[(e / 86400) % 7], d, mn[(mo - 1) % 12]);
        text(&back, r_clock.x + (r_clock.w - tw(buf)) / 2, r_clock.y + r_clock.h - 17 * S, buf, C_MUTED);
    } else {
        u64 upt = time_ms() / 1000;
        snprintf(buf, sizeof(buf), "up %llu:%02llu", (unsigned long long)(upt / 3600),
                 (unsigned long long)(upt / 60 % 60));
        textb(&back, r_clock.x + (r_clock.w - twb(buf)) / 2, r_clock.y + TEXT_Y(r_clock.h), buf, C_TEXT);
    }
}

static void draw_launchers(void)
{
    for (int i = 0; i < NLAUNCH; i++) {
        struct rect r = launcher_rect(i);
        if (hover_hit == HIT_ICON && hover_idx == i)
            gfx_round_rect(&back, r, 8 * S, 0xffffff, 26);
        draw_icon(&back, launchers[i].icon, rect_make(r.x + (r.w - 48 * S) / 2, r.y + 6 * S, 48 * S, 48 * S));
        int lw = tw(launchers[i].label);
        int lx = r.x + (r.w - lw) / 2, ly = r.y + 60 * S;
        text(&back, lx + S, ly + S, launchers[i].label, 0x0a0812); /* shadow for legibility */
        text(&back, lx, ly, launchers[i].label, C_TEXT);
    }
}

static void pop_frame(void)
{
    gfx_shadow(&back, pop_r, SHADOW, 190);
    gfx_round_rect(&back, pop_r, 10 * S, C_FRAME, 256);
    gfx_round_rect(&back, rect_make(pop_r.x + S, pop_r.y + S, pop_r.w - 2 * S, pop_r.h - 2 * S), 9 * S, C_POP, 256);
}

static int result_icon(const struct result *r)
{
    switch (r->kind) {
    case R_ASK: return IC_DEEPSEEK;
    case R_APP: return apps[r->app].icon;
    case R_CMD: return IC_CMD;
    case R_FILE: return IC_FILE;
    case R_DIR: return IC_FOLDER;
    case R_CALC: return IC_CALC;
    }
    return IC_FILE;
}

static void row(int y, int icon, const char *label, const char *detail, bool hot, bool selected)
{
    struct rect rr = rect_make(pop_r.x + 6 * S, y, pop_r.w - 12 * S, ROW_H);
    if (selected)
        gfx_round_rect(&back, rr, 6 * S, C_SEL, 256);
    else if (hot)
        gfx_round_rect(&back, rr, 6 * S, C_HOVER, 256);
    draw_icon(&back, icon, rect_make(rr.x + 8 * S, y + (ROW_H - 20 * S) / 2, 20 * S, 20 * S));
    int x = rr.x + 38 * S, ty = y + TEXT_Y(ROW_H);
    int lw = fitb(&back, x, ty, label, C_TEXT, rr.w - 46 * S);
    if (detail && detail[0] && x + lw + 16 * S < rr.x + rr.w - 60 * S)
        fit(&back, x + lw + 14 * S, ty, detail, selected ? 0xcfc7ee : C_DIM, rr.x + rr.w - 10 * S - (x + lw + 14 * S));
}

static void draw_popup(void)
{
    if (popup == POP_NONE)
        return;
    pop_frame();
    char buf[160];
    if (popup == POP_MENU) {
        sonar_mark(&back, pop_r.x + 30 * S, pop_r.y + 28 * S, 14 * S, C_ACCENT, C_POP);
        textb(&back, pop_r.x + 54 * S, pop_r.y + 12 * S, OS_NAME " " OS_VERSION, C_TEXT);
        text(&back, pop_r.x + 54 * S, pop_r.y + 30 * S, "with DeepSeek built in", C_MUTED);
        gfx_hline(&back, pop_r.x + 12 * S, pop_r.y + MENU_HEAD - 4 * S, pop_r.w - 24 * S, C_FRAME);
        for (int k = 0; k < NMENU; k++) {
            int y = pop_r.y + MENU_HEAD + k * ROW_H;
            if (menu_items[k] < 0) {
                gfx_hline(&back, pop_r.x + 12 * S, y + ROW_H / 2, pop_r.w - 24 * S, C_FRAME);
                continue;
            }
            const struct app *a = &apps[menu_items[k]];
            row(y, a->icon, a->name, NULL, pop_hover == k, false);
        }
    } else if (popup == POP_SEARCH) {
        text(&back, pop_r.x + 16 * S, pop_r.y + 12 * S,
             search_f.len ? "Results" : "Type to search apps, commands and files, or ask DeepSeek",
             C_MUTED);
        if (idx_wanted)
            text(&back, pop_r.x + pop_r.w - 16 * S - tw("indexing files..."), pop_r.y + 12 * S,
                 "indexing files...", C_DIM);
        for (int k = 0; k < nres; k++)
            row(pop_r.y + 40 * S + k * ROW_H, result_icon(&res[k]), res[k].label, res[k].detail, pop_hover == k,
                k == sel);
        text(&back, pop_r.x + 16 * S, pop_r.y + pop_r.h - 24 * S, "Enter opens   Up/Down selects   Esc closes",
             C_DIM);
    } else if (popup == POP_DEEPSEEK) {
        int x = pop_r.x + 16 * S, y = pop_r.y + 14 * S;
        draw_icon(&back, IC_DEEPSEEK, rect_make(x, y, 36 * S, 36 * S));
        textb(&back, x + 48 * S, y + 2 * S, "DeepSeek", C_TEXT);
        const char *m = ai_model_name();
        snprintf(buf, sizeof(buf), "%s", m ? m : "No model loaded: choose one in DeepSeek Setup");
        fit(&back, x + 48 * S, y + 20 * S, buf, m ? C_MUTED : C_WARN, pop_r.w - 80 * S);
        struct rect fr = rect_make(pop_r.x + 14 * S, pop_r.y + DS_HEAD, pop_r.w - 28 * S, 38 * S);
        draw_field(&back, fr, &ask_f, "Ask anything about this computer or the world...", true, false);
        text(&back, pop_r.x + 16 * S, pop_r.y + DS_HEAD + 54 * S, "Try", C_DIM);
        for (int k = 0; k < NSUGG; k++)
            row(pop_r.y + DS_HEAD + 52 * S + 26 * S + k * ROW_H, IC_DEEPSEEK, suggestions[k], NULL,
                pop_hover == k, false);
        text(&back, pop_r.x + 16 * S, pop_r.y + pop_r.h - 26 * S,
             "Answers stream into the Terminal. Changes ask first.", C_DIM);
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

static struct rect pointer_rect(int x, int y) { return rect_make(x, y, (ARROW_W + 2) * S, (ARROW_H + 2) * S); }

static void draw_pointer(int x, int y)
{
    for (int j = 0; j < ARROW_H; j++) /* soft shadow */
        for (int i = 0; arrow[j][i]; i++)
            if (arrow[j][i] != ' ')
                gfx_blend(&back, rect_make(x + (i + 1) * S, y + (j + 2) * S, S, S), 0, 90);
    for (int j = 0; j < ARROW_H; j++)
        for (int i = 0; arrow[j][i]; i++) {
            char c = arrow[j][i];
            if (c == ' ')
                continue;
            gfx_fill(&back, rect_make(x + i * S, y + j * S, S, S), c == 'X' ? 0x0b0912 : 0xffffff);
        }
}

static void compose(struct rect c)
{
    back.clip = c;
    for (int y = c.y; y < c.y + c.h; y++)
        memcpy(back.px + (size_t)y * back.stride + c.x, wall.px + (size_t)y * wall.stride + c.x,
               (size_t)c.w * 4);
    if (c.x < ICON_COL)
        draw_launchers();
    int top = top_window();
    for (int z = 0; z < NWIN; z++) {
        int i = zorder[z];
        if (visible(i) && !rect_empty(rect_intersect(outer_rect(&win[i]), c)))
            draw_window(i, i == top && popup == POP_NONE);
    }
    if (c.y + c.h > H - BAR_H)
        draw_bar();
    if (popup != POP_NONE && !rect_empty(rect_intersect(pop_outer(), c)))
        draw_popup();
    draw_tip();
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
/* Sine with a 1024-step period, amplitude 1024 (fixed point). */
static int fsin(int a)
{
    a &= 1023;
    int half = a & 511, sign = a < 512 ? 1 : -1;
    /* Bhaskara I's approximation on a half period */
    int x = half * 180 / 512; /* degrees 0..180 */
    int v = 4 * x * (180 - x) * 1024 / (40500 - x * (180 - x));
    return sign * v;
}

static void build_wallpaper(void)
{
    /* violet gradient with a soft lavender glow in the upper left */
    for (int y = 0; y < H; y++) {
        u32 base = rgb_mix(0x2a1f48, 0x110d1d, y * 256 / H);
        u32 *p = wall.px + (size_t)y * wall.stride;
        for (int x = 0; x < W; x++) {
            int dx = x - W / 5, dy = y - H / 6;
            u32 d = isqrt32((u32)(dx * dx + dy * dy));
            u32 c = base;
            int lim = W * 2 / 3;
            if ((int)d < lim)
                c = rgb_mix(c, 0x5a4a9a, ((lim - (int)d) * 90) / lim);
            p[x] = c;
        }
    }
    /* flowing ribbons across the lower half: lavender, then one crimson */
    static const struct {
        int y100, amp, freq, phase, width, alpha;
        u32 color;
    } ribbons[] = {
        { 56, 60, 3, 0, 90, 60, 0x7d6cd6 },
        { 64, 48, 4, 300, 46, 70, 0xa898f0 },
        { 70, 70, 2, 620, 120, 45, 0x5c4cb0 },
        { 76, 40, 5, 150, 18, 120, 0xe0306a },
        { 80, 55, 3, 800, 70, 55, 0x8b7be0 },
    };
    for (size_t k = 0; k < ARRAY_SIZE(ribbons); k++) {
        for (int x = 0; x < W; x++) {
            int cy = H * ribbons[k].y100 / 100 +
                     ribbons[k].amp * S * fsin(x * ribbons[k].freq * 1024 / W / 2 + ribbons[k].phase) / 1024;
            int hw2 = ribbons[k].width * S / 2;
            for (int y = MAX(cy - hw2 - 20 * S, 0); y < MIN(cy + hw2 + 20 * S, H); y++) {
                int d = y - cy;
                int ad = d < 0 ? -d : d;
                int a;
                if (ad <= hw2)
                    a = ribbons[k].alpha * (hw2 - ad / 2) / MAX(hw2, 1);
                else
                    a = ribbons[k].alpha / 2 * (20 * S - (ad - hw2)) / (20 * S);
                if (a <= 0)
                    continue;
                /* brighter top edge for a ribbon look */
                if (d < 0 && ad > hw2 - 3 * S && ad <= hw2)
                    a = MIN(a + 60, 200);
                u32 *px = wall.px + (size_t)y * wall.stride + x;
                *px = rgb_mix(*px, ribbons[k].color, a);
            }
        }
    }
    /* a faint Sonar mark in the lower right */
    int mx = W - 120 * S, my = H - BAR_H - 110 * S;
    struct surface tmp = wall;
    for (int r = 60 * S; r > 0; r -= 18 * S) {
        for (int y = my - r; y <= my + r; y++)
            for (int x = mx - r; x <= mx + r; x++) {
                int dx = x - mx, dy = y - my;
                int d = (int)isqrt32((u32)(dx * dx + dy * dy));
                if (d <= r && d > r - 3 * S && !(dx < 0 && dy > 0) && y >= 0 && y < H && x >= 0 && x < W)
                    tmp.px[(size_t)y * tmp.stride + x] = rgb_mix(tmp.px[(size_t)y * tmp.stride + x], 0xc8bcff, 50);
            }
    }
    gfx_disc(&wall, mx, my, 5 * S, rgb_mix(0x2a1f48, 0xc8bcff, 90));
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

static void fmt_size(u64 bytes, char *out, size_t n) { blk_format_size(bytes, out, n); }

static void card(struct surface *s, int x, int *y, int w, int h, const char *title, int icon)
{
    gfx_round_rect(s, rect_make(x, *y, w, h), 8 * S, C_BODY_ALT, 256);
    draw_icon(s, icon, rect_make(x + 12 * S, *y + 10 * S, 18 * S, 18 * S));
    textb(s, x + 38 * S, *y + 11 * S, title, C_TEXT);
}

static void line(struct surface *s, int x, int y, int maxw, const char *t, u32 c) { fit(s, x, y, t, c, maxw); }

static void bar(struct surface *s, int x, int y, int w, u64 part, u64 whole, u32 color)
{
    gfx_round_rect(s, rect_make(x, y, w, 8 * S), 4 * S, C_TRACK, 256);
    int fw = whole ? (int)((u64)w * MIN(part, whole) / whole) : 0;
    if (fw > 0)
        gfx_round_rect(s, rect_make(x, y, MAX(fw, 8 * S), 8 * S), 4 * S, color, 256);
}

static void paint_monitor(struct window *w)
{
    struct surface *s = &w->body;
    char buf[128], a[24], b[24];
    gfx_fill(s, rect_make(0, 0, s->w, s->h), C_BODY);
    int x = 12 * S, y = 12 * S, cw = s->w - 24 * S, ix = x + 14 * S, iw = cw - 28 * S;

    card(s, x, &y, cw, 66 * S, "System", IC_ABOUT);
    snprintf(buf, sizeof(buf), "%s %s on aarch64, %d of %d CPUs online", OS_NAME, OS_VERSION, smp_num_cpus(),
             smp_num_present());
    line(s, ix, y + 32 * S, iw, buf, C_TEXT);
    u64 up = time_ms() / 1000;
    snprintf(buf, sizeof(buf), "Up %llud %02lluh %02llum %02llus", (unsigned long long)(up / 86400),
             (unsigned long long)(up / 3600 % 24), (unsigned long long)(up / 60 % 60), (unsigned long long)(up % 60));
    line(s, ix, y + 48 * S, iw, buf, C_MUTED);
    y += 74 * S;

    card(s, x, &y, cw, 66 * S, "Memory", IC_MONITOR);
    u64 total = pmm_total_bytes(), freeb = pmm_free_bytes();
    fmt_size(total - freeb, a, sizeof(a));
    fmt_size(total, b, sizeof(b));
    snprintf(buf, sizeof(buf), "%s used of %s", a, b);
    line(s, ix, y + 32 * S, iw, buf, C_TEXT);
    bar(s, ix, y + 52 * S, iw, total - freeb, total, C_ACCENT);
    y += 74 * S;

    card(s, x, &y, cw, 66 * S, "Disk", IC_FOLDER);
    u64 dt, df;
    char label[40];
    fs_usage(&dt, &df, label, sizeof(label));
    if (dt) {
        fmt_size(dt - df, a, sizeof(a));
        fmt_size(dt, b, sizeof(b));
        snprintf(buf, sizeof(buf), "%s (\"%s\"): %s used of %s%s", fs_device(), label, a, b,
                 fs_is_ram() ? ", in RAM" : "");
        line(s, ix, y + 32 * S, iw, buf, C_TEXT);
        bar(s, ix, y + 52 * S, iw, dt - df, dt, C_RED);
    } else {
        line(s, ix, y + 32 * S, iw, "No file system mounted", C_WARN);
    }
    y += 74 * S;

    struct netif *n = net_iface();
    int gh = 26 * S;
    card(s, x, &y, cw, 56 * S + gh, "Network", IC_CMD);
    if (n) {
        char ip[20];
        ip4_format(n->ip, ip, sizeof(ip));
        fmt_size(n->rx_bytes, a, sizeof(a));
        fmt_size(n->tx_bytes, b, sizeof(b));
        snprintf(buf, sizeof(buf), "%s  %s%s   in %s, out %s", n->name, n->ip ? ip : "no address",
                 n->dhcp ? " (DHCP)" : "", a, b);
        line(s, ix, y + 32 * S, iw, buf, C_TEXT);
        u32 peak = 1;
        for (int i = 0; i < HIST; i++)
            peak = MAX(peak, net_hist[i]);
        int colw = MAX(iw / HIST, 1), gy = y + 50 * S;
        for (int i = 0; i < HIST; i++) {
            int hgt = (int)((u64)net_hist[i] * gh / peak);
            if (net_hist[i] && hgt < S)
                hgt = S;
            gfx_fill(s, rect_make(ix + i * colw, gy + gh - hgt, MAX(colw - 1, 1), hgt), C_GOOD);
        }
        gfx_hline(s, ix, gy + gh, iw, C_TRACK);
    } else {
        line(s, ix, y + 32 * S, iw, "No network interface", C_MUTED);
    }
    y += 64 * S + gh;

    card(s, x, &y, cw, 66 * S, "DeepSeek", IC_DEEPSEEK);
    const char *model = ai_model_name();
    if (model) {
        line(s, ix, y + 32 * S, iw, model, C_TEXT);
        u32 sp = ai_last_speed_x10();
        if (sp)
            snprintf(buf, sizeof(buf), "Last answer: %u.%u tokens/s", sp / 10, sp % 10);
        else
            snprintf(buf, sizeof(buf), "Ready. Ask from the taskbar or with ? in the Terminal.");
        line(s, ix, y + 48 * S, iw, buf, C_MUTED);
    } else {
        line(s, ix, y + 32 * S, iw, "No model loaded", C_WARN);
        line(s, ix, y + 48 * S, iw, "Open DeepSeek Setup from the Sonar menu.", C_MUTED);
    }
}

/* ---------------------------------------------------------------- files */
struct fentry {
    char name[FS_NAME_MAX + 1];
    bool dir;
    u64 size, mtime;
};
static char files_path[FS_PATH_MAX] = "/";
static struct fentry *files;
static int nfiles, files_scroll, files_sel = -1;
static char files_status[128];
#define FROW (26 * S)
#define FHEAD (40 * S)
#define FFOOT (36 * S)

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

static int files_rows(const struct window *w) { return (w->body.h - FHEAD - FFOOT) / FROW; }

static void files_status_for(int i)
{
    const struct fentry *f = &files[i];
    char sz[24];
    fmt_size(f->size, sz, sizeof(sz));
    int yy, mo, d, h, mi, s;
    epoch_to_date(f->mtime, &yy, &mo, &d, &h, &mi, &s);
    snprintf(files_status, sizeof(files_status), "%s  %s  %04d-%02d-%02d %02d:%02d", f->name, sz, yy, mo, d, h,
             mi);
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
        while (j >= 0 && (list[j].dir < t.dir || (list[j].dir == t.dir && strcmp(list[j].name, t.name) > 0))) {
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
    if (files_pick[0]) {
        for (int i = 0; i < n; i++)
            if (!strcmp(list[i].name, files_pick)) {
                files_sel = i;
                int rows = files_rows(&win[W_FILES]);
                if (i >= rows)
                    files_scroll = i - rows / 2;
                files_status_for(i);
            }
        files_pick[0] = 0;
    }
    repaint(W_FILES);
}

/* Footer buttons for the selected file. */
static struct rect files_btn(const struct window *w, int b)
{
    int bw = b == 0 ? 112 * S : 64 * S;
    int x = w->body.w - 10 * S - 112 * S - (b == 1 ? 8 * S + 64 * S : 0);
    return rect_make(x, w->body.h - FFOOT + 6 * S, bw, FFOOT - 12 * S);
}

static void paint_files(struct window *w)
{
    struct surface *s = &w->body;
    char buf[96];
    gfx_fill(s, rect_make(0, 0, s->w, s->h), C_BODY);
    gfx_fill(s, rect_make(0, 0, s->w, FHEAD), C_BODY_ALT);
    gfx_hline(s, 0, FHEAD - 1, s->w, 0x0d0a14);
    int ty = TEXT_Y(FHEAD);
    gfx_round_rect(s, rect_make(8 * S, 7 * S, 30 * S, FHEAD - 14 * S), 6 * S, files_has_up() ? C_HOVER : 0x261f38, 256);
    for (int k = 0; k < 5 * S; k++) /* up arrow */
        gfx_fill(s, rect_make(23 * S - k, 14 * S + k, 2 * k + 1, 1), files_has_up() ? C_TEXT : C_DIM);
    gfx_fill(s, rect_make(22 * S, 19 * S, 3 * S, 8 * S), files_has_up() ? C_TEXT : C_DIM);
    /* path as a breadcrumb box */
    gfx_round_rect(s, rect_make(46 * S, 7 * S, s->w - 56 * S, FHEAD - 14 * S), 6 * S, C_FIELD, 256);
    fit(s, 56 * S, ty, files_path, C_TEXT, s->w - 76 * S);

    int rows = files_rows(w), y = FHEAD + 2 * S;
    for (int k = 0; k < rows && files_scroll + k < nfiles; k++) {
        int i = files_scroll + k;
        const struct fentry *f = &files[i];
        if (i == files_sel)
            gfx_round_rect(s, rect_make(4 * S, y, s->w - 8 * S, FROW), 5 * S, C_SEL, 256);
        else if (k % 2)
            gfx_fill(s, rect_make(4 * S, y, s->w - 8 * S, FROW), 0x1f1a2c);
        draw_icon(s, f->dir ? IC_FOLDER : IC_FILE, rect_make(12 * S, y + (FROW - 18 * S) / 2, 18 * S, 18 * S));
        int sizew = 0;
        if (!f->dir) {
            fmt_size(f->size, buf, sizeof(buf));
            sizew = tw(buf);
            text(s, s->w - 14 * S - sizew, y + TEXT_Y(FROW), buf, C_MUTED);
        }
        fit(s, 38 * S, y + TEXT_Y(FROW), f->name, C_TEXT, s->w - 38 * S - sizew - 28 * S);
        y += FROW;
    }
    if (!nfiles)
        text(s, 16 * S, FHEAD + 12 * S, "This folder is empty", C_DIM);
    if (nfiles > rows) {
        int track = rows * FROW, th = MAX(track * rows / nfiles, 12 * S);
        int tyy = FHEAD + (track - th) * files_scroll / MAX(nfiles - rows, 1);
        gfx_round_rect(s, rect_make(s->w - 5 * S, tyy, 3 * S, th), S, 0x5a5078, 256);
    }
    gfx_fill(s, rect_make(0, s->h - FFOOT, s->w, FFOOT), C_BODY_ALT);
    bool file_sel = files_sel >= 0 && files_sel < nfiles && !files[files_sel].dir;
    int sw = s->w - 24 * S - (file_sel ? 200 * S : 0);
    fit(s, 12 * S, s->h - FFOOT + TEXT_Y(FFOOT), files_status, C_MUTED, sw);
    if (file_sel) {
        struct rect a = files_btn(w, 0), v = files_btn(w, 1);
        gfx_round_rect(s, a, 6 * S, C_DS, 256);
        draw_icon(s, IC_DEEPSEEK, rect_make(a.x + 6 * S, a.y + (a.h - 14 * S) / 2, 14 * S, 14 * S));
        text(s, a.x + 26 * S, a.y + TEXT_Y(a.h), "Ask DeepSeek", 0xffffff);
        gfx_round_rect(s, v, 6 * S, C_HOVER, 256);
        text(s, v.x + (v.w - tw("View")) / 2, v.y + TEXT_Y(v.h), "View", C_TEXT);
    }
}

static void files_click(struct window *w, int x, int y)
{
    if (y < FHEAD) {
        if (files_has_up() && x < 40 * S)
            path_parent(files_next, sizeof(files_next), files_path);
        return;
    }
    if (y >= w->body.h - FFOOT) {
        bool file_sel = files_sel >= 0 && files_sel < nfiles && !files[files_sel].dir;
        if (!file_sel)
            return;
        char full[FS_PATH_MAX], q[FS_PATH_MAX + 80];
        path_join(full, sizeof(full), files_path, files[files_sel].name);
        if (rect_contains(files_btn(w, 0), x, y)) {
            snprintf(q, sizeof(q), "What is in the file %s? Summarize it briefly.", full);
            ask_deepseek(q);
        } else if (rect_contains(files_btn(w, 1), x, y)) {
            snprintf(q, sizeof(q), "head -n 40 %s", full);
            run_command(q);
        }
        return;
    }
    int k = (y - FHEAD - 2 * S) / FROW;
    if (k < 0 || k >= files_rows(w))
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
    files_status_for(i);
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
    gfx_vgradient(s, rect_make(0, 0, s->w, 96 * S), 0x3a2e6e, C_BODY);
    int x = 24 * S, y = 22 * S;
    sonar_mark(s, x + 24 * S, y + 24 * S, 24 * S, 0xffffff, rgb_mix(0x3a2e6e, C_BODY, 60));
    gfx_text_bold(s, x + 64 * S, y + 2 * S, OS_NAME " " OS_VERSION, C_TEXT, 2 * S);
    text(s, x + 66 * S, y + 36 * S, "An operating system with DeepSeek built in", C_MUTED);
    y = 112 * S;
    static const char *const lines[] = {
        "Kernel, drivers, file system, network, desktop and",
        "AI engine are all part of Sonar itself.",
        "",
        "Super      search everything, or ask DeepSeek",
        "F6         DeepSeek box",
        "F1         full-screen console / desktop",
        "F2 F3 F4   Terminal, System Monitor, Files",
        "",
        "Typing goes to the Terminal unless a search or",
        "DeepSeek box is open.",
    };
    for (size_t i = 0; i < ARRAY_SIZE(lines); i++) {
        bool key = lines[i][0] == 'S' || lines[i][0] == 'F';
        if (key) {
            char k[12];
            size_t n = 0;
            while (lines[i][n] && lines[i][n] != ' ' && n < 11)
                k[n] = lines[i][n], n++;
            k[n] = 0;
            const char *rest = lines[i] + 11;
            if (!strncmp(lines[i], "F2 F3 F4", 8))
                strlcpy(k, "F2 F3 F4", sizeof(k));
            gfx_round_rect(s, rect_make(x - 4 * S, y - 2 * S, twb(k) + 10 * S, 20 * S), 4 * S, C_HOVER, 256);
            textb(s, x + S, y, k, C_TEXT);
            text(s, x + 90 * S, y, rest, C_MUTED);
        } else {
            text(s, x, y, lines[i], C_MUTED);
        }
        y += 22 * S;
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
    close_popup();
    u64 f = console_lock();
    active = false;
    grab = false;
    size_t n = console_log_copy_locked(replay, sizeof(replay));
    fbcon_retarget_locked(NULL, replay, n);
    console_unlock(f);
}

static int drag_win = -1, drag_dx, drag_dy;

static void bar_press(int x, int y)
{
    if (rect_contains(r_menu, x, y)) {
        if (popup == POP_MENU)
            close_popup();
        else
            open_popup(POP_MENU);
    } else if (rect_contains(r_ds, x, y) || rect_contains(r_ai, x, y)) {
        if (popup == POP_DEEPSEEK)
            close_popup();
        else
            open_popup(POP_DEEPSEEK);
    } else if (rect_contains(r_search, x, y)) {
        if (popup != POP_SEARCH)
            open_popup(POP_SEARCH);
    } else if (rect_contains(r_net, x, y) || rect_contains(r_mem, x, y)) {
        close_popup();
        show_window(W_MONITOR);
    } else {
        close_popup();
        for (int k = 0; k < ntask; k++)
            if (rect_contains(r_task[k], x, y)) {
                int i = task_win[k];
                if (win[i].minimized || top_window() != i)
                    show_window(i);
                else
                    minimize_window(i);
            }
    }
}

static void press(int x, int y)
{
    if (y >= H - BAR_H) {
        bar_press(x, y);
        return;
    }
    if (popup != POP_NONE) {
        int r = popup_row(x, y);
        if (r >= 0) {
            popup_activate(r);
            return;
        }
        if (rect_contains(pop_r, x, y))
            return;
        close_popup();
        return;
    }
    int i = window_at(x, y);
    if (i < 0) {
        for (int k = 0; k < NLAUNCH; k++)
            if (rect_contains(launcher_rect(k), x, y))
                launcher_activate(k);
        return;
    }
    struct window *w = &win[i];
    if (top_window() != i)
        raise_window(i);
    for (int b = 0; b < 3; b++)
        if (rect_contains(title_button(w, b), x, y)) {
            if (b == TB_MIN)
                minimize_window(i);
            else if (b == TB_MAX)
                toggle_maximize(i);
            else
                close_window(i);
            return;
        }
    if (y < w->r.y + TITLE_H) {
        if (!w->maximized) {
            drag_win = i;
            drag_dx = x - w->r.x;
            drag_dy = y - w->r.y;
        }
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
    case UI_FKEY_SUPER:
        if (popup == POP_SEARCH)
            close_popup();
        else
            open_popup(POP_SEARCH);
        break;
    case 2: show_window(W_TERM); break;
    case 3: toggle_window(W_MONITOR); break;
    case 4: toggle_window(W_FILES); break;
    case 5: toggle_window(W_ABOUT); break;
    case 6:
        if (popup == POP_DEEPSEEK)
            close_popup();
        else
            open_popup(POP_DEEPSEEK);
        break;
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
    if (e->kind == UEV_KEY) {
        if (popup == POP_SEARCH || popup == POP_DEEPSEEK)
            popup_key(e->val);
        else
            grab = false;
        return;
    }
    if (e->kind == UEV_WHEEL) {
        int i = window_at(e->x, e->y);
        if (popup == POP_NONE && i >= 0 && win[i].wheel)
            win[i].wheel(&win[i], e->val);
        return;
    }
    if (e->button != 0) {
        if (e->down && e->button == 1 && e->y < H - BAR_H && window_at(e->x, e->y) < 0 && popup == POP_NONE)
            open_popup(POP_MENU); /* right click on the desktop */
        return;
    }
    if (e->down)
        press(e->x, e->y);
    else
        drag_win = -1;
}

static void set_hover(int hit, int idx, int hwin, int hbtn, int phov)
{
    if (hit == hover_hit && idx == hover_idx && hwin == hover_win && hbtn == hover_btn && phov == pop_hover)
        return;
    struct rect old_tip = tip_rect();
    if (hover_win >= 0)
        damage(rect_make(win[hover_win].r.x, win[hover_win].r.y, win[hover_win].r.w, TITLE_H));
    if (hover_hit == HIT_ICON || hit == HIT_ICON)
        damage(rect_make(0, 0, ICON_COL, NLAUNCH * 92 * S + 20 * S));
    hover_hit = hit, hover_idx = idx, hover_win = hwin, hover_btn = hbtn, pop_hover = phov;
    if (hover_win >= 0)
        damage(rect_make(win[hover_win].r.x, win[hover_win].r.y, win[hover_win].r.w, TITLE_H));
    damage(old_tip);
    damage(tip_rect());
    damage_bar();
    if (popup != POP_NONE)
        damage(pop_r);
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
            int ny = CLAMP(py - drag_dy, 0, work_h() - TITLE_H);
            if (nx != w->r.x || ny != w->r.y) {
                damage(outer_rect(w));
                w->r.x = nx, w->r.y = ny;
                damage(outer_rect(w));
            }
        }
    }
    int hit = HIT_NONE, idx = -1, hwin = -1, hbtn = -1, phov = -1;
    if (py >= H - BAR_H) {
        if (rect_contains(r_menu, px, py))
            hit = HIT_MENU;
        else if (rect_contains(r_ds, px, py))
            hit = HIT_DS;
        else if (rect_contains(r_search, px, py))
            hit = HIT_SEARCH;
        else if (rect_contains(r_ai, px, py))
            hit = HIT_TRAY_AI;
        else if (rect_contains(r_net, px, py))
            hit = HIT_TRAY_NET;
        else if (rect_contains(r_mem, px, py))
            hit = HIT_TRAY_MEM;
        else if (rect_contains(r_clock, px, py))
            hit = HIT_CLOCK;
        else
            for (int k = 0; k < ntask; k++)
                if (rect_contains(r_task[k], px, py))
                    hit = HIT_TASK, idx = k;
    } else if (popup != POP_NONE && rect_contains(pop_r, px, py)) {
        hit = HIT_POP;
        phov = popup_row(px, py);
    } else {
        int i = window_at(px, py);
        if (i >= 0) {
            for (int b = 0; b < 3; b++)
                if (rect_contains(title_button(&win[i], b), px, py))
                    hwin = i, hbtn = b;
        } else {
            for (int k = 0; k < NLAUNCH; k++)
                if (rect_contains(launcher_rect(k), px, py))
                    hit = HIT_ICON, idx = k;
        }
    }
    set_hover(hit, idx, hwin, hbtn, phov);
}

static u64 last_second;
static int last_ntask = -1, last_model = -1;

static void tick(void)
{
    if (!started || smp_cpu_id() != 0)
        return;
    struct uev e;
    while (pop_event(&e))
        handle(&e);
    if (!active)
        return;
    /* the taskbar's layout follows open windows and the model state */
    int nt = 0;
    for (int i = 0; i < NWIN; i++)
        nt += win[i].open;
    int ml = ai_model_name() != NULL;
    if (nt != last_ntask || ml != last_model) {
        last_ntask = nt, last_model = ml;
        layout_bar();
        damage_bar();
    }
    track_pointer();
    if (console_waiting_input()) { /* safe to use the disk */
        if (files_next[0]) {
            char path[FS_PATH_MAX];
            strlcpy(path, files_next, sizeof(path));
            files_next[0] = 0;
            files_load(path);
        }
        if (idx_wanted) {
            idx_wanted = false;
            index_files();
            if (popup == POP_SEARCH) {
                search_update();
                refresh_popup();
            }
        }
    }
    u64 now = time_ms();
    if (now - last_second >= 1000) {
        last_second = now;
        sample_net();
        if (visible(W_MONITOR))
            repaint(W_MONITOR);
        damage_bar();
    }
    present();
}

/* ---------------------------------------------------------------- setup */
static bool make_window(int i, const char *title, int icon, int x, int y, int bw, int bh)
{
    struct window *w = &win[i];
    w->title = title;
    w->icon = icon;
    if (!surface_alloc(&w->body, bw, bh))
        return false;
    w->r = rect_make(x, y, bw + 2 * S, TITLE_H + bh + S);
    return true;
}

static bool desktop_setup(void)
{
    W = (int)hw->width, H = (int)hw->height;
    if (W < 800 || H < 560) {
        printk("ui: the screen (%dx%d) is too small for the desktop\n", W, H);
        return false;
    }
    S = W >= 2400 ? 2 : 1;
    if (!surface_alloc(&back, W, H) || !surface_alloc(&wall, W, H)) {
        printk("ui: not enough memory for the desktop\n");
        return false;
    }
    build_wallpaper();

    /* Terminal next to the launchers; monitor and files stacked on the
     * right when there is room, otherwise they open on top when asked. */
    int top = MARGIN, bottom = work_h() - MARGIN;
    int side_w = 360 * S;
    bool wide = W >= 1200 * S;
    int left = ICON_COL + 8 * S;
    int cw = 8 * S, chh = 16 * S;
    int avail_w = wide ? W - left - side_w - 2 * MARGIN - 2 * S : W - left - MARGIN - 2 * S;
    int cols = MIN(avail_w / cw, 110), rows = MIN((bottom - top - TITLE_H - S) / chh, 36);
    if (!make_window(W_TERM, "Terminal", IC_TERM, left, top, cols * cw, rows * chh))
        return false;
    int sx = W - MARGIN - side_w - 2 * S;
    int mon_h = 418 * S;
    if (!make_window(W_MONITOR, "System Monitor", IC_MONITOR, wide ? sx : W - side_w - 2 * MARGIN,
                     wide ? top : top + 40 * S, side_w, mon_h))
        return false;
    int files_y = top + TITLE_H + mon_h + S + 12 * S;
    int files_h = bottom - files_y - TITLE_H - S;
    if (!wide || files_h < 160 * S)
        files_y = top + 70 * S, files_h = 300 * S;
    if (!make_window(W_FILES, "Files", IC_FILES, wide ? sx : W - side_w - 3 * MARGIN, files_y, side_w, files_h))
        return false;
    if (!make_window(W_ABOUT, "About Sonar", IC_ABOUT, (W - 460 * S) / 2, top + 60 * S, 460 * S, 360 * S))
        return false;
    win[W_MONITOR].paint = paint_monitor;
    win[W_FILES].paint = paint_files;
    win[W_FILES].click = files_click;
    win[W_FILES].wheel = files_wheel;
    win[W_ABOUT].paint = paint_about;

    struct window *t = &win[W_TERM];
    term_fb = (struct framebuffer){ "terminal", (u8 *)t->body.px, (u32)t->body.w, (u32)t->body.h,
                                    (u32)t->body.w * 4, false, term_flush, NULL };
    t->open = true;
    win[W_MONITOR].open = wide;
    win[W_FILES].open = wide;
    files_load("/");
    for (int i = 0; i < NWIN; i++)
        repaint(i);
    layout_bar();
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
    printk("ui: desktop on %dx%d%s; Super searches, F6 asks DeepSeek, F1 switches to the console\n", W, H,
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
