/*
 * Framebuffer console: draws console output with Sonar's 8x16 font
 * (font8x16.h, scaled up on large screens) and understands the subset of
 * ANSI/VT100 sequences the kernel emits: SGR colours, erase in line and
 * display, and cursor movement. UTF-8 text is decoded; characters outside
 * the font are shown as close ASCII equivalents or a box.
 *
 * Called with the console output lock held (console_write/console_flush).
 */
#include "fbcon.h"
#include "console.h"
#include "font8x16.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"

static struct framebuffer *fb;
static u32 scale, cw, ch, cols, rows;
static u32 cx, cy;          /* cursor cell */
static bool wrap_pending;   /* cursor sits past the last column */
static u32 fg, bg;
static bool bold, dim, inverse;
static bool cursor_drawn;
static u32 dx0, dy0, dx1, dy1; /* dirty rectangle (pixels), empty if dx0 >= dx1 */
static u64 last_flush;

/* parser */
static int esc_state; /* 0 text, 1 after ESC, 2 in CSI */
static int params[8], nparams;
static bool param_started;
static u32 utf8_cp;
static int utf8_left;

static const u32 palette[16] = {
    0x1c2026, 0xe06c75, 0x98c379, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xc8ccd4,
    0x5c6370, 0xff7b86, 0xb5e890, 0xffd68a, 0x8cc8ff, 0xdf9cf3, 0x7fdbe6, 0xffffff,
};
#define DEFAULT_FG 0xc8ccd4
#define DEFAULT_BG 0x0f1115

static inline u32 pix(u32 rgb)
{
    if (!fb->bgr)
        return rgb;
    return ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff);
}

static void dirty(u32 x, u32 y, u32 w, u32 h)
{
    if (dx0 >= dx1) {
        dx0 = x, dy0 = y, dx1 = x + w, dy1 = y + h;
        return;
    }
    dx0 = MIN(dx0, x), dy0 = MIN(dy0, y);
    dx1 = MAX(dx1, x + w), dy1 = MAX(dy1, y + h);
}

static void fill(u32 x, u32 y, u32 w, u32 h, u32 rgb)
{
    u32 p = pix(rgb);
    for (u32 j = 0; j < h; j++) {
        u32 *row = (u32 *)(fb->base + (size_t)(y + j) * fb->stride) + x;
        for (u32 i = 0; i < w; i++)
            row[i] = p;
    }
    dirty(x, y, w, h);
}

static void draw_glyph(u32 col, u32 row, u8 c, u32 f, u32 b)
{
    const u8 *g = font8x16[c & 127];
    u32 pf = pix(f), pb = pix(b);
    u32 x0 = col * cw, y0 = row * ch;
    for (u32 gy = 0; gy < 16; gy++) {
        u8 bits = g[gy];
        for (u32 sy = 0; sy < scale; sy++) {
            u32 *p = (u32 *)(fb->base + (size_t)(y0 + gy * scale + sy) * fb->stride) + x0;
            for (u32 gx = 0; gx < 8; gx++) {
                u32 v = (bits & (0x80 >> gx)) ? pf : pb;
                for (u32 sx = 0; sx < scale; sx++)
                    *p++ = v;
            }
        }
    }
    dirty(x0, y0, cw, ch);
}

/* The character under the cursor is remembered so the cursor can be undrawn. */
static u8 *screen_chars; /* rows * cols */
static u32 *screen_fg, *screen_bg;

static void put_cell(u32 col, u32 row, u8 c, u32 f, u32 b)
{
    size_t i = (size_t)row * cols + col;
    screen_chars[i] = c;
    screen_fg[i] = f;
    screen_bg[i] = b;
    draw_glyph(col, row, c, f, b);
}

static void hide_cursor(void)
{
    if (!cursor_drawn)
        return;
    cursor_drawn = false;
    if (cx < cols && cy < rows) {
        size_t i = (size_t)cy * cols + cx;
        draw_glyph(cx, cy, screen_chars[i], screen_fg[i], screen_bg[i]);
    }
}

