/*
 * TCP client connections (RFC 9293), with:
 *  - MSS and window-scale options (RFC 7323), so large downloads are not
 *    limited to a 64 KiB window;
 *  - retransmission timeout from RTT measurement (RFC 6298, Karn's rule);
 *  - slow start / congestion avoidance / fast retransmit (RFC 5681);
 *  - delayed ACKs: data is acknowledged once per batch of received frames
 *    (at least every second segment when the stack keeps up), or after
 *    20 ms; each acknowledgment is a costly VM exit under virtualization;
 *  - RFC 5961 challenge ACKs for in-window SYN and inexact RST.
 *  - out-of-order segments are kept (written straight into their place in
 *    the receive ring) and reported with SACK (RFC 2018), so a lost packet
 *    costs one retransmission instead of the rest of the window.
 * Received SACK blocks are ignored (Sonar sends little data). Only active
 * opens are supported: Sonar does not run servers.
 */
#include "console.h"
#include "irq.h"
#include "mm.h"
#include "netpriv.h"
#include "random.h"

enum {
    CLOSED, SYN_SENT, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSING, TIME_WAIT, CLOSE_WAIT,
    LAST_ACK,
};
static const char *state_names[] = {
    "CLOSED", "SYN_SENT", "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2", "CLOSING", "TIME_WAIT",
    "CLOSE_WAIT", "LAST_ACK",
};

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define RCVBUF (1u << 20)
#define SNDBUF (64u << 10)
#define RCV_WSCALE 5 /* RCVBUF >> 5 fits the 16-bit window field */
#define MAX_CONNS 16
#define RTO_INIT 1000
#define RTO_MIN 200
#define RTO_MAX 60000
#define MAX_RETRIES 8
#define TIME_WAIT_MS 4000
#define ORPHAN_MS 30000
#define DELACK_MS 20
#define OOO_MAX 16

struct range {
    u32 start, end; /* [start, end) */
};

struct tcp_conn {
    int state;
    ip4_t lip, rip;
    u16 lport, rport;
    /* send side: sbuf holds bytes [snd_una, snd_una + s_len) */
    u32 iss, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2;
    u8 snd_ws, rcv_ws;
    u32 mss, cwnd, ssthresh;
    int dupacks;
    u8 *sbuf;
    u32 s_head, s_len;
    bool fin_queued, fin_acked;
    /* receive side */
    u32 irs, rcv_nxt, rcv_adv;
    u8 *rbuf;
    u32 r_head, r_len;
    bool fin_rcvd;
    /* out-of-order data held in the ring beyond rcv_nxt, sorted */
    struct range ooo[OOO_MAX];
    int n_ooo;
    u32 sack_recent; /* start of the block holding the latest arrival */
    bool sack_ok;    /* peer accepted SACK */
    bool fin_pending; /* a FIN arrived out of order, at fin_seq */
    u32 fin_seq;
    u32 mss_local;   /* largest segment our interface carries */
    int ack_pending;
    bool ack_now; /* send an ACK when the current batch of frames is done */
    u64 ack_deadline;
    /* timers */
    u64 rtx_deadline, tw_deadline, orphan_deadline;
    u32 rto, srtt, rttvar;
    bool rtt_valid, rtt_timing;
    u32 rtt_seq;
    u64 rtt_start;
    int retries;
    int err;
    bool orphan;
};

static struct tcp_conn *conns[MAX_CONNS];

/* ------------------------------------------------------------ helpers */
static u32 rcv_space(struct tcp_conn *c) { return RCVBUF - c->r_len; }

static u16 window_field(struct tcp_conn *c, bool syn)
{
    u32 w = rcv_space(c);
    if (syn)
        return (u16)(w > 65535 ? 65535 : w);
    w >>= c->rcv_ws;
    return (u16)(w > 65535 ? 65535 : w);
}

