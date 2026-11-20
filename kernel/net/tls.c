/*
 * TLS 1.3 client (RFC 8446), just enough to download from GitHub.
 *
 *   cipher suite   TLS_CHACHA20_POLY1305_SHA256 only (constant time in
 *                  software; offered by github.com, api.github.com and
 *                  GitHub's release download hosts)
 *   key exchange   X25519 only (no HelloRetryRequest)
 *   no PSK/resumption, no 0-RTT, no client certificates
 *
 * THE SERVER IS NOT AUTHENTICATED. The certificate chain is received and
 * hashed into the transcript but not checked against any root, and the
 * CertificateVerify signature is not checked. So TLS here protects
 * against passive eavesdropping only; an active attacker on the path can
 * impersonate any server. That is acceptable for its one user, the
 * updater, because every update is an Ed25519-signed manifest plus a
 * SHA-256 of the kernel, checked against a key compiled into the kernel
 * (kernel/update): a forged server can withhold an update but cannot make
 * Sonar install anything. It is NOT acceptable for secrets: a GitHub
 * token sent over this connection (update.token) can be stolen by such an
 * attacker. docs/updates.md explains the trade-off.
 */
#include "crypto.h"
#include "kernel.h"
#include "mm.h"
#include "net.h"
#include "string.h"

#define REC_MAX (16384 + 256) /* largest ciphertext record body */
#define HS_MAX (64 * 1024)    /* largest handshake message accepted */

enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APPDATA = 23 };
enum {
    HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4,
    HS_ENCRYPTED_EXTENSIONS = 8, HS_CERTIFICATE = 11, HS_CERTIFICATE_REQUEST = 13,
    HS_CERTIFICATE_VERIFY = 15, HS_FINISHED = 20, HS_KEY_UPDATE = 24,
};
#define SUITE_CHACHA20_POLY1305_SHA256 0x1303
#define GROUP_X25519 0x001d

struct tls_conn {
    struct tcp_conn *tcp;
    u32 timeout_ms;
    /* bytes received from TCP, not yet used */
    u8 in[8192];
    size_t in_pos, in_len;
    /* the current record (decrypted in place) */
    u8 rec[5 + REC_MAX];
    size_t plain_pos, plain_len; /* application data within rec */
    /* handshake bytes not yet parsed into messages */
    u8 *hs;
    size_t hs_len, hs_used; /* hs_used: bytes handed out by next_handshake */
    /* traffic keys */
    bool rcrypt, wcrypt;
    u8 rsecret[32], rkey[32], riv[12];
    u8 wsecret[32], wkey[32], wiv[12];
    u64 rseq, wseq;
    bool eof;
    char err[120];
};

static bool fail(struct tls_conn *t, const char *fmt, ...) __printf(2, 3);
static bool fail(struct tls_conn *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->err, sizeof(t->err), fmt, ap);
    va_end(ap);
    return false;
}

const char *tls_error(struct tls_conn *t) { return t->err[0] ? t->err : "TLS error"; }

/* ------------------------------------------------------------ key schedule */
static void expand_label(const u8 secret[32], const char *label, const void *ctx, size_t clen,
                         void *out, size_t olen)
{
    u8 info[2 + 1 + 6 + 32 + 1 + 64];
    size_t ll = strlen(label), n = 0;
    info[n++] = (u8)(olen >> 8);
    info[n++] = (u8)olen;
    info[n++] = (u8)(6 + ll);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, ll);
    n += ll;
    info[n++] = (u8)clen;
    memcpy(info + n, ctx, clen);
    n += clen;
    hkdf_expand(secret, info, n, out, olen);
}

static void derive_secret(const u8 secret[32], const char *label, const u8 hash[32], u8 out[32])
{
    expand_label(secret, label, hash, 32, out, 32);
}

static void transcript_hash(const struct sha256 *tr, u8 out[32])
{
    struct sha256 c = *tr;
    sha256_final(&c, out);
}

static void set_keys(const u8 secret[32], u8 saved[32], u8 key[32], u8 iv[12], u64 *seq)
{
    memcpy(saved, secret, 32);
    expand_label(secret, "key", NULL, 0, key, 32);
    expand_label(secret, "iv", NULL, 0, iv, 12);
    *seq = 0;
}

