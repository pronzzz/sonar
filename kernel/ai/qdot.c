/*
 * Integer matrix multiplication for quantized weights (the fast path).
 *
 * Activations are quantized to int8 in blocks of 32 values (one f32 scale
 * per block, plus the scaled block sum for weight-offset terms). Each
 * 256-value weight super-block is unpacked to int8 with its scales, then
 * multiplied with SDOT (ARMv8.2 dot product) when the CPU has it, or with
 * SMULL/SADALP on plain ARMv8.0. This is the arithmetic llama.cpp uses,
 * with finer activation scales for K-quants (per 32 rather than per 256).
 *
 * Unpacked value i of a super-block is
 *     d32[i/32] * w[i] + mn[i/32]    (uni: one scale per 32 values)
 *     d16[i/16] * w[i] + mn[i/32]    (otherwise: one scale per 16 values)
 * which tests check bit-for-bit against the float dequantizers in quant.c.
 */
#include "quant.h"
#include "aimath.h"

#ifdef __aarch64__
#include <arm_neon.h>

struct wblk {
    i8 w[256] __attribute__((aligned(16)));
    float d32[8] __attribute__((aligned(16)));
    float d16[16] __attribute__((aligned(16)));
    float mn[8] __attribute__((aligned(16)));
    bool uni;    /* scales per 32 values (d32), else per 16 (d16) */
    bool has_mn; /* mn terms present */
};

struct qact {
    const i8 *q;
    const float *d, *s;
    int cols, nb;
};

struct qjob {
    const struct qmat *m;
    const struct qact *a;
    float *out;
    int os, T;
};

static inline float rdh(const u8 *p) { return fp16_to_f32((u16)(p[0] | p[1] << 8)); }

static inline void set_uniform(struct wblk *b, int nsub)
{
    (void)nsub;
    b->uni = true;
}

/* ---- per-format unpackers: nsub 32-value groups starting at src ---- */

static void up_q4_0(const u8 *src, int nsub, struct wblk *b, bool offset_form)
{
    /* Q4_0: d, qs[16]  value = d*(q-8)
     * Q4_1: d, m, qs[16]  value = d*q + m  (offset_form) */
    const uint8x16_t m4 = vdupq_n_u8(0x0f);
    const int8x16_t o8 = vdupq_n_s8(offset_form ? 0 : 8);
    const int bs = offset_form ? 20 : 18, qo = offset_form ? 4 : 2;
    for (int j = 0; j < nsub; j++) {
        const u8 *p = src + (size_t)j * bs;
        b->d32[j] = rdh(p);
        b->mn[j] = offset_form ? rdh(p + 2) : 0.0f;
        uint8x16_t q = vld1q_u8(p + qo);
        vst1q_s8(b->w + 32 * j, vsubq_s8(vreinterpretq_s8_u8(vandq_u8(q, m4)), o8));
        vst1q_s8(b->w + 32 * j + 16, vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(q, 4)), o8));
    }
    set_uniform(b, nsub);
    b->has_mn = offset_form;
}

