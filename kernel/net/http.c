/* HTTP/1.1 GET over TCP or TLS, with redirects (see http.h). */
#include "http.h"
#include "kernel.h"
#include "mm.h"
#include "net.h"
#include "string.h"

#define MAX_REDIRECTS 8

struct hurl {
    bool tls;
    char host[128];
    u16 port;
    char path[1024];
};

static bool parse_url(const char *s, struct hurl *u, const struct hurl *base)
{
    if (s[0] == '/' && base) { /* absolute path on the same server */
        *u = *base;
        strlcpy(u->path, s, sizeof(u->path));
        return strlen(s) < sizeof(u->path);
    }
    if (!strncmp(s, "https://", 8)) {
        u->tls = true;
        s += 8;
        u->port = 443;
    } else if (!strncmp(s, "http://", 7)) {
        u->tls = false;
        s += 7;
        u->port = 80;
    } else {
        return false;
    }
    const char *slash = strchr(s, '/');
    const char *q = strchr(s, '?');
    if (q && (!slash || q < slash))
        slash = q;
    size_t hl = slash ? (size_t)(slash - s) : strlen(s);
    if (!hl || hl >= sizeof(u->host))
        return false;
    memcpy(u->host, s, hl);
    u->host[hl] = 0;
    char *colon = strchr(u->host, ':');
    if (colon) {
        *colon = 0;
        long p = strtol(colon + 1, NULL, 10);
        if (p <= 0 || p > 65535)
            return false;
        u->port = (u16)p;
    }
    if (!slash)
        strlcpy(u->path, "/", sizeof(u->path));
    else if (*slash == '?')
        snprintf(u->path, sizeof(u->path), "/%s", slash);
    else
        strlcpy(u->path, slash, sizeof(u->path));
    return strlen(slash ? slash : "/") < sizeof(u->path) - 1;
}

static bool same_server(const struct hurl *a, const struct hurl *b)
{
    return a->tls == b->tls && a->port == b->port && !strcmp(a->host, b->host);
}

/* ------------------------------------------------------------- streams */
struct conn {
    struct tcp_conn *tcp;
    struct tls_conn *tls;
    u8 buf[16384];
    size_t pos, len;
    u32 timeout_ms;
    int err;
    char *errmsg;
    size_t errlen;
};

static bool fill(struct conn *c)
{
    if (c->pos < c->len)
        return true;
    ssize_t n = c->tls ? tls_recv(c->tls, c->buf, sizeof(c->buf), c->timeout_ms)
                       : tcp_recv(c->tcp, c->buf, sizeof(c->buf), c->timeout_ms);
    if (n <= 0) {
        c->err = n < 0 ? (int)n : NET_ECLOSED;
        if (n == NET_ETLS && c->tls)
            snprintf(c->errmsg, c->errlen, "%s", tls_error(c->tls));
        return false;
    }
    c->pos = 0;
    c->len = (size_t)n;
    return true;
}

