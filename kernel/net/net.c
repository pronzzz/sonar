/*
 * Network core: interfaces, Ethernet II, ARP (RFC 826), IPv4 (RFC 791),
 * ICMP echo (RFC 792), UDP (RFC 768) and a loopback path.
 *
 * Packets are processed when the stack is polled: from the shell's idle
 * loop and from every blocking network call. IPv4 fragments are not
 * reassembled (TCP uses the MSS, DNS/DHCP fit one packet) and outgoing
 * packets carry Don't Fragment.
 */
#include "arch.h"
#include "config.h"
#include "console.h"
#include "irq.h"
#include "mm.h"
#include "netpriv.h"
#include "random.h"

#define ETH_HLEN 14
#define ETH_IP 0x0800
#define ETH_ARP 0x0806
#define MAX_IFACES 4

static struct netif *ifaces[MAX_IFACES];
static int nifaces;
static u16 ip_id;
/* Testing aid: config "net.test_loss" = percent of frames dropped at random in
 * each direction, to exercise retransmission. Read when the stack starts. */
static u32 test_loss;

static bool lose(void) { return test_loss && random_u32() % 100 < test_loss; }
static const u8 bcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

const char *net_strerror(int e)
{
    switch (e) {
    case NET_OK: return "success";
    case NET_ETIMEDOUT: return "timed out";
    case NET_EUNREACH: return "network unreachable";
    case NET_ERESET: return "connection reset by peer";
    case NET_EREFUSED: return "connection refused";
    case NET_ENOMEM: return "out of memory";
    case NET_EINTR: return "interrupted";
    case NET_ENOTFOUND: return "host not found";
    case NET_EINVAL: return "invalid argument";
    case NET_ECLOSED: return "connection closed";
    case NET_ENODEV: return "no network interface";
    case NET_ETLS: return "TLS error";
    }
    return "error";
}

void netif_register(struct netif *nif)
{
    if (nifaces < MAX_IFACES)
        ifaces[nifaces++] = nif;
    random_mix(nif->mac, 6);
}

struct netif *net_iface(void) { return nifaces ? ifaces[0] : NULL; }

