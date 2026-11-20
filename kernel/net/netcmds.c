/* Network shell commands: ifconfig, dhcp, ping, host, arp, netstat, fetch. */
#include "block.h"
#include "config.h"
#include "console.h"
#include "fs.h"
#include "irq.h"
#include "mm.h"
#include "netpriv.h"
#include "shell.h"

static void fmt_bytes(u64 b, char *out, size_t n) { blk_format_size(b, out, n); }

static int need_iface(struct netif **nif)
{
    *nif = net_iface();
    if (!*nif) {
        printk("no network interface (QEMU: -device virtio-net-device,netdev=n0 "
               "-netdev user,id=n0)\n");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ ifconfig / dhcp */
static void show_iface(struct netif *nif)
{
    printk("%s: link %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, MTU %d\n", nif->name,
           nif->link_up ? "up" : "down", nif->mac[0], nif->mac[1], nif->mac[2], nif->mac[3],
           nif->mac[4], nif->mac[5], nif->mtu);
    if (nif->ip) {
        char ip[16], gw[16], dns[16];
        ip4_format(nif->ip, ip, sizeof(ip));
        ip4_format(nif->gw, gw, sizeof(gw));
        ip4_format(nif->dns, dns, sizeof(dns));
        printk("      inet %s/%d  gateway %s  dns %s", ip, __builtin_popcount(nif->mask),
               nif->gw ? gw : "none", nif->dns ? dns : "none");
        if (nif->dhcp) {
            u64 now = time_ms();
            u64 left = nif->lease_end_ms > now ? (nif->lease_end_ms - now) / 1000 : 0;
            printk("  (DHCP, lease %llu h %llu min left)", (unsigned long long)(left / 3600),
                   (unsigned long long)(left % 3600 / 60));
        } else {
            printk("  (static)");
        }
        printk("\n");
    } else {
        printk("      no IPv4 address (run 'dhcp' or 'ifconfig static ...')\n");
    }
    char rx[24], tx[24];
    fmt_bytes(nif->rx_bytes, rx, sizeof(rx));
    fmt_bytes(nif->tx_bytes, tx, sizeof(tx));
    printk("      rx %llu packets (%s), tx %llu packets (%s), dropped %llu/%llu\n",
           (unsigned long long)nif->rx_packets, rx, (unsigned long long)nif->tx_packets, tx,
           (unsigned long long)nif->rx_dropped, (unsigned long long)nif->tx_dropped);
}

static int apply_config(void)
{
    if (!config_save())
        printk("(could not save %s; the setting lasts until reboot)\n", CONFIG_PATH);
    int r = net_configure(false);
    struct netif *nif = net_iface();
    if (r == NET_OK && nif)
        show_iface(nif);
    else
        printk("ifconfig: %s\n", net_strerror(r));
    return r == NET_OK ? 0 : 1;
}

static int cmd_ifconfig(int argc, char **argv)
{
    struct netif *nif;
    if (need_iface(&nif))
        return 1;
    if (argc == 1) {
        show_iface(nif);
        return 0;
    }
    if (!strcmp(argv[1], "dhcp")) {
        config_set("net.mode", "dhcp");
        return apply_config();
    }
    if (!strcmp(argv[1], "off")) {
        config_set("net.mode", "off");
        return apply_config();
    }
    if (!strcmp(argv[1], "static") && argc >= 4) {
        ip4_t gw, dns = 0;
        char addr[32];
        strlcpy(addr, argv[2], sizeof(addr));
        char *slash = strchr(addr, '/');
        if (slash)
            *slash = 0;
        ip4_t ip;
        if (!slash || !ip4_parse(addr, &ip) || !ip4_parse(argv[3], &gw) ||
            (argc >= 5 && !ip4_parse(argv[4], &dns))) {
            printk("ifconfig: expected ADDRESS/PREFIX GATEWAY [DNS], e.g. "
                   "192.168.1.20/24 192.168.1.1 1.1.1.1\n");
            return 1;
        }
        config_set("net.mode", "static");
        config_set("net.address", argv[2]);
        config_set("net.gateway", argv[3]);
        config_set("net.dns", argc >= 5 ? argv[4] : argv[3]);
        return apply_config();
    }
    printk("usage: ifconfig [dhcp | off | static ADDRESS/PREFIX GATEWAY [DNS]]\n");
    return 1;
}
SHELL_COMMAND(ifconfig, "ifconfig", "ifconfig [dhcp | off | static ADDR/PREFIX GW [DNS]]",
              "show or configure the network interface", cmd_ifconfig);

static int cmd_dhcp(int argc, char **argv)
{
    struct netif *nif;
    if (need_iface(&nif))
        return 1;
    printk("dhcp: requesting an address on %s...\n", nif->name);
    config_set("net.mode", "dhcp");
    return apply_config();
}
SHELL_COMMAND(dhcp, "dhcp", "dhcp", "get a network address from DHCP", cmd_dhcp);

/* ------------------------------------------------------------ ping / host */
static int cmd_ping(int argc, char **argv)
{
    const char *target = NULL;
    long count = 4;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            count = strtol(argv[++i], NULL, 10);
        else
            target = argv[i];
    }
    if (!target || count < 1) {
        printk("usage: ping HOST [-c COUNT]\n");
        return 1;
    }
    ip4_t dst;
    int r = dns_resolve(target, &dst);
    if (r != NET_OK) {
        printk("ping: %s: %s\n", target, net_strerror(r));
        return 1;
    }
    char ip[16];
    ip4_format(dst, ip, sizeof(ip));
    printk("PING %s (%s): 56 data bytes\n", target, ip);
    int sent = 0, got = 0;
    u64 sum = 0, mn = ~0ULL, mx = 0;
    for (long seq = 1; seq <= count; seq++) {
        u64 t0 = time_ms();
        int ttl = 0;
        int us = icmp_ping(dst, (u16)seq, 56, 2000, &ttl);
        sent++;
        if (us == NET_EINTR)
            break;
        if (us >= 0) {
            got++;
            sum += (u64)us;
            mn = MIN(mn, (u64)us);
            mx = MAX(mx, (u64)us);
            printk("64 bytes from %s: icmp_seq=%ld ttl=%d time=%d.%03d ms\n", ip, seq, ttl,
                   us / 1000, us % 1000);
        } else {
            printk("from %s: icmp_seq=%ld %s\n", ip, seq, net_strerror(us));
        }
        if (seq < count) { /* one per second */
            u64 next = t0 + 1000;
            bool stop = false;
            while (time_ms() < next && !stop) {
                if (net_wait_check(next) == NET_EINTR)
                    stop = true;
                else
                    net_wait();
            }
            if (stop)
                break;
        }
    }
    printk("--- %s ping statistics ---\n%d packets transmitted, %d received, %d%% packet loss\n",
           target, sent, got, sent ? (sent - got) * 100 / sent : 0);
    if (got)
        printk("round-trip min/avg/max = %llu.%03llu/%llu.%03llu/%llu.%03llu ms\n",
               (unsigned long long)(mn / 1000), (unsigned long long)(mn % 1000),
               (unsigned long long)(sum / got / 1000), (unsigned long long)(sum / got % 1000),
               (unsigned long long)(mx / 1000), (unsigned long long)(mx % 1000));
    return got ? 0 : 1;
}
SHELL_COMMAND(ping, "ping", "ping HOST [-c COUNT]", "send ICMP echo requests", cmd_ping);

static int cmd_host(int argc, char **argv)
{
    if (argc != 2) {
        printk("usage: host NAME\n");
        return 1;
    }
    ip4_t a;
    int r = dns_resolve(argv[1], &a);
    if (r != NET_OK) {
        printk("host: %s: %s\n", argv[1], net_strerror(r));
        return 1;
    }
    char ip[16];
    ip4_format(a, ip, sizeof(ip));
    printk("%s has address %s\n", argv[1], ip);
    return 0;
}
SHELL_COMMAND(host, "host", "host NAME", "look up a host name (DNS)", cmd_host);

static int cmd_arp(int argc, char **argv)
{
    arp_list();
    return 0;
}
SHELL_COMMAND(arp, "arp", "arp", "show the ARP cache", cmd_arp);

static int cmd_netstat(int argc, char **argv)
{
    tcp_list();
    udp_list();
    return 0;
}
SHELL_COMMAND(netstat, "netstat", "netstat", "show network connections", cmd_netstat);

/* ------------------------------------------------------------ HTTP client */
struct reader {
    struct tcp_conn *c;
    u8 buf[16384];
    size_t pos, len;
    int err;
};

static bool fill(struct reader *r)
{
    if (r->pos < r->len)
        return true;
    ssize_t n = tcp_recv(r->c, r->buf, sizeof(r->buf), 30000);
    if (n <= 0) {
        r->err = n < 0 ? (int)n : NET_ECLOSED;
        return false;
    }
    r->pos = 0;
    r->len = (size_t)n;
    return true;
}

/* Read a CRLF-terminated line (without the line ending). */
static bool read_line(struct reader *r, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        if (!fill(r))
            return false;
        char ch = (char)r->buf[r->pos++];
        if (ch == '\n')
            break;
        if (n + 1 < cap)
            out[n++] = ch;
    }
    if (n && out[n - 1] == '\r')
        n--;
    out[n] = 0;
    return true;
}