static void show_cursor(void)
{
    if (cursor_drawn || cx >= cols || cy >= rows)
        return;
    size_t i = (size_t)cy * cols + cx;
    draw_glyph(cx, cy, screen_chars[i], screen_bg[i], DEFAULT_FG); /* inverse block */
    cursor_drawn = true;
}

static void scroll_up(void)
{
    size_t line = (size_t)ch * fb->stride;
    memmove(fb->base, fb->base + line, line * (rows - 1));
    memmove(screen_chars, screen_chars + cols, (size_t)cols * (rows - 1));
    memmove(screen_fg, screen_fg + cols, sizeof(u32) * cols * (rows - 1));
    memmove(screen_bg, screen_bg + cols, sizeof(u32) * cols * (rows - 1));
    for (u32 i = 0; i < cols; i++) {
        size_t k = (size_t)(rows - 1) * cols + i;
        screen_chars[k] = ' ';
        screen_fg[k] = fg;
        screen_bg[k] = DEFAULT_BG;
    }
    fill(0, (rows - 1) * ch, cols * cw, ch, DEFAULT_BG);
    dirty(0, 0, cols * cw, rows * ch);
}

static void newline(void)
{
    wrap_pending = false;
    cx = 0;
    if (cy + 1 < rows)
        cy++;
    else
        scroll_up();
}

static void erase_cells(u32 row, u32 from, u32 to)
{
    for (u32 c = from; c < to && c < cols; c++)
        put_cell(c, row, ' ', fg, DEFAULT_BG);
}

static u32 cur_fg(void)
{
    u32 f = fg;
    if (dim) /* halfway to the background */
        f = (((f >> 16 & 255) + (DEFAULT_BG >> 16 & 255)) / 2) << 16 |
            (((f >> 8 & 255) + (DEFAULT_BG >> 8 & 255)) / 2) << 8 |
            (((f & 255) + (DEFAULT_BG & 255)) / 2);
    return f;
}

static void put_char(u8 c)
{
    if (wrap_pending)
        newline();
    u32 f = cur_fg(), b = bg;
    if (inverse) {
        u32 t = f;
        f = b;
        b = t;
    }
    put_cell(cx, cy, c, f, b);
    if (cx + 1 < cols)
        cx++;
    else
        wrap_pending = true;
}

static void sgr(void)
{
    if (nparams == 0)
        params[nparams++] = 0;
    for (int i = 0; i < nparams; i++) {
        int p = params[i];
        if (p == 0) {
            fg = DEFAULT_FG, bg = DEFAULT_BG;
            bold = dim = inverse = false;
        } else if (p == 1) {
            bold = true;
            dim = false;
        } else if (p == 2) {
            dim = true;
        } else if (p == 22) {
            bold = dim = false;
        } else if (p == 7) {
            inverse = true;
        } else if (p == 27) {
            inverse = false;
        } else if (p >= 30 && p <= 37) {
            fg = palette[p - 30 + (bold ? 8 : 0)];
        } else if (p == 39) {
            fg = DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            bg = palette[p - 40];
        } else if (p == 49) {
            bg = DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            fg = palette[p - 90 + 8];
        }
    }
    if (bold && fg == DEFAULT_FG)
        fg = 0xffffff;
}

static void csi(char final)
{
    int n = nparams ? params[0] : 0;
    int n1 = n ? n : 1;
    switch (final) {
    case 'm': sgr(); break;
    case 'K':
        wrap_pending = false;
        if (n == 0)
            erase_cells(cy, cx, cols);
        else if (n == 1)
            erase_cells(cy, 0, cx + 1);
        else
            erase_cells(cy, 0, cols);
        break;
    case 'J':
        if (n == 2 || n == 3) {
            for (u32 r = 0; r < rows; r++)
                erase_cells(r, 0, cols);
        } else if (n == 0) {
            erase_cells(cy, cx, cols);
            for (u32 r = cy + 1; r < rows; r++)
                erase_cells(r, 0, cols);
        }
        break;
    case 'H':
    case 'f': {
        int r = n1, c = nparams > 1 && params[1] ? params[1] : 1;
        cy = MIN((u32)r - 1, rows - 1);
        cx = MIN((u32)c - 1, cols - 1);
        wrap_pending = false;
        break;
    }
    case 'A': cy = cy >= (u32)n1 ? cy - (u32)n1 : 0; wrap_pending = false; break;
    case 'B': cy = MIN(cy + (u32)n1, rows - 1); wrap_pending = false; break;
    case 'C': cx = MIN(cx + (u32)n1, cols - 1); wrap_pending = false; break;
    case 'D':
        if (wrap_pending) { /* the cursor is logically one past the last column */
            wrap_pending = false;
            n1--;
        }
        cx = cx >= (u32)n1 ? cx - (u32)n1 : 0;
        break;
    case 'G': cx = MIN((u32)n1 - 1, cols - 1); wrap_pending = false; break;
    default: break; /* unsupported sequences are ignored */
    }
}