void ip4_format(ip4_t a, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

bool ip4_parse(const char *s, ip4_t *out)
{
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        if (!isdigit((u8)*s))
            return false;
        u32 part = 0;
        int digits = 0;
        while (isdigit((u8)*s)) {
            part = part * 10 + (u32)(*s++ - '0');
            if (++digits > 3 || part > 255)
                return false;
        }
        v = v << 8 | part;
        if (i < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *out = v;
    return true;
}

u16 net_checksum(const void *data, size_t len, u32 sum32)
{
    const u8 *p = data;
    u64 sum = sum32;
    while (len > 1) {
        sum += (u32)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (u32)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (u16)~sum;
}

u32 net_pseudo_sum(ip4_t src, ip4_t dst, u8 proto, u16 len)
{
    return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto + len;
}

/* ------------------------------------------------------------ Ethernet */
static bool eth_send(struct netif *nif, const u8 *dst, u16 type, const void *payload, size_t len)
{
    u8 frame[ETH_HLEN + 1500];
    if (len > (size_t)nif->mtu)
        return false;
    memcpy(frame, dst, 6);
    memcpy(frame + 6, nif->mac, 6);
    put16(frame + 12, type);
    memcpy(frame + ETH_HLEN, payload, len);
    size_t flen = ETH_HLEN + len;
    if (lose())
        return true;
    if (flen < 60) { /* minimum frame size (without FCS) */
        memset(frame + flen, 0, 60 - flen);
        flen = 60;
    }
    if (!nif->send(nif, frame, flen)) {
        nif->tx_dropped++;
        return false;
    }
    nif->tx_packets++;
    nif->tx_bytes += flen;
    return true;
}

/* ------------------------------------------------------------ ARP */
#define ARP_SIZE 32
#define ARP_QLEN 4
#define ARP_TTL_MS (10 * 60 * 1000)
enum { ARP_FREE, ARP_PENDING, ARP_OK };

struct arp_entry {
    ip4_t ip;
    u8 mac[6];
    int state, tries;
    u64 time; /* learned (OK) or last request (PENDING) */
    u8 *q[ARP_QLEN];
    u16 qlen[ARP_QLEN];
    int nq;
};
static struct arp_entry arp[ARP_SIZE];

static void arp_send(struct netif *nif, u16 op, const u8 *tha, ip4_t tpa, const u8 *dst)
{
    u8 p[28];
    put16(p, 1);      /* Ethernet */
    put16(p + 2, ETH_IP);
    p[4] = 6;
    p[5] = 4;
    put16(p + 6, op);
    memcpy(p + 8, nif->mac, 6);
    put32(p + 14, nif->ip);
    memcpy(p + 18, tha, 6);
    put32(p + 24, tpa);
    eth_send(nif, dst, ETH_ARP, p, sizeof(p));
}

static void arp_drop_queue(struct arp_entry *e)
{
    for (int i = 0; i < e->nq; i++)
        kfree(e->q[i]);
    e->nq = 0;
}

static struct arp_entry *arp_find(ip4_t ip)
{
    for (int i = 0; i < ARP_SIZE; i++)
        if (arp[i].state != ARP_FREE && arp[i].ip == ip)
            return &arp[i];
    return NULL;
}

static struct arp_entry *arp_alloc(ip4_t ip)
{
    struct arp_entry *victim = NULL;
    for (int i = 0; i < ARP_SIZE; i++) {
        if (arp[i].state == ARP_FREE) {
            victim = &arp[i];
            break;
        }
        if (!victim || arp[i].time < victim->time)
            victim = &arp[i]; /* evict the oldest */
    }
    arp_drop_queue(victim);
    memset(victim, 0, sizeof(*victim));
    victim->ip = ip;
    return victim;
}

static void arp_learn(struct netif *nif, ip4_t ip, const u8 *mac, bool create)
{
    struct arp_entry *e = arp_find(ip);
    if (!e) {
        if (!create)
            return;
        e = arp_alloc(ip);
    }
    memcpy(e->mac, mac, 6);
    e->state = ARP_OK;
    e->time = time_ms();
    for (int i = 0; i < e->nq; i++) {
        eth_send(nif, e->mac, ETH_IP, e->q[i], e->qlen[i]);
        kfree(e->q[i]);
    }
    e->nq = 0;
}

static void arp_input(struct netif *nif, const u8 *p, size_t len)
{
    if (len < 28 || get16(p) != 1 || get16(p + 2) != ETH_IP || p[4] != 6 || p[5] != 4)
        return;
    u16 op = get16(p + 6);
    const u8 *sha = p + 8;
    ip4_t spa = get32(p + 14), tpa = get32(p + 24);
    if (!spa || !nif->ip)
        return;
    bool for_us = tpa == nif->ip;
    arp_learn(nif, spa, sha, for_us);
    if (for_us && op == 1)
        arp_send(nif, 2, sha, spa, sha);
}

static void arp_tick(struct netif *nif)
{
    u64 now = time_ms();
    for (int i = 0; i < ARP_SIZE; i++) {
        struct arp_entry *e = &arp[i];
        if (e->state == ARP_PENDING && now - e->time >= 1000) {
            if (e->tries >= 3) {
                arp_drop_queue(e);
                e->state = ARP_FREE;
                continue;
            }
            e->tries++;
            e->time = now;
            static const u8 zero[6];
            arp_send(nif, 1, zero, e->ip, bcast_mac);
        } else if (e->state == ARP_OK && now - e->time >= ARP_TTL_MS) {
            e->state = ARP_FREE;
        }
    }
}

void arp_list(void)
{
    int n = 0;
    for (int i = 0; i < ARP_SIZE; i++) {
        struct arp_entry *e = &arp[i];
        if (e->state == ARP_FREE)
            continue;
        char ip[16];
        ip4_format(e->ip, ip, sizeof(ip));
        if (e->state == ARP_OK)
            printk("%-15s  %02x:%02x:%02x:%02x:%02x:%02x  %llu s ago\n", ip, e->mac[0], e->mac[1],
                   e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                   (unsigned long long)((time_ms() - e->time) / 1000));
        else
            printk("%-15s  (resolving)\n", ip);
        n++;
    }
    if (!n)
        printk("(ARP cache empty)\n");
}

/* Send an IP packet to an on-link next hop, resolving its address. */
static int arp_output(struct netif *nif, ip4_t hop, const u8 *pkt, size_t len)
{
    struct arp_entry *e = arp_find(hop);
    if (e && e->state == ARP_OK)
        return eth_send(nif, e->mac, ETH_IP, pkt, len) ? NET_OK : NET_ENOMEM;
    if (!e) {
        e = arp_alloc(hop);
        e->state = ARP_PENDING;
        e->time = time_ms();
        e->tries = 1;
        static const u8 zero[6];
        arp_send(nif, 1, zero, hop, bcast_mac);
    }
    if (e->nq >= ARP_QLEN)
        return NET_OK; /* dropped, as if lost; protocols retransmit */
    u8 *copy = kmalloc(len);
    if (!copy)
        return NET_ENOMEM;
    memcpy(copy, pkt, len);
    e->q[e->nq] = copy;
    e->qlen[e->nq] = (u16)len;
    e->nq++;
    return NET_OK;
}

/* ------------------------------------------------------------ IPv4 */
#define LOOP_Q 16
static u8 *loopq[LOOP_Q];
static u16 loopq_len[LOOP_Q];
static int loop_head, loop_count;

static bool is_local(struct netif *nif, ip4_t a)
{
    return (a >> 24) == 127 || (nif->ip && a == nif->ip);
}

int ip_send(ip4_t dst, u8 proto, const void *payload, size_t len)
{
    struct netif *nif = net_iface();
    if (!nif)
        return NET_ENODEV;
    if (len + 20 > (size_t)nif->mtu)
        return NET_EINVAL;
    u8 pkt[1500];
    ip4_t src = nif->ip;
    if ((dst >> 24) == 127)
        src = dst;
    pkt[0] = 0x45;
    pkt[1] = 0;
    put16(pkt + 2, (u16)(20 + len));
    put16(pkt + 4, ip_id++);
    put16(pkt + 6, 0x4000); /* Don't Fragment */
    pkt[8] = 64;
    pkt[9] = proto;
    put16(pkt + 10, 0);
    put32(pkt + 12, src);
    put32(pkt + 16, dst);
    put16(pkt + 10, net_checksum(pkt, 20, 0));
    memcpy(pkt + 20, payload, len);
    size_t total = 20 + len;

    if (is_local(nif, dst)) {
        if (loop_count >= LOOP_Q)
            return NET_OK; /* dropped */
        u8 *copy = kmalloc(total);
        if (!copy)
            return NET_ENOMEM;
        memcpy(copy, pkt, total);
        int slot = (loop_head + loop_count) % LOOP_Q;
        loopq[slot] = copy;
        loopq_len[slot] = (u16)total;
        loop_count++;
        return NET_OK;
    }
    if (!nif->link_up)
        return NET_EUNREACH;
    bool bcast = dst == IP4_BROADCAST || (nif->mask && nif->ip && dst == (nif->ip | ~nif->mask));
    if (bcast)
        return eth_send(nif, bcast_mac, ETH_IP, pkt, total) ? NET_OK : NET_ENOMEM;
    if (!nif->ip)
        return NET_EUNREACH;
    ip4_t hop;
    if ((dst & nif->mask) == (nif->ip & nif->mask))
        hop = dst;
    else if (nif->gw)
        hop = nif->gw;
    else
        return NET_EUNREACH;
    return arp_output(nif, hop, pkt, total);
}

static void ip_input(struct netif *nif, const u8 *p, size_t len)
{
    if (len < 20 || (p[0] >> 4) != 4)
        return;
    size_t ihl = (size_t)(p[0] & 15) * 4, total = get16(p + 2);
    if (ihl < 20 || total < ihl || total > len || net_checksum(p, ihl, 0) != 0)
        return;
    if (get16(p + 6) & 0x3fff)
        return; /* fragment: not supported */
    ip4_t src = get32(p + 12), dst = get32(p + 16);
    bool bcast = dst == IP4_BROADCAST || (nif->mask && nif->ip && dst == (nif->ip | ~nif->mask));
    if (!bcast && nif->ip && dst != nif->ip && !is_local(nif, dst))
        return; /* not for us (an unconfigured interface accepts all, for DHCP) */
    const u8 *pl = p + ihl;
    size_t plen = total - ihl;
    switch (p[9]) {
    case PROTO_ICMP: icmp_input(src, dst, pl, plen, p[8], bcast); break;
    case PROTO_UDP: udp_input(src, dst, pl, plen); break;
    case PROTO_TCP:
        if (!bcast)
            tcp_input(src, dst, pl, plen);
        break;
    }
}

void net_input(struct netif *nif, const u8 *f, size_t len)
{
    nif->rx_packets++;
    nif->rx_bytes += len;
    if (len < ETH_HLEN || lose())
        return;
    bool to_us = !memcmp(f, nif->mac, 6) || !memcmp(f, bcast_mac, 6);
    if (!to_us) {
        nif->rx_dropped++;
        return;
    }
    u16 type = get16(f + 12);
    if (type == ETH_ARP)
        arp_input(nif, f + ETH_HLEN, len - ETH_HLEN);
    else if (type == ETH_IP)
        ip_input(nif, f + ETH_HLEN, len - ETH_HLEN);
}

/* ------------------------------------------------------------ ICMP */
static struct {
    bool waiting, got;
    u16 id, seq;
    ip4_t dst;
    int ttl;
    u64 t_recv;
} ping;

void icmp_input(ip4_t src, ip4_t dst, const u8 *p, size_t len, int ttl, bool bcast)
{
    if (len < 8 || net_checksum(p, len, 0) != 0)
        return;
    if (p[0] == 8 && p[1] == 0 && !bcast) { /* echo request */
        u8 *r = kmalloc(len);
        if (!r)
            return;
        memcpy(r, p, len);
        r[0] = 0;
        put16(r + 2, 0);
        put16(r + 2, net_checksum(r, len, 0));
        ip_send(src, PROTO_ICMP, r, len);
        kfree(r);
    } else if (p[0] == 0 && ping.waiting && get16(p + 4) == ping.id && get16(p + 6) == ping.seq &&
               src == ping.dst) {
        ping.got = true;
        ping.ttl = ttl;
        ping.t_recv = time_ns();
    }
}

int icmp_ping(ip4_t dst, u16 seq, size_t payload, u32 timeout_ms, int *ttl)
{
    if (payload > 1400)
        payload = 1400;
    u8 pkt[1408];
    if (!ping.id)
        ping.id = (u16)(random_u32() | 1);
    pkt[0] = 8;
    pkt[1] = 0;
    put16(pkt + 2, 0);
    put16(pkt + 4, ping.id);
    put16(pkt + 6, seq);
    for (size_t i = 0; i < payload; i++)
        pkt[8 + i] = (u8)(0x10 + i);
    put16(pkt + 2, net_checksum(pkt, 8 + payload, 0));
    ping.waiting = true;
    ping.got = false;
    ping.seq = seq;
    ping.dst = dst;
    u64 t0 = time_ns();
    int r = ip_send(dst, PROTO_ICMP, pkt, 8 + payload);
    u64 deadline = time_ms() + timeout_ms;
    while (r == NET_OK && !ping.got) {
        r = net_wait_check(deadline);
        if (r == NET_OK)
            net_wait();
    }
    ping.waiting = false;
    if (!ping.got)
        return r == NET_OK ? NET_ETIMEDOUT : r;
    if (ttl)
        *ttl = ping.ttl;
    return (int)((ping.t_recv - t0) / 1000);
}

/* ------------------------------------------------------------ UDP */
#define UDP_SOCKS 16
#define UDP_QUEUE 8
struct udp_dgram {
    ip4_t src;
    u16 sport, len;
    u8 data[];
};
struct udp_sock {
    bool used;
    u16 port;
    struct udp_dgram *q[UDP_QUEUE];
    int head, count;
};
static struct udp_sock udp_socks[UDP_SOCKS];

static bool udp_port_used(u16 port)
{
    for (int i = 0; i < UDP_SOCKS; i++)
        if (udp_socks[i].used && udp_socks[i].port == port)
            return true;
    return false;
}

struct udp_sock *udp_open(u16 port)
{
    if (port && udp_port_used(port))
        return NULL;
    while (!port) {
        u16 p = (u16)(49152 + random_u32() % 16384);
        if (!udp_port_used(p))
            port = p;
    }
    for (int i = 0; i < UDP_SOCKS; i++) {
        if (!udp_socks[i].used) {
            memset(&udp_socks[i], 0, sizeof(udp_socks[i]));
            udp_socks[i].used = true;
            udp_socks[i].port = port;
            return &udp_socks[i];
        }
    }
    return NULL;
}

void udp_close(struct udp_sock *s)
{
    if (!s)
        return;
    while (s->count) {
        kfree(s->q[s->head]);
        s->head = (s->head + 1) % UDP_QUEUE;
        s->count--;
    }
    s->used = false;
}

int udp_sendto(struct udp_sock *s, ip4_t dst, u16 port, const void *buf, size_t len)
{
    struct netif *nif = net_iface();
    if (!nif)
        return NET_ENODEV;
    if (len > 1472)
        return NET_EINVAL;
    u8 pkt[1480];
    put16(pkt, s->port);
    put16(pkt + 2, port);
    put16(pkt + 4, (u16)(8 + len));
    put16(pkt + 6, 0);
    memcpy(pkt + 8, buf, len);
    ip4_t src = (dst >> 24) == 127 ? dst : nif->ip;
    u16 ck = net_checksum(pkt, 8 + len, net_pseudo_sum(src, dst, PROTO_UDP, (u16)(8 + len)));
    put16(pkt + 6, ck ? ck : 0xffff);
    return ip_send(dst, PROTO_UDP, pkt, 8 + len);
}

void udp_input(ip4_t src, ip4_t dst, const u8 *p, size_t len)
{
    if (len < 8)
        return;
    u16 ulen = get16(p + 4);
    if (ulen < 8 || ulen > len)
        return;
    if (get16(p + 6) && net_checksum(p, ulen, net_pseudo_sum(src, dst, PROTO_UDP, ulen)) != 0)
        return;
    u16 dport = get16(p + 2);
    for (int i = 0; i < UDP_SOCKS; i++) {
        struct udp_sock *s = &udp_socks[i];
        if (!s->used || s->port != dport)
            continue;
        if (s->count >= UDP_QUEUE)
            return;
        struct udp_dgram *d = kmalloc(sizeof(*d) + ulen - 8);
        if (!d)
            return;
        d->src = src;
        d->sport = get16(p);
        d->len = (u16)(ulen - 8);
        memcpy(d->data, p + 8, d->len);
        s->q[(s->head + s->count) % UDP_QUEUE] = d;
        s->count++;
        return;
    }
}

int udp_recvfrom(struct udp_sock *s, void *buf, size_t len, ip4_t *src, u16 *sport,
                 u32 timeout_ms)
{
    u64 deadline = time_ms() + timeout_ms;
    for (;;) {
        net_poll();
        if (s->count) {
            struct udp_dgram *d = s->q[s->head];
            s->head = (s->head + 1) % UDP_QUEUE;
            s->count--;
            size_t n = d->len < len ? d->len : len;
            memcpy(buf, d->data, n);
            if (src)
                *src = d->src;
            if (sport)
                *sport = d->sport;
            kfree(d);
            return (int)n;
        }
        int r = net_wait_check(deadline);
        if (r != NET_OK)
            return r;
        net_wait();
    }
}

void udp_list(void)
{
    for (int i = 0; i < UDP_SOCKS; i++)
        if (udp_socks[i].used)
            printk("udp   *:%-5u  (%d queued)\n", udp_socks[i].port, udp_socks[i].count);
}

/* ------------------------------------------------------------ polling */
static bool in_poll;
static u64 last_tick;

void net_poll(void)
{
    if (in_poll || !nifaces)
        return;
    in_poll = true;
    for (int i = 0; i < nifaces; i++)
        ifaces[i]->poll(ifaces[i]);
    tcp_flush();
    while (loop_count) {
        u8 *pkt = loopq[loop_head];
        u16 len = loopq_len[loop_head];
        loop_head = (loop_head + 1) % LOOP_Q;
        loop_count--;
        ip_input(ifaces[0], pkt, len);
        kfree(pkt);
        tcp_flush();
    }
    u64 now = time_ms();
    if (now - last_tick >= 10) {
        last_tick = now;
        arp_tick(ifaces[0]);
    }
    tcp_tick();
    in_poll = false;
}

void net_wait(void)
{
    net_poll();
    u64 f = irq_save();
    for (int i = 0; i < nifaces; i++)
        if (ifaces[i]->arm)
            ifaces[i]->arm(ifaces[i], true);
    /* check after arming, so a frame arriving now still interrupts the wfi */
    bool pending = loop_count > 0;
    for (int i = 0; i < nifaces && !pending; i++)
        pending = ifaces[i]->pending && ifaces[i]->pending(ifaces[i]);
    if (!pending)
        wfi(); /* wakes on the device interrupt or the next timer tick */
    for (int i = 0; i < nifaces; i++)
        if (ifaces[i]->arm)
            ifaces[i]->arm(ifaces[i], false);
    irq_restore(f);
}

int net_wait_check(u64 deadline_ms)
{
    if (console_interrupted())
        return NET_EINTR;
    if (time_ms() >= deadline_ms)
        return NET_ETIMEDOUT;
    return NET_OK;
}

static void net_idle(void)
{
    net_poll();
    if (nifaces)
        dhcp_tick(ifaces[0]);
}

void net_describe(char *out, size_t n)
{
    struct netif *nif = net_iface();
    if (!nif) {
        strlcpy(out, "no network interface", n);
        return;
    }
    if (!nif->ip) {
        snprintf(out, n, "%s not configured%s", nif->name, nif->link_up ? "" : " (link down)");
        return;
    }
    char ip[16], gw[16], dns[16];
    ip4_format(nif->ip, ip, sizeof(ip));
    ip4_format(nif->gw, gw, sizeof(gw));
    ip4_format(nif->dns, dns, sizeof(dns));
    int prefix = __builtin_popcount(nif->mask);
    snprintf(out, n, "%s %s/%d, gateway %s, DNS %s (%s)", nif->name, ip, prefix,
             nif->gw ? gw : "none", nif->dns ? dns : "none", nif->dhcp ? "DHCP" : "static");
}

/* Apply the configured address ("net.mode" = dhcp | static | off). */
int net_configure(bool quiet)
{
    struct netif *nif = net_iface();
    if (!nif)
        return NET_ENODEV;
    const char *mode = config_get("net.mode", "dhcp");
    nif->ip = nif->mask = nif->gw = nif->dns = 0;
    nif->dhcp = false;
    if (!strcmp(mode, "off"))
        return NET_OK;
    if (!strcmp(mode, "static")) {
        ip4_t ip, gw = 0, dns = 0;
        const char *a = config_get("net.address", "");
        char addr[32];
        strlcpy(addr, a, sizeof(addr));
        char *slash = strchr(addr, '/');
        long prefix = 24;
        if (slash) {
            *slash = 0;
            prefix = strtol(slash + 1, NULL, 10);
        }
        if (!ip4_parse(addr, &ip) || prefix < 1 || prefix > 32) {
            printk("net: invalid net.address '%s' (expected e.g. 192.168.1.20/24)\n", a);
            return NET_EINVAL;
        }
        ip4_parse(config_get("net.gateway", ""), &gw);
        ip4_parse(config_get("net.dns", ""), &dns);
        nif->ip = ip;
        nif->mask = prefix == 32 ? 0xffffffffu : ~((1u << (32 - prefix)) - 1);
        nif->gw = gw;
        nif->dns = dns;
        return NET_OK;
    }
    return dhcp_configure(nif, quiet);
}

void net_boot(void)
{
    struct netif *nif = net_iface();
    if (!nif)
        return;
    console_add_idle_hook(net_idle);
    long loss = config_get_int("net.test_loss", 0);
    if (loss > 0 && loss < 100) {
        test_loss = (u32)loss;
        printk("net: TESTING: dropping %u%% of frames (config net.test_loss)\n", test_loss);
    }
    int r = net_configure(true);
    char desc[160];
    net_describe(desc, sizeof(desc));
    if (r == NET_OK)
        printk("net: %s\n", desc);
    else
        printk("net: %s: %s (retry with 'dhcp')\n", nif->name, net_strerror(r));
}