/* 32 high bits (bit i -> value i) to bytes of 0x10 or 0. */
static inline void hbits32(u32 qh, uint8x16_t *lo, uint8x16_t *hi)
{
    static const u8 sel[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
    static const u8 il[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 };
    static const u8 ih[16] = { 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3 };
    uint8x16_t tbl = vreinterpretq_u8_u32(vdupq_n_u32(qh));
    uint8x16_t s = vld1q_u8(sel), x10 = vdupq_n_u8(0x10);
    *lo = vandq_u8(vtstq_u8(vqtbl1q_u8(tbl, vld1q_u8(il)), s), x10);
    *hi = vandq_u8(vtstq_u8(vqtbl1q_u8(tbl, vld1q_u8(ih)), s), x10);
}

static void up_q5_0(const u8 *src, int nsub, struct wblk *b, bool offset_form)
{
    /* Q5_0: d, qh[4], qs[16]  value = d*(q-16)
     * Q5_1: d, m, qh[4], qs[16]  value = d*q + m */
    const uint8x16_t m4 = vdupq_n_u8(0x0f);
    const int8x16_t o16 = vdupq_n_s8(offset_form ? 0 : 16);
    const int bs = offset_form ? 24 : 22, ho = offset_form ? 4 : 2;
    for (int j = 0; j < nsub; j++) {
        const u8 *p = src + (size_t)j * bs;
        b->d32[j] = rdh(p);
        b->mn[j] = offset_form ? rdh(p + 2) : 0.0f;
        u32 qh = (u32)p[ho] | (u32)p[ho + 1] << 8 | (u32)p[ho + 2] << 16 | (u32)p[ho + 3] << 24;
        uint8x16_t hl, hh;
        hbits32(qh, &hl, &hh);
        uint8x16_t q = vld1q_u8(p + ho + 4);
        uint8x16_t lo = vorrq_u8(vandq_u8(q, m4), hl), hi = vorrq_u8(vshrq_n_u8(q, 4), hh);
        vst1q_s8(b->w + 32 * j, vsubq_s8(vreinterpretq_s8_u8(lo), o16));
        vst1q_s8(b->w + 32 * j + 16, vsubq_s8(vreinterpretq_s8_u8(hi), o16));
    }
    set_uniform(b, nsub);
    b->has_mn = offset_form;
}

static void up_q8_0(const u8 *src, int nsub, struct wblk *b)
{
    for (int j = 0; j < nsub; j++) {
        const u8 *p = src + (size_t)j * 34;
        b->d32[j] = rdh(p);
        b->mn[j] = 0.0f;
        vst1q_s8(b->w + 32 * j, vld1q_s8((const i8 *)p + 2));
        vst1q_s8(b->w + 32 * j + 16, vld1q_s8((const i8 *)p + 18));
    }
    set_uniform(b, nsub);
    b->has_mn = false;
}

/* Q4_K/Q5_K: 8 six-bit scales and 8 six-bit mins packed in 12 bytes.
 * Decoded with the 32-bit mask arrangement used by llama.cpp; the scale
 * is folded into the per-group float multiplier, so the kernel takes the
 * uniform (sc == 1) branch. value = d*sc[g]*q - dmin*m[g]. */
static void k_scales(const u8 *p, struct wblk *b)
{
    const u32 k1 = 0x3f3f3f3f, k2 = 0x0f0f0f0f, k3 = 0x03030303;
    u32 u[4];
    memcpy(u, p + 4, 12);
    u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);
    u32 aux = u[1] & k1;
    u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);
    u[2] = aux;
    u[0] &= k1;
    /* bytes 0..7 of u: scales, bytes 8..15: mins */
    uint8x16_t v = vld1q_u8((const u8 *)u);
    uint16x8_t sc16 = vmovl_u8(vget_low_u8(v)), mn16 = vmovl_u8(vget_high_u8(v));
    float d = rdh(p), dmin = rdh(p + 2);
    vst1q_f32(b->d32, vmulq_n_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(sc16))), d));
    vst1q_f32(b->d32 + 4, vmulq_n_f32(vcvtq_f32_u32(vmovl_high_u16(sc16)), d));
    vst1q_f32(b->mn, vmulq_n_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(mn16))), -dmin));
    vst1q_f32(b->mn + 4, vmulq_n_f32(vcvtq_f32_u32(vmovl_high_u16(mn16)), -dmin));
    set_uniform(b, 8);
    b->has_mn = true;
}

static void up_q4_k(const u8 *p, struct wblk *b)
{
    /* d, dmin, scales[12], qs[128]; 64 values per 32 bytes of qs */
    k_scales(p, b);
    const uint8x16_t m4 = vdupq_n_u8(0x0f);
    const u8 *q = p + 16;
    for (int c = 0; c < 4; c++) {
        for (int h = 0; h < 2; h++) {
            uint8x16_t x = vld1q_u8(q + 32 * c + 16 * h);
            vst1q_u8((u8 *)b->w + 64 * c + 16 * h, vandq_u8(x, m4));
            vst1q_u8((u8 *)b->w + 64 * c + 32 + 16 * h, vshrq_n_u8(x, 4));
        }
    }
}

static void up_q5_k(const u8 *p, struct wblk *b)
{
    /* d, dmin, scales[12], qh[32], qs[128] */
    k_scales(p, b);
    const uint8x16_t m4 = vdupq_n_u8(0x0f), x10 = vdupq_n_u8(0x10);
    const u8 *qh = p + 16, *ql = p + 48;
    for (int c = 0; c < 4; c++) {
        uint8x16_t u1 = vdupq_n_u8((u8)(1u << (2 * c))), u2 = vdupq_n_u8((u8)(2u << (2 * c)));
        for (int h = 0; h < 2; h++) {
            uint8x16_t x = vld1q_u8(ql + 32 * c + 16 * h), hb = vld1q_u8(qh + 16 * h);
            uint8x16_t lo = vorrq_u8(vandq_u8(x, m4), vandq_u8(vtstq_u8(hb, u1), x10));
            uint8x16_t hi = vorrq_u8(vshrq_n_u8(x, 4), vandq_u8(vtstq_u8(hb, u2), x10));
            vst1q_u8((u8 *)b->w + 64 * c + 16 * h, lo);
            vst1q_u8((u8 *)b->w + 64 * c + 32 + 16 * h, hi);
        }
    }
}

