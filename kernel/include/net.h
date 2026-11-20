/*
 * Networking: Ethernet interfaces, ARP, IPv4, ICMP, UDP, TCP, DHCP and DNS.
 *
 * The stack is polled: drivers hand received frames to net_input() from
 * their poll function, which runs while the shell waits for input and
 * inside every blocking network call. IPv4 addresses are u32 values in
 * host byte order.
 */
#pragma once
#include "types.h"

typedef u32 ip4_t;
#define IP4(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))
#define IP4_BROADCAST 0xffffffffu

enum {
    NET_OK = 0,
    NET_ETIMEDOUT = -1,
    NET_EUNREACH = -2,  /* no route / no address / ARP failed */
    NET_ERESET = -3,    /* connection reset by peer */
    NET_EREFUSED = -4,  /* connection refused */
    NET_ENOMEM = -5,
    NET_EINTR = -6,     /* interrupted with Ctrl-C */
    NET_ENOTFOUND = -7, /* name does not resolve */
    NET_EINVAL = -8,
    NET_ECLOSED = -9,   /* connection closed */
    NET_ENODEV = -10,   /* no network interface */
    NET_ETLS = -11,     /* TLS handshake or record error (details: tls_error) */
};
const char *net_strerror(int e);

struct netif {
    char name[8];
    u8 mac[6];
    int mtu;
    bool link_up;
    /* IPv4 configuration (from DHCP or set by hand) */
    ip4_t ip, mask, gw, dns;
    bool dhcp;
    ip4_t dhcp_server;
    u64 lease_renew_ms, lease_end_ms; /* time_ms() deadlines */
    /* driver */
    bool (*send)(struct netif *nif, const void *frame, size_t len); /* Ethernet frame */
    void (*poll)(struct netif *nif);   /* deliver received frames with net_input() */
    bool (*pending)(struct netif *nif); /* frames waiting to be polled */
    void (*arm)(struct netif *nif, bool on); /* receive interrupts on/off */
    void *priv;
    /* statistics */
    u64 rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped, tx_dropped;
};

void netif_register(struct netif *nif);
struct netif *net_iface(void); /* the (first) interface, or NULL */
void net_input(struct netif *nif, const u8 *frame, size_t len);

void net_boot(void);
/* Apply the configured addressing (config net.mode: dhcp, static or off). */
int net_configure(bool quiet);
/* Process received packets and protocol timers; never blocks. */
void net_poll(void);
/* Poll, then sleep until the next interrupt if nothing arrived. */
void net_wait(void);
/* One-line summary, e.g. "eth0 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3". */
void net_describe(char *out, size_t n);

void ip4_format(ip4_t a, char *out, size_t n);
bool ip4_parse(const char *s, ip4_t *out);

/* IPv4 output: chooses the next hop and resolves it with ARP (queueing the
 * packet until the reply arrives). Returns NET_OK or an error. */
int ip_send(ip4_t dst, u8 proto, const void *payload, size_t len);
u16 net_checksum(const void *data, size_t len, u32 sum);
u32 net_pseudo_sum(ip4_t src, ip4_t dst, u8 proto, u16 len);

/* ICMP echo (ping). Returns the round-trip time in microseconds, or an error. */
int icmp_ping(ip4_t dst, u16 seq, size_t payload, u32 timeout_ms, int *ttl);

/* UDP */
struct udp_sock;
struct udp_sock *udp_open(u16 port); /* 0 picks an ephemeral port */
void udp_close(struct udp_sock *s);
int udp_sendto(struct udp_sock *s, ip4_t dst, u16 port, const void *buf, size_t len);
/* Returns the datagram length, or NET_ETIMEDOUT / NET_EINTR. */
int udp_recvfrom(struct udp_sock *s, void *buf, size_t len, ip4_t *src, u16 *sport,
                 u32 timeout_ms);

/* DHCP: configure the interface. */
int dhcp_configure(struct netif *nif, bool quiet);
void dhcp_tick(struct netif *nif);

/* DNS: dotted quads are parsed directly; other names are queried. */
int dns_resolve(const char *name, ip4_t *out);

/* TCP client connections (byte streams). */
struct tcp_conn;
struct tcp_conn *tcp_connect(ip4_t dst, u16 port, u32 timeout_ms, int *err);
/* Queue data for sending; blocks while the send buffer is full. */
ssize_t tcp_send(struct tcp_conn *c, const void *buf, size_t len);
/* Returns bytes read, 0 at end of stream, or an error. */
ssize_t tcp_recv(struct tcp_conn *c, void *buf, size_t len, u32 timeout_ms);
/* Graceful close (FIN); the connection is freed when the exchange ends. */
void tcp_close(struct tcp_conn *c);
void tcp_list(void); /* print connections (netstat) */
void tcp_input(ip4_t src, ip4_t dst, const u8 *seg, size_t len);
void tcp_tick(void);
void tcp_flush(void); /* send acknowledgments deferred during a batch */

/* TLS 1.3 client over a TCP connection (kernel/net/tls.c). Takes over the
 * connection: tls_close (or a failed tls_connect) closes it. The server's
 * certificate is NOT verified; see the comment at the top of tls.c. */
struct tls_conn;
struct tls_conn *tls_connect(struct tcp_conn *tcp, const char *host, u32 timeout_ms,
                             char *err, size_t errlen);
ssize_t tls_send(struct tls_conn *t, const void *buf, size_t len);
ssize_t tls_recv(struct tls_conn *t, void *buf, size_t len, u32 timeout_ms); /* 0 at end */
const char *tls_error(struct tls_conn *t); /* why the last call failed */
void tls_close(struct tls_conn *t);
