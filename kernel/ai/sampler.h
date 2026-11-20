/* Next-token sampling: greedy, or temperature + top-k + top-p + min-p. */
#pragma once

#include "platform.h"

struct sampler {
    float temperature; /* <= 0: greedy */
    float top_p;
    float min_p;
    int top_k;
    u64 s[4]; /* xoshiro256** state */
};

void sampler_init(struct sampler *s, float temperature, float top_p, int top_k, u64 seed);
int sampler_sample(struct sampler *s, const float *logits, int n);
int sampler_argmax(const float *logits, int n);
u64 sampler_rand(struct sampler *s);