static void up_q6_k(const u8 *p, struct wblk *b)
{
    /* ql[128], qh[64], scales[16] (int8), d   value = d*sc[i/16]*(q-32) */
    const uint8x16_t m4 = vdupq_n_u8(0x0f), m3 = vdupq_n_u8(3);
    const int8x16_t o32 = vdupq_n_s8(32);
    const u8 *ql = p, *qh = p + 128;
    const i8 *sc = (const i8 *)(p + 192);
    float d = rdh(p + 208);
    for (int n = 0; n < 2; n++) {
        for (int h = 0; h < 2; h++) {
            uint8x16_t l0 = vld1q_u8(ql + 64 * n + 16 * h);
            uint8x16_t l1 = vld1q_u8(ql + 64 * n + 32 + 16 * h);
            uint8x16_t hb = vld1q_u8(qh + 32 * n + 16 * h);
            uint8x16_t q1 = vorrq_u8(vandq_u8(l0, m4), vshlq_n_u8(vandq_u8(hb, m3), 4));
            uint8x16_t q2 = vorrq_u8(vandq_u8(l1, m4), vshlq_n_u8(vandq_u8(vshrq_n_u8(hb, 2), m3), 4));
            uint8x16_t q3 = vorrq_u8(vshrq_n_u8(l0, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(hb, 4), m3), 4));
            uint8x16_t q4 = vorrq_u8(vshrq_n_u8(l1, 4), vshlq_n_u8(vshrq_n_u8(hb, 6), 4));
            i8 *w = b->w + 128 * n + 16 * h;
            vst1q_s8(w + 0, vsubq_s8(vreinterpretq_s8_u8(q1), o32));
            vst1q_s8(w + 32, vsubq_s8(vreinterpretq_s8_u8(q2), o32));
            vst1q_s8(w + 64, vsubq_s8(vreinterpretq_s8_u8(q3), o32));
            vst1q_s8(w + 96, vsubq_s8(vreinterpretq_s8_u8(q4), o32));
        }
    }
    int8x16_t s8 = vld1q_s8(sc);
    int16x8_t lo = vmovl_s8(vget_low_s8(s8)), hi = vmovl_high_s8(s8);
    vst1q_f32(b->d16 + 0, vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(lo))), d));
    vst1q_f32(b->d16 + 4, vmulq_n_f32(vcvtq_f32_s32(vmovl_high_s16(lo)), d));
    vst1q_f32(b->d16 + 8, vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(hi))), d));
    vst1q_f32(b->d16 + 12, vmulq_n_f32(vcvtq_f32_s32(vmovl_high_s16(hi)), d));
    b->uni = false;
    b->has_mn = false;
}

/* nsub: 32-value groups to unpack (8 for a full super-block). */
static void unpack(int type, const u8 *src, int nsub, struct wblk *b)
{
    switch (type) {
    case GGML_TYPE_Q4_0: up_q4_0(src, nsub, b, false); break;
    case GGML_TYPE_Q4_1: up_q4_0(src, nsub, b, true); break;
    case GGML_TYPE_Q5_0: up_q5_0(src, nsub, b, false); break;
    case GGML_TYPE_Q5_1: up_q5_0(src, nsub, b, true); break;
    case GGML_TYPE_Q8_0: up_q8_0(src, nsub, b); break;
    case GGML_TYPE_Q4_K: up_q4_k(src, b); break;
    case GGML_TYPE_Q5_K: up_q5_k(src, b); break;
    case GGML_TYPE_Q6_K: up_q6_k(src, b); break;
    }
}

bool qdot_type_supported(int type)
{
    switch (type) {
    case GGML_TYPE_Q4_0: case GGML_TYPE_Q4_1: case GGML_TYPE_Q5_0: case GGML_TYPE_Q5_1:
    case GGML_TYPE_Q8_0: case GGML_TYPE_Q4_K: case GGML_TYPE_Q5_K: case GGML_TYPE_Q6_K:
        return true;
    }
    return false;
}

/* ---- the two kernel builds ---- */
static inline int32x4_t dot_base(int32x4_t acc, int8x16_t a, int8x16_t b)
{
    acc = vpadalq_s16(acc, vmull_s8(vget_low_s8(a), vget_low_s8(b)));
    return vpadalq_s16(acc, vmull_high_s8(a, b));
}

#define QNAME(x) x##_dp
#define QATTR __attribute__((target("dotprod")))
#define QDOT(acc, a, b) vdotq_s32(acc, a, b)
#include "qdot_kern.h"
#undef QNAME
#undef QATTR
#undef QDOT

#define QNAME(x) x##_base
#define QATTR
#define QDOT(acc, a, b) dot_base(acc, a, b)
#include "qdot_kern.h"
#undef QNAME
#undef QATTR
#undef QDOT

