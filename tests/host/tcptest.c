/*
 * Host test for Sonar's TCP (kernel/net/tcp.c).
 *
 * The real tcp.c is compiled against a simulated network: packets are
 * delayed (reordered), dropped and duplicated at random, and the other end
 * is a small test server written here that sends a known byte stream,
 * honours (or ignores) SACK, and checks every segment Sonar sends
 * (checksums, SACK blocks that only cover data really delivered, window
 * limits). The client side runs the same calls the fetch command uses:
 * tcp_connect, tcp_send, tcp_recv until end of stream, tcp_close.
 *
 *   tcptest            run all scenarios
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h" /* the kernel's network API (kernel/include) */

#define MY_IP 0x0a00020f   /* 10.0.2.15 */
#define PEER_IP 0x0a000202 /* 10.0.2.2 */
#define PEER_PORT 80
#define MSS 1460

/* ------------------------------------------------------------ platform stubs */
static u64 now; /* simulated milliseconds */
u64 time_ms(void) { return now; }
u64 time_ns(void) { return now * 1000000ULL; }

static u64 rng = 0x9e3779b97f4a7c15ULL;
static u32 rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (u32)(rng >> 16);
}
u32 random_u32(void) { return rnd(); }
u64 random_u64(void) { return ((u64)rnd() << 32) | rnd(); }

void *kmalloc(size_t n) { return malloc(n); }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
int printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
bool console_interrupted(void) { return false; }

static struct netif nif = { .name = "eth0", .mtu = 1500, .link_up = true, .ip = MY_IP,
                            .mask = 0xffffff00, .gw = PEER_IP };
struct netif *net_iface(void) { return &nif; }

void ip4_format(ip4_t a, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
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

static u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8), p[1] = (u8)v; }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }
static bool lt(u32 a, u32 b) { return (i32)(a - b) < 0; }
static bool le(u32 a, u32 b) { return (i32)(a - b) <= 0; }

/* ------------------------------------------------------------ network */
struct scenario {
    const char *name;
    int loss_pct, dup_pct;
    int delay_min, delay_max; /* ms; a spread reorders packets */
    bool sack;                /* test server uses SACK */
    u32 total;                /* bytes the server sends */
    int read_chunk;           /* client read size */
    int read_pause;           /* ms the client waits after each read */
};
static const struct scenario *sc;

struct pkt {
    u64 at;
    bool to_sonar;
    size_t len;
    u8 data[1500];
    struct pkt *next;
};
static struct pkt *queue;
static int failures;

#define CHECK(cond, ...)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            printf("  FAIL (%s:%d): ", __FILE__, __LINE__);                                 \
            printf(__VA_ARGS__);                                                            \
            printf("\n");                                                                   \
            failures++;                                                                     \
        }                                                                                   \
    } while (0)

static void enqueue(bool to_sonar, const u8 *p, size_t len)
{
    int copies = 1 + ((int)(rnd() % 100) < sc->dup_pct ? 1 : 0);
    for (int i = 0; i < copies; i++) {
        if ((int)(rnd() % 100) < sc->loss_pct)
            continue;
        struct pkt *k = malloc(sizeof(*k));
        k->at = now + (u64)sc->delay_min + rnd() % (u32)(sc->delay_max - sc->delay_min + 1);
        k->to_sonar = to_sonar;
        k->len = len;
        memcpy(k->data, p, len);
        k->next = queue;
        queue = k;
    }
}

int ip_send(ip4_t dst, u8 proto, const void *p, size_t len)
{
    CHECK(dst == PEER_IP && proto == 6, "unexpected destination or protocol");
    CHECK(len <= 1480, "segment of %zu bytes exceeds the MTU", len);
    enqueue(false, p, len);
    return 0;
}

/* ------------------------------------------------------------ test server */
static u8 pattern(u32 i) { return (u8)((i * 2654435761u) >> 24); }

static struct {
    int state; /* 0 listen, 1 syn-rcvd, 2 established, 3 fin sent, 4 done */
    u32 iss, una, nxt, max; /* stream offsets are seq - (iss + 1) */
    u32 rcv_nxt;            /* next byte expected from Sonar */
    u32 wnd;                /* Sonar's window, scaled */
    int ws;                 /* Sonar's window scale */
    bool sack_ok;
    u16 cport;
    int dupacks;
    u64 rto_at, last_probe;
    u8 *sent_ok;  /* per segment: delivered to Sonar at least once */
    u64 *sent_at; /* per segment: last transmission */
    u8 *sacked;
    u32 nseg;
    u32 request_bytes;
    bool got_fin;
    u32 retransmits, segs_sent;
} srv;