static void send_raw(ip4_t lip, ip4_t rip, u16 lport, u16 rport, u32 seq, u32 ack, u8 flags,
                     u16 win, const u8 *opts, size_t olen, const u8 *data, size_t dlen,
                     const u8 *data2, size_t dlen2)
{
    u8 seg[1500];
    size_t hlen = 20 + olen, len = hlen + dlen + dlen2;
    if (len > sizeof(seg))
        return;
    put16(seg, lport);
    put16(seg + 2, rport);
    put32(seg + 4, seq);
    put32(seg + 8, ack);
    seg[12] = (u8)((hlen / 4) << 4);
    seg[13] = flags;
    put16(seg + 14, win);
    put16(seg + 16, 0);
    put16(seg + 18, 0);
    if (olen)
        memcpy(seg + 20, opts, olen);
    if (dlen)
        memcpy(seg + hlen, data, dlen);
    if (dlen2)
        memcpy(seg + hlen + dlen, data2, dlen2);
    put16(seg + 16, net_checksum(seg, len, net_pseudo_sum(lip, rip, PROTO_TCP, (u16)len)));
    ip_send(rip, PROTO_TCP, seg, len);
}

/* Send a segment carrying len bytes of the send buffer at offset off from snd_una. */
static void send_segment(struct tcp_conn *c, u32 seq, u32 off, u32 len, u8 flags)
{
    const u8 *d1 = NULL, *d2 = NULL;
    size_t l1 = 0, l2 = 0;
    if (len) {
        u32 start = (c->s_head + off) % SNDBUF;
        l1 = MIN(len, SNDBUF - start);
        d1 = c->sbuf + start;
        if (l1 < len) {
            d2 = c->sbuf;
            l2 = len - l1;
        }
    }
    u8 opts[40];
    size_t olen = 0;
    if (flags & F_SYN) {
        opts[0] = 2, opts[1] = 4; /* MSS */
        put16(opts + 2, (u16)c->mss);
        opts[4] = 1;              /* NOP */
        opts[5] = 3, opts[6] = 3; /* window scale */
        opts[7] = RCV_WSCALE;
        opts[8] = 1, opts[9] = 1; /* NOP NOP */
        opts[10] = 4, opts[11] = 2; /* SACK permitted */
        olen = 12;
    } else if ((flags & F_ACK) && c->sack_ok && c->n_ooo) {
        /* SACK blocks: the most recent arrival first (RFC 2018 section 4) */
        int room = (int)(c->mss_local - len) - 4;
        int nb = MIN(MIN(c->n_ooo, 4), room / 8);
        if (nb > 0) {
            opts[0] = 1, opts[1] = 1, opts[2] = 5, opts[3] = (u8)(2 + 8 * nb);
            int first = 0;
            for (int i = 0; i < c->n_ooo; i++)
                if (c->ooo[i].start == c->sack_recent)
                    first = i;
            int k = 0;
            put32(opts + 4, c->ooo[first].start);
            put32(opts + 8, c->ooo[first].end);
            k = 1;
            for (int i = 0; i < c->n_ooo && k < nb; i++) {
                if (i == first)
                    continue;
                put32(opts + 4 + 8 * k, c->ooo[i].start);
                put32(opts + 8 + 8 * k, c->ooo[i].end);
                k++;
            }
            olen = (size_t)(4 + 8 * nb);
        }
    }
    u16 win = window_field(c, flags & F_SYN);
    if (flags & F_ACK) {
        c->rcv_adv = c->rcv_nxt + ((u32)win << ((flags & F_SYN) ? 0 : c->rcv_ws));
        c->ack_pending = 0;
        c->ack_now = false;
        c->ack_deadline = 0;
    }
    send_raw(c->lip, c->rip, c->lport, c->rport, seq, (flags & F_ACK) ? c->rcv_nxt : 0, flags, win,
             opts, olen, d1, l1, d2, l2);
}

static void send_ack(struct tcp_conn *c) { send_segment(c, c->snd_nxt, 0, 0, F_ACK); }

static void reply_rst(ip4_t src, ip4_t dst, const u8 *h, u32 seglen)
{
    u8 flags = h[13];
    if (flags & F_RST)
        return;
    u16 sport = get16(h), dport = get16(h + 2);
    if (flags & F_ACK)
        send_raw(dst, src, dport, sport, get32(h + 8), 0, F_RST, 0, NULL, 0, NULL, 0, NULL, 0);
    else
        send_raw(dst, src, dport, sport, 0, get32(h + 4) + seglen, F_RST | F_ACK, 0, NULL, 0,
                 NULL, 0, NULL, 0);
}

