/* SHA-256 and SHA-512 (FIPS 180-4), HMAC-SHA-256 (RFC 2104), HKDF (RFC 5869). */
#include "crypto.h"
#include "string.h"

static inline u32 ror32(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
static inline u64 ror64(u64 x, int n) { return (x >> n) | (x << (64 - n)); }

static inline u32 be32(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static inline u64 be64(const u8 *p) { return (u64)be32(p) << 32 | be32(p + 4); }
static inline void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}
static inline void put_be64(u8 *p, u64 v)
{
    put_be32(p, (u32)(v >> 32));
    put_be32(p + 4, (u32)v);
}

/* ------------------------------------------------------------ SHA-256 */
/* First 32 bits of the fractional parts of the cube roots of the first 64
 * primes (and, for the initial hash, square roots of the first 8). */
static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
static const u32 H256[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

static void sha256_block(u32 h[8], const u8 *p)
{
    u32 w[64];
    for (int i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        u32 s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = k + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        u32 t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

void sha256_init(struct sha256 *c)
{
    for (int i = 0; i < 8; i++)
        c->h[i] = H256[i];
    c->len = 0;
    c->n = 0;
}

void sha256_update(struct sha256 *c, const void *data, size_t len)
{
    const u8 *p = data;
    c->len += len;
    if (c->n) {
        while (len && c->n < 64) {
            c->buf[c->n++] = *p++;
            len--;
        }
        if (c->n < 64)
            return;
        sha256_block(c->h, c->buf);
        c->n = 0;
    }
    for (; len >= 64; p += 64, len -= 64)
        sha256_block(c->h, p);
    while (len--)
        c->buf[c->n++] = *p++;
}

void sha256_final(struct sha256 *c, u8 out[SHA256_LEN])
{
    u64 bits = c->len * 8;
    c->buf[c->n++] = 0x80;
    if (c->n > 56) {
        while (c->n < 64)
            c->buf[c->n++] = 0;
        sha256_block(c->h, c->buf);
        c->n = 0;
    }
    while (c->n < 56)
        c->buf[c->n++] = 0;
    put_be64(c->buf + 56, bits);
    sha256_block(c->h, c->buf);
    for (int i = 0; i < 8; i++)
        put_be32(out + 4 * i, c->h[i]);
    crypto_wipe(c, sizeof(*c));
}

void sha256(const void *data, size_t len, u8 out[SHA256_LEN])
{
    struct sha256 c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* ------------------------------------------------------------ SHA-512 */
static const u64 K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};
static const u64 H512[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

static void sha512_block(u64 h[8], const u8 *p)
{
    u64 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = be64(p + 8 * i);
    for (int i = 16; i < 80; i++) {
        u64 s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        u64 s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u64 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 80; i++) {
        u64 t1 = k + (ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        u64 t2 = (ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += k;
}

void sha512_init(struct sha512 *c)
{
    for (int i = 0; i < 8; i++)
        c->h[i] = H512[i];
    c->len = 0;
    c->n = 0;
}

void sha512_update(struct sha512 *c, const void *data, size_t len)
{
    const u8 *p = data;
    c->len += len;
    if (c->n) {
        while (len && c->n < 128) {
            c->buf[c->n++] = *p++;
            len--;
        }
        if (c->n < 128)
            return;
        sha512_block(c->h, c->buf);
        c->n = 0;
    }
    for (; len >= 128; p += 128, len -= 128)
        sha512_block(c->h, p);
    while (len--)
        c->buf[c->n++] = *p++;
}

void sha512_final(struct sha512 *c, u8 out[SHA512_LEN])
{
    u64 bits = c->len * 8;
    c->buf[c->n++] = 0x80;
    if (c->n > 112) {
        while (c->n < 128)
            c->buf[c->n++] = 0;
        sha512_block(c->h, c->buf);
        c->n = 0;
    }
    while (c->n < 120) /* the length is 128 bits; messages here are < 2^64 bits */
        c->buf[c->n++] = 0;
    put_be64(c->buf + 120, bits);
    sha512_block(c->h, c->buf);
    for (int i = 0; i < 8; i++)
        put_be64(out + 8 * i, c->h[i]);
    crypto_wipe(c, sizeof(*c));
}

/* --------------------------------------------------------------- HMAC */
void hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen,
                 u8 out[SHA256_LEN])
{
    u8 k[64], pad[64], inner[SHA256_LEN];
    memset(k, 0, sizeof(k));
    if (klen > 64)
        sha256(key, klen, k);
    else
        memcpy(k, key, klen);
    struct sha256 c;
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, msg, mlen);
    sha256_final(&c, inner);
    for (int i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, inner, sizeof(inner));
    sha256_final(&c, out);
    crypto_wipe(k, sizeof(k));
    crypto_wipe(pad, sizeof(pad));
    crypto_wipe(inner, sizeof(inner));
}

/* ------------------------------------------------------------- HKDF */
void hkdf_extract(const void *salt, size_t slen, const void *ikm, size_t ilen,
                  u8 prk[SHA256_LEN])
{
    u8 zero[SHA256_LEN];
    if (!salt || !slen) {
        memset(zero, 0, sizeof(zero));
        salt = zero;
        slen = sizeof(zero);
    }
    hmac_sha256(salt, slen, ikm, ilen, prk);
}

void hkdf_expand(const u8 prk[SHA256_LEN], const void *info, size_t ilen, void *out,
                 size_t out_len)
{
    /* T(i) = HMAC(PRK, T(i-1) | info | i). info is at most a few hundred
     * bytes here (TLS labels), so one buffer holds the HMAC input. */
    u8 t[SHA256_LEN], msg[SHA256_LEN + 512 + 1];
    size_t tlen = 0, done = 0;
    if (ilen > 512)
        ilen = 512;
    for (u8 i = 1; done < out_len; i++) {
        memcpy(msg, t, tlen);
        memcpy(msg + tlen, info, ilen);
        msg[tlen + ilen] = i;
        hmac_sha256(prk, SHA256_LEN, msg, tlen + ilen + 1, t);
        tlen = SHA256_LEN;
        size_t n = out_len - done < SHA256_LEN ? out_len - done : SHA256_LEN;
        memcpy((u8 *)out + done, t, n);
        done += n;
    }
    crypto_wipe(t, sizeof(t));
    crypto_wipe(msg, sizeof(msg));
}

/* ------------------------------------------------------------ helpers */
bool crypto_equal(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    u8 d = 0;
    for (size_t i = 0; i < n; i++)
        d |= x[i] ^ y[i];
    return d == 0;
}

void crypto_wipe(void *p, size_t n)
{
    volatile u8 *v = p;
    while (n--)
        *v++ = 0;
}

void hex_encode(const void *in, size_t n, char *out)
{
    const u8 *p = in;
    for (size_t i = 0; i < n; i++) {
        u8 hi = p[i] >> 4, lo = p[i] & 15;
        out[2 * i] = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
        out[2 * i + 1] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
    }
    out[2 * n] = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool hex_decode(const char *in, size_t inlen, void *out, size_t n)
{
    if (inlen != 2 * n)
        return false;
    u8 *o = out;
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(in[2 * i]), lo = hexval(in[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        o[i] = (u8)(hi << 4 | lo);
    }
    return true;
}
