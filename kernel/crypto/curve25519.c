/*
 * Curve25519: X25519 key exchange (RFC 7748) and Ed25519 signature
 * verification (RFC 8032).
 *
 * Field elements mod p = 2^255 - 19 are 16 signed 64-bit limbs of 16 bits
 * each (radix 2^16). Additions are left unreduced; multiplication folds
 * the high half back with 2^256 = 38 (mod p) and carries. The ladder and
 * the field code are constant time; signature verification works on
 * public data only and need not be.
 *
 * All curve constants (d, sqrt(-1), the base point) are computed from
 * their definitions at run time, so no limb tables need to be trusted.
 */
#include "crypto.h"
#include "string.h"

typedef i64 fe[16];

static void fe_zero(fe o)
{
    for (int i = 0; i < 16; i++)
        o[i] = 0;
}
static void fe_small(fe o, i64 v)
{
    fe_zero(o);
    o[0] = v & 0xffff;
    o[1] = v >> 16;
}
static void fe_copy(fe o, const fe a)
{
    for (int i = 0; i < 16; i++)
        o[i] = a[i];
}
static void fe_add(fe o, const fe a, const fe b)
{
    for (int i = 0; i < 16; i++)
        o[i] = a[i] + b[i];
}
static void fe_sub(fe o, const fe a, const fe b)
{
    for (int i = 0; i < 16; i++)
        o[i] = a[i] - b[i];
}

/* Bring every limb into [0, 2^16), folding the carry out of the top limb
 * back in with weight 38. */
static void fe_carry(fe o)
{
    for (int i = 0; i < 16; i++) {
        i64 c = o[i] >> 16; /* arithmetic shift: floor division */
        o[i] -= c * 65536;
        if (i < 15)
            o[i + 1] += c;
        else
            o[0] += 38 * c;
    }
}

static void fe_mul(fe o, const fe a, const fe b)
{
    i64 t[31];
    for (int i = 0; i < 31; i++)
        t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++)
        o[i] = t[i];
    fe_carry(o);
    fe_carry(o);
}

static void fe_sq(fe o, const fe a) { fe_mul(o, a, a); }

