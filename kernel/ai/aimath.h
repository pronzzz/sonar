/* Freestanding math for the AI engine (no libm in the kernel). */
#pragma once

#include "platform.h"

double ai_exp(double x);
double ai_ln(double x);
double ai_sin(double x);
double ai_cos(double x);
double ai_pow(double base, double e);
double ai_floor(double x);
double ai_ceil(double x);

static inline float ai_sqrtf(float x)
{
    float r;
    __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
    return r;
}

static inline double ai_sqrt(double x)
{
    double r;
    __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x));
    return r;
}

/* expf with ~1 ulp-class accuracy over the range used by softmax/SiLU. */
static inline float ai_expf(float x)
{
    if (x > 88.0f)
        x = 88.0f;
    if (x < -87.0f)
        return 0.0f;
    float kf = x * 1.44269504088896341f;
    int k = (int)(kf + (kf >= 0 ? 0.5f : -0.5f));
    float r = x - (float)k * 0.693145751953125f - (float)k * 1.428606765330187e-06f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (0.166666671633720f +
              r * (0.0416666679084301f + r * (0.00833333376795053f +
              r * 0.00138888892252f)))));
    union { u32 u; float f; } s;
    s.u = (u32)(k + 127) << 23;
    return p * s.f;
}

static inline float fp16_to_f32(u16 h)
{
    __fp16 v;
    memcpy(&v, &h, 2);
    return (float)v;
}

static inline u16 f32_to_fp16(float f)
{
    __fp16 v = (__fp16)f;
    u16 h;
    memcpy(&h, &v, 2);
    return h;
}

static inline float bf16_to_f32(u16 h)
{
    union { u32 u; float f; } v;
    v.u = (u32)h << 16;
    return v.f;
}