struct url {
    char host[128];
    u16 port;
    char path[512];
};

static bool parse_url(const char *s, struct url *u, const struct url *base)
{
    if (s[0] == '/' && base) { /* absolute path on the same host */
        *u = *base;
        strlcpy(u->path, s, sizeof(u->path));
        return true;
    }
    if (strncmp(s, "http://", 7))
        return false;
    s += 7;
    const char *slash = strchr(s, '/');
    size_t hl = slash ? (size_t)(slash - s) : strlen(s);
    if (!hl || hl >= sizeof(u->host))
        return false;
    memcpy(u->host, s, hl);
    u->host[hl] = 0;
    u->port = 80;
    char *colon = strchr(u->host, ':');
    if (colon) {
        *colon = 0;
        long p = strtol(colon + 1, NULL, 10);
        if (p <= 0 || p > 65535)
            return false;
        u->port = (u16)p;
    }
    strlcpy(u->path, slash ? slash : "/", sizeof(u->path));
    return true;
}

static bool header_is(const char *line, const char *name, const char **value)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++)
        if (tolower((u8)line[i]) != name[i])
            return false;
    if (line[n] != ':')
        return false;
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    *value = v;
    return true;
}

struct sink {
    int fd;            /* file, or -1 for the console */
    u64 done, total;   /* total 0 = unknown */
    u64 t0, last_print;
    bool progress;
};

