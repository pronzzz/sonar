/* Virtio entropy device (virtio 1.x, section 5.4): seeds the kernel's
 * random number generator at boot. */
#include "kernel.h"
#include "random.h"
#include "virtio.h"

static struct virtq *rng_q; /* kept for crypto_random() (kernel/crypto/rng.c) */

/* Up to len bytes from the device (0 if there is none). */
size_t virtio_rng_read(void *out, size_t len)
{
    static u8 buf[64];
    if (!rng_q)
        return 0;
    if (len > sizeof(buf))
        len = sizeof(buf);
    struct vq_buf b = { buf, (u32)len, true };
    u32 got = 0;
    if (!virtq_run(rng_q, &b, 1, &got) || !got)
        return 0;
    if (got > len)
        got = (u32)len;
    for (u32 i = 0; i < got; i++)
        ((u8 *)out)[i] = buf[i];
    return got;
}

bool virtio_rng_probe(struct virtio_dev *d)
{
    if (!virtio_negotiate(d, 0))
        return false;
    struct virtq *q = virtio_setup_queue(d, 0, 8);
    if (!q) {
        virtio_fail(d);
        return false;
    }
    virtq_interrupts(q, false);
    virtio_driver_ok(d);
    static u8 buf[64];
    struct vq_buf b = { buf, sizeof(buf), true };
    u32 got = 0;
    if (virtq_run(q, &b, 1, &got) && got) {
        random_mix(buf, got > sizeof(buf) ? sizeof(buf) : got);
        printk("  rng0: virtio entropy source (%u bytes mixed in)\n", got);
        rng_q = q;
    } else {
        printk("  rng0: virtio entropy source did not respond\n");
    }
    return true;
}