static void free_conn(struct tcp_conn *c)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i] == c)
            conns[i] = NULL;
    kfree(c->sbuf);
    kfree(c->rbuf);
    kfree(c);
}

/* Orphaned connections are freed by tcp_tick, never while being processed. */
static void set_closed(struct tcp_conn *c, int err)
{
    if (err && !c->err)
        c->err = err;
    c->state = CLOSED;
    c->rtx_deadline = 0;
    c->ack_deadline = 0;
}

static void enter_time_wait(struct tcp_conn *c)
{
    c->state = TIME_WAIT;
    c->rtx_deadline = 0;
    c->tw_deadline = time_ms() + TIME_WAIT_MS;
}

static void arm_rtx(struct tcp_conn *c)
{
    if (!c->rtx_deadline)
        c->rtx_deadline = time_ms() + c->rto;
}

static void rtt_sample(struct tcp_conn *c, u32 r)
{
    if (!c->rtt_valid) {
        c->srtt = r;
        c->rttvar = r / 2;
        c->rtt_valid = true;
    } else {
        u32 diff = c->srtt > r ? c->srtt - r : r - c->srtt;
        c->rttvar = (3 * c->rttvar + diff) / 4;
        c->srtt = (7 * c->srtt + r) / 8;
    }
    u32 rto = c->srtt + MAX(10u, 4 * c->rttvar);
    c->rto = CLAMP(rto, (u32)RTO_MIN, (u32)RTO_MAX);
}

/* Send whatever the windows allow (data, then FIN when everything is queued). */
static void output(struct tcp_conn *c)
{
    if (c->state != ESTABLISHED && c->state != CLOSE_WAIT && c->state != FIN_WAIT_1 &&
        c->state != CLOSING && c->state != LAST_ACK)
        return;
    u32 wnd = MIN(c->snd_wnd, c->cwnd);
    for (;;) {
        u32 off = c->snd_nxt - c->snd_una;
        if (off > c->s_len)
            break; /* FIN already sent */
        u32 avail = c->s_len - off;
        u32 room = off < wnd ? wnd - off : 0;
        u32 n = MIN(MIN(avail, room), c->mss);
        bool fin = c->fin_queued && off + n == c->s_len;
        if (!n && !fin)
            break;
        u8 flags = F_ACK | (n ? F_PSH : 0) | (fin ? F_FIN : 0);
        if (!c->rtt_timing && n) {
            c->rtt_timing = true;
            c->rtt_seq = c->snd_nxt;
            c->rtt_start = time_ms();
        }
        send_segment(c, c->snd_nxt, off, n, flags);
        c->snd_nxt += n + (fin ? 1 : 0);
        if (seq_gt(c->snd_nxt, c->snd_max))
            c->snd_max = c->snd_nxt;
        arm_rtx(c);
        if (fin)
            break;
    }
    /* zero window with data waiting: the retransmit timer sends probes */
    if (c->s_len > c->snd_nxt - c->snd_una && !c->snd_wnd)
        arm_rtx(c);
}

static struct tcp_conn *lookup(ip4_t rip, u16 rport, u16 lport)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        struct tcp_conn *c = conns[i];
        if (c && c->state != CLOSED && c->rip == rip && c->rport == rport && c->lport == lport)
            return c;
    }
    return NULL;
}

