/*
 * Desktop icons, drawn from simple shapes.
 *
 * Each icon is a stack of layers. A layer is a shape given by its signed
 * distance function (negative inside, in units of the icon's size) and a
 * color. A pixel's coverage comes from its distance to the edge, so edges
 * are antialiased at every size. Icons are rendered once per size into a
 * small cache of ARGB images and blended onto the screen from there.
 */
#include "icons.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"

/* ---------------------------------------------------------------- math */
static inline float fsqrt(float x)
{
    float r;
    __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
    return r;
}
static inline float fmin2(float a, float b) { return a < b ? a : b; }
static inline float fmax2(float a, float b) { return a > b ? a : b; }
static inline float fabs1(float a) { return a < 0 ? -a : a; }
static inline float fclamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float len2(float x, float y) { return fsqrt(x * x + y * y); }

/* ---------------------------------------------------------------- shapes */
struct pt {
    float x, y;
};

static float sd_circle(struct pt p, float cx, float cy, float r) { return len2(p.x - cx, p.y - cy) - r; }

static float sd_rrect(struct pt p, float x0, float y0, float x1, float y1, float r)
{
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    float qx = fabs1(p.x - cx) - ((x1 - x0) / 2 - r), qy = fabs1(p.y - cy) - ((y1 - y0) / 2 - r);
    return len2(fmax2(qx, 0), fmax2(qy, 0)) + fmin2(fmax2(qx, qy), 0) - r;
}

/* A line from a to b with round ends, r thick on each side. */
static float sd_seg(struct pt p, float ax, float ay, float bx, float by, float r)
{
    float pax = p.x - ax, pay = p.y - ay, bax = bx - ax, bay = by - ay;
    float h = fclamp((pax * bax + pay * bay) / (bax * bax + bay * bay), 0, 1);
    return len2(pax - bax * h, pay - bay * h) - r;
}

/* Axis-aligned ellipse (a close approximation of the true distance). */
static float sd_ellipse(struct pt p, float cx, float cy, float a, float b)
{
    float x = p.x - cx, y = p.y - cy;
    float k0 = len2(x / a, y / b), k1 = len2(x / (a * a), y / (b * b));
    return k1 < 1e-6f ? -fmin2(a, b) : k0 * (k0 - 1) / k1;
}

/* Polygon given as x0,y0,x1,y1,... */
static float sd_poly(struct pt p, const float *v, int n)
{
    float dx = p.x - v[0], dy = p.y - v[1];
    float d = dx * dx + dy * dy, s = 1;
    for (int i = 0, j = n - 1; i < n; j = i, i++) {
        float ex = v[2 * j] - v[2 * i], ey = v[2 * j + 1] - v[2 * i + 1];
        float wx = p.x - v[2 * i], wy = p.y - v[2 * i + 1];
        float h = fclamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0, 1);
        float bx = wx - ex * h, by = wy - ey * h;
        d = fmin2(d, bx * bx + by * by);
        bool c1 = p.y >= v[2 * i + 1], c2 = p.y < v[2 * j + 1], c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3))
            s = -s;
    }
    return s * fsqrt(d);
}

/* ---------------------------------------------------------------- painting */
struct pc {
    struct pt p;
    float px;         /* one pixel, in icon units */
    float r, g, b, a; /* premultiplied */
};

static void paint(struct pc *c, float d, u32 col, float alpha, float soft)
{
    float cov = 0.5f - d / soft;
    if (cov <= 0)
        return;
    cov = fmin2(cov, 1) * alpha;
    c->r = (float)(col >> 16 & 255) * cov + c->r * (1 - cov);
    c->g = (float)(col >> 8 & 255) * cov + c->g * (1 - cov);
    c->b = (float)(col & 255) * cov + c->b * (1 - cov);
    c->a = cov + c->a * (1 - cov);
}

static void layer(struct pc *c, float d, u32 col) { paint(c, d, col, 1, c->px); }
static void layer_a(struct pc *c, float d, u32 col, float alpha) { paint(c, d, col, alpha, c->px); }

/* Vertical gradient between the shape's top and bottom (y0..y1). */
static u32 grad(u32 top, u32 bot, float y, float y0, float y1)
{
    float t = fclamp((y - y0) / (y1 - y0), 0, 1);
    return rgb_mix(top, bot, (int)(t * 256));
}