static bool read_line(struct conn *c, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        if (!fill(c))
            return false;
        char ch = (char)c->buf[c->pos++];
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

static bool conn_send(struct conn *c, const char *s, size_t n)
{
    ssize_t w = c->tls ? tls_send(c->tls, s, n) : tcp_send(c->tcp, s, n);
    return w == (ssize_t)n;
}

static void conn_close(struct conn *c)
{
    if (c->tls)
        tls_close(c->tls);
    else if (c->tcp)
        tcp_close(c->tcp);
    c->tls = NULL;
    c->tcp = NULL;
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

/* --------------------------------------------------------------- body */
struct copy {
    struct http_get *g;
    u64 last_report;
};

static bool deliver(struct copy *cp, const void *data, size_t n)
{
    struct http_get *g = cp->g;
    if (g->max_bytes && g->received + n > g->max_bytes) {
        snprintf(g->error, sizeof(g->error), "the response is larger than %llu bytes",
                 (unsigned long long)g->max_bytes);
        return false;
    }
    g->received += n;
    if (g->body && !g->body(g->ctx, data, n)) {
        if (!g->error[0])
            snprintf(g->error, sizeof(g->error), "stopped");
        return false;
    }
    if (g->progress && (g->received - cp->last_report >= 64 * 1024 || g->received == g->length)) {
        cp->last_report = g->received;
        g->progress(g->ctx, g->received, g->length == ~0ULL ? 0 : g->length);
    }
    return true;
}

static bool copy_n(struct conn *c, struct copy *cp, u64 n)
{
    while (n) {
        if (!fill(c))
            return n == ~0ULL && c->err == NET_ECLOSED; /* read-until-close ends here */
        size_t k = c->len - c->pos;
        if (n != ~0ULL && k > n)
            k = (size_t)n;
        if (!deliver(cp, c->buf + c->pos, k))
            return false;
        c->pos += k;
        if (n != ~0ULL)
            n -= k;
    }
    return true;
}

static bool copy_chunked(struct conn *c, struct copy *cp)
{
    char line[128];
    for (;;) {
        if (!read_line(c, line, sizeof(line)))
            return false;
        u64 n = strtoull(line, NULL, 16);
        if (!n)
            break;
        if (!copy_n(c, cp, n) || !read_line(c, line, sizeof(line)))
            return false;
    }
    while (read_line(c, line, sizeof(line)) && line[0]) /* trailers */
        ;
    return true;
}

/* ---------------------------------------------------------------- GET */
static bool net_fail(struct http_get *g, struct conn *c, const char *what)
{
    if (!g->error[0]) {
        if (c->err == NET_ETLS && c->errmsg[0])
            snprintf(g->error, sizeof(g->error), "%s: %s", what, c->errmsg);
        else
            snprintf(g->error, sizeof(g->error), "%s: %s", what, net_strerror(c->err));
    }
    return false;
}

bool http_get(struct http_get *g)
{
    struct hurl first, u, next;
    g->status = 0;
    g->length = ~0ULL;
    g->received = 0;
    g->redirects = 0;
    g->error[0] = 0;
    u32 timeout = g->timeout_ms ? g->timeout_ms : 30000;
    if (!parse_url(g->url, &u, NULL)) {
        snprintf(g->error, sizeof(g->error), "not an http:// or https:// URL: %s", g->url);
        return false;
    }
    first = u;
    strlcpy(g->final_url, g->url, sizeof(g->final_url));

    struct conn *c = kzalloc(sizeof(*c));
    char *line = kmalloc(2048), *req = kmalloc(2048), *location = kmalloc(1024);
    char tlserr[160] = "";
    bool ok = false;
    if (!c || !line || !req || !location) {
        snprintf(g->error, sizeof(g->error), "out of memory");
        goto out;
    }

    for (;;) {
        memset(c, 0, sizeof(*c));
        c->timeout_ms = timeout;
        c->errmsg = tlserr;
        c->errlen = sizeof(tlserr);
        tlserr[0] = 0;

        ip4_t ip;
        int e = dns_resolve(u.host, &ip);
        if (e != NET_OK) {
            snprintf(g->error, sizeof(g->error), "%s: %s", u.host, net_strerror(e));
            goto out;
        }
        c->tcp = tcp_connect(ip, u.port, timeout < 15000 ? timeout : 15000, &e);
        if (!c->tcp) {
            snprintf(g->error, sizeof(g->error), "connect to %s:%u: %s", u.host, u.port,
                     net_strerror(e));
            goto out;
        }
        if (u.tls) {
            struct tcp_conn *tcp = c->tcp;
            c->tcp = NULL;
            c->tls = tls_connect(tcp, u.host, timeout, g->error, sizeof(g->error));
            if (!c->tls)
                goto out;
        }

        /* Credentials go to the URL asked for and nowhere else: GitHub
         * redirects asset downloads to other servers, which must not see
         * the token (and refuse requests that carry one). */
        bool send_auth = g->auth && !g->redirects && same_server(&u, &first);
        int rl = snprintf(req, 2048,
                          "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s/%s\r\n"
                          "Accept: %s\r\n%s%s%sConnection: close\r\n\r\n",
                          u.path, u.host, OS_NAME, OS_VERSION, g->accept ? g->accept : "*/*",
                          send_auth ? "Authorization: " : "", send_auth ? g->auth : "",
                          send_auth ? "\r\n" : "");
        if (rl >= 2048) {
            snprintf(g->error, sizeof(g->error), "request too long");
            goto out;
        }
        if (!conn_send(c, req, (size_t)rl)) {
            net_fail(g, c, "send");
            goto out;
        }

        if (!read_line(c, line, 2048)) {
            net_fail(g, c, u.host);
            goto out;
        }
        if (strncmp(line, "HTTP/1.", 7) || strlen(line) < 12) {
            snprintf(g->error, sizeof(g->error), "bad response from %s: %.60s", u.host, line);
            goto out;
        }
        g->status = (int)strtol(line + 9, NULL, 10);
        u64 length = ~0ULL;
        bool chunked = false;
        location[0] = 0;
        for (;;) {
            if (!read_line(c, line, 2048)) {
                net_fail(g, c, u.host);
                goto out;
            }
            if (!line[0])
                break;
            const char *v;
            if (header_is(line, "content-length", &v))
                length = strtoull(v, NULL, 10);
            else if (header_is(line, "transfer-encoding", &v))
                chunked = strstr(v, "chunked") != NULL;
            else if (header_is(line, "location", &v))
                strlcpy(location, v, 1024);
        }

        if (g->status == 301 || g->status == 302 || g->status == 303 || g->status == 307 ||
            g->status == 308) {
            conn_close(c);
            if (++g->redirects > MAX_REDIRECTS) {
                snprintf(g->error, sizeof(g->error), "too many redirects");
                goto out;
            }
            if (!location[0] || !parse_url(location, &next, &u)) {
                snprintf(g->error, sizeof(g->error), "redirected to an unsupported URL: %.120s",
                         location);
                goto out;
            }
            u = next;
            strlcpy(g->final_url, location[0] == '/' ? u.path : location, sizeof(g->final_url));
            continue;
        }
        if (g->status != 200) {
            /* keep a little of the body: servers explain errors there */
            size_t n = 0;
            char *msg = line;
            while (n < 160 && fill(c)) {
                size_t k = c->len - c->pos;
                if (k > 160 - n)
                    k = 160 - n;
                memcpy(msg + n, c->buf + c->pos, k);
                c->pos += k;
                n += k;
                if (length != ~0ULL && n >= length)
                    break;
            }
            msg[n] = 0;
            for (size_t i = 0; i < n; i++)
                if (msg[i] == '\r' || msg[i] == '\n')
                    msg[i] = ' ';
            snprintf(g->error, sizeof(g->error), "%s answered HTTP %d%s%.120s", u.host,
                     g->status, n ? ": " : "", msg);
            goto out;
        }

        g->length = chunked ? ~0ULL : length;
        struct copy cp = { g, 0 };
        if (g->progress)
            g->progress(g->ctx, 0, g->length == ~0ULL ? 0 : g->length);
        bool done = chunked ? copy_chunked(c, &cp) : copy_n(c, &cp, length);
        if (!done) {
            if (!g->error[0])
                net_fail(g, c, "download");
            goto out;
        }
        if (length != ~0ULL && !chunked && g->received != length) {
            snprintf(g->error, sizeof(g->error), "download ended early");
            goto out;
        }
        ok = true;
        break;
    }
out:
    if (c)
        conn_close(c);
    kfree(c);
    kfree(line);
    kfree(req);
    kfree(location);
    return ok;
}