static void parse_options(const u8 *o, size_t len, u32 *mss, int *ws, bool *sack)
{
    size_t i = 0;
    while (i < len) {
        u8 kind = o[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        if (i + 1 >= len || o[i + 1] < 2 || i + o[i + 1] > len)
            break;
        u8 l = o[i + 1];
        if (kind == 2 && l == 4)
            *mss = get16(o + i + 2);
        else if (kind == 3 && l == 3)
            *ws = MIN(o[i + 2], 14);
        else if (kind == 4 && l == 2)
            *sack = true;
        i += l;
    }
}

/* ------------------------------------------------------------ input */
/* Write n bytes received at sequence seq (rcv_nxt <= seq, within the window)
 * into their place in the receive ring. */
static void ring_put(struct tcp_conn *c, u32 seq, const u8 *data, u32 n)
{
    u32 pos = (c->r_head + c->r_len + (seq - c->rcv_nxt)) % RCVBUF;
    u32 first = MIN(n, RCVBUF - pos);
    memcpy(c->rbuf + pos, data, first);
    memcpy(c->rbuf, data + first, n - first);
}

/* Record [s, e) as held out of order, merging with neighbouring blocks. */
static void ooo_add(struct tcp_conn *c, u32 s, u32 e)
{
    for (int i = 0; i < c->n_ooo;) {
        struct range *r = &c->ooo[i];
        if (seq_le(r->start, e) && seq_le(s, r->end)) {
            if (seq_lt(r->start, s))
                s = r->start;
            if (seq_gt(r->end, e))
                e = r->end;
            c->n_ooo--;
            memmove(r, r + 1, sizeof(*r) * (size_t)(c->n_ooo - i));
            continue;
        }
        i++;
    }
    if (c->n_ooo == OOO_MAX)
        return; /* too fragmented: forget this block (the peer resends it) */
    int at = 0;
    while (at < c->n_ooo && seq_lt(c->ooo[at].start, s))
        at++;
    memmove(c->ooo + at + 1, c->ooo + at, sizeof(c->ooo[0]) * (size_t)(c->n_ooo - at));
    c->ooo[at].start = s;
    c->ooo[at].end = e;
    c->n_ooo++;
    c->sack_recent = s;
}
static void process_ack(struct tcp_conn *c, u32 seq, u32 ack, u32 win, u32 dlen, u8 flags)
{
    if (seq_gt(ack, c->snd_max)) {
        send_ack(c); /* acknowledges something not yet sent */
        return;
    }
    u32 flight_before = c->snd_max - c->snd_una;
    if (seq_gt(ack, c->snd_una)) {
        u32 acked = ack - c->snd_una;
        u32 data = MIN(acked, c->s_len);
        c->s_head = (c->s_head + data) % SNDBUF;
        c->s_len -= data;
        if (c->fin_queued && acked > data)
            c->fin_acked = true;
        c->snd_una = ack;
        if (seq_lt(c->snd_nxt, c->snd_una))
            c->snd_nxt = c->snd_una;
        if (c->rtt_timing && seq_gt(ack, c->rtt_seq)) {
            rtt_sample(c, (u32)(time_ms() - c->rtt_start));
            c->rtt_timing = false;
        }
        /* congestion control */
        if (c->dupacks >= 3)
            c->cwnd = c->ssthresh; /* leave fast recovery */
        else if (c->cwnd < c->ssthresh)
            c->cwnd += MIN(data, c->mss);
        else
            c->cwnd += MAX(1u, c->mss * c->mss / c->cwnd);
        c->dupacks = 0;
        c->retries = 0;
        c->rtx_deadline = 0;
        if (c->snd_una != c->snd_max)
            arm_rtx(c);
    } else if (ack == c->snd_una && !dlen && !(flags & (F_SYN | F_FIN)) && flight_before &&
               (win << c->snd_ws) == c->snd_wnd) {
        if (++c->dupacks == 3) {
            /* fast retransmit */
            c->ssthresh = MAX(flight_before / 2, 2 * c->mss);
            c->cwnd = c->ssthresh + 3 * c->mss;
            c->rtt_timing = false;
            u32 n = MIN(c->s_len, c->mss);
            bool fin = c->fin_queued && n == c->s_len;
            send_segment(c, c->snd_una, 0, n, F_ACK | (fin ? F_FIN : 0));
        } else if (c->dupacks > 3) {
            c->cwnd += c->mss;
        }
    }
    /* send window update (RFC 9293 3.10.7.4) */
    if (seq_lt(c->snd_wl1, seq) || (c->snd_wl1 == seq && seq_le(c->snd_wl2, ack))) {
        c->snd_wnd = win << c->snd_ws;
        c->snd_wl1 = seq;
        c->snd_wl2 = ack;
    }
    if (c->fin_acked) {
        if (c->state == FIN_WAIT_1)
            c->state = FIN_WAIT_2;
        else if (c->state == CLOSING)
            enter_time_wait(c);
        else if (c->state == LAST_ACK)
            set_closed(c, 0);
    }
}

void tcp_input(ip4_t src, ip4_t dst, const u8 *h, size_t len)
{
    if (len < 20 || net_checksum(h, len, net_pseudo_sum(src, dst, PROTO_TCP, (u16)len)) != 0)
        return;
    size_t hlen = (size_t)(h[12] >> 4) * 4;
    if (hlen < 20 || hlen > len)
        return;
    u16 sport = get16(h), dport = get16(h + 2);
    u32 seq = get32(h + 4), ack = get32(h + 8);
    u8 flags = h[13];
    u32 win = get16(h + 14);
    const u8 *data = h + hlen;
    u32 dlen = (u32)(len - hlen);
    u32 seglen = dlen + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0);

    struct tcp_conn *c = lookup(src, sport, dport);
    if (!c) {
        reply_rst(src, dst, h, seglen);
        return;
    }

    if (c->state == SYN_SENT) {
        bool ack_ok = (flags & F_ACK) && ack == c->iss + 1;
        if ((flags & F_ACK) && !ack_ok) {
            reply_rst(src, dst, h, seglen);
            return;
        }
        if (flags & F_RST) {
            if (ack_ok)
                set_closed(c, NET_EREFUSED);
            return;
        }
        if (!(flags & F_SYN) || !ack_ok)
            return; /* simultaneous open is not supported */
        u32 mss = 536;
        int ws = -1;
        bool sack = false;
        parse_options(h + 20, hlen - 20, &mss, &ws, &sack);
        c->sack_ok = sack;
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->mss = MAX(64u, MIN(c->mss, mss));
        if (ws >= 0) {
            c->snd_ws = (u8)ws;
            c->rcv_ws = RCV_WSCALE;
        } else {
            c->snd_ws = c->rcv_ws = 0;
        }
        c->snd_wnd = win; /* never scaled in a SYN */
        c->snd_wl1 = seq;
        c->snd_wl2 = ack;
        c->cwnd = 10 * c->mss;
        c->ssthresh = 0xffffffff;
        if (c->rtt_timing) {
            rtt_sample(c, (u32)(time_ms() - c->rtt_start));
            c->rtt_timing = false;
        }
        c->retries = 0;
        c->rtx_deadline = 0;
        c->state = ESTABLISHED;
        send_ack(c);
        return;
    }

    /* synchronized states: acceptability test (RFC 9293 3.10.7.4) */
    u32 rwnd = c->rcv_adv - c->rcv_nxt;
    if (seq_lt(c->rcv_adv, c->rcv_nxt))
        rwnd = 0;
    bool acceptable;
    if (seglen == 0)
        acceptable = rwnd == 0 ? seq == c->rcv_nxt
                               : seq_ge(seq, c->rcv_nxt) && seq_lt(seq, c->rcv_nxt + rwnd);
    else
        acceptable = rwnd != 0 && ((seq_ge(seq, c->rcv_nxt) && seq_lt(seq, c->rcv_nxt + rwnd)) ||
                                   (seq_ge(seq + seglen - 1, c->rcv_nxt) &&
                                    seq_lt(seq + seglen - 1, c->rcv_nxt + rwnd)));
    if (!acceptable) {
        if (!(flags & F_RST))
            send_ack(c);
        return;
    }
    if (flags & F_RST) {
        if (seq == c->rcv_nxt)
            set_closed(c, (c->state == ESTABLISHED || c->state == FIN_WAIT_1 ||
                           c->state == FIN_WAIT_2 || c->state == CLOSE_WAIT)
                              ? NET_ERESET
                              : 0);
        else
            send_ack(c); /* challenge ACK */
        return;
    }
    if (flags & F_SYN) {
        send_ack(c); /* challenge ACK */
        return;
    }
    if (!(flags & F_ACK))
        return;
    process_ack(c, seq, ack, win, dlen, flags);
    if (c->state == CLOSED)
        return;

    /* trim data already received */
    if (seq_lt(seq, c->rcv_nxt)) {
        u32 skip = c->rcv_nxt - seq;
        if (skip > dlen) {
            /* only a duplicate FIN remains */
            if (flags & F_FIN)
                send_ack(c);
            return;
        }
        data += skip;
        dlen -= skip;
        seq = c->rcv_nxt;
    }
    bool can_receive = c->state == ESTABLISHED || c->state == FIN_WAIT_1 || c->state == FIN_WAIT_2;
    if (dlen && can_receive && c->orphan) {
        /* nobody will read it: reset (RFC 2525 2.17) */
        send_raw(c->lip, c->rip, c->lport, c->rport, c->snd_nxt, 0, F_RST, 0, NULL, 0, NULL, 0,
                 NULL, 0);
        set_closed(c, 0);
        return;
    }
    if (can_receive && (dlen || (flags & F_FIN))) {
        u32 off = seq - c->rcv_nxt, space = rcv_space(c);
        u32 n = off < space ? MIN(dlen, space - off) : 0;
        if (n < dlen)
            flags &= (u8)~F_FIN; /* the FIN lies beyond what we accepted */
        if (n)
            ring_put(c, seq, data, n);
        if (off) {
            /* out of order: keep it, and send an immediate duplicate ACK
             * carrying SACK so the peer retransmits only the gap */
            if (n)
                ooo_add(c, seq, seq + n);
            if (flags & F_FIN) {
                c->fin_pending = true;
                c->fin_seq = seq + n;
            }
            send_ack(c);
            output(c);
            return;
        }
        bool filled = c->n_ooo > 0;
        c->r_len += n;
        c->rcv_nxt += n;
        while (c->n_ooo && seq_le(c->ooo[0].start, c->rcv_nxt)) {
            if (seq_gt(c->ooo[0].end, c->rcv_nxt)) {
                u32 more = c->ooo[0].end - c->rcv_nxt;
                c->r_len += more;
                c->rcv_nxt += more;
            }
            c->n_ooo--;
            memmove(c->ooo, c->ooo + 1, sizeof(c->ooo[0]) * (size_t)c->n_ooo);
        }
        if ((flags & F_FIN) && seq + n == c->rcv_nxt) {
            c->fin_pending = true;
            c->fin_seq = c->rcv_nxt;
        }
        if (n) {
            c->ack_pending++;
            if (!c->ack_deadline)
                c->ack_deadline = time_ms() + DELACK_MS;
            if (filled)
                send_ack(c); /* a gap was filled: acknowledge at once (RFC 5681 4.2) */
            else if (c->ack_pending >= 2 || (flags & F_PSH) || n < dlen)
                c->ack_now = true; /* sent by tcp_flush() after this batch */
        }
    }
    if (c->fin_pending && !c->fin_rcvd && c->fin_seq == c->rcv_nxt) {
        c->rcv_nxt++;
        c->fin_rcvd = true;
        if (c->state == ESTABLISHED)
            c->state = CLOSE_WAIT;
        else if (c->state == FIN_WAIT_1)
            c->state = c->fin_acked ? TIME_WAIT : CLOSING;
        else if (c->state == FIN_WAIT_2)
            c->state = TIME_WAIT;
        if (c->state == TIME_WAIT)
            enter_time_wait(c);
        send_ack(c);
    } else if ((flags & F_FIN) && c->state == TIME_WAIT) {
        send_ack(c); /* retransmitted FIN */
        c->tw_deadline = time_ms() + TIME_WAIT_MS;
    }
    output(c);
}