/* A soft drop shadow under a shape, for icons large enough to show it. */
static void shadow(struct pc *c, float d_offset)
{
    if (c->px < 1.0f / 30)
        paint(c, d_offset + 0.02f, 0x000000, 0.35f, 0.09f);
}

/* Rounded-square tile with a gradient, a top highlight and an inner edge. */
static void tile(struct pc *c, u32 top, u32 bot)
{
    struct pt p = c->p, q = { p.x, p.y - 0.03f };
    shadow(c, sd_rrect(q, .07f, .07f, .93f, .93f, .2f));
    float t = sd_rrect(p, .07f, .07f, .93f, .93f, .2f);
    layer(c, t, grad(top, bot, p.y, .07f, .93f));
    layer_a(c, fmax2(t, sd_rrect(p, .07f, .07f, .93f, .5f, .2f)), 0xffffff, .07f); /* sheen */
    layer_a(c, fabs1(t + c->px * 0.5f) - c->px * 0.5f, 0xffffff, .16f);         /* edge */
}

static void disc(struct pc *c, u32 top, u32 bot)
{
    struct pt p = c->p, q = { p.x, p.y - 0.03f };
    shadow(c, sd_circle(q, .5f, .5f, .43f));
    float d = sd_circle(p, .5f, .5f, .43f);
    layer(c, d, grad(top, bot, p.y, .07f, .93f));
    layer_a(c, fmax2(d, sd_circle(p, .5f, .2f, .45f)), 0xffffff, .08f);
    layer_a(c, fabs1(d + c->px * 0.5f) - c->px * 0.5f, 0xffffff, .14f);
}

static void folder(struct pc *c, u32 back_top, u32 back_bot, u32 front_top, u32 front_bot)
{
    struct pt p = c->p, q = { p.x, p.y - 0.03f };
    shadow(c, sd_rrect(q, .06f, .16f, .94f, .86f, .08f));
    float back = fmin2(sd_rrect(p, .06f, .16f, .94f, .86f, .08f), sd_rrect(p, .06f, .11f, .45f, .3f, .06f));
    layer(c, back, grad(back_top, back_bot, p.y, .11f, .86f));
    layer_a(c, sd_rrect(p, .15f, .23f, .85f, .7f, .03f), 0xf6f3ff, .95f); /* paper inside */
    float front = sd_rrect(p, .06f, .33f, .94f, .86f, .08f);
    layer(c, front, grad(front_top, front_bot, p.y, .33f, .86f));
    layer_a(c, fmax2(front, -sd_rrect(p, .06f, .35f, .94f, .9f, .08f)), 0xffffff, .45f); /* lip */
}

