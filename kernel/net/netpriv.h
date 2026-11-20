/* Internal helpers shared by the network stack. */
#pragma once
#include "kernel.h"
#include "net.h"
#include "string.h"

#define PROTO_ICMP 1
#define PROTO_TCP 6
#define PROTO_UDP 17

static inline u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static inline u32 get32(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (u32)p[3];
}
static inline void put16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}
static inline void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* TCP-style sequence arithmetic (mod 2^32). */
static inline bool seq_lt(u32 a, u32 b) { return (i32)(a - b) < 0; }
static inline bool seq_le(u32 a, u32 b) { return (i32)(a - b) <= 0; }
static inline bool seq_gt(u32 a, u32 b) { return (i32)(a - b) > 0; }
static inline bool seq_ge(u32 a, u32 b) { return (i32)(a - b) >= 0; }

/* Waits in blocking calls end on timeout or Ctrl-C. */
int net_wait_check(u64 deadline_ms);

void icmp_input(ip4_t src, ip4_t dst, const u8 *p, size_t len, int ttl, bool bcast);
void udp_input(ip4_t src, ip4_t dst, const u8 *p, size_t len);
void arp_list(void);
void udp_list(void);
