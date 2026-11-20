/*
 * Kernel random numbers: xoshiro256** whose state is stirred with every
 * input given to random_mix() (counter jitter, device entropy). Good for
 * protocol identifiers (TCP sequence numbers, DNS/DHCP ids, ports); it is
 * not a cryptographic generator.
 */
#include "random.h"
#include "arch.h"
#include "irq.h"
#include "smp.h"

static u64 s[4];
static bool seeded;
static spinlock_t lock = SPINLOCK_INIT;

static u64 splitmix(u64 *x)
{
    u64 z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static inline u64 rotl(u64 x, int k) { return (x << k) | (x >> (64 - k)); }

static u64 next(void)
{
    u64 r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return r;
}

static void stir(u64 v)
{
    u64 x = v ^ read_sysreg(cntvct_el0);
    for (int i = 0; i < 4; i++)
        s[i] ^= splitmix(&x);
    if (!(s[0] | s[1] | s[2] | s[3]))
        s[0] = 1;
    next();
}

void random_mix(const void *data, size_t len)
{
    const u8 *p = data;
    u64 f = spin_lock_irqsave(&lock);
    while (len) {
        u64 v = 0;
        size_t n = len < 8 ? len : 8;
        for (size_t i = 0; i < n; i++)
            v |= (u64)p[i] << (8 * i);
        stir(v);
        p += n;
        len -= n;
    }
    seeded = true;
    spin_unlock_irqrestore(&lock, f);
}

u64 random_u64(void)
{
    u64 f = spin_lock_irqsave(&lock);
    if (!seeded) {
        stir(time_ns());
        seeded = true;
    }
    stir(time_ns()); /* timing jitter of each request */
    u64 r = next();
    spin_unlock_irqrestore(&lock, f);
    return r;
}

u32 random_u32(void) { return (u32)(random_u64() >> 32); }

void random_bytes(void *out, size_t len)
{
    u8 *p = out;
    while (len) {
        u64 v = random_u64();
        size_t n = len < 8 ? len : 8;
        for (size_t i = 0; i < n; i++)
            p[i] = (u8)(v >> (8 * i));
        p += n;
        len -= n;
    }
}