/* Called after each batch of received frames. */
void tcp_flush(void)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        struct tcp_conn *c = conns[i];
        if (c && c->ack_now && c->state != CLOSED)
            send_ack(c);
    }
}

/* ------------------------------------------------------------ timers */
void tcp_tick(void)
{
    u64 now = time_ms();
    for (int i = 0; i < MAX_CONNS; i++) {
        struct tcp_conn *c = conns[i];
        if (!c)
            continue;
        if (c->state == TIME_WAIT) {
            if (now >= c->tw_deadline)
                set_closed(c, 0);
            continue;
        }
        if (c->state == CLOSED) {
            if (c->orphan)
                free_conn(c);
            continue;
        }
        if (c->orphan && c->orphan_deadline && now >= c->orphan_deadline) {
            send_raw(c->lip, c->rip, c->lport, c->rport, c->snd_nxt, 0, F_RST, 0, NULL, 0, NULL,
                     0, NULL, 0);
            set_closed(c, 0);
            continue;
        }
        if (c->ack_deadline && now >= c->ack_deadline)
            send_ack(c);
        if (!c->rtx_deadline || now < c->rtx_deadline)
            continue;
        c->rtx_deadline = 0;
        bool probe = c->snd_nxt == c->snd_una && c->state != SYN_SENT;
        if (!probe && ++c->retries > MAX_RETRIES) {
            send_raw(c->lip, c->rip, c->lport, c->rport, c->snd_nxt, 0, F_RST, 0, NULL, 0, NULL,
                     0, NULL, 0);
            set_closed(c, NET_ETIMEDOUT);
            continue;
        }
        c->rto = MIN(c->rto * 2, (u32)RTO_MAX);
        c->rtt_timing = false; /* Karn: no samples from retransmissions */
        if (c->state == SYN_SENT) {
            send_segment(c, c->iss, 0, 0, F_SYN);
            arm_rtx(c);
            continue;
        }
        if (c->snd_nxt != c->snd_una) {
            u32 flight = c->snd_max - c->snd_una;
            c->ssthresh = MAX(flight / 2, 2 * c->mss);
            c->cwnd = c->mss;
            c->dupacks = 0;
            c->snd_nxt = c->snd_una; /* go back N */
            output(c);
            arm_rtx(c);
        } else if (c->s_len && !c->snd_wnd) {
            /* zero-window probe: one byte beyond the window (RFC 9293 3.8.6.1) */
            send_segment(c, c->snd_una, 0, 1, F_ACK);
            c->snd_nxt = c->snd_una + 1;
            if (seq_gt(c->snd_nxt, c->snd_max))
                c->snd_max = c->snd_nxt;
            arm_rtx(c);
        }
    }
}