/* ASCII stand-ins for common characters the font lacks. */
static const char *fallback(u32 cp)
{
    switch (cp) {
    case 0x2018: case 0x2019: case 0x2032: return "'";
    case 0x201c: case 0x201d: case 0x2033: return "\"";
    case 0x2013: case 0x2014: case 0x2212: case 0x2500: return "-";
    case 0x2026: return "...";
    case 0x2022: case 0x00b7: case 0x2219: return "*";
    case 0x2192: case 0x27f6: return "->";
    case 0x2190: return "<-";
    case 0x21d2: return "=>";
    case 0x00d7: return "x";
    case 0x00f7: return "/";
    case 0x2264: return "<=";
    case 0x2265: return ">=";
    case 0x2260: return "!=";
    case 0x2248: return "~";
    case 0x00b0: return "o";
    case 0x00a0: case 0x2003: case 0x2002: case 0x2009: return " ";
    case 0x2502: return "|";
    case 0x2713: case 0x2714: return "v";
    case 0x00e9: case 0x00e8: case 0x00ea: return "e";
    case 0x00e1: case 0x00e0: case 0x00e2: case 0x00e4: return "a";
    case 0x00f3: case 0x00f6: case 0x00f4: return "o";
    case 0x00fa: case 0x00fc: return "u";
    case 0x00ed: return "i";
    case 0x00f1: return "n";
    case 0x00e7: return "c";
    }
    if (cp >= 0x2500 && cp <= 0x257f)
        return "+"; /* box drawing */
    return NULL;
}

static void put_codepoint(u32 cp)
{
    if (cp >= 0xfe00 && cp <= 0xfe0f)
        return; /* variation selectors */
    if (cp == 0x200b || cp == 0x200d || cp == 0xfeff)
        return; /* zero-width */
    const char *s = fallback(cp);
    if (s) {
        while (*s)
            put_char((u8)*s++);
    } else {
        put_char(0x7f); /* box */
    }
}

static void feed(u8 c)
{
    if (utf8_left) {
        if ((c & 0xc0) == 0x80) {
            utf8_cp = utf8_cp << 6 | (c & 0x3f);
            if (--utf8_left == 0)
                put_codepoint(utf8_cp);
            return;
        }
        utf8_left = 0;
        put_char(0x7f);
    }
    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            nparams = 0;
            param_started = false;
            memset(params, 0, sizeof(params));
        } else {
            esc_state = 0;
        }
        return;
    }
    if (esc_state == 2) {
        if (c >= '0' && c <= '9') {
            if (!param_started) {
                if (nparams < 8)
                    nparams++;
                param_started = true;
            }
            params[nparams - 1] = params[nparams - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (!param_started && nparams < 8)
                nparams++; /* empty parameter */
            param_started = false;
        } else if (c == '?' || c == '>') {
            /* private-mode prefix: parsed and ignored */
        } else if (c >= 0x40 && c <= 0x7e) {
            esc_state = 0;
            csi((char)c);
        } else {
            esc_state = 0;
        }
        return;
    }
    if (c >= 0x80) {
        if ((c & 0xe0) == 0xc0)
            utf8_cp = c & 0x1f, utf8_left = 1;
        else if ((c & 0xf0) == 0xe0)
            utf8_cp = c & 0x0f, utf8_left = 2;
        else if ((c & 0xf8) == 0xf0)
            utf8_cp = c & 0x07, utf8_left = 3;
        else
            put_char(0x7f);
        return;
    }
    switch (c) {
    case 0x1b: esc_state = 1; break;
    case '\n': newline(); break;
    case '\r': cx = 0; wrap_pending = false; break;
    case '\b':
        if (wrap_pending)
            wrap_pending = false;
        else if (cx)
            cx--;
        break;
    case '\t':
        do
            put_char(' ');
        while (cx % 8 && !wrap_pending);
        break;
    case 0x07: break;
    default:
        if (c >= 0x20 && c < 0x7f)
            put_char(c);
        break;
    }
}