static void show_progress(struct sink *s, bool final)
{
    u64 now = time_ms();
    if (!final && now - s->last_print < 250)
        return;
    s->last_print = now;
    char a[24], b[24], rate[24];
    fmt_bytes(s->done, a, sizeof(a));
    u64 el = now - s->t0;
    fmt_bytes(el ? s->done * 1000 / el : 0, rate, sizeof(rate));
    if (s->total) {
        fmt_bytes(s->total, b, sizeof(b));
        printk("\r  %s / %s  %3llu%%  %s/s   ", a, b,
               (unsigned long long)(s->done * 100 / s->total), rate);
    } else {
        printk("\r  %s  %s/s   ", a, rate);
    }
}

static int sink_write(struct sink *s, const u8 *p, size_t n)
{
    if (s->fd >= 0) {
        ssize_t w = fs_write(s->fd, p, n);
        if (w != (ssize_t)n) {
            printk("\nfetch: write failed: %s\n", fs_strerror(w < 0 ? (int)w : FS_ENOSPC));
            return -1;
        }
    } else {
        console_write((const char *)p, n);
    }
    s->done += n;
    if (s->progress)
        show_progress(s, false);
    return 0;
}

/* Copy n bytes (or until end of stream if n == ~0) from the reader to the sink. */
static int copy_body(struct reader *r, struct sink *s, u64 n)
{
    while (n) {
        if (!fill(r))
            return n == ~0ULL && r->err == NET_ECLOSED ? 0 : -1;
        size_t take = r->len - r->pos;
        if (take > n)
            take = (size_t)n;
        if (sink_write(s, r->buf + r->pos, take) < 0)
            return -2;
        r->pos += take;
        if (n != ~0ULL)
            n -= take;
    }
    return 0;
}

static int copy_chunked(struct reader *r, struct sink *s)
{
    char line[128];
    for (;;) {
        if (!read_line(r, line, sizeof(line)))
            return -1;
        u64 size = strtoull(line, NULL, 16);
        if (!size)
            break;
        int rc = copy_body(r, s, size);
        if (rc < 0)
            return rc;
        if (!read_line(r, line, sizeof(line))) /* CRLF after the chunk */
            return -1;
    }
    while (read_line(r, line, sizeof(line)) && line[0]) /* trailers */
        ;
    return 0;
}

