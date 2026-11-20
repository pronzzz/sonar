/*
 * HTTP/1.1 GET client over TCP or TLS (kernel/net/http.c), following
 * redirects between http:// and https:// URLs. HTTPS uses kernel/net/tls.c,
 * which does not authenticate servers: only use it for data that is
 * verified some other way (signed updates).
 */
#pragma once
#include "types.h"

struct http_get {
    const char *url;
    const char *accept;      /* Accept header, or NULL for any */
    const char *auth;        /* Authorization header value, or NULL. Sent with the
                                first request only, never after a redirect. */
    u64 max_bytes;           /* refuse bodies larger than this (0 = no limit) */
    u32 timeout_ms;          /* connect and per-read timeout (0 = 30 s) */
    /* Called with each piece of the body; return false to stop. */
    bool (*body)(void *ctx, const void *data, size_t len);
    void *ctx;
    /* Called now and then with bytes so far and the total (0 if unknown). */
    void (*progress)(void *ctx, u64 done, u64 total);

    /* results */
    int status;              /* final HTTP status (0 if none was received) */
    u64 length;              /* Content-Length of the final response, ~0 if unknown */
    u64 received;            /* body bytes delivered */
    int redirects;
    char final_url[512];
    char error[200];         /* why http_get failed */
};

/* Fetch g->url. Returns true when the final response was 200 and the
 * whole body was delivered; otherwise false with g->error set (g->status
 * tells an HTTP error apart from a network or TLS one). */
bool http_get(struct http_get *g);