static void paint_icon(int kind, struct pc *c)
{
    struct pt p = c->p;
    switch (kind) {
    case IC_TERM:
        tile(c, 0x4b4468, 0x1a1727);
        layer(c, fmin2(sd_seg(p, .27f, .35f, .45f, .5f, .055f), sd_seg(p, .45f, .5f, .27f, .65f, .055f)), 0x8ef0b0);
        layer(c, sd_seg(p, .53f, .67f, .73f, .67f, .05f), 0xe8e3f7);
        break;
    case IC_CMD:
        tile(c, 0x7565d8, 0x3f3394);
        layer(c, fmin2(sd_seg(p, .32f, .32f, .55f, .5f, .07f), sd_seg(p, .55f, .5f, .32f, .68f, .07f)), 0xffffff);
        break;
    case IC_CONSOLE:
        tile(c, 0x413a5a, 0x1f1b2e);
        layer(c, fabs1(sd_rrect(p, .2f, .23f, .8f, .65f, .06f)) - .035f, 0xffffff);
        layer_a(c, sd_rrect(p, .26f, .29f, .74f, .59f, .02f), 0x8f7fe0, .5f);
        layer(c, fmin2(sd_seg(p, .5f, .66f, .5f, .76f, .03f), sd_seg(p, .36f, .78f, .64f, .78f, .03f)), 0xffffff);
        break;
    case IC_FILES:
    case IC_FOLDER:
        folder(c, 0xe39a2d, 0xbd7210, 0xffd570, 0xf3a537);
        break;
    case IC_HOME: {
        folder(c, 0x7f6ee0, 0x5040b0, 0xbcb0ff, 0x8a79e8);
        static const float roof[] = { .5f, .42f, .71f, .6f, .29f, .6f };
        float house = fmin2(sd_poly(p, roof, 3) - .015f, sd_rrect(p, .35f, .56f, .65f, .78f, .02f));
        layer(c, house, 0xffffff);
        layer(c, sd_rrect(p, .46f, .65f, .54f, .79f, .015f), 0x9687ec);
        break;
    }
    case IC_FILE: {
        static const float page[] = { .22f, .1f, .6f, .1f, .78f, .28f, .78f, .9f, .22f, .9f };
        static const float fold[] = { .6f, .1f, .6f, .28f, .78f, .28f };
        struct pt q = { p.x, p.y - 0.03f };
        shadow(c, sd_poly(q, page, 5));
        float d = sd_poly(p, page, 5) - .015f;
        layer(c, d, grad(0xffffff, 0xd9d3ee, p.y, .1f, .9f));
        layer(c, sd_poly(p, fold, 3) - .01f, 0xb3a9d6);
        float lines = fmin2(fmin2(sd_seg(p, .33f, .45f, .67f, .45f, .025f), sd_seg(p, .33f, .58f, .67f, .58f, .025f)),
                            sd_seg(p, .33f, .71f, .55f, .71f, .025f));
        layer(c, lines, 0x9890b8);
        break;
    }
    case IC_MONITOR: {
        tile(c, 0x35ccb2, 0x15738c);
        for (int k = 0; k < 3; k++)
            layer_a(c, sd_seg(p, .18f, .36f + k * .19f, .82f, .36f + k * .19f, .006f), 0xffffff, .22f);
        static const float pts[] = { .18f, .7f, .34f, .52f, .49f, .62f, .65f, .34f, .82f, .45f };
        float d = 1;
        for (int k = 0; k < 4; k++)
            d = fmin2(d, sd_seg(p, pts[2 * k], pts[2 * k + 1], pts[2 * k + 2], pts[2 * k + 3], .045f));
        layer(c, d, 0xffffff);
        layer(c, sd_circle(p, .65f, .34f, .075f), 0xffffff);
        layer(c, sd_circle(p, .65f, .34f, .035f), 0x1f9aa5);
        break;
    }
    case IC_ABOUT:
        disc(c, 0xa595f5, 0x5c4cc2);
        layer(c, fmin2(sd_circle(p, .5f, .3f, .065f), sd_seg(p, .5f, .46f, .5f, .72f, .06f)), 0xffffff);
        break;
    case IC_DEEPSEEK: {
        disc(c, 0x6d89ff, 0x3550e0);
        /* A whale facing right, its tail curling up behind it. */
        float body = fmin2(sd_ellipse(p, .55f, .58f, .27f, .155f), sd_circle(p, .65f, .53f, .135f));
        float tail = fmin2(sd_seg(p, .37f, .6f, .25f, .43f, .06f),
                           fmin2(sd_seg(p, .13f, .37f, .24f, .31f, .05f), sd_seg(p, .25f, .43f, .19f, .34f, .055f)));
        float spout = fmin2(sd_seg(p, .64f, .33f, .64f, .23f, .025f),
                            fmin2(sd_seg(p, .64f, .27f, .57f, .2f, .022f), sd_seg(p, .64f, .27f, .71f, .2f, .022f)));
        layer(c, fmin2(body, tail) - .005f, 0xffffff);
        layer(c, spout, 0xdfe6ff);
        layer(c, sd_circle(p, .7f, .5f, .032f), 0x3d5cf0);
        layer_a(c, sd_seg(p, .6f, .64f, .79f, .6f, .014f), 0x3d5cf0, .8f);
        break;
    }
    case IC_POWER: {
        disc(c, 0xf2507a, 0xa8123c);
        float ring = fabs1(sd_circle(p, .5f, .54f, .21f)) - .052f;
        ring = fmax2(ring, -sd_seg(p, .5f, .1f, .5f, .42f, .11f));
        layer(c, fmin2(ring, sd_seg(p, .5f, .24f, .5f, .5f, .052f)), 0xffffff);
        break;
    }
    case IC_RESTART: {
        disc(c, 0x6496f5, 0x2c58c4);
        float ring = fabs1(sd_circle(p, .5f, .53f, .21f)) - .052f;
        ring = fmax2(ring, -sd_circle(p, .69f, .3f, .13f));
        static const float head[] = { .44f, .19f, .65f, .32f, .44f, .45f };
        layer(c, fmin2(ring, sd_poly(p, head, 3) - .01f), 0xffffff);
        break;
    }
    case IC_CALC:
        tile(c, 0xb470ea, 0x6a33b4);
        layer(c, sd_rrect(p, .2f, .18f, .8f, .38f, .05f), 0xf1ecff);
        for (int k = 0; k < 6; k++) {
            float x = .2f + (k % 3) * .22f, y = .47f + (k / 3) * .2f;
            layer_a(c, sd_rrect(p, x, y, x + .16f, y + .14f, .035f), k == 5 ? 0xffb54a : 0xffffff, k == 5 ? 1 : .85f);
        }
        break;
    case IC_SETUP: {
        disc(c, 0x8a83a8, 0x48415f);
        float g = sd_circle(p, .5f, .5f, .21f);
        g = fmin2(g, sd_seg(p, .5f, .2f, .5f, .8f, .065f));
        g = fmin2(g, sd_seg(p, .2f, .5f, .8f, .5f, .065f));
        g = fmin2(g, sd_seg(p, .288f, .288f, .712f, .712f, .065f));
        g = fmin2(g, sd_seg(p, .288f, .712f, .712f, .288f, .065f));
        g = fmax2(g, -sd_circle(p, .5f, .5f, .085f));
        layer(c, g, 0xffffff);
        break;
    }
    case IC_SONAR: {
        /* A dot and two arcs of a ping, opening to the upper right. */
        float cx = .28f, cy = .72f;
        float quad = fmax2(cx - p.x, p.y - cy);
        for (int k = 0; k < 2; k++) {
            float r = k ? .58f : .34f;
            float arc = fmax2(fabs1(sd_circle(p, cx, cy, r)) - .06f, quad);
            /* round the ends */
            arc = fmin2(arc, sd_circle(p, cx + r, cy, .06f));
            arc = fmin2(arc, sd_circle(p, cx, cy - r, .06f));
            layer(c, arc, k ? 0x9d8cf0 : 0xc4b8ff);
        }
        layer(c, sd_circle(p, cx, cy, .1f), 0xe9e4ff);
        break;
    }
    }
}