/* ------------------------------------------------------------ API */
struct tcp_conn *tcp_connect(ip4_t dst, u16 port, u32 timeout_ms, int *err)
{
    struct netif *nif = net_iface();
    int slot = -1;
    for (int i = 0; i < MAX_CONNS; i++)
        if (!conns[i]) {
            slot = i;
            break;
        }
    if (!nif || slot < 0) {
        *err = nif ? NET_ENOMEM : NET_ENODEV;
        return NULL;
    }
    if (!nif->ip && (dst >> 24) != 127) {
        *err = NET_EUNREACH;
        return NULL;
    }
    struct tcp_conn *c = kzalloc(sizeof(*c));
    if (c) {
        c->sbuf = kmalloc(SNDBUF);
        c->rbuf = kmalloc(RCVBUF);
    }
    if (!c || !c->sbuf || !c->rbuf) {
        if (c) {
            kfree(c->sbuf);
            kfree(c->rbuf);
            kfree(c);
        }
        *err = NET_ENOMEM;
        return NULL;
    }
    c->rip = dst;
    c->rport = port;
    c->lip = (dst >> 24) == 127 ? dst : nif->ip;
    for (;;) {
        c->lport = (u16)(49152 + random_u32() % 16384);
        bool used = false;
        for (int i = 0; i < MAX_CONNS; i++)
            if (conns[i] && conns[i]->lport == c->lport)
                used = true;
        if (!used)
            break;
    }
    c->mss = c->mss_local = (u32)(nif->mtu - 40);
    c->rto = RTO_INIT;
    c->iss = random_u32();
    c->snd_una = c->iss;
    c->snd_nxt = c->snd_max = c->iss + 1;
    c->state = SYN_SENT;
    conns[slot] = c;
    c->rtt_timing = true;
    c->rtt_start = time_ms();
    send_segment(c, c->iss, 0, 0, F_SYN);
    arm_rtx(c);
    u64 deadline = time_ms() + timeout_ms;
    while (c->state == SYN_SENT) {
        int r = net_wait_check(deadline);
        if (r != NET_OK) {
            c->err = r;
            break;
        }
        net_wait();
    }
    if (c->state != ESTABLISHED) {
        *err = c->err ? c->err : NET_ETIMEDOUT;
        free_conn(c);
        return NULL;
    }
    *err = NET_OK;
    return c;
}