/* Constant-time swap of a and b when bit is 1. */
static void fe_cswap(fe a, fe b, int bit)
{
    i64 m = -(i64)bit;
    for (int i = 0; i < 16; i++) {
        i64 t = m & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

/* Canonical little-endian encoding (fully reduced mod p). */
static void fe_pack(u8 out[32], const fe a)
{
    fe t, m;
    fe_copy(t, a);
    fe_carry(t);
    fe_carry(t);
    fe_carry(t);
    /* t < 2^256 now; subtract p at most twice */
    for (int pass = 0; pass < 2; pass++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int borrow = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        fe_cswap(t, m, 1 - borrow); /* keep t - p unless it borrowed */
    }
    for (int i = 0; i < 16; i++) {
        out[2 * i] = (u8)t[i];
        out[2 * i + 1] = (u8)(t[i] >> 8);
    }
}

static void fe_unpack(fe o, const u8 in[32])
{
    for (int i = 0; i < 16; i++)
        o[i] = in[2 * i] | (i64)in[2 * i + 1] << 8;
    o[15] &= 0x7fff;
}

static bool fe_equal(const fe a, const fe b)
{
    u8 x[32], y[32];
    fe_pack(x, a);
    fe_pack(y, b);
    return crypto_equal(x, y, 32);
}

static int fe_parity(const fe a)
{
    u8 x[32];
    fe_pack(x, a);
    return x[0] & 1;
}

/* a^(p-2) = 1/a. p - 2 = 2^255 - 21: every bit set except bits 2 and 4. */
static void fe_inv(fe o, const fe a)
{
    fe c;
    fe_copy(c, a);
    for (int i = 253; i >= 0; i--) {
        fe_sq(c, c);
        if (i != 2 && i != 4)
            fe_mul(c, c, a);
    }
    fe_copy(o, c);
}

/* a^((p-5)/8) = a^(2^252 - 3): every bit set except bit 1. */
static void fe_pow2523(fe o, const fe a)
{
    fe c;
    fe_copy(c, a);
    for (int i = 250; i >= 0; i--) {
        fe_sq(c, c);
        if (i != 1)
            fe_mul(c, c, a);
    }
    fe_copy(o, c);
}

/* ------------------------------------------------------------ X25519 */
void x25519(u8 out[32], const u8 scalar[32], const u8 point[32])
{
    u8 k[32];
    memcpy(k, scalar, 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    fe x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb, a24, t;
    fe_unpack(x1, point);
    fe_small(x2, 1);
    fe_zero(z2);
    fe_copy(x3, x1);
    fe_small(z3, 1);
    fe_small(a24, 121665);
    int swap = 0;
    for (int i = 254; i >= 0; i--) {
        int bit = (k[i >> 3] >> (i & 7)) & 1;
        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_add(a, x2, z2);
        fe_sq(aa, a);
        fe_sub(b, x2, z2);
        fe_sq(bb, b);
        fe_sub(e, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(t, da, cb);
        fe_sq(x3, t);
        fe_sub(t, da, cb);
        fe_sq(t, t);
        fe_mul(z3, x1, t);
        fe_mul(x2, aa, bb);
        fe_mul(t, a24, e);
        fe_add(t, aa, t);
        fe_mul(z2, e, t);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_inv(z2, z2);
    fe_mul(x2, x2, z2);
    fe_pack(out, x2);
    crypto_wipe(k, sizeof(k));
    crypto_wipe(x2, sizeof(x2));
    crypto_wipe(z2, sizeof(z2));
    crypto_wipe(x3, sizeof(x3));
    crypto_wipe(z3, sizeof(z3));
}

void x25519_base(u8 out[32], const u8 scalar[32])
{
    u8 nine[32];
    memset(nine, 0, sizeof(nine));
    nine[0] = 9;
    x25519(out, scalar, nine);
}

/* ----------------------------------------------------------- Ed25519 */
/* Points in extended coordinates (X:Y:Z:T), x = X/Z, y = Y/Z, xy = T/Z,
 * on -x^2 + y^2 = 1 + d x^2 y^2. */
struct ge {
    fe X, Y, Z, T;
};

struct ed_consts {
    fe d, d2, sqrtm1;
};

static void ed_consts(struct ed_consts *k)
{
    fe n, den;
    fe_small(n, 121665);
    fe_small(den, 121666);
    fe_inv(den, den);
    fe_mul(n, n, den);
    fe_zero(k->d);
    fe_sub(k->d, k->d, n); /* d = -121665/121666 */
    fe_carry(k->d);
    fe_add(k->d2, k->d, k->d);
    /* sqrt(-1) = 2^((p-1)/4) = (2^((p-5)/8))^2 * 2 */
    fe two;
    fe_small(two, 2);
    fe_pow2523(k->sqrtm1, two);
    fe_sq(k->sqrtm1, k->sqrtm1);
    fe_mul(k->sqrtm1, k->sqrtm1, two);
}

static void ge_identity(struct ge *p)
{
    fe_zero(p->X);
    fe_small(p->Y, 1);
    fe_small(p->Z, 1);
    fe_zero(p->T);
}

/* p = p + q (add-2008-hwcd-3; also correct for doubling). */
static void ge_add(struct ge *p, const struct ge *q, const struct ed_consts *k)
{
    fe a, b, c, d, t, e, f, g, h;
    fe_sub(a, p->Y, p->X);
    fe_sub(t, q->Y, q->X);
    fe_mul(a, a, t);
    fe_add(b, p->X, p->Y);
    fe_add(t, q->X, q->Y);
    fe_mul(b, b, t);
    fe_mul(c, p->T, q->T);
    fe_mul(c, c, k->d2);
    fe_mul(d, p->Z, q->Z);
    fe_add(d, d, d);
    fe_sub(e, b, a);
    fe_sub(f, d, c);
    fe_add(g, d, c);
    fe_add(h, b, a);
    fe_mul(p->X, e, f);
    fe_mul(p->Y, h, g);
    fe_mul(p->Z, g, f);
    fe_mul(p->T, e, h);
}

/* o = s * p, s a 256-bit little-endian scalar (variable time). */
static void ge_scalarmult(struct ge *o, const struct ge *p, const u8 s[32],
                          const struct ed_consts *k)
{
    struct ge q;
    ge_identity(&q);
    for (int i = 255; i >= 0; i--) {
        ge_add(&q, &q, k);
        if ((s[i >> 3] >> (i & 7)) & 1)
            ge_add(&q, p, k);
    }
    *o = q;
}

static void ge_pack(u8 out[32], const struct ge *p)
{
    fe zi, x, y;
    fe_inv(zi, p->Z);
    fe_mul(x, p->X, zi);
    fe_mul(y, p->Y, zi);
    fe_pack(out, y);
    out[31] ^= (u8)(fe_parity(x) << 7);
}

/* Decode a point (RFC 8032 5.1.3). Rejects non-canonical y and points not
 * on the curve. */
static bool ge_unpack(struct ge *p, const u8 in[32], const struct ed_consts *k)
{
    u8 check[32];
    int sign = in[31] >> 7;
    fe_unpack(p->Y, in);
    fe_pack(check, p->Y); /* y must be < p */
    for (int i = 0; i < 31; i++)
        if (check[i] != in[i])
            return false;
    if (check[31] != (in[31] & 0x7f))
        return false;
    fe_small(p->Z, 1);

    fe u, v, v3, x, t, one;
    fe_small(one, 1);
    fe_sq(u, p->Y);
    fe_mul(v, u, k->d);
    fe_sub(u, u, one);  /* u = y^2 - 1 */
    fe_add(v, v, one);  /* v = d y^2 + 1 */
    fe_sq(v3, v);
    fe_mul(v3, v3, v);  /* v^3 */
    fe_sq(x, v3);
    fe_mul(x, x, v);
    fe_mul(x, x, u);    /* u v^7 */
    fe_pow2523(x, x);
    fe_mul(x, x, v3);
    fe_mul(x, x, u);    /* x = u v^3 (u v^7)^((p-5)/8) */

    fe_sq(t, x);
    fe_mul(t, t, v);    /* v x^2 */
    if (!fe_equal(t, u)) {
        fe neg;
        fe_zero(neg);
        fe_sub(neg, neg, u);
        if (!fe_equal(t, neg))
            return false;
        fe_mul(x, x, k->sqrtm1);
    }
    fe zero;
    fe_zero(zero);
    if (fe_equal(x, zero) && sign)
        return false;
    if (fe_parity(x) != sign) {
        fe_sub(x, zero, x);
    }
    fe_copy(p->X, x);
    fe_mul(p->T, p->X, p->Y);
    return true;
}

/* ------------------------------------------------- scalars mod L */
/* L = 2^252 + 27742317777372353535851937790883648493, little-endian. */
static const u8 L_BYTES[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
};

/* a >= b for 33-byte little-endian numbers */
static bool ge_bytes(const u8 *a, const u8 *b, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] > b[i];
    return true;
}

/* out = (64-byte little-endian number) mod L, by shifting in one bit at a
 * time (r = 2r + bit; subtract L when r >= L). Slow but plainly correct,
 * and only run once per signature. */
static void reduce_mod_l(u8 out[32], const u8 in[64])
{
    u8 r[33], l[33];
    memset(r, 0, sizeof(r));
    memcpy(l, L_BYTES, 32);
    l[32] = 0;
    for (int bit = 511; bit >= 0; bit--) {
        int carry = (in[bit >> 3] >> (bit & 7)) & 1;
        for (int i = 0; i < 33; i++) {
            int v = (r[i] << 1) | carry;
            carry = v >> 8;
            r[i] = (u8)v;
        }
        if (ge_bytes(r, l, 33)) {
            int borrow = 0;
            for (int i = 0; i < 33; i++) {
                int v = r[i] - l[i] - borrow;
                borrow = v < 0;
                r[i] = (u8)v;
            }
        }
    }
    memcpy(out, r, 32);
}

bool ed25519_verify(const u8 sig[64], const void *msg, size_t len, const u8 pub[32])
{
    struct ed_consts k;
    ed_consts(&k);

    /* S must be canonical (S < L), else signatures are malleable */
    u8 s33[33], l33[33];
    memcpy(s33, sig + 32, 32);
    s33[32] = 0;
    memcpy(l33, L_BYTES, 32);
    l33[32] = 0;
    if (ge_bytes(s33, l33, 33))
        return false;

    struct ge a, base;
    if (!ge_unpack(&a, pub, &k))
        return false;
    /* The base point: y = 4/5, x even ("5866...66"). */
    u8 benc[32];
    memset(benc, 0x66, 32);
    benc[0] = 0x58;
    if (!ge_unpack(&base, benc, &k))
        return false;

    /* h = SHA-512(R || A || M) mod L */
    u8 digest[64], h[32];
    struct sha512 c;
    sha512_init(&c);
    sha512_update(&c, sig, 32);
    sha512_update(&c, pub, 32);
    sha512_update(&c, msg, len);
    sha512_final(&c, digest);
    reduce_mod_l(h, digest);

    /* check [S]B - [h]A == R, comparing encodings */
    fe_sub(a.X, (fe){ 0 }, a.X);
    fe_sub(a.T, (fe){ 0 }, a.T);
    struct ge sb, ha;
    ge_scalarmult(&sb, &base, sig + 32, &k);
    ge_scalarmult(&ha, &a, h, &k);
    ge_add(&sb, &ha, &k);
    u8 r[32];
    ge_pack(r, &sb);
    return crypto_equal(r, sig, 32);
}