static void srv_send(u32 seq, u8 flags, const u8 *data, u32 len, bool synopts)
{
    u8 s[1500];
    size_t hl = synopts ? 32 : 20;
    put16(s, PEER_PORT);
    put16(s + 2, srv.cport);
    put32(s + 4, seq);
    put32(s + 8, srv.rcv_nxt);
    s[12] = (u8)((hl / 4) << 4);
    s[13] = flags;
    put16(s + 14, 65535);
    put16(s + 16, 0);
    put16(s + 18, 0);
    if (synopts) {
        u8 *o = s + 20;
        o[0] = 2, o[1] = 4;
        put16(o + 2, MSS);
        o[4] = 1, o[5] = 3, o[6] = 3, o[7] = 7;  /* window scale 7 */
        o[8] = 1, o[9] = 1;
        o[10] = srv.sack_ok ? 4 : 1, o[11] = srv.sack_ok ? 2 : 1; /* SACK permitted or NOPs */
    }
    memcpy(s + hl, data, len);
    put16(s + 16, net_checksum(s, hl + len, net_pseudo_sum(PEER_IP, MY_IP, 6, (u16)(hl + len))));
    enqueue(true, s, hl + len);
}

static void srv_send_segment(u32 k)
{
    u32 off = k * MSS, len = off + MSS > sc->total ? sc->total - off : MSS;
    u8 d[MSS];
    for (u32 i = 0; i < len; i++)
        d[i] = pattern(off + i);
    srv_send(srv.iss + 1 + off, 0x10 | 0x08, d, len, false);
    if (srv.sent_at[k])
        srv.retransmits++;
    srv.sent_at[k] = now ? now : 1;
    srv.segs_sent++;
}

static u32 off_of(u32 seq) { return seq - (srv.iss + 1); }

static void srv_output(void)
{
    if (srv.state != 2)
        return;
    u32 una = off_of(srv.una);
    /* SACK-based recovery: resend holes below the highest SACKed segment */
    if (srv.sack_ok) {
        u32 hi = 0;
        for (u32 k = una / MSS; k < srv.nseg; k++)
            if (srv.sacked[k])
                hi = k;
        for (u32 k = una / MSS; k < hi; k++)
            if (!srv.sacked[k] && now - srv.sent_at[k] >= 8)
                srv_send_segment(k);
    }
    /* new data within Sonar's window (and a fixed 256-segment cap) */
    for (;;) {
        u32 nxt = off_of(srv.nxt);
        if (nxt >= sc->total)
            break;
        u32 len = nxt + MSS > sc->total ? sc->total - nxt : MSS;
        if (nxt + len > una + srv.wnd || nxt - una >= 256 * MSS)
            break;
        srv_send_segment(nxt / MSS);
        srv.nxt += len;
    }
    if (off_of(srv.nxt) >= sc->total && una >= sc->total && srv.state == 2) {
        srv_send(srv.iss + 1 + sc->total, 0x11, NULL, 0, false); /* FIN */
        srv.state = 3;
        srv.rto_at = now + 50;
    }
    /* zero window: probe */
    if (srv.wnd == 0 && una < sc->total && now - srv.last_probe >= 20) {
        srv.last_probe = now;
        srv_send_segment(una / MSS);
    }
}