ssize_t tcp_send(struct tcp_conn *c, const void *buf, size_t len)
{
    const u8 *p = buf;
    size_t done = 0;
    while (done < len) {
        if (c->err)
            return c->err;
        if (c->fin_queued || (c->state != ESTABLISHED && c->state != CLOSE_WAIT))
            return NET_ECLOSED;
        u32 space = SNDBUF - c->s_len;
        if (!space) {
            if (console_interrupted())
                return NET_EINTR;
            net_wait();
            continue;
        }
        u32 n = (u32)MIN((size_t)space, len - done);
        u32 pos = (c->s_head + c->s_len) % SNDBUF;
        u32 first = MIN(n, SNDBUF - pos);
        memcpy(c->sbuf + pos, p + done, first);
        memcpy(c->sbuf, p + done + first, n - first);
        c->s_len += n;
        done += n;
        output(c);
    }
    net_poll();
    return (ssize_t)done;
}

ssize_t tcp_recv(struct tcp_conn *c, void *buf, size_t len, u32 timeout_ms)
{
    u64 deadline = time_ms() + timeout_ms;
    for (;;) {
        net_poll();
        if (c->r_len) {
            u32 n = (u32)MIN((size_t)c->r_len, len);
            u32 first = MIN(n, RCVBUF - c->r_head);
            memcpy(buf, c->rbuf + c->r_head, first);
            memcpy((u8 *)buf + first, c->rbuf, n - first);
            c->r_head = (c->r_head + n) % RCVBUF;
            c->r_len -= n;
            /* Window update once the peer's view of the window has shrunk to
             * half the buffer and reading would at least double it (receiver
             * silly-window avoidance, RFC 9293 3.8.6.2.2; Linux uses the same
             * rule). Otherwise the next ACK carries the new window. */
            u32 adv = seq_gt(c->rcv_adv, c->rcv_nxt) ? c->rcv_adv - c->rcv_nxt : 0;
            u32 now_wnd = (rcv_space(c) >> c->rcv_ws) << c->rcv_ws;
            if (c->state != CLOSED && adv <= RCVBUF / 2 && now_wnd >= 2 * adv &&
                now_wnd - adv >= c->mss)
                send_ack(c);
            return (ssize_t)n;
        }
        if (c->fin_rcvd)
            return 0;
        if (c->err)
            return c->err;
        if (c->state == CLOSED)
            return NET_ECLOSED;
        int r = net_wait_check(deadline);
        if (r != NET_OK)
            return r;
        net_wait();
    }
}

