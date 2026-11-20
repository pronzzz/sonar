/* ChaCha20, Poly1305 and the ChaCha20-Poly1305 AEAD (RFC 8439). */
#include "crypto.h"
#include "string.h"

static inline u32 le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static inline void put_le32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}
static inline u32 rol32(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

/* ----------------------------------------------------------- ChaCha20 */
#define QR(a, b, c, d)                                                                             \
    do {                                                                                           \
        a += b; d ^= a; d = rol32(d, 16);                                                          \
        c += d; b ^= c; b = rol32(b, 12);                                                          \
        a += b; d ^= a; d = rol32(d, 8);                                                           \
        c += d; b ^= c; b = rol32(b, 7);                                                           \
    } while (0)

void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64])
{
    u32 s[16], x[16];
    s[0] = 0x61707865; /* "expand 32-byte k" */
    s[1] = 0x3320646e;
    s[2] = 0x79622d32;
    s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++)
        s[4 + i] = le32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++)
        s[13 + i] = le32(nonce + 4 * i);
    for (int i = 0; i < 16; i++)
        x[i] = s[i];
    for (int r = 0; r < 10; r++) {
        QR(x[0], x[4], x[8], x[12]);
        QR(x[1], x[5], x[9], x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8], x[13]);
        QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++)
        put_le32(out + 4 * i, x[i] + s[i]);
    crypto_wipe(s, sizeof(s));
    crypto_wipe(x, sizeof(x));
}

void chacha20_xor(const u8 key[32], u32 counter, const u8 nonce[12], const void *in,
                  void *out, size_t len)
{
    const u8 *src = in;
    u8 *dst = out, ks[64];
    while (len) {
        chacha20_block(key, counter++, nonce, ks);
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++)
            dst[i] = src[i] ^ ks[i];
        src += n;
        dst += n;
        len -= n;
    }
    crypto_wipe(ks, sizeof(ks));
}

/* ----------------------------------------------------------- Poly1305 */
/* The accumulator in five 26-bit limbs; products fit in 64 bits. */
struct poly {
    u32 r[5], s[4], h[5], pad[4];
    u8 buf[16];
    u32 n;
};

static void poly_init(struct poly *p, const u8 key[32])
{
    u32 t0 = le32(key), t1 = le32(key + 4), t2 = le32(key + 8), t3 = le32(key + 12);
    /* r with the clamping of RFC 8439 2.5.1 applied */
    p->r[0] = t0 & 0x3ffffff;
    p->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
    p->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    p->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    p->r[4] = (t3 >> 8) & 0x00fffff;
    for (int i = 0; i < 4; i++) {
        p->s[i] = p->r[i + 1] * 5;
        p->pad[i] = le32(key + 16 + 4 * i);
    }
    for (int i = 0; i < 5; i++)
        p->h[i] = 0;
    p->n = 0;
}

static void poly_blocks(struct poly *p, const u8 *m, size_t len, u32 hibit)
{
    const u32 M = 0x3ffffff;
    u32 r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    u32 s1 = p->s[0], s2 = p->s[1], s3 = p->s[2], s4 = p->s[3];
    u32 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    for (; len >= 16; m += 16, len -= 16) {
        u32 t0 = le32(m), t1 = le32(m + 4), t2 = le32(m + 8), t3 = le32(m + 12);
        h0 += t0 & M;
        h1 += ((t0 >> 26) | (t1 << 6)) & M;
        h2 += ((t1 >> 20) | (t2 << 12)) & M;
        h3 += ((t2 >> 14) | (t3 << 18)) & M;
        h4 += (t3 >> 8) | hibit;

        u64 d0 = (u64)h0 * r0 + (u64)h1 * s4 + (u64)h2 * s3 + (u64)h3 * s2 + (u64)h4 * s1;
        u64 d1 = (u64)h0 * r1 + (u64)h1 * r0 + (u64)h2 * s4 + (u64)h3 * s3 + (u64)h4 * s2;
        u64 d2 = (u64)h0 * r2 + (u64)h1 * r1 + (u64)h2 * r0 + (u64)h3 * s4 + (u64)h4 * s3;
        u64 d3 = (u64)h0 * r3 + (u64)h1 * r2 + (u64)h2 * r1 + (u64)h3 * r0 + (u64)h4 * s4;
        u64 d4 = (u64)h0 * r4 + (u64)h1 * r3 + (u64)h2 * r2 + (u64)h3 * r1 + (u64)h4 * r0;

        u64 c = d0 >> 26;
        h0 = (u32)d0 & M;
        d1 += c;
        c = d1 >> 26;
        h1 = (u32)d1 & M;
        d2 += c;
        c = d2 >> 26;
        h2 = (u32)d2 & M;
        d3 += c;
        c = d3 >> 26;
        h3 = (u32)d3 & M;
        d4 += c;
        c = d4 >> 26;
        h4 = (u32)d4 & M;
        h0 += (u32)c * 5;
        h1 += h0 >> 26;
        h0 &= M;
    }
    p->h[0] = h0;
    p->h[1] = h1;
    p->h[2] = h2;
    p->h[3] = h3;
    p->h[4] = h4;
}

