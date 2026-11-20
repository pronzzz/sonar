/* Kernel random numbers (not cryptographic): an entropy pool stirred with
 * timer jitter and, when present, a virtio-rng device. */
#pragma once
#include "types.h"

void random_mix(const void *data, size_t len);
u64 random_u64(void);
u32 random_u32(void);
void random_bytes(void *out, size_t len);