void tcp_close(struct tcp_conn *c)
{
    if (!c)
        return;
    c->orphan = true;
    c->orphan_deadline = time_ms() + ORPHAN_MS;
    switch (c->state) {
    case ESTABLISHED:
    case CLOSE_WAIT:
        if (c->r_len) {
            /* unread data: abort (RFC 2525 2.17) */
            send_raw(c->lip, c->rip, c->lport, c->rport, c->snd_nxt, 0, F_RST, 0, NULL, 0, NULL,
                     0, NULL, 0);
            set_closed(c, 0);
            return;
        }
        c->fin_queued = true;
        c->state = c->state == ESTABLISHED ? FIN_WAIT_1 : LAST_ACK;
        output(c);
        net_poll();
        break;
    case SYN_SENT:
    case CLOSED:
        set_closed(c, 0);
        break;
    default:
        break; /* already closing */
    }
}

void tcp_list(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CONNS; i++) {
        struct tcp_conn *c = conns[i];
        if (!c)
            continue;
        char l[16], r[16], lp[24], rp[24];
        ip4_format(c->lip, l, sizeof(l));
        ip4_format(c->rip, r, sizeof(r));
        snprintf(lp, sizeof(lp), "%s:%u", l, c->lport);
        snprintf(rp, sizeof(rp), "%s:%u", r, c->rport);
        printk("tcp   %-21s %-21s %-11s rto %u ms\n", lp, rp, state_names[c->state], c->rto);
        n++;
    }
    if (!n)
        printk("(no TCP connections)\n");
}