/* ---------------------------------------------------------------- cache */
struct cached {
    int kind, size;
    u32 *argb;
};
#define NCACHE 64
static struct cached cache[NCACHE];
static int next_slot;

static const u32 *render(int kind, int size)
{
    for (int i = 0; i < NCACHE; i++)
        if (cache[i].argb && cache[i].kind == kind && cache[i].size == size)
            return cache[i].argb;
    u32 *img = kmalloc((size_t)size * size * 4);
    if (!img)
        return NULL;
    float px = 1.0f / (float)size;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            struct pc c = { { ((float)x + .5f) * px, ((float)y + .5f) * px }, px, 0, 0, 0, 0 };
            paint_icon(kind, &c);
            u32 a = (u32)(c.a * 255 + .5f);
            u32 v = 0;
            if (a) {
                u32 r = (u32)fclamp(c.r / c.a + .5f, 0, 255), g = (u32)fclamp(c.g / c.a + .5f, 0, 255),
                    b = (u32)fclamp(c.b / c.a + .5f, 0, 255);
                v = a << 24 | r << 16 | g << 8 | b;
            }
            img[y * size + x] = v;
        }
    struct cached *slot = &cache[next_slot];
    next_slot = (next_slot + 1) % NCACHE;
    kfree(slot->argb);
    *slot = (struct cached){ kind, size, img };
    return img;
}

void icon_draw(struct surface *s, int kind, struct rect r)
{
    int size = r.w < r.h ? r.w : r.h;
    if (size <= 0 || kind < 0 || kind >= IC_COUNT)
        return;
    const u32 *img = render(kind, size);
    if (!img)
        return;
    int x0 = r.x + (r.w - size) / 2, y0 = r.y + (r.h - size) / 2;
    struct rect area = rect_intersect(rect_make(x0, y0, size, size), s->clip);
    area = rect_intersect(area, rect_make(0, 0, s->w, s->h));
    for (int y = area.y; y < area.y + area.h; y++) {
        const u32 *src = img + (y - y0) * size + (area.x - x0);
        u32 *dst = s->px + (size_t)y * s->stride + area.x;
        for (int x = 0; x < area.w; x++) {
            u32 v = src[x], a = v >> 24;
            if (a == 255)
                dst[x] = v & 0xffffff;
            else if (a)
                dst[x] = rgb_mix(dst[x], v & 0xffffff, (int)(a + (a >> 7)));
        }
    }
}
