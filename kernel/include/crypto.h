/*
 * Cryptography for TLS 1.3 and signed updates (kernel/crypto).
 *
 *   SHA-256, SHA-512        FIPS 180-4
 *   HMAC-SHA-256, HKDF      RFC 2104, RFC 5869
 *   ChaCha20-Poly1305       RFC 8439
 *   X25519                  RFC 7748
 *   Ed25519 (verify)        RFC 8032
 *
 * Written for Sonar; checked against the RFC test vectors by
 * tests/host/cryptotest.c (make test-crypto). The code has no initialised
 * pointers in data, so the UEFI loader stub can call it before the kernel
 * has relocated itself.
 */
#pragma once
#include "types.h"

/* ------------------------------------------------------------- SHA-2 */
#define SHA256_LEN 32
#define SHA512_LEN 64

struct sha256 {
    u32 h[8];
    u64 len;
    u8 buf[64];
    u32 n;
};
void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t len);
void sha256_final(struct sha256 *c, u8 out[SHA256_LEN]);
void sha256(const void *data, size_t len, u8 out[SHA256_LEN]);

struct sha512 {
    u64 h[8];
    u64 len;
    u8 buf[128];
    u32 n;
};
void sha512_init(struct sha512 *c);
void sha512_update(struct sha512 *c, const void *data, size_t len);
void sha512_final(struct sha512 *c, u8 out[SHA512_LEN]);

/* ------------------------------------------------------ HMAC and HKDF */
void hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen,
                 u8 out[SHA256_LEN]);
void hkdf_extract(const void *salt, size_t slen, const void *ikm, size_t ilen,
                  u8 prk[SHA256_LEN]);
/* out_len <= 255 * 32 */
void hkdf_expand(const u8 prk[SHA256_LEN], const void *info, size_t ilen, void *out,
                 size_t out_len);

/* ------------------------------------------------- ChaCha20-Poly1305 */
void chacha20_xor(const u8 key[32], u32 counter, const u8 nonce[12], const void *in,
                  void *out, size_t len);
void chacha20_block(const u8 key[32], u32 counter, const u8 nonce[12], u8 out[64]);
void poly1305(const u8 key[32], const void *msg, size_t len, u8 tag[16]);

#define AEAD_TAG_LEN 16
/* out = ciphertext followed by the 16-byte tag (len + 16 bytes). */
void aead_chacha20_poly1305_seal(const u8 key[32], const u8 nonce[12], const void *aad,
                                 size_t alen, const void *in, size_t len, void *out);
/* in = ciphertext + tag (len includes the tag). Writes len - 16 bytes.
 * Returns false (and writes nothing useful) if the tag does not match. */
bool aead_chacha20_poly1305_open(const u8 key[32], const u8 nonce[12], const void *aad,
                                 size_t alen, const void *in, size_t len, void *out);

/* ------------------------------------------------------ Curve25519 */
void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]);
void x25519_base(u8 out[32], const u8 scalar[32]); /* scalar * 9 */
bool ed25519_verify(const u8 sig[64], const void *msg, size_t len, const u8 pub[32]);

/* ------------------------------------------------------------ helpers */
bool crypto_equal(const void *a, const void *b, size_t n); /* constant time */
void crypto_wipe(void *p, size_t n);
/* Lower-case hex, NUL-terminated: out needs 2 * n + 1 bytes. */
void hex_encode(const void *in, size_t n, char *out);
/* Exactly 2 * n hex digits; false otherwise. */
bool hex_decode(const char *in, size_t inlen, void *out, size_t n);

/* Random bytes for keys (kernel/crypto/rng.c): a ChaCha20 generator
 * seeded from the virtio entropy device, the CPU's RNDR instruction when
 * it has one, and timer jitter. Kernel only (not the loader stub). */
void crypto_random(void *out, size_t len);