static void do_flush(void)
{
    show_cursor();
    if (dx0 < dx1 && fb->flush)
        fb->flush(fb->ctx, dx0, dy0, dx1 - dx0, dy1 - dy0);
    dx0 = dx1 = 0;
    last_flush = time_ms();
}

static void fb_write(const char *s, size_t len)
{
    hide_cursor();
    for (size_t i = 0; i < len; i++)
        feed((u8)s[i]);
    /* keep streaming output (e.g. the assistant's answer) visible */
    if (time_ms() - last_flush >= 30)
        do_flush();
}

static void fb_flush(void)
{
    do_flush();
}

static struct console_sink fb_sink = { "framebuffer", fb_write, fb_flush };

static struct framebuffer *screen; /* the display itself (fb may be a window) */

bool fbcon_active(void) { return fb != NULL; }
struct framebuffer *fbcon_screen(void) { return screen; }

static bool grid_alloc(struct framebuffer *f, u32 s)
{
    u32 c = f->width / (8 * s), r = f->height / (16 * s);
    size_t cells = (size_t)c * r;
    u8 *chars = kmalloc(cells);
    u32 *fgs = kmalloc(cells * sizeof(u32)), *bgs = kmalloc(cells * sizeof(u32));
    if (!c || !r || !chars || !fgs || !bgs) {
        kfree(chars);
        kfree(fgs);
        kfree(bgs);
        return false;
    }
    kfree(screen_chars);
    kfree(screen_fg);
    kfree(screen_bg);
    scale = s, cw = 8 * s, ch = 16 * s, cols = c, rows = r;
    screen_chars = chars, screen_fg = fgs, screen_bg = bgs;
    fg = DEFAULT_FG, bg = DEFAULT_BG;
    bold = dim = inverse = false;
    cx = cy = 0;
    wrap_pending = cursor_drawn = false;
    esc_state = 0;
    utf8_left = 0;
    for (size_t i = 0; i < cells; i++) {
        chars[i] = ' ';
        fgs[i] = DEFAULT_FG;
        bgs[i] = DEFAULT_BG;
    }
    fb = f;
    fill(0, 0, f->width, f->height, DEFAULT_BG);
    return true;
}

/*
 * Move the console onto another surface (a desktop window), or back to the
 * screen with NULL. The recent console output is replayed so the text is
 * re-wrapped for the new width. Caller holds the console output lock.
 */
bool fbcon_retarget_locked(struct framebuffer *f, const char *replay, size_t len)
{
    if (!fb)
        return false;
    if (!f)
        f = screen;
    if (!grid_alloc(f, scale))
        return false;
    /* start replay at a line boundary so no escape sequence is cut */
    const char *nl = len ? memchr(replay, '\n', len) : NULL;
    if (nl) {
        len -= (size_t)(nl + 1 - replay);
        replay = nl + 1;
        for (size_t i = 0; i < len; i++)
            feed((u8)replay[i]);
    }
    do_flush();
    return true;
}

void fbcon_attach(struct framebuffer *f)
{
    if (fb || !f->base || f->width < 320 || f->height < 200)
        return;
    u32 s = f->width >= 2400 ? 2 : 1; /* keep text readable on high-resolution screens */
    if (!grid_alloc(f, s))
        return;
    screen = f;
    do_flush();
    console_register(&fb_sink);
    printk("  %s: %ux%u framebuffer console, %ux%u characters\n", f->name, f->width, f->height,
           cols, rows);
}
