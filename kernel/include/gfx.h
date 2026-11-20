/* 2D drawing on 32-bit 0x00RRGGBB surfaces (kernel/ui/gfx.c). Every
 * operation is clipped to the surface's clip rectangle. */
#pragma once
#include "types.h"

struct rect {
    int x, y, w, h;
};

struct surface {
    u32 *px;
    int w, h;
    int stride;       /* in pixels */
    struct rect clip; /* drawing is limited to this rectangle */
};

static inline struct rect rect_make(int x, int y, int w, int h) { return (struct rect){ x, y, w, h }; }
bool rect_empty(struct rect r);
bool rect_contains(struct rect r, int x, int y);
struct rect rect_intersect(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b); /* bounding box; empty rects ignored */

bool surface_alloc(struct surface *s, int w, int h);
void surface_free(struct surface *s);
void surface_reset_clip(struct surface *s);

u32 rgb_mix(u32 a, u32 b, int alpha); /* alpha 0..256: 0 = a, 256 = b */

void gfx_fill(struct surface *s, struct rect r, u32 color);
void gfx_blend(struct surface *s, struct rect r, u32 color, int alpha);
void gfx_round_rect(struct surface *s, struct rect r, int radius, u32 color, int alpha);
void gfx_vgradient(struct surface *s, struct rect r, u32 top, u32 bottom);
void gfx_frame(struct surface *s, struct rect r, u32 color, int alpha); /* 1px outline */
void gfx_shadow(struct surface *s, struct rect r, int spread, int strength);
void gfx_disc(struct surface *s, int cx, int cy, int radius, u32 color);
void gfx_hline(struct surface *s, int x, int y, int w, u32 color);
void gfx_vline(struct surface *s, int x, int y, int h, u32 color);
void gfx_blit(struct surface *dst, int x, int y, const struct surface *src);

/* Text with the 8x16 console font, scaled by an integer factor. */
int gfx_text(struct surface *s, int x, int y, const char *str, u32 color, int scale);
int gfx_text_bold(struct surface *s, int x, int y, const char *str, u32 color, int scale);
int gfx_text_width(const char *str, int scale);
/* Text cut to fit max_w pixels, ending in "..." when shortened. */
int gfx_text_fit(struct surface *s, int x, int y, const char *str, u32 color, int scale, int max_w);

u32 isqrt32(u32 v);