static void srv_input(const u8 *s, size_t len)
{
    CHECK(len >= 20, "short segment");
    CHECK(net_checksum(s, len, net_pseudo_sum(MY_IP, PEER_IP, 6, (u16)len)) == 0,
          "bad TCP checksum from Sonar");
    size_t hl = (size_t)(s[12] >> 4) * 4;
    u8 flags = s[13];
    u32 seq = get32(s + 4), ack = get32(s + 8);
    u32 dlen = (u32)(len - hl);
    if (flags & 0x04) {
        if (srv.state != 4)
            printf("  (Sonar reset the connection)\n");
        srv.state = 4;
        return;
    }
    /* options */
    u32 sack[4][2];
    int nsack = 0, ws = -1;
    bool sackperm = false;
    for (size_t i = 20; i < hl;) {
        u8 kind = s[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        u8 l = s[i + 1];
        if (kind == 3)
            ws = s[i + 2];
        if (kind == 4)
            sackperm = true;
        if (kind == 5)
            for (int b = 0; b < (l - 2) / 8 && b < 4; b++) {
                sack[b][0] = get32(s + i + 2 + 8 * b);
                sack[b][1] = get32(s + i + 6 + 8 * b);
                nsack++;
            }
        i += l;
    }
    if (flags & 0x02) { /* SYN */
        CHECK(sackperm, "Sonar did not offer SACK");
        CHECK(ws == 5, "Sonar window scale %d", ws);
        srv.ws = ws < 0 ? 0 : ws;
        srv.cport = get16(s);
        srv.rcv_nxt = seq + 1;
        if (srv.state == 0) {
            srv.iss = rnd();
            srv.una = srv.nxt = srv.iss + 1;
        }
        srv.state = 1;
        srv_send(srv.iss, 0x12, NULL, 0, true);
        return;
    }
    if (srv.state == 1 && (flags & 0x10) && ack == srv.iss + 1)
        srv.state = 2;
    if (srv.state < 2)
        return;
    CHECK(!nsack || srv.sack_ok, "SACK blocks sent without SACK being permitted");
    /* request data from Sonar */
    if (dlen && seq == srv.rcv_nxt) {
        srv.rcv_nxt += dlen;
        srv.request_bytes += dlen;
        srv_send(srv.nxt, 0x10, NULL, 0, false);
    }
    if (flags & 0x01) {
        if (seq + dlen == srv.rcv_nxt) {
            srv.rcv_nxt++;
            srv.got_fin = true;
        }
        srv_send(srv.state >= 3 ? srv.iss + 2 + sc->total : srv.nxt, 0x10, NULL, 0, false);
    }
    if (!(flags & 0x10))
        return;
    /* the window Sonar advertises must never shrink its right edge much */
    u32 wnd = (u32)get16(s + 14) << srv.ws;
    CHECK(le(ack, srv.max > srv.nxt ? srv.max : srv.nxt) || srv.state >= 3,
          "Sonar acknowledged data never sent");
    if (lt(srv.una, ack))
        srv.dupacks = 0;
    else if (ack == srv.una && !dlen)
        srv.dupacks++;
    if (lt(srv.una, ack) && le(ack, srv.iss + 1 + sc->total)) {
        srv.una = ack;
        srv.rto_at = now + 60;
    }
    if (srv.state == 3 && ack == srv.iss + 2 + sc->total)
        srv.state = srv.got_fin ? 4 : 3;
    srv.wnd = wnd;
    for (int b = 0; b < nsack; b++) {
        u32 a = off_of(sack[b][0]), e = off_of(sack[b][1]);
        CHECK(lt(ack, sack[b][0]) && lt(sack[b][0], sack[b][1]), "malformed SACK block");
        /* blocks start where a segment starts; the end may fall inside one when
         * Sonar accepted only the part that fit its window */
        CHECK(a % MSS == 0, "SACK block does not start on a segment");
        for (u32 k = a / MSS; k * MSS < e && k < srv.nseg; k++) {
            CHECK(srv.sent_ok[k], "SACK covers segment %u that never arrived", k);
            u32 end = k * MSS + MSS > sc->total ? sc->total : k * MSS + MSS;
            if (le(end, e))
                srv.sacked[k] = 1; /* fully covered */
        }
    }
    if (!srv.sack_ok && srv.dupacks == 3) /* fast retransmit, Reno style */
        srv_send_segment(off_of(srv.una) / MSS);
    srv_output();
}

static void srv_tick(void)
{
    if (srv.state == 2 && srv.rto_at && now >= srv.rto_at && off_of(srv.una) < sc->total &&
        srv.una != srv.nxt) {
        srv_send_segment(off_of(srv.una) / MSS);
        srv.rto_at = now + 60;
    }
    if (srv.state == 3 && now >= srv.rto_at) {
        srv_send(srv.iss + 1 + sc->total, 0x11, NULL, 0, false);
        srv.rto_at = now + 50;
    }
    srv_output();
}

/* ------------------------------------------------------------ event loop */
static void deliver(void)
{
    for (;;) {
        struct pkt **pp = &queue, **best = NULL;
        for (; *pp; pp = &(*pp)->next)
            if ((*pp)->at <= now && (!best || (*pp)->at < (*best)->at))
                best = pp;
        if (!best)
            break;
        struct pkt *k = *best;
        *best = k->next;
        if (k->to_sonar) {
            /* note which data segments reached Sonar */
            size_t hl = (size_t)(k->data[12] >> 4) * 4;
            if (k->len > hl) {
                u32 off = get32(k->data + 4) - (srv.iss + 1);
                if (off / MSS < srv.nseg)
                    srv.sent_ok[off / MSS] = 1;
            }
            tcp_input(PEER_IP, MY_IP, k->data, k->len);
        } else {
            srv_input(k->data, k->len);
        }
        free(k);
    }
}

void net_poll(void)
{
    deliver();
    tcp_flush();
    tcp_tick();
    srv_tick();
}

void net_wait(void)
{
    net_poll();
    now++;
}

int net_wait_check(u64 deadline)
{
    return now >= deadline ? -1 : 0;
}

/* ------------------------------------------------------------ scenarios */
static void run(const struct scenario *s)
{
    sc = s;
    memset(&srv, 0, sizeof(srv));
    srv.sack_ok = s->sack;
    srv.nseg = (s->total + MSS - 1) / MSS;
    srv.sent_ok = calloc(srv.nseg + 1, 1);
    srv.sacked = calloc(srv.nseg + 1, 1);
    srv.sent_at = calloc(srv.nseg + 1, sizeof(u64));
    int fails_before = failures;
    u64 t0 = now;
    int err;
    struct tcp_conn *c = tcp_connect(PEER_IP, PEER_PORT, 30000, &err);
    CHECK(c != NULL, "connect failed: %d", err);
    if (!c)
        return;
    const char req[] = "GET /stream HTTP/1.1\r\n\r\n";
    CHECK(tcp_send(c, req, sizeof(req) - 1) == (ssize_t)(sizeof(req) - 1), "send failed");
    u8 *buf = malloc((size_t)s->read_chunk);
    u32 got = 0;
    bool ok = true;
    for (;;) {
        ssize_t n = tcp_recv(c, buf, (size_t)s->read_chunk, 60000);
        if (n == 0)
            break;
        if (n < 0) {
            CHECK(0, "recv error %ld after %u bytes", n, got);
            ok = false;
            break;
        }
        for (ssize_t i = 0; i < n && ok; i++)
            if (buf[i] != pattern(got + (u32)i)) {
                CHECK(0, "wrong byte at offset %u", got + (u32)i);
                ok = false;
            }
        got += (u32)n;
        for (int p = 0; p < s->read_pause; p++)
            net_wait();
        if (!ok)
            break;
    }
    CHECK(!ok || got == s->total, "received %u of %u bytes", got, s->total);
    CHECK(srv.request_bytes == sizeof(req) - 1, "server saw %u request bytes", srv.request_bytes);
    tcp_close(c);
    for (int i = 0; i < 20000 && (srv.state != 4 || queue); i++)
        net_wait();
    CHECK(srv.got_fin, "server never received Sonar's FIN");
    CHECK(srv.state == 4, "connection did not finish closing (server state %d)", srv.state);
    for (int i = 0; i < 6000; i++) /* TIME_WAIT expires and the connection is freed */
        net_wait();
    printf("%-34s %s  %u bytes in %llu ms simulated, %u segments, %u retransmitted\n", s->name,
           failures == fails_before ? "ok  " : "FAIL", got, (unsigned long long)(now - t0),
           srv.segs_sent, srv.retransmits);
    free(buf);
    free(srv.sent_ok);
    free(srv.sacked);
    free(srv.sent_at);
    while (queue) {
        struct pkt *k = queue;
        queue = k->next;
        free(k);
    }
}

int main(void)
{
    static const struct scenario scenarios[] = {
        { "clean network", 0, 0, 1, 1, true, 4u << 20, 16384, 0 },
        { "5% loss, reordering, SACK", 5, 0, 1, 6, true, 4u << 20, 16384, 0 },
        { "5% loss, reordering, no SACK", 5, 0, 1, 6, false, 2u << 20, 16384, 0 },
        { "20% loss, reordering, SACK", 20, 0, 1, 10, true, 1u << 20, 16384, 0 },
        { "duplicates and reordering", 2, 10, 1, 8, true, 2u << 20, 16384, 0 },
        { "slow reader (zero windows)", 1, 0, 1, 3, true, 3u << 20, 4096, 2 },
        { "odd sizes, 1-byte reads", 3, 2, 1, 4, true, 100001, 1, 0 },
    };
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
        run(&scenarios[i]);
    printf(failures ? "\n%d check(s) FAILED\n" : "\nall TCP tests passed\n", failures);
    return failures ? 1 : 0;
}
