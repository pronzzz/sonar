/*
 * Random bytes for keys: a ChaCha20 generator (key erasure after every
 * request) whose key is SHA-256 of everything available:
 *   - fresh bytes from the virtio entropy device, when there is one;
 *   - the RNDR instruction (FEAT_RNG), when the CPU has it;
 *   - timer jitter: the counter's low bits across many short loops;
 *   - the previous key.
 * kernel/core/random.c stays the fast, non-cryptographic generator for
 * protocol identifiers.
 */
#include "arch.h"
#include "crypto.h"
#include "irq.h"
#include "random.h"
#include "string.h"

size_t virtio_rng_read(void *out, size_t len);

static u8 key[32];
static u64 requests;

static bool have_rndr(void)
{
    u64 isar0;
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    return ((isar0 >> 60) & 0xf) >= 1;
}

static bool rndr(u64 *v)
{
    u64 x, ok;
    /* RNDR is s3_3_c2_c4_0; NZCV.Z is set on failure */
    __asm__ volatile("mrs %0, s3_3_c2_c4_0\n\tcset %1, ne" : "=r"(x), "=r"(ok) : : "cc");
    *v = x;
    return ok;
}

static void reseed(void)
{
    struct sha256 c;
    sha256_init(&c);
    sha256_update(&c, key, sizeof(key));
    sha256_update(&c, &requests, sizeof(requests));

    u8 dev[64];
    size_t n = virtio_rng_read(dev, sizeof(dev));
    sha256_update(&c, dev, n);
    crypto_wipe(dev, sizeof(dev));

    if (have_rndr())
        for (int i = 0; i < 4; i++) {
            u64 v;
            if (rndr(&v))
                sha256_update(&c, &v, sizeof(v));
        }

    /* Jitter: how long short busy loops take varies with caches, other
     * CPUs, interrupts and the host; keep the raw samples. */
    u64 samples[64];
    for (int i = 0; i < 64; i++) {
        u64 t0 = read_sysreg(cntvct_el0);
        volatile u32 spin = 0;
        for (int j = 0; j < 50 + (int)(t0 & 31); j++)
            spin += (u32)j;
        samples[i] = read_sysreg(cntvct_el0) - t0 + (t0 << 20);
    }
    sha256_update(&c, samples, sizeof(samples));
    u64 extra[3] = { time_ns(), random_u64(), random_u64() };
    sha256_update(&c, extra, sizeof(extra));
    sha256_final(&c, key);
}

void crypto_random(void *out, size_t len)
{
    reseed();
    requests++;
    u8 nonce[12];
    memset(nonce, 0, sizeof(nonce));
    memcpy(nonce, &requests, sizeof(requests));
    u8 block[64];
    u8 *p = out;
    u32 ctr = 1;
    while (len) {
        chacha20_block(key, ctr++, nonce, block);
        size_t n = len < 64 ? len : 64;
        memcpy(p, block, n);
        p += n;
        len -= n;
    }
    /* key erasure: block 0 becomes the next key */
    chacha20_block(key, 0, nonce, block);
    memcpy(key, block, 32);
    crypto_wipe(block, sizeof(block));
}