static void poly_update(struct poly *p, const void *data, size_t len)
{
    const u8 *m = data;
    if (p->n) {
        while (len && p->n < 16) {
            p->buf[p->n++] = *m++;
            len--;
        }
        if (p->n < 16)
            return;
        poly_blocks(p, p->buf, 16, 1u << 24);
        p->n = 0;
    }
    size_t whole = len & ~(size_t)15;
    poly_blocks(p, m, whole, 1u << 24);
    m += whole;
    len -= whole;
    while (len--)
        p->buf[p->n++] = *m++;
}

static void poly_final(struct poly *p, u8 tag[16])
{
    const u32 M = 0x3ffffff;
    if (p->n) { /* last partial block: append 1, pad with zeros, no high bit */
        p->buf[p->n++] = 1;
        while (p->n < 16)
            p->buf[p->n++] = 0;
        poly_blocks(p, p->buf, 16, 0);
    }
    u32 h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= M; h2 += c;
    c = h2 >> 26; h2 &= M; h3 += c;
    c = h3 >> 26; h3 &= M; h4 += c;
    c = h4 >> 26; h4 &= M; h0 += c * 5;
    c = h0 >> 26; h0 &= M; h1 += c;

    /* g = h + 5 - 2^130; use g if it is not negative (h >= p) */
    u32 g0 = h0 + 5;
    c = g0 >> 26; g0 &= M;
    u32 g1 = h1 + c;
    c = g1 >> 26; g1 &= M;
    u32 g2 = h2 + c;
    c = g2 >> 26; g2 &= M;
    u32 g3 = h3 + c;
    c = g3 >> 26; g3 &= M;
    u32 g4 = h4 + c - (1u << 26);
    u32 mask = (g4 >> 31) - 1; /* all ones if g4 did not go negative */
    h0 = (h0 & ~mask) | (g0 & mask);
    h1 = (h1 & ~mask) | (g1 & mask);
    h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask);
    h4 = (h4 & ~mask) | (g4 & mask);

    /* h mod 2^128, plus the pad */
    u32 w0 = h0 | (h1 << 26);
    u32 w1 = (h1 >> 6) | (h2 << 20);
    u32 w2 = (h2 >> 12) | (h3 << 14);
    u32 w3 = (h3 >> 18) | (h4 << 8);
    u64 f = (u64)w0 + p->pad[0];
    put_le32(tag, (u32)f);
    f = (u64)w1 + p->pad[1] + (f >> 32);
    put_le32(tag + 4, (u32)f);
    f = (u64)w2 + p->pad[2] + (f >> 32);
    put_le32(tag + 8, (u32)f);
    f = (u64)w3 + p->pad[3] + (f >> 32);
    put_le32(tag + 12, (u32)f);
    crypto_wipe(p, sizeof(*p));
}

void poly1305(const u8 key[32], const void *msg, size_t len, u8 tag[16])
{
    struct poly p;
    poly_init(&p, key);
    poly_update(&p, msg, len);
    poly_final(&p, tag);
}

/* --------------------------------------------------------------- AEAD */
static void aead_tag(const u8 key[32], const u8 nonce[12], const void *aad, size_t alen,
                     const void *ct, size_t clen, u8 tag[16])
{
    u8 block[64], zeros[16], lens[16];
    chacha20_block(key, 0, nonce, block); /* the one-time Poly1305 key */
    struct poly p;
    poly_init(&p, block);
    memset(zeros, 0, sizeof(zeros));
    poly_update(&p, aad, alen);
    poly_update(&p, zeros, (16 - alen % 16) % 16);
    poly_update(&p, ct, clen);
    poly_update(&p, zeros, (16 - clen % 16) % 16);
    for (int i = 0; i < 8; i++) {
        lens[i] = (u8)((u64)alen >> (8 * i));
        lens[8 + i] = (u8)((u64)clen >> (8 * i));
    }
    poly_update(&p, lens, 16);
    poly_final(&p, tag);
    crypto_wipe(block, sizeof(block));
}

void aead_chacha20_poly1305_seal(const u8 key[32], const u8 nonce[12], const void *aad,
                                 size_t alen, const void *in, size_t len, void *out)
{
    chacha20_xor(key, 1, nonce, in, out, len);
    aead_tag(key, nonce, aad, alen, out, len, (u8 *)out + len);
}

bool aead_chacha20_poly1305_open(const u8 key[32], const u8 nonce[12], const void *aad,
                                 size_t alen, const void *in, size_t len, void *out)
{
    if (len < AEAD_TAG_LEN)
        return false;
    len -= AEAD_TAG_LEN;
    u8 tag[16];
    aead_tag(key, nonce, aad, alen, in, len, tag);
    if (!crypto_equal(tag, (const u8 *)in + len, 16))
        return false;
    chacha20_xor(key, 1, nonce, in, out, len);
    return true;
}