static void nonce_for(const u8 iv[12], u64 seq, u8 nonce[12])
{
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[11 - i] ^= (u8)(seq >> (8 * i));
}

/* ------------------------------------------------------------- transport */
static bool read_exact(struct tls_conn *t, u8 *dst, size_t n)
{
    while (n) {
        if (t->in_pos == t->in_len) {
            ssize_t r = tcp_recv(t->tcp, t->in, sizeof(t->in), t->timeout_ms);
            if (r <= 0)
                return fail(t, "%s", r == 0 ? "the server closed the connection"
                                            : net_strerror((int)r));
            t->in_pos = 0;
            t->in_len = (size_t)r;
        }
        size_t k = t->in_len - t->in_pos < n ? t->in_len - t->in_pos : n;
        memcpy(dst, t->in + t->in_pos, k);
        t->in_pos += k;
        dst += k;
        n -= k;
    }
    return true;
}

static bool send_all(struct tls_conn *t, const u8 *p, size_t n)
{
    while (n) {
        ssize_t w = tcp_send(t->tcp, p, n);
        if (w <= 0)
            return fail(t, "send: %s", net_strerror(w < 0 ? (int)w : NET_ECLOSED));
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* Send one record of the given content type (encrypted once keys are set). */
static bool write_record(struct tls_conn *t, u8 type, const void *data, size_t len)
{
    static u8 out[5 + 16384 + 1 + AEAD_TAG_LEN]; /* only CPU 0 uses TLS */
    if (len > 16384)
        return fail(t, "record too large");
    if (!t->wcrypt) {
        out[0] = type;
        out[1] = 3;
        out[2] = type == CT_HANDSHAKE ? 1 : 3; /* ClientHello: legacy 0x0301 */
        out[3] = (u8)(len >> 8);
        out[4] = (u8)len;
        memcpy(out + 5, data, len);
        return send_all(t, out, 5 + len);
    }
    size_t inner = len + 1, clen = inner + AEAD_TAG_LEN;
    out[0] = CT_APPDATA;
    out[1] = 3;
    out[2] = 3;
    out[3] = (u8)(clen >> 8);
    out[4] = (u8)clen;
    u8 *pt = out + 5;
    memcpy(pt, data, len);
    pt[len] = type;
    u8 nonce[12];
    nonce_for(t->wiv, t->wseq++, nonce);
    aead_chacha20_poly1305_seal(t->wkey, nonce, out, 5, pt, inner, pt);
    return send_all(t, out, 5 + clen);
}

static const char *alert_name(u8 d)
{
    switch (d) {
    case 0: return "close_notify";
    case 10: return "unexpected_message";
    case 20: return "bad_record_mac";
    case 40: return "handshake_failure";
    case 42: return "bad_certificate";
    case 47: return "illegal_parameter";
    case 50: return "decode_error";
    case 51: return "decrypt_error";
    case 70: return "protocol_version";
    case 71: return "insufficient_security";
    case 80: return "internal_error";
    case 109: return "missing_extension";
    case 110: return "unsupported_extension";
    case 112: return "unrecognized_name";
    case 120: return "no_application_protocol";
    }
    return "alert";
}

/* Read one record; *data and *len are its (decrypted) content. Skips the
 * compatibility ChangeCipherSpec records servers may send. */
static bool read_record(struct tls_conn *t, u8 *type, u8 **data, size_t *len)
{
    for (;;) {
        u8 *h = t->rec;
        if (!read_exact(t, h, 5))
            return false;
        size_t n = (size_t)h[3] << 8 | h[4];
        if (h[1] != 3 || n > REC_MAX)
            return fail(t, "not a TLS record (is the server speaking TLS?)");
        if (!read_exact(t, h + 5, n))
            return false;
        u8 *body = h + 5;
        if (h[0] == CT_CCS) {
            if (n == 1 && body[0] == 1)
                continue;
            return fail(t, "unexpected change_cipher_spec");
        }
        if (!t->rcrypt) {
            *type = h[0];
            *data = body;
            *len = n;
            return true;
        }
        if (h[0] != CT_APPDATA)
            return fail(t, "unencrypted record of type %u after the handshake keys", h[0]);
        u8 nonce[12];
        nonce_for(t->riv, t->rseq++, nonce);
        if (!aead_chacha20_poly1305_open(t->rkey, nonce, h, 5, body, n, body))
            return fail(t, "record failed authentication (bad_record_mac)");
        size_t m = n - AEAD_TAG_LEN;
        while (m && !body[m - 1]) /* strip padding; the last non-zero byte is the type */
            m--;
        if (!m)
            return fail(t, "record without a content type");
        *type = body[m - 1];
        *data = body;
        *len = m - 1;
        return true;
    }
}

static bool handle_alert(struct tls_conn *t, const u8 *d, size_t n)
{
    if (n >= 2 && d[1] == 0) {
        t->eof = true;
        return true;
    }
    return fail(t, "the server sent alert %u (%s)", n >= 2 ? d[1] : 0,
                alert_name(n >= 2 ? d[1] : 0));
}

/* Next complete handshake message (header included) into *msg and *mlen. The
 * message stays valid until the next call. */
static bool next_handshake(struct tls_conn *t, u8 **msg, size_t *mlen)
{
    if (t->hs_used) {
        memmove(t->hs, t->hs + t->hs_used, t->hs_len - t->hs_used);
        t->hs_len -= t->hs_used;
        t->hs_used = 0;
    }
    for (;;) {
        if (t->hs_len >= 4) {
            size_t body = (size_t)t->hs[1] << 16 | (size_t)t->hs[2] << 8 | t->hs[3];
            if (body + 4 > HS_MAX)
                return fail(t, "handshake message too large (%lu bytes)", (unsigned long)body);
            if (t->hs_len >= body + 4) {
                *msg = t->hs;
                *mlen = body + 4;
                t->hs_used = body + 4;
                return true;
            }
        }
        u8 type, *data;
        size_t n;
        if (!read_record(t, &type, &data, &n))
            return false;
        if (type == CT_ALERT) {
            handle_alert(t, data, n);
            if (t->eof)
                return fail(t, "the server closed the connection during the handshake");
            return false;
        }
        if (type != CT_HANDSHAKE)
            return fail(t, "unexpected record type %u during the handshake", type);
        if (t->hs_len + n > HS_MAX)
            return fail(t, "handshake too large");
        memcpy(t->hs + t->hs_len, data, n);
        t->hs_len += n;
    }
}

/* ------------------------------------------------------------- parsing */
struct rd {
    const u8 *p, *end;
    bool bad;
};
static u32 rd_n(struct rd *r, int bytes)
{
    if (r->end - r->p < bytes) {
        r->bad = true;
        r->p = r->end;
        return 0;
    }
    u32 v = 0;
    for (int i = 0; i < bytes; i++)
        v = v << 8 | *r->p++;
    return v;
}
static const u8 *rd_bytes(struct rd *r, size_t n)
{
    if ((size_t)(r->end - r->p) < n) {
        r->bad = true;
        r->p = r->end;
        return NULL;
    }
    const u8 *q = r->p;
    r->p += n;
    return q;
}

/* "HelloRetryRequest" hashed with SHA-256 (RFC 8446 4.1.3) */
static const u8 HRR_RANDOM[32] = {
    0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
    0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c,
};

static bool parse_server_hello(struct tls_conn *t, const u8 *msg, size_t len, u8 server_pub[32])
{
    struct rd r = { msg + 4, msg + len, false };
    u32 ver = rd_n(&r, 2);
    const u8 *random = rd_bytes(&r, 32);
    u32 sid_len = rd_n(&r, 1);
    rd_bytes(&r, sid_len);
    u32 suite = rd_n(&r, 2);
    u32 comp = rd_n(&r, 1);
    if (r.bad || ver != 0x0303)
        return fail(t, "malformed ServerHello");
    if (!memcmp(random, HRR_RANDOM, 32))
        return fail(t, "the server asked for a key exchange other than X25519 (HelloRetryRequest)");
    if (suite != SUITE_CHACHA20_POLY1305_SHA256 || comp != 0)
        return fail(t, "the server chose cipher suite %#x (only ChaCha20-Poly1305 is supported)", suite);
    u32 ext_len = rd_n(&r, 2);
    struct rd e = { r.p, r.p + ext_len, false };
    if (ext_len > (size_t)(r.end - r.p))
        return fail(t, "malformed ServerHello extensions");
    bool tls13 = false, share = false;
    while (e.p < e.end && !e.bad) {
        u32 type = rd_n(&e, 2), n = rd_n(&e, 2);
        const u8 *d = rd_bytes(&e, n);
        if (e.bad)
            break;
        if (type == 43 && n == 2 && d[0] == 3 && d[1] == 4)
            tls13 = true;
        if (type == 51 && n == 36 && d[0] == 0 && d[1] == 0x1d && d[2] == 0 && d[3] == 32) {
            memcpy(server_pub, d + 4, 32);
            share = true;
        }
    }
    if (!tls13)
        return fail(t, "the server does not speak TLS 1.3");
    if (!share)
        return fail(t, "the server sent no X25519 key share");
    return true;
}

/* ------------------------------------------------------------- ClientHello */
static size_t put16(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
    return 2;
}

static size_t build_client_hello(u8 *out, const char *host, const u8 pub[32])
{
    u8 *p = out + 4; /* handshake header filled in last */
    p += put16(p, 0x0303);
    crypto_random(p, 32);
    p += 32;
    *p++ = 0; /* legacy_session_id: none */
    p += put16(p, 2);
    p += put16(p, SUITE_CHACHA20_POLY1305_SHA256);
    *p++ = 1; /* compression methods: null */
    *p++ = 0;
    u8 *ext_len = p;
    p += 2;

    ip4_t literal;
    size_t hl = strlen(host);
    if (!ip4_parse(host, &literal) && hl && hl < 256) { /* server_name (RFC 6066) */
        p += put16(p, 0);
        p += put16(p, hl + 5);
        p += put16(p, hl + 3);
        *p++ = 0; /* host_name */
        p += put16(p, hl);
        memcpy(p, host, hl);
        p += hl;
    }
    p += put16(p, 10); /* supported_groups: x25519 */
    p += put16(p, 4);
    p += put16(p, 2);
    p += put16(p, GROUP_X25519);
    /* signature_algorithms: what the server may sign with (not checked,
     * but servers refuse a handshake without it) */
    static const u16 sigs[] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601, 0x0807 };
    p += put16(p, 13);
    p += put16(p, 2 + 2 * ARRAY_SIZE(sigs));
    p += put16(p, 2 * ARRAY_SIZE(sigs));
    for (size_t i = 0; i < ARRAY_SIZE(sigs); i++)
        p += put16(p, sigs[i]);
    p += put16(p, 16); /* ALPN: http/1.1 */
    p += put16(p, 11);
    p += put16(p, 9);
    *p++ = 8;
    memcpy(p, "http/1.1", 8);
    p += 8;
    p += put16(p, 43); /* supported_versions: TLS 1.3 */
    p += put16(p, 3);
    *p++ = 2;
    p += put16(p, 0x0304);
    p += put16(p, 51); /* key_share */
    p += put16(p, 2 + 4 + 32);
    p += put16(p, 4 + 32);
    p += put16(p, GROUP_X25519);
    p += put16(p, 32);
    memcpy(p, pub, 32);
    p += 32;
    put16(ext_len, (u32)(p - ext_len - 2));

    size_t body = (size_t)(p - out) - 4;
    out[0] = HS_CLIENT_HELLO;
    out[1] = (u8)(body >> 16);
    out[2] = (u8)(body >> 8);
    out[3] = (u8)body;
    return (size_t)(p - out);
}

/* ---------------------------------------------------------------- handshake */
static bool handshake(struct tls_conn *t, const char *host)
{
    u8 priv[32], pub[32], spub[32], shared[32], h[32], tmp[32];
    u8 early[32], hs_secret[32], master[32], c_hs[32], s_hs[32];
    static u8 hello[512];
    struct sha256 tr;
    sha256_init(&tr);

    crypto_random(priv, 32);
    x25519_base(pub, priv);
    size_t n = build_client_hello(hello, host, pub);
    sha256_update(&tr, hello, n);
    if (!write_record(t, CT_HANDSHAKE, hello, n))
        return false;

    u8 *msg;
    size_t mlen;
    if (!next_handshake(t, &msg, &mlen))
        return false;
    if (msg[0] != HS_SERVER_HELLO)
        return fail(t, "expected ServerHello, got handshake message %u", msg[0]);
    if (!parse_server_hello(t, msg, mlen, spub))
        return false;
    sha256_update(&tr, msg, mlen);
    if (t->hs_len != mlen)
        return fail(t, "data after ServerHello in the same record");

    x25519(shared, priv, spub);
    crypto_wipe(priv, sizeof(priv));
    u8 zero[32];
    memset(zero, 0, 32);
    if (crypto_equal(shared, zero, 32))
        return fail(t, "the server's key share is invalid");

    /* RFC 8446 7.1 */
    hkdf_extract(NULL, 0, zero, 32, early);
    sha256("", 0, h);
    derive_secret(early, "derived", h, tmp);
    hkdf_extract(tmp, 32, shared, 32, hs_secret);
    crypto_wipe(shared, sizeof(shared));
    transcript_hash(&tr, h);
    derive_secret(hs_secret, "c hs traffic", h, c_hs);
    derive_secret(hs_secret, "s hs traffic", h, s_hs);
    sha256("", 0, h);
    derive_secret(hs_secret, "derived", h, tmp);
    hkdf_extract(tmp, 32, zero, 32, master);

    set_keys(s_hs, t->rsecret, t->rkey, t->riv, &t->rseq);
    t->rcrypt = true;

    /* EncryptedExtensions, Certificate, CertificateVerify, Finished */
    bool got_ee = false, got_cert = false;
    for (;;) {
        if (!next_handshake(t, &msg, &mlen))
            return false;
        u8 type = msg[0];
        if (type == HS_ENCRYPTED_EXTENSIONS && !got_ee) {
            got_ee = true;
        } else if (type == HS_CERTIFICATE_REQUEST) {
            return fail(t, "the server wants a client certificate");
        } else if (type == HS_CERTIFICATE && got_ee && !got_cert) {
            got_cert = true; /* not verified: see the top of this file */
        } else if (type == HS_CERTIFICATE_VERIFY && got_cert) {
            /* not verified either */
        } else if (type == HS_FINISHED && got_cert) {
            u8 fkey[32], want[32];
            expand_label(s_hs, "finished", NULL, 0, fkey, 32);
            transcript_hash(&tr, h);
            hmac_sha256(fkey, 32, h, 32, want);
            if (mlen != 4 + 32 || !crypto_equal(want, msg + 4, 32))
                return fail(t, "the server's Finished message does not match the handshake");
            sha256_update(&tr, msg, mlen);
            break;
        } else {
            return fail(t, "unexpected handshake message %u", type);
        }
        sha256_update(&tr, msg, mlen);
    }

    /* application traffic secrets from the transcript through server Finished */
    u8 c_ap[32], s_ap[32];
    transcript_hash(&tr, h);
    derive_secret(master, "c ap traffic", h, c_ap);
    derive_secret(master, "s ap traffic", h, s_ap);

    /* client Finished under the client handshake keys */
    u8 fkey[32], fin[4 + 32];
    expand_label(c_hs, "finished", NULL, 0, fkey, 32);
    hmac_sha256(fkey, 32, h, 32, fin + 4);
    fin[0] = HS_FINISHED;
    fin[1] = 0;
    fin[2] = 0;
    fin[3] = 32;
    set_keys(c_hs, t->wsecret, t->wkey, t->wiv, &t->wseq);
    t->wcrypt = true;
    if (!write_record(t, CT_HANDSHAKE, fin, sizeof(fin)))
        return false;

    set_keys(s_ap, t->rsecret, t->rkey, t->riv, &t->rseq);
    set_keys(c_ap, t->wsecret, t->wkey, t->wiv, &t->wseq);
    crypto_wipe(early, 32);
    crypto_wipe(hs_secret, 32);
    crypto_wipe(master, 32);
    crypto_wipe(c_hs, 32);
    crypto_wipe(s_hs, 32);
    crypto_wipe(c_ap, 32);
    crypto_wipe(s_ap, 32);
    crypto_wipe(fkey, 32);
    return true;
}

struct tls_conn *tls_connect(struct tcp_conn *tcp, const char *host, u32 timeout_ms,
                             char *err, size_t errlen)
{
    struct tls_conn *t = kzalloc(sizeof(*t));
    u8 *hs = kmalloc(HS_MAX);
    if (!t || !hs) {
        kfree(t);
        kfree(hs);
        tcp_close(tcp);
        if (errlen)
            strlcpy(err, "out of memory", errlen);
        return NULL;
    }
    t->tcp = tcp;
    t->hs = hs;
    t->timeout_ms = timeout_ms;
    if (!handshake(t, host)) {
        if (errlen)
            snprintf(err, errlen, "TLS handshake with %s: %s", host, tls_error(t));
        tcp_close(tcp);
        crypto_wipe(t, sizeof(*t));
        kfree(hs);
        kfree(t);
        return NULL;
    }
    t->hs_len = t->hs_used = 0;
    return t;
}

/* --------------------------------------------------------------- data */
ssize_t tls_send(struct tls_conn *t, const void *buf, size_t len)
{
    const u8 *p = buf;
    size_t done = 0;
    while (done < len) {
        size_t n = len - done < 16384 ? len - done : 16384;
        if (!write_record(t, CT_APPDATA, p + done, n))
            return NET_ETLS;
        done += n;
    }
    return (ssize_t)done;
}

/* A handshake message after the handshake: session tickets are ignored
 * (no resumption), KeyUpdate is followed. */
static bool post_handshake(struct tls_conn *t, const u8 *data, size_t n)
{
    if (t->hs_len + n > HS_MAX)
        return fail(t, "post-handshake message too large");
    memcpy(t->hs + t->hs_len, data, n);
    t->hs_len += n;
    while (t->hs_len >= 4) {
        size_t body = (size_t)t->hs[1] << 16 | (size_t)t->hs[2] << 8 | t->hs[3];
        if (t->hs_len < body + 4)
            break;
        if (t->hs[0] == HS_KEY_UPDATE && body == 1) {
            u8 next[32];
            expand_label(t->rsecret, "traffic upd", NULL, 0, next, 32);
            set_keys(next, t->rsecret, t->rkey, t->riv, &t->rseq);
            if (t->hs[4] == 1) { /* update_requested: answer, then update ours */
                u8 ku[5] = { HS_KEY_UPDATE, 0, 0, 1, 0 };
                if (!write_record(t, CT_HANDSHAKE, ku, sizeof(ku)))
                    return false;
                expand_label(t->wsecret, "traffic upd", NULL, 0, next, 32);
                set_keys(next, t->wsecret, t->wkey, t->wiv, &t->wseq);
            }
            crypto_wipe(next, 32);
        } else if (t->hs[0] != HS_NEW_SESSION_TICKET) {
            return fail(t, "unexpected handshake message %u after the handshake", t->hs[0]);
        }
        memmove(t->hs, t->hs + body + 4, t->hs_len - body - 4);
        t->hs_len -= body + 4;
    }
    return true;
}

ssize_t tls_recv(struct tls_conn *t, void *buf, size_t len, u32 timeout_ms)
{
    t->timeout_ms = timeout_ms;
    while (t->plain_pos == t->plain_len) {
        if (t->eof)
            return 0;
        u8 type, *data;
        size_t n;
        if (!read_record(t, &type, &data, &n))
            return NET_ETLS;
        if (type == CT_APPDATA) {
            t->plain_pos = (size_t)(data - t->rec);
            t->plain_len = t->plain_pos + n;
        } else if (type == CT_HANDSHAKE) {
            if (!post_handshake(t, data, n))
                return NET_ETLS;
        } else if (type == CT_ALERT) {
            if (!handle_alert(t, data, n))
                return NET_ETLS;
        } else {
            fail(t, "unexpected record type %u", type);
            return NET_ETLS;
        }
    }
    size_t k = t->plain_len - t->plain_pos < len ? t->plain_len - t->plain_pos : len;
    memcpy(buf, t->rec + t->plain_pos, k);
    t->plain_pos += k;
    return (ssize_t)k;
}

void tls_close(struct tls_conn *t)
{
    if (!t)
        return;
    if (!t->eof) {
        u8 alert[2] = { 1, 0 }; /* warning, close_notify */
        write_record(t, CT_ALERT, alert, 2);
    }
    tcp_close(t->tcp);
    kfree(t->hs);
    crypto_wipe(t, sizeof(*t));
    kfree(t);
}
