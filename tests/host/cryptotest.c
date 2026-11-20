/*
 * Host test for kernel/crypto: SHA-256/512, HMAC, HKDF, Poly1305,
 * ChaCha20-Poly1305, X25519 and Ed25519 against results computed with
 * OpenSSL (tests/host/crypto_vectors.h, from tools/gen_crypto_vectors.py),
 * plus RFC test vectors and tampering checks.
 *
 *   make test-crypto
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "crypto_vectors.h"

static int failures, checks;

static size_t unhex(const char *s, u8 **out)
{
    size_t n = strlen(s) / 2;
    *out = malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        (*out)[i] = (u8)v;
    }
    return n;
}

static void expect(const char *what, int i, const void *got, const char *want_hex)
{
    u8 *want;
    size_t n = unhex(want_hex, &want);
    checks++;
    if (memcmp(got, want, n)) {
        failures++;
        printf("FAIL %s #%d\n  want %s\n  got  ", what, i, want_hex);
        for (size_t j = 0; j < n; j++)
            printf("%02x", ((const u8 *)got)[j]);
        printf("\n");
    }
    free(want);
}

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL %s\n", what);
    }
}

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

int main(void)
{
    u8 out[256];

    for (int i = 0; i < N(hash_vec); i++) {
        u8 *m;
        size_t n = unhex(hash_vec[i].msg, &m);
        sha256(m, n, out);
        expect("sha256", i, out, hash_vec[i].sha256);
        /* the same in uneven pieces */
        struct sha256 c;
        sha256_init(&c);
        for (size_t off = 0; off < n;) {
            size_t k = (off * 7 + 1) % 70 + 1;
            if (k > n - off)
                k = n - off;
            sha256_update(&c, m + off, k);
            off += k;
        }
        sha256_final(&c, out);
        expect("sha256 pieces", i, out, hash_vec[i].sha256);
        struct sha512 c5;
        sha512_init(&c5);
        for (size_t off = 0; off < n;) {
            size_t k = (off * 5 + 3) % 150 + 1;
            if (k > n - off)
                k = n - off;
            sha512_update(&c5, m + off, k);
            off += k;
        }
        sha512_final(&c5, out);
        expect("sha512", i, out, hash_vec[i].sha512);
        free(m);
    }

    for (int i = 0; i < N(hmac_vec); i++) {
        u8 *k, *m;
        size_t kl = unhex(hmac_vec[i].key, &k), ml = unhex(hmac_vec[i].msg, &m);
        hmac_sha256(k, kl, m, ml, out);
        expect("hmac", i, out, hmac_vec[i].mac);
        free(k);
        free(m);
    }

    for (int i = 0; i < N(hkdf_vec); i++) {
        u8 *s, *ikm, *info, prk[32], *okm;
        size_t sl = unhex(hkdf_vec[i].salt, &s), il = unhex(hkdf_vec[i].ikm, &ikm);
        size_t nl = unhex(hkdf_vec[i].info, &info), ol = strlen(hkdf_vec[i].okm) / 2;
        hkdf_extract(sl ? s : NULL, sl, ikm, il, prk);
        expect("hkdf extract", i, prk, hkdf_vec[i].prk);
        okm = malloc(ol);
        hkdf_expand(prk, info, nl, okm, ol);
        expect("hkdf expand", i, okm, hkdf_vec[i].okm);
        free(s);
        free(ikm);
        free(info);
        free(okm);
    }

    for (int i = 0; i < N(poly_vec); i++) {
        u8 *k, *m;
        unhex(poly_vec[i].key, &k);
        size_t ml = unhex(poly_vec[i].msg, &m);
        poly1305(k, m, ml, out);
        expect("poly1305", i, out, poly_vec[i].tag);
        free(k);
        free(m);
    }

    /* RFC 8439 2.3.2: the ChaCha20 block function */
    {
        u8 key[32], nonce[12] = { 0, 0, 0, 9, 0, 0, 0, 0x4a, 0, 0, 0, 0 };
        for (int i = 0; i < 32; i++)
            key[i] = (u8)i;
        chacha20_block(key, 1, nonce, out);
        expect("chacha20 rfc8439 2.3.2", 0, out,
               "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
               "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e");
    }

    for (int i = 0; i < N(aead_vec); i++) {
        u8 *k, *nonce, *aad, *pt, *ct;
        unhex(aead_vec[i].key, &k);
        unhex(aead_vec[i].nonce, &nonce);
        size_t al = unhex(aead_vec[i].aad, &aad), pl = unhex(aead_vec[i].pt, &pt);
        size_t cl = unhex(aead_vec[i].ct, &ct);
        u8 *buf = malloc(pl + 16), *back = malloc(pl + 1);
        aead_chacha20_poly1305_seal(k, nonce, aad, al, pt, pl, buf);
        expect("aead seal", i, buf, aead_vec[i].ct);
        check("aead open", aead_chacha20_poly1305_open(k, nonce, aad, al, ct, cl, back) &&
                               !memcmp(back, pt, pl));
        ct[cl / 2] ^= 1;
        check("aead rejects a changed byte", !aead_chacha20_poly1305_open(k, nonce, aad, al, ct, cl, back));
        free(k), free(nonce), free(aad), free(pt), free(ct), free(buf), free(back);
    }

    /* RFC 7748 5.2 */
    {
        u8 *s, *u;
        unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", &s);
        unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", &u);
        x25519(out, s, u);
        expect("x25519 rfc7748", 0, out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
        free(s);
        free(u);
    }
    for (int i = 0; i < N(x25519_vec); i++) {
        u8 *a, *pa, *bb, pb[32], s1[32], s2[32];
        unhex(x25519_vec[i].a, &a);
        unhex(x25519_vec[i].pa, &pa);
        unhex(x25519_vec[i].b, &bb);
        x25519_base(out, a);
        expect("x25519 public", i, out, x25519_vec[i].pa);
        x25519_base(pb, bb);
        x25519(s1, a, pb);
        x25519(s2, bb, pa);
        expect("x25519 shared a", i, s1, x25519_vec[i].shared);
        expect("x25519 shared b", i, s2, x25519_vec[i].shared);
        free(a), free(pa), free(bb);
    }

    for (int i = 0; i < N(ed25519_vec); i++) {
        u8 *pub, *msg, *sig;
        unhex(ed25519_vec[i].pub, &pub);
        size_t ml = unhex(ed25519_vec[i].msg, &msg);
        unhex(ed25519_vec[i].sig, &sig);
        check("ed25519 verify", ed25519_verify(sig, msg, ml, pub));
        sig[5] ^= 0x10;
        check("ed25519 rejects a changed R", !ed25519_verify(sig, msg, ml, pub));
        sig[5] ^= 0x10;
        sig[40] ^= 1;
        check("ed25519 rejects a changed S", !ed25519_verify(sig, msg, ml, pub));
        sig[40] ^= 1;
        if (ml) {
            msg[0] ^= 0x80;
            check("ed25519 rejects a changed message", !ed25519_verify(sig, msg, ml, pub));
            msg[0] ^= 0x80;
        }
        pub[3] ^= 4;
        check("ed25519 rejects another key", !ed25519_verify(sig, msg, ml, pub));
        pub[3] ^= 4;
        /* S + L is the same signature mathematically but must be refused */
        static const u8 L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                  0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0x10 };
        int carry = 0;
        for (int j = 0; j < 32; j++) {
            int v = sig[32 + j] + L[j] + carry;
            sig[32 + j] = (u8)v;
            carry = v >> 8;
        }
        check("ed25519 rejects non-canonical S", !ed25519_verify(sig, msg, ml, pub));
        free(pub), free(msg), free(sig);
    }

    /* RFC 8032 7.1, test 1 (empty message) */
    {
        u8 *pub, *sig;
        unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", &pub);
        unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
              &sig);
        check("ed25519 rfc8032 test 1", ed25519_verify(sig, "", 0, pub));
        free(pub);
        free(sig);
    }

    char hx[9];
    hex_encode("\x01\xab\xff\x00", 4, hx);
    check("hex_encode", !strcmp(hx, "01abff00"));
    u8 bin[4];
    check("hex_decode", hex_decode("01ABff00", 8, bin, 4) && !memcmp(bin, "\x01\xab\xff\x00", 4));
    check("hex_decode rejects junk", !hex_decode("01ABfg00", 8, bin, 4));

    printf("cryptotest: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}