/* ---- activation quantization ---- */
static void quantize_act(const float *x, int n, i8 *q, float *d, float *s)
{
    for (int b = 0; b < n / 32; b++, x += 32, q += 32) {
        float32x4_t v[8];
        float32x4_t mx = vdupq_n_f32(0.0f);
        for (int k = 0; k < 8; k++) {
            v[k] = vld1q_f32(x + 4 * k);
            mx = vmaxq_f32(mx, vabsq_f32(v[k]));
        }
        float amax = vmaxvq_f32(mx);
        float dd = amax / 127.0f, id = amax > 0.0f ? 127.0f / amax : 0.0f;
        int32x4_t sum = vdupq_n_s32(0);
        int32x4_t qi[8];
        for (int k = 0; k < 8; k++) {
            qi[k] = vcvtnq_s32_f32(vmulq_n_f32(v[k], id));
            sum = vaddq_s32(sum, qi[k]);
        }
        for (int k = 0; k < 2; k++) {
            int16x8_t a = vcombine_s16(vmovn_s32(qi[4 * k]), vmovn_s32(qi[4 * k + 1]));
            int16x8_t c = vcombine_s16(vmovn_s32(qi[4 * k + 2]), vmovn_s32(qi[4 * k + 3]));
            vst1q_s8(q + 16 * k, vcombine_s8(vmovn_s16(a), vmovn_s16(c)));
        }
        d[b] = dd;
        s[b] = dd * (float)vaddvq_s32(sum);
    }
}

/* Activation scratch, grown on demand (matmuls run one at a time). */
static u8 *scratch;
static size_t scratch_size;

bool qdot_mul(const struct qmat *m, const float *x, int x_stride, float *out, int out_stride,
              int T, int mode)
{
    if (!qdot_type_supported(m->type) || m->cols % 32 || T <= 0 || T > AI_MAX_BATCH)
        return false;
    int nb = m->cols / 32;
    size_t need = (size_t)T * m->cols + 2 * sizeof(float) * (size_t)T * nb + 64;
    if (need > scratch_size) {
        u8 *p = ai_realloc(scratch, need);
        if (!p)
            return false;
        scratch = p;
        scratch_size = need;
    }
    float *d = (float *)(((uintptr_t)scratch + 15) & ~(uintptr_t)15);
    float *s = d + (size_t)T * nb;
    i8 *q = (i8 *)(s + (size_t)T * nb);
    for (int t = 0; t < T; t++)
        quantize_act(x + (size_t)t * x_stride, m->cols, q + (size_t)t * m->cols,
                     d + (size_t)t * nb, s + (size_t)t * nb);
    struct qact a = { q, d, s, m->cols, nb };
    struct qjob j = { m, &a, out, out_stride, T };
    int threads = ai_num_threads();
    int grain = m->rows / (threads * 8);
    if (grain < 4)
        grain = 4;
    bool dp = mode == QMODE_AUTO ? ai_cpu_has_dotprod() : mode == QMODE_DOTPROD;
    void (*fn)(void *, int, int) = dp ? mul_rows_dp : mul_rows_base;
    if (T == 1 && m->type == GGML_TYPE_Q4_K && m->cols % 256 == 0)
        fn = dp ? mul_rows_q4k1_dp : mul_rows_q4k1_base;
    else if (T == 1 && m->type == GGML_TYPE_Q6_K && m->cols % 256 == 0)
        fn = dp ? mul_rows_q6k1_dp : mul_rows_q6k1_base;
    ai_parallel_for(m->rows, grain, fn, &j);
    return true;
}

/* Test hook: rebuild floats from the int8 unpacking of one row. */
bool qdot_unpack_row(const struct qmat *m, int r, float *out)
{
    if (!qdot_type_supported(m->type) || m->cols % 32)
        return false;
    const int blck = ggml_blck_size(m->type);
    const size_t bsize = ggml_type_size(m->type);
    const u8 *row = m->data + (size_t)r * m->row_bytes;
    struct wblk wb;
    for (int c0 = 0; c0 < m->cols; c0 += 256) {
        int nsub = (m->cols - c0 < 256 ? m->cols - c0 : 256) / 32;
        unpack(m->type, row + (size_t)(c0 / blck) * bsize, nsub, &wb);
        for (int i = 0; i < nsub * 32; i++)
            out[c0 + i] = (wb.uni ? wb.d32[i / 32] : wb.d16[i / 16]) * (float)wb.w[i] +
                          (wb.has_mn ? wb.mn[i / 32] : 0.0f);
    }
    return true;
}

#else /* not AArch64: the float path in quant.c handles everything */

bool qdot_type_supported(int type) { return false; }
bool qdot_mul(const struct qmat *m, const float *x, int x_stride, float *out, int out_stride,
              int T, int mode)
{
    return false;
}
bool qdot_unpack_row(const struct qmat *m, int r, float *out) { return false; }

#endif