static int cmd_fetch(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printk("usage: fetch http://HOST[:PORT]/PATH [FILE]\n"
               "  Downloads with HTTP/1.1. Without FILE the body is printed.\n"
               "  Example (model served from the host machine running QEMU):\n"
               "    fetch http://10.0.2.2:8000/model.gguf /models/model.gguf\n");
        return 1;
    }
    struct url u;
    if (!strncmp(argv[1], "https://", 8)) {
        printk("fetch: HTTPS is not supported yet (Sonar has no TLS). Download on another "
               "computer and serve it over http://, or attach it as a disk.\n");
        return 1;
    }
    if (!parse_url(argv[1], &u, NULL)) {
        printk("fetch: not an http:// URL: %s\n", argv[1]);
        return 1;
    }
    struct reader *r = kzalloc(sizeof(*r));
    if (!r)
        return 1;
    const char *file = argc == 3 ? argv[2] : NULL;
    int rc = 1;
    for (int redirects = 0;; redirects++) {
        ip4_t ip;
        int e = dns_resolve(u.host, &ip);
        if (e != NET_OK) {
            printk("fetch: %s: %s\n", u.host, net_strerror(e));
            break;
        }
        r->c = tcp_connect(ip, u.port, 10000, &e);
        if (!r->c) {
            printk("fetch: connect to %s:%u: %s\n", u.host, u.port, net_strerror(e));
            break;
        }
        r->pos = r->len = 0;
        r->err = 0;
        char hosthdr[140], req[800];
        if (u.port == 80)
            strlcpy(hosthdr, u.host, sizeof(hosthdr));
        else
            snprintf(hosthdr, sizeof(hosthdr), "%s:%u", u.host, u.port);
        int n = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Sonar/0.1\r\n"
                         "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
                         u.path, hosthdr);
        if (tcp_send(r->c, req, (size_t)n) != n) {
            printk("fetch: could not send the request\n");
            tcp_close(r->c);
            break;
        }
        char line[1024];
        int status = 0;
        u64 length = 0;
        bool have_length = false, chunked = false;
        char location[512] = "";
        if (!read_line(r, line, sizeof(line)) || strncmp(line, "HTTP/1.", 7) || strlen(line) < 12) {
            printk("fetch: bad response from server: %s\n",
                   r->err ? net_strerror(r->err) : "not HTTP");
            tcp_close(r->c);
            break;
        }
        status = (int)strtol(line + 9, NULL, 10);
        char reason[64];
        strlcpy(reason, strlen(line) > 13 ? line + 13 : "", sizeof(reason));
        while (read_line(r, line, sizeof(line)) && line[0]) {
            const char *v;
            if (header_is(line, "content-length", &v)) {
                length = strtoull(v, NULL, 10);
                have_length = true;
            } else if (header_is(line, "transfer-encoding", &v)) {
                chunked = strstr(v, "chunked") != NULL;
            } else if (header_is(line, "location", &v)) {
                strlcpy(location, v, sizeof(location));
            }
        }
        if (r->err) {
            printk("fetch: connection lost: %s\n", net_strerror(r->err));
            tcp_close(r->c);
            break;
        }
        if (status >= 300 && status < 400 && location[0]) {
            tcp_close(r->c);
            if (redirects >= 5) {
                printk("fetch: too many redirects\n");
                break;
            }
            struct url next;
            if (!parse_url(location, &next, &u)) {
                printk("fetch: redirected to %s, which is not supported%s\n", location,
                       !strncmp(location, "https://", 8) ? " (HTTPS needs TLS)" : "");
                break;
            }
            printk("fetch: redirected to %s\n", location);
            u = next;
            continue;
        }
        if (status != 200) {
            printk("fetch: server answered %d %s\n", status, reason);
            tcp_close(r->c);
            break;
        }
        struct sink s = { -1, 0, have_length ? length : 0, time_ms(), 0, false };
        if (file) {
            s.fd = fs_open(file, O_WRITE | O_CREATE | O_TRUNC);
            if (s.fd < 0) {
                printk("fetch: %s: %s\n", file, fs_strerror(s.fd));
                tcp_close(r->c);
                break;
            }
            s.progress = true;
            char sz[24];
            fmt_bytes(length, sz, sizeof(sz));
            printk("fetch: saving %s to %s%s%s\n", u.path, file, have_length ? ", " : "",
                   have_length ? sz : "");
        }
        int cr = chunked ? copy_chunked(r, &s)
                         : copy_body(r, &s, have_length ? length : ~0ULL);
        tcp_close(r->c);
        if (s.progress)
            show_progress(&s, true);
        if (file) {
            fs_close(s.fd);
            printk("\n");
        }
        if (cr < 0) {
            if (cr == -1)
                printk("fetch: download failed: %s\n", net_strerror(r->err));
            if (file) {
                fs_unlink(file);
                printk("fetch: removed the incomplete %s\n", file);
            }
            break;
        }
        if (file) {
            char sz[24];
            fmt_bytes(s.done, sz, sizeof(sz));
            u64 el = time_ms() - s.t0;
            printk("fetch: saved %s (%s in %llu.%llu s)\n", file, sz,
                   (unsigned long long)(el / 1000), (unsigned long long)(el % 1000 / 100));
        }
        rc = 0;
        break;
    }
    kfree(r);
    return rc;
}
SHELL_COMMAND(fetch, "fetch", "fetch http://HOST/PATH [FILE]", "download a file over HTTP",
              cmd_fetch);
