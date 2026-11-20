/* Framebuffer text console: renders the kernel console on a linear
 * framebuffer (virtio-gpu, or the UEFI GOP framebuffer on real hardware). */
#pragma once
#include "types.h"

struct framebuffer {
    const char *name;
    u8 *base;              /* 32-bit pixels */
    u32 width, height;     /* pixels */
    u32 stride;            /* bytes per row */
    bool bgr;              /* pixel = 0x00BBGGRR instead of 0x00RRGGBB */
    /* Make a changed rectangle visible (NULL if writes show directly). */
    void (*flush)(void *ctx, u32 x, u32 y, u32 w, u32 h);
    void *ctx;
};

/* Start mirroring the console onto fb (the first framebuffer wins). */
void fbcon_attach(struct framebuffer *fb);
bool fbcon_active(void);

/* The display the console was first attached to (NULL if none). */
struct framebuffer *fbcon_screen(void);
/* Draw the console into f instead (NULL: back onto the screen), replaying
 * recent output. Caller holds the console lock (console_lock()). */
bool fbcon_retarget_locked(struct framebuffer *f, const char *replay, size_t len);
