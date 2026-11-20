/*
 * 2D drawing for the desktop: rectangles, alpha blending, rounded corners,
 * soft shadows, gradients and text in Sonar's 8x16 font. Surfaces hold
 * 0x00RRGGBB pixels; the compositor converts to the screen's byte order.
 */
#include "gfx.h"
#include "../drivers/font8x16.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"

bool rect_empty(struct rect r) { return r.w <= 0 || r.h <= 0; }

bool rect_contains(struct rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

struct rect rect_intersect(struct rect a, struct rect b)
{
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return (struct rect){ 0, 0, 0, 0 };
    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

struct rect rect_union(struct rect a, struct rect b)
{
    if (rect_empty(a))
        return b;
    if (rect_empty(b))
        return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

bool surface_alloc(struct surface *s, int w, int h)
{
    u64 bytes = (u64)w * h * 4;
    s->px = page_alloc_nozero(ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE);
    if (!s->px)
        return false;
    s->w = w, s->h = h, s->stride = w;
    surface_reset_clip(s);
    return true;
}

void surface_free(struct surface *s)
{
    /* Surfaces live as long as the desktop; nothing frees pages yet. */
    s->px = NULL;
}

void surface_reset_clip(struct surface *s) { s->clip = (struct rect){ 0, 0, s->w, s->h }; }

u32 rgb_mix(u32 a, u32 b, int alpha)
{
    u32 ra = a >> 16 & 255, ga = a >> 8 & 255, ba = a & 255;
    u32 rb = b >> 16 & 255, gb = b >> 8 & 255, bb = b & 255;
    u32 r = (ra * (256 - alpha) + rb * alpha) >> 8;
    u32 g = (ga * (256 - alpha) + gb * alpha) >> 8;
    u32 bl = (ba * (256 - alpha) + bb * alpha) >> 8;
    return r << 16 | g << 8 | bl;
}

static inline u32 *row(struct surface *s, int y) { return s->px + (size_t)y * s->stride; }

void gfx_fill(struct surface *s, struct rect r, u32 color)
{
    r = rect_intersect(r, s->clip);
    for (int y = r.y; y < r.y + r.h; y++) {
        u32 *p = row(s, y) + r.x;
        for (int x = 0; x < r.w; x++)
            p[x] = color;
    }
}

void gfx_blend(struct surface *s, struct rect r, u32 color, int alpha)
{
    if (alpha >= 256) {
        gfx_fill(s, r, color);
        return;
    }
    r = rect_intersect(r, s->clip);
    for (int y = r.y; y < r.y + r.h; y++) {
        u32 *p = row(s, y) + r.x;
        for (int x = 0; x < r.w; x++)
            p[x] = rgb_mix(p[x], color, alpha);
    }
}

void gfx_hline(struct surface *s, int x, int y, int w, u32 color)
{
    gfx_fill(s, rect_make(x, y, w, 1), color);
}

void gfx_vline(struct surface *s, int x, int y, int h, u32 color)
{
    gfx_fill(s, rect_make(x, y, 1, h), color);
}

void gfx_frame(struct surface *s, struct rect r, u32 color, int alpha)
{
    gfx_blend(s, rect_make(r.x, r.y, r.w, 1), color, alpha);
    gfx_blend(s, rect_make(r.x, r.y + r.h - 1, r.w, 1), color, alpha);
    gfx_blend(s, rect_make(r.x, r.y + 1, 1, r.h - 2), color, alpha);
    gfx_blend(s, rect_make(r.x + r.w - 1, r.y + 1, 1, r.h - 2), color, alpha);
}

u32 isqrt32(u32 v)
{
    u32 r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* Coverage (0..256) of a pixel near a rounded corner, with a soft edge. */
static int corner_cover(int dx, int dy, int radius)
{
    /* distance from the corner circle's centre, in 1/16 pixel */
    int d = (int)isqrt32((u32)((dx * 2 + 1) * (dx * 2 + 1) + (dy * 2 + 1) * (dy * 2 + 1)) * 64);
    int edge = radius * 16;
    if (d <= edge - 8)
        return 256;
    if (d >= edge + 8)
        return 0;
    return (edge + 8 - d) * 16;
}

void gfx_round_rect(struct surface *s, struct rect r, int radius, u32 color, int alpha)
{
    radius = MIN(radius, MIN(r.w, r.h) / 2);
    struct rect c = rect_intersect(r, s->clip);
    for (int y = c.y; y < c.y + c.h; y++) {
        u32 *p = row(s, y);
        int ry = y - r.y, dy = -1;
        if (ry < radius)
            dy = radius - 1 - ry;
        else if (ry >= r.h - radius)
            dy = ry - (r.h - radius);
        for (int x = c.x; x < c.x + c.w; x++) {
            int a = alpha;
            if (dy >= 0) {
                int rx = x - r.x, dx = -1;
                if (rx < radius)
                    dx = radius - 1 - rx;
                else if (rx >= r.w - radius)
                    dx = rx - (r.w - radius);
                if (dx >= 0)
                    a = a * corner_cover(dx, dy, radius) >> 8;
            }
            if (a >= 256)
                p[x] = color;
            else if (a > 0)
                p[x] = rgb_mix(p[x], color, a);
        }
    }
}

void gfx_vgradient(struct surface *s, struct rect r, u32 top, u32 bottom)
{
    struct rect c = rect_intersect(r, s->clip);
    for (int y = c.y; y < c.y + c.h; y++) {
        u32 color = rgb_mix(top, bottom, r.h > 1 ? (y - r.y) * 256 / (r.h - 1) : 0);
        u32 *p = row(s, y) + c.x;
        for (int x = 0; x < c.w; x++)
            p[x] = color;
    }
}

/* A soft dark halo around r (drawn before r itself), offset slightly down. */
void gfx_shadow(struct surface *s, struct rect r, int spread, int strength)
{
    struct rect o = { r.x - spread, r.y - spread + spread / 3, r.w + 2 * spread, r.h + 2 * spread };
    struct rect c = rect_intersect(o, s->clip);
    int sy0 = r.y + spread / 3, sy1 = sy0 + r.h;
    int r2 = spread * spread;
    for (int y = c.y; y < c.y + c.h; y++) {
        u32 *p = row(s, y);
        int dy = y < sy0 ? sy0 - y : (y >= sy1 ? y - sy1 + 1 : 0);
        for (int x = c.x; x < c.x + c.w; x++) {
            int dx = x < r.x ? r.x - x : (x >= r.x + r.w ? x - (r.x + r.w) + 1 : 0);
            int d2 = dx * dx + dy * dy;
            if (d2 >= r2)
                continue;
            int a = strength * (r2 - d2) / r2;
            a = a * (r2 - d2) / r2; /* quadratic falloff */
            if (a > 0)
                p[x] = rgb_mix(p[x], 0, a);
        }
    }
}

void gfx_disc(struct surface *s, int cx, int cy, int radius, u32 color)
{
    gfx_round_rect(s, rect_make(cx - radius, cy - radius, radius * 2, radius * 2), radius, color,
                   256);
}

void gfx_blit(struct surface *dst, int x, int y, const struct surface *src)
{
    struct rect r = rect_intersect(rect_make(x, y, src->w, src->h), dst->clip);
    for (int j = r.y; j < r.y + r.h; j++)
        memcpy(row(dst, j) + r.x, src->px + (size_t)(j - y) * src->stride + (r.x - x),
               (size_t)r.w * 4);
}

static void glyph(struct surface *s, int x, int y, u8 c, u32 color, int scale)
{
    struct rect g = rect_intersect(rect_make(x, y, 8 * scale, 16 * scale), s->clip);
    if (rect_empty(g))
        return;
    const u8 *bits = font8x16[c & 127];
    for (int py = g.y; py < g.y + g.h; py++) {
        u8 b = bits[(py - y) / scale];
        if (!b)
            continue;
        u32 *p = row(s, py);
        for (int px = g.x; px < g.x + g.w; px++)
            if (b & (0x80 >> ((px - x) / scale)))
                p[px] = color;
    }
}

int gfx_text(struct surface *s, int x, int y, const char *str, u32 color, int scale)
{
    int x0 = x;
    for (; *str; str++) {
        u8 c = (u8)*str;
        glyph(s, x, y, c < 128 ? c : '?', color, scale);
        x += 8 * scale;
    }
    return x - x0;
}

int gfx_text_bold(struct surface *s, int x, int y, const char *str, u32 color, int scale)
{
    gfx_text(s, x + 1, y, str, color, scale);
    return gfx_text(s, x, y, str, color, scale) + 1;
}

int gfx_text_width(const char *str, int scale) { return (int)strlen(str) * 8 * scale; }

int gfx_text_fit(struct surface *s, int x, int y, const char *str, u32 color, int scale, int max_w)
{
    int fit = max_w / (8 * scale);
    int len = (int)strlen(str);
    if (len <= fit)
        return gfx_text(s, x, y, str, color, scale);
    if (fit <= 3)
        return 0;
    char buf[160];
    int keep = MIN(fit - 3, (int)sizeof(buf) - 4);
    memcpy(buf, str, (size_t)keep);
    memcpy(buf + keep, "...", 4);
    return gfx_text(s, x, y, buf, color, scale);
}

/* ---- UI text: the console font, proportionally spaced and smoothed ----
 * Each glyph is trimmed to the columns it uses (plus one column of space);
 * pixels that close a diagonal step get a faint coverage, which takes the
 * hard edge off the bitmap font at UI sizes. */
static u8 ui_left[128], ui_adv[128];
static bool ui_ready;

static void ui_metrics(void)
{
    for (int c = 0; c < 128; c++) {
        u8 used = 0;
        for (int r = 0; r < 16; r++)
            used |= font8x16[c][r];
        if (!used) {
            ui_left[c] = 0;
            ui_adv[c] = c == ' ' ? 4 : 0;
            continue;
        }
        int l = 0, rgt = 7;
        while (!(used & (0x80 >> l)))
            l++;
        while (!(used & (0x80 >> rgt)))
            rgt--;
        ui_left[c] = (u8)l;
        ui_adv[c] = (u8)(rgt - l + 2);
    }
    ui_ready = true;
}

static inline bool fbit(const u8 *bits, int gx, int gy)
{
    return gx >= 0 && gx < 8 && gy >= 0 && gy < 16 && (bits[gy] & (0x80 >> gx));
}

static void ui_glyph(struct surface *s, int x, int y, u8 c, u32 color, int scale, bool bold)
{
    const u8 *bits = font8x16[c & 127];
    int l = ui_left[c & 127];
    for (int gy = 0; gy < 16; gy++)
        for (int gx = 0; gx < 8; gx++) {
            bool on = fbit(bits, gx, gy) || (bold && fbit(bits, gx - 1, gy));
            int a = 256;
            if (!on) {
                bool lft = fbit(bits, gx - 1, gy) || (bold && fbit(bits, gx - 2, gy));
                bool rgt = fbit(bits, gx + 1, gy) || (bold && fbit(bits, gx, gy));
                bool up = fbit(bits, gx, gy - 1) || (bold && fbit(bits, gx - 1, gy - 1));
                bool dn = fbit(bits, gx, gy + 1) || (bold && fbit(bits, gx - 1, gy + 1));
                if (!((lft || rgt) && (up || dn)))
                    continue;
                a = 72; /* inside corner of a diagonal step */
            }
            struct rect px = rect_intersect(rect_make(x + (gx - l) * scale, y + gy * scale, scale, scale),
                                            s->clip);
            if (rect_empty(px))
                continue;
            if (a == 256)
                gfx_fill(s, px, color);
            else
                gfx_blend(s, px, color, a);
        }
}

int gfx_ui_width(const char *str, int scale, bool bold)
{
    if (!ui_ready)
        ui_metrics();
    int w = 0;
    for (; *str; str++) {
        u8 c = (u8)*str < 128 ? (u8)*str : '?';
        w += (ui_adv[c] + (bold && ui_adv[c] && c != ' ')) * scale;
    }
    return w;
}

int gfx_ui_text(struct surface *s, int x, int y, const char *str, u32 color, int scale, bool bold)
{
    if (!ui_ready)
        ui_metrics();
    int x0 = x;
    for (; *str; str++) {
        u8 c = (u8)*str < 128 ? (u8)*str : '?';
        if (c != ' ')
            ui_glyph(s, x, y, c, color, scale, bold);
        x += (ui_adv[c] + (bold && ui_adv[c] && c != ' ')) * scale;
    }
    return x - x0;
}

int gfx_ui_fit(struct surface *s, int x, int y, const char *str, u32 color, int scale, bool bold,
               int max_w)
{
    if (gfx_ui_width(str, scale, bold) <= max_w)
        return gfx_ui_text(s, x, y, str, color, scale, bold);
    char buf[200];
    int n = (int)MIN(strlen(str), sizeof(buf) - 4);
    memcpy(buf, str, (size_t)n);
    while (n > 0) {
        memcpy(buf + n, "...", 4);
        if (gfx_ui_width(buf, scale, bold) <= max_w)
            break;
        n--;
    }
    return n > 0 ? gfx_ui_text(s, x, y, buf, color, scale, bold) : 0;
}
