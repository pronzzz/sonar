/* DNS stub resolver (RFC 1035): A records over UDP, with a small cache. */
#include "irq.h"
#include "netpriv.h"
#include "random.h"

#define CACHE 16

static struct {
    char name[64];
    ip4_t addr;
    u64 expires;
} cache[CACHE];

static bool name_eq(const char *a, const char *b)
{
    while (*a && *b)
        if (tolower((u8)*a++) != tolower((u8)*b++))
            return false;
    return *a == *b;
}

static size_t encode_name(const char *name, u8 *out, size_t cap)
{
    size_t n = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t l = dot ? (size_t)(dot - name) : strlen(name);
        if (l == 0 || l > 63 || n + l + 2 > cap)
            return 0;
        out[n++] = (u8)l;
        memcpy(out + n, name, l);
        n += l;
        name += l;
        if (*name == '.')
            name++;
    }
    out[n++] = 0;
    return n;
}

/* Skip a (possibly compressed) name; returns the offset after it, or 0. */
static size_t skip_name(const u8 *m, size_t len, size_t i)
{
    while (i < len) {
        u8 l = m[i];
        if ((l & 0xc0) == 0xc0)
            return i + 2 <= len ? i + 2 : 0;
        if (l & 0xc0)
            return 0;
        i += 1 + (size_t)l;
        if (l == 0)
            return i <= len ? i : 0;
    }
    return 0;
}

static int query(ip4_t server, const char *name, ip4_t *out, u32 *ttl)
{
    u8 q[512];
    u16 id = (u16)random_u32();
    memset(q, 0, 12);
    put16(q, id);
    put16(q + 2, 0x0100); /* recursion desired */
    put16(q + 4, 1);
    size_t n = encode_name(name, q + 12, sizeof(q) - 16);
    if (!n)
        return NET_EINVAL;
    n += 12;
    put16(q + n, 1); /* A */
    put16(q + n + 2, 1); /* IN */
    n += 4;
    struct udp_sock *s = udp_open(0);
    if (!s)
        return NET_ENOMEM;
    int r = NET_ETIMEDOUT;
    u8 m[1500];
    for (int attempt = 0; attempt < 3 && r == NET_ETIMEDOUT; attempt++) {
        r = udp_sendto(s, server, 53, q, n);
        if (r < 0)
            break;
        u64 deadline = time_ms() + 2000;
        r = NET_ETIMEDOUT;
        for (;;) {
            u64 now = time_ms();
            if (now >= deadline)
                break;
            ip4_t from;
            u16 sport;
            int len = udp_recvfrom(s, m, sizeof(m), &from, &sport, (u32)(deadline - now));
            if (len == NET_EINTR) {
                r = len;
                break;
            }
            if (len < 12 || from != server || sport != 53 || get16(m) != id || !(m[2] & 0x80))
                continue;
            int rcode = m[3] & 15;
            if (rcode == 3) { /* NXDOMAIN */
                r = NET_ENOTFOUND;
                break;
            }
            if (rcode != 0) {
                r = NET_ENOTFOUND;
                break;
            }
            u16 qd = get16(m + 4), an = get16(m + 6);
            size_t i = 12;
            for (int k = 0; k < qd && i; k++) {
                i = skip_name(m, (size_t)len, i);
                if (i)
                    i += 4;
            }
            r = NET_ENOTFOUND;
            for (int k = 0; k < an && i && i < (size_t)len; k++) {
                i = skip_name(m, (size_t)len, i);
                if (!i || i + 10 > (size_t)len)
                    break;
                u16 type = get16(m + i), cls = get16(m + i + 2), rdlen = get16(m + i + 8);
                u32 t = get32(m + i + 4);
                i += 10;
                if (i + rdlen > (size_t)len)
                    break;
                if (type == 1 && cls == 1 && rdlen == 4) {
                    *out = get32(m + i);
                    *ttl = t;
                    r = NET_OK;
                    break;
                }
                i += rdlen;
            }
            break;
        }
    }
    udp_close(s);
    return r;
}

int dns_resolve(const char *name, ip4_t *out)
{
    if (ip4_parse(name, out))
        return NET_OK;
    if (name_eq(name, "localhost")) {
        *out = IP4(127, 0, 0, 1);
        return NET_OK;
    }
    if (!*name || strlen(name) >= sizeof(cache[0].name))
        return NET_EINVAL;
    u64 now = time_ms();
    for (int i = 0; i < CACHE; i++) {
        if (cache[i].expires > now && name_eq(cache[i].name, name)) {
            *out = cache[i].addr;
            return NET_OK;
        }
    }
    struct netif *nif = net_iface();
    if (!nif)
        return NET_ENODEV;
    if (!nif->dns)
        return NET_EUNREACH;
    u32 ttl = 0;
    int r = query(nif->dns, name, out, &ttl);
    if (r != NET_OK)
        return r;
    if (ttl < 30)
        ttl = 30;
    if (ttl > 3600)
        ttl = 3600;
    int slot = 0;
    for (int i = 1; i < CACHE; i++)
        if (cache[i].expires < cache[slot].expires)
            slot = i;
    strlcpy(cache[slot].name, name, sizeof(cache[slot].name));
    cache[slot].addr = *out;
    cache[slot].expires = now + (u64)ttl * 1000;
    return NET_OK;
}
