/*
 * DHCP client (RFC 2131 / RFC 2132): DISCOVER -> OFFER -> REQUEST -> ACK,
 * then renewal by unicast REQUEST at T1. If the lease runs out without a
 * renewal, the client starts over.
 */
#include "irq.h"
#include "netpriv.h"
#include "random.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define MAGIC 0x63825363

enum { DISCOVER = 1, OFFER = 2, REQUEST = 3, DECLINE = 4, ACK = 5, NAK = 6, RELEASE = 7 };

struct lease {
    ip4_t addr, mask, gw, dns, server;
    u32 lease_s, t1_s;
};

static size_t build(struct netif *nif, u8 *m, int type, u32 xid, ip4_t ciaddr, ip4_t want,
                    ip4_t server)
{
    memset(m, 0, 240);
    m[0] = 1; /* BOOTREQUEST */
    m[1] = 1; /* Ethernet */
    m[2] = 6;
    put32(m + 4, xid);
    put16(m + 10, ciaddr ? 0 : 0x8000); /* ask for broadcast replies until configured */
    put32(m + 12, ciaddr);
    memcpy(m + 28, nif->mac, 6);
    put32(m + 236, MAGIC);
    size_t n = 240;
    m[n++] = 53, m[n++] = 1, m[n++] = (u8)type;
    m[n++] = 61, m[n++] = 7, m[n++] = 1; /* client identifier: hardware type + MAC */
    memcpy(m + n, nif->mac, 6);
    n += 6;
    if (want) {
        m[n++] = 50, m[n++] = 4;
        put32(m + n, want);
        n += 4;
    }
    if (server) {
        m[n++] = 54, m[n++] = 4;
        put32(m + n, server);
        n += 4;
    }
    static const char host[] = "sonar";
    m[n++] = 12, m[n++] = sizeof(host) - 1;
    memcpy(m + n, host, sizeof(host) - 1);
    n += sizeof(host) - 1;
    m[n++] = 55, m[n++] = 5; /* parameter request list */
    m[n++] = 1, m[n++] = 3, m[n++] = 6, m[n++] = 51, m[n++] = 58;
    m[n++] = 57, m[n++] = 2; /* maximum message size */
    put16(m + n, 576);
    n += 2;
    m[n++] = 255;
    while (n < 300) /* BOOTP minimum */
        m[n++] = 0;
    return n;
}

/* Parse a reply; returns its message type (0 if not ours or malformed). */
static int parse(struct netif *nif, const u8 *m, size_t len, u32 xid, struct lease *l)
{
    if (len < 240 || m[0] != 2 || get32(m + 4) != xid || memcmp(m + 28, nif->mac, 6) ||
        get32(m + 236) != MAGIC)
        return 0;
    memset(l, 0, sizeof(*l));
    l->addr = get32(m + 16);
    int type = 0;
    size_t i = 240;
    while (i < len && m[i] != 255) {
        if (m[i] == 0) {
            i++;
            continue;
        }
        if (i + 2 > len || i + 2 + m[i + 1] > len)
            break;
        u8 code = m[i], olen = m[i + 1];
        const u8 *v = m + i + 2;
        switch (code) {
        case 53: if (olen >= 1) type = v[0]; break;
        case 1: if (olen >= 4) l->mask = get32(v); break;
        case 3: if (olen >= 4) l->gw = get32(v); break;
        case 6: if (olen >= 4) l->dns = get32(v); break;
        case 51: if (olen >= 4) l->lease_s = get32(v); break;
        case 54: if (olen >= 4) l->server = get32(v); break;
        case 58: if (olen >= 4) l->t1_s = get32(v); break;
        }
        i += 2 + (size_t)olen;
    }
    return type;
}

/* Send a request and wait for a reply of one of the wanted types. */
static int exchange(struct udp_sock *s, struct netif *nif, const u8 *msg, size_t len, ip4_t to,
                    u32 xid, int want1, int want2, struct lease *l, u32 timeout_ms)
{
    int r = udp_sendto(s, to, DHCP_SERVER_PORT, msg, len);
    if (r < 0)
        return r;
    u64 deadline = time_ms() + timeout_ms;
    u8 buf[1500];
    for (;;) {
        u64 now = time_ms();
        if (now >= deadline)
            return NET_ETIMEDOUT;
        int n = udp_recvfrom(s, buf, sizeof(buf), NULL, NULL, (u32)(deadline - now));
        if (n < 0)
            return n;
        int t = parse(nif, buf, (size_t)n, xid, l);
        if (t && (t == want1 || t == want2))
            return t;
    }
}

