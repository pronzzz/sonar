/*
 * Firmware framebuffer: a linear framebuffer the firmware already set up,
 * either the UEFI GOP framebuffer (kernel/arch/arm64/efi.c) or a device
 * tree "simple-framebuffer" node (Linux binding simple-framebuffer.yaml, as
 * provided by U-Boot or by m1n1 on Apple silicon Macs).
 *
 * The console draws into a shadow copy in RAM; a flush copies the changed
 * rectangle to the framebuffer (converting the pixel format if needed) and
 * cleans it from the data cache, so it works whether the framebuffer is
 * mapped cacheable (ordinary RAM) or not, and scrolling never reads the
 * slow framebuffer.
 */
#include "arch.h"
#include "efi.h"
#include "fbcon.h"
#include "fdt.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"

enum { FMT_XRGB8888, FMT_XBGR8888, FMT_XRGB2101010, FMT_RGB565 };

struct sfb {
    struct framebuffer fb; /* the shadow the console draws into */
    u8 *hw;
    u32 hw_stride, format;
};

static struct sfb sfb;

static void flush(void *ctx, u32 x, u32 y, u32 w, u32 h)
{
    struct sfb *s = ctx;
    for (u32 r = y; r < y + h; r++) {
        const u32 *src = (const u32 *)(s->fb.base + (u64)r * s->fb.stride) + x;
        u8 *row = s->hw + (u64)r * s->hw_stride;
        switch (s->format) {
        case FMT_XRGB8888:
        case FMT_XBGR8888:
            memcpy(row + (u64)x * 4, src, (size_t)w * 4);
            break;
        case FMT_XRGB2101010: {
            u32 *dst = (u32 *)row + x;
            for (u32 i = 0; i < w; i++) {
                u32 p = src[i], rr = p >> 16 & 0xff, g = p >> 8 & 0xff, b = p & 0xff;
                dst[i] = (rr << 2 | rr >> 6) << 20 | (g << 2 | g >> 6) << 10 | (b << 2 | b >> 6);
            }
            break;
        }
        case FMT_RGB565: {
            u16 *dst = (u16 *)row + x;
            for (u32 i = 0; i < w; i++) {
                u32 p = src[i];
                dst[i] = (u16)((p >> 8 & 0xf800) | (p >> 5 & 0x07e0) | (p >> 3 & 0x001f));
            }
            break;
        }
        }
    }
    uintptr_t start = (uintptr_t)(s->hw + (u64)y * s->hw_stride);
    dcache_clean_range(start, start + (u64)h * s->hw_stride);
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool in_dt_ram(u64 base)
{
    int depth = 0, n = 0;
    while ((n = fdt_next_node(n, &depth)) >= 0) {
        const char *type = fdt_prop_str(n, "device_type");
        if (!type || strcmp(type, "memory") != 0)
            continue;
        u64 a, sz;
        for (int i = 0; fdt_reg(n, i, &a, &sz); i++)
            if (base >= a && base < a + sz)
                return true;
    }
    return false;
}

static bool start(const char *what, u64 base, u64 size, u32 w, u32 h, u32 stride, u32 format,
                  bool in_ram)
{
    u32 bpp = format == FMT_RGB565 ? 2 : 4;
    if (w < 320 || h < 200 || stride < w * bpp || (u64)stride * h > size) {
        printk("  fb: %s framebuffer has an unusable geometry (%ux%u, stride %u)\n", what, w, h,
               stride);
        return false;
    }
    /* Cacheable if it is RAM (then cleaned after each flush), else
     * normal non-cacheable (write-combining). */
    if (!mmu_map(ALIGN_DOWN(base, PAGE_SIZE), ALIGN_UP(size + (base & (PAGE_SIZE - 1)), PAGE_SIZE),
                 in_ram ? MAP_NORMAL : MAP_NOCACHE))
        return false;
    u64 shadow_bytes = (u64)w * h * 4;
    u8 *shadow = page_alloc(ALIGN_UP(shadow_bytes, PAGE_SIZE) / PAGE_SIZE);
    if (!shadow) {
        printk("  fb: no memory for a %ux%u console\n", w, h);
        return false;
    }
    sfb.hw = (u8 *)(uintptr_t)base;
    sfb.hw_stride = stride;
    sfb.format = format;
    sfb.fb = (struct framebuffer){ "fb0", shadow, w, h, w * 4, format == FMT_XBGR8888, flush, &sfb };
    fbcon_attach(&sfb.fb);
    printk("  fb0: %s framebuffer at %#llx\n", what, (unsigned long long)base);
    return true;
}

void simplefb_probe(void)
{
    if (efi_booted() && efi_handoff.fb_base) {
        const struct efi_handoff *e = &efi_handoff;
        if (start("UEFI GOP", e->fb_base, e->fb_size, e->fb_width, e->fb_height, e->fb_stride,
                  e->fb_bgr ? FMT_XBGR8888 : FMT_XRGB8888, e->fb_in_ram))
            return;
    }
    int n = -1;
    while ((n = fdt_find_compatible(n, "simple-framebuffer")) >= 0) {
        if (!fdt_is_enabled(n))
            continue;
        u64 base, size;
        u32 w, h, stride;
        const char *fmt = fdt_prop_str(n, "format");
        if (!fdt_reg(n, 0, &base, &size) || !fdt_prop_u32(n, "width", &w) ||
            !fdt_prop_u32(n, "height", &h) || !fdt_prop_u32(n, "stride", &stride) || !fmt)
            continue;
        int format;
        if (!strcmp(fmt, "a8r8g8b8") || !strcmp(fmt, "x8r8g8b8"))
            format = FMT_XRGB8888;
        else if (!strcmp(fmt, "a8b8g8r8") || !strcmp(fmt, "x8b8g8r8"))
            format = FMT_XBGR8888;
        else if (!strcmp(fmt, "x2r10g10b10") || !strcmp(fmt, "a2r10g10b10"))
            format = FMT_XRGB2101010;
        else if (!strcmp(fmt, "r5g6b5"))
            format = FMT_RGB565;
        else {
            printk("  fb: simple-framebuffer format \"%s\" is not supported\n", fmt);
            continue;
        }
        if (start("device tree", base, size, w, h, stride, format, in_dt_ram(base)))
            return;
    }
}