static void apply(struct netif *nif, const struct lease *l)
{
    nif->ip = l->addr;
    nif->mask = l->mask ? l->mask : IP4(255, 255, 255, 0);
    nif->gw = l->gw;
    nif->dns = l->dns;
    nif->dhcp = true;
    nif->dhcp_server = l->server;
    u32 lease = l->lease_s ? l->lease_s : 3600;
    if (lease == 0xffffffff)
        lease = 0x7fffffff / 1000; /* "infinite": renew rarely */
    u32 t1 = l->t1_s && l->t1_s < lease ? l->t1_s : lease / 2;
    u64 now = time_ms();
    nif->lease_renew_ms = now + (u64)t1 * 1000;
    nif->lease_end_ms = now + (u64)lease * 1000;
}

int dhcp_configure(struct netif *nif, bool quiet)
{
    struct udp_sock *s = udp_open(DHCP_CLIENT_PORT);
    if (!s)
        return NET_ENOMEM;
    nif->ip = nif->mask = nif->gw = nif->dns = 0;
    nif->dhcp = false;
    u8 msg[548];
    struct lease offer, ack;
    int r = NET_ETIMEDOUT;
    static const u32 waits[] = { 1000, 2000, 3000 };
    for (int attempt = 0; attempt < 3; attempt++) {
        u32 xid = random_u32();
        size_t n = build(nif, msg, DISCOVER, xid, 0, 0, 0);
        r = exchange(s, nif, msg, n, IP4_BROADCAST, xid, OFFER, 0, &offer, waits[attempt]);
        if (r == NET_EINTR)
            break;
        if (r != OFFER)
            continue;
        n = build(nif, msg, REQUEST, xid, 0, offer.addr, offer.server);
        r = exchange(s, nif, msg, n, IP4_BROADCAST, xid, ACK, NAK, &ack, 3000);
        if (r == ACK) {
            if (!ack.server)
                ack.server = offer.server;
            apply(nif, &ack);
            r = NET_OK;
            break;
        }
        if (r == NET_EINTR)
            break;
        r = NET_ETIMEDOUT; /* NAK or no answer: start over */
    }
    udp_close(s);
    if (r != NET_OK && !quiet)
        printk("dhcp: no answer from a DHCP server\n");
    return r;
}

/* Renew the lease at T1 (unicast to the server); start over if it ends. */
void dhcp_tick(struct netif *nif)
{
    if (!nif->dhcp || !nif->ip)
        return;
    u64 now = time_ms();
    if (now >= nif->lease_end_ms) {
        printk("\ndhcp: lease expired; reconfiguring\n");
        dhcp_configure(nif, false);
        return;
    }
    if (now < nif->lease_renew_ms)
        return;
    nif->lease_renew_ms = now + 60 * 1000; /* if this attempt fails, retry in a minute */
    struct udp_sock *s = udp_open(DHCP_CLIENT_PORT);
    if (!s)
        return;
    u8 msg[548];
    u32 xid = random_u32();
    size_t n = build(nif, msg, REQUEST, xid, nif->ip, 0, 0);
    struct lease l;
    int r = exchange(s, nif, msg, n, nif->dhcp_server ? nif->dhcp_server : IP4_BROADCAST, xid,
                     ACK, NAK, &l, 2000);
    udp_close(s);
    if (r == ACK && l.addr == nif->ip) {
        if (!l.server)
            l.server = nif->dhcp_server;
        if (!l.mask)
            l.mask = nif->mask;
        if (!l.gw)
            l.gw = nif->gw;
        if (!l.dns)
            l.dns = nif->dns;
        apply(nif, &l);
    } else if (r == NAK) {
        printk("\ndhcp: lease refused; reconfiguring\n");
        dhcp_configure(nif, false);
    }
}
