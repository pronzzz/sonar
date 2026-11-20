/*
 * GGML block formats (layouts as defined by ggml's ggml-common.h) and the
 * quantized matrix multiply used for every linear layer.
 *
 * Verification (tests/host, `aitest MODEL --qtest`):
 *  - F32, F16, Q4_0, Q4_K, Q6_K: real model files (DeepSeek-R1-0528-Qwen3-8B,
 *    Llama-3-8B, Qwen2.5-Coder-7B) generate correct text, and the integer
 *    path matches these float dequantizers bit-for-bit on every matrix.
 *  - Q4_1, Q5_0, Q5_1, Q8_0, Q5_K: implemented from the ggml reference
 *    layouts; the float dequantizer here and the separately written int8
 *    unpacker in qdot.c agree bit-for-bit on random blocks. No real model
 *    file of these types has been run yet.
 *  - BF16: implemented from the layout only.
 *
 * qmat_mul uses the integer path (qdot.c) for quantized types; the float
 * path below handles F32/F16/BF16 and serves as the reference.
 */
#include "quant.h"
#include "aimath.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define QK_K 256

struct type_info {
    const char *name;
    u16 blck;
    u16 size;
    bool supported;
};

static const struct type_info *info(int type)
{
    static const struct type_info t_f32 = { "F32", 1, 4, true };
    static const struct type_info t_f16 = { "F16", 1, 2, true };
    static const struct type_info t_bf16 = { "BF16", 1, 2, true };
    static const struct type_info t_q4_0 = { "Q4_0", 32, 18, true };
    static const struct type_info t_q4_1 = { "Q4_1", 32, 20, true };
    static const struct type_info t_q5_0 = { "Q5_0", 32, 22, true };
    static const struct type_info t_q5_1 = { "Q5_1", 32, 24, true };
    static const struct type_info t_q8_0 = { "Q8_0", 32, 34, true };
    static const struct type_info t_q8_1 = { "Q8_1", 32, 36, false };
    static const struct type_info t_q2_k = { "Q2_K", 256, 84, false };
    static const struct type_info t_q3_k = { "Q3_K", 256, 110, false };
    static const struct type_info t_q4_k = { "Q4_K", 256, 144, true };
    static const struct type_info t_q5_k = { "Q5_K", 256, 176, true };
    static const struct type_info t_q6_k = { "Q6_K", 256, 210, true };
    static const struct type_info t_q8_k = { "Q8_K", 256, 292, false };
    switch (type) {
    case GGML_TYPE_F32: return &t_f32;
    case GGML_TYPE_F16: return &t_f16;
    case GGML_TYPE_BF16: return &t_bf16;
    case GGML_TYPE_Q4_0: return &t_q4_0;
    case GGML_TYPE_Q4_1: return &t_q4_1;
    case GGML_TYPE_Q5_0: return &t_q5_0;
    case GGML_TYPE_Q5_1: return &t_q5_1;
    case GGML_TYPE_Q8_0: return &t_q8_0;
    case GGML_TYPE_Q8_1: return &t_q8_1;
    case GGML_TYPE_Q2_K: return &t_q2_k;
    case GGML_TYPE_Q3_K: return &t_q3_k;
    case GGML_TYPE_Q4_K: return &t_q4_k;
    case GGML_TYPE_Q5_K: return &t_q5_k;
    case GGML_TYPE_Q6_K: return &t_q6_k;
    case GGML_TYPE_Q8_K: return &t_q8_k;
    default: return NULL;
    }
}

size_t ggml_blck_size(int type) { return info(type) ? info(type)->blck : 0; }
size_t ggml_type_size(int type) { return info(type) ? info(type)->size : 0; }
const char *ggml_type_name(int type) { return info(type) ? info(type)->name : "unknown"; }
bool ggml_type_supported(int type) { return info(type) && info(type)->supported; }

static inline u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static inline float rdh(const u8 *p) { return fp16_to_f32(rd16(p)); }

/* ---- per-format dequantization of whole blocks ---- */

static void dq_q4_0(const u8 *b, float *y)
{
    float d = rdh(b);
    const u8 *qs = b + 2;
    for (int j = 0; j < 16; j++) {
        y[j] = (float)((qs[j] & 0x0f) - 8) * d;
        y[j + 16] = (float)((qs[j] >> 4) - 8) * d;
    }
}

static void dq_q4_1(const u8 *b, float *y)
{
    float d = rdh(b), m = rdh(b + 2);
    const u8 *qs = b + 4;
    for (int j = 0; j < 16; j++) {
        y[j] = (float)(qs[j] & 0x0f) * d + m;
        y[j + 16] = (float)(qs[j] >> 4) * d + m;
    }
}

static void dq_q5_0(const u8 *b, float *y)
{
    float d = rdh(b);
    u32 qh;
    memcpy(&qh, b + 2, 4);
    const u8 *qs = b + 6;
    for (int j = 0; j < 16; j++) {
        u8 xh0 = (u8)(((qh >> j) << 4) & 0x10);
        u8 xh1 = (u8)((qh >> (j + 12)) & 0x10);
        y[j] = (float)(((qs[j] & 0x0f) | xh0) - 16) * d;
        y[j + 16] = (float)(((qs[j] >> 4) | xh1) - 16) * d;
    }
}

static void dq_q5_1(const u8 *b, float *y)
{
    float d = rdh(b), m = rdh(b + 2);
    u32 qh;
    memcpy(&qh, b + 4, 4);
    const u8 *qs = b + 8;
    for (int j = 0; j < 16; j++) {
        u8 xh0 = (u8)(((qh >> j) << 4) & 0x10);
        u8 xh1 = (u8)((qh >> (j + 12)) & 0x10);
        y[j] = (float)((qs[j] & 0x0f) | xh0) * d + m;
        y[j + 16] = (float)((qs[j] >> 4) | xh1) * d + m;
    }
}

static void dq_q8_0(const u8 *b, float *y)
{
    float d = rdh(b);
    const i8 *qs = (const i8 *)(b + 2);
    for (int j = 0; j < 32; j++)
        y[j] = (float)qs[j] * d;
}

static inline void get_scale_min_k4(int j, const u8 *q, u8 *d, u8 *m)
{
    if (j < 4) {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (u8)((q[j + 4] & 0xf) | ((q[j - 4] >> 6) << 4));
        *m = (u8)((q[j + 4] >> 4) | ((q[j] >> 6) << 4));
    }
}

static void dq_q4_k(const u8 *b, float *y)
{
    float d = rdh(b), min = rdh(b + 2);
    const u8 *scales = b + 4;
    const u8 *q = b + 16;
    int is = 0;
    for (int j = 0; j < QK_K; j += 64) {
        u8 sc, m;
        get_scale_min_k4(is, scales, &sc, &m);
        float d1 = d * sc, m1 = min * m;
        get_scale_min_k4(is + 1, scales, &sc, &m);
        float d2 = d * sc, m2 = min * m;
        for (int l = 0; l < 32; l++)
            y[l] = d1 * (float)(q[l] & 0xf) - m1;
        for (int l = 0; l < 32; l++)
            y[32 + l] = d2 * (float)(q[l] >> 4) - m2;
        y += 64;
        q += 32;
        is += 2;
    }
}

static void dq_q5_k(const u8 *b, float *y)
{
    float d = rdh(b), min = rdh(b + 2);
    const u8 *scales = b + 4;
    const u8 *qh = b + 16;
    const u8 *ql = b + 48;
    int is = 0;
    u8 u1 = 1, u2 = 2;
    for (int j = 0; j < QK_K; j += 64) {
        u8 sc, m;
        get_scale_min_k4(is, scales, &sc, &m);
        float d1 = d * sc, m1 = min * m;
        get_scale_min_k4(is + 1, scales, &sc, &m);
        float d2 = d * sc, m2 = min * m;
        for (int l = 0; l < 32; l++)
            y[l] = d1 * (float)((ql[l] & 0xf) + ((qh[l] & u1) ? 16 : 0)) - m1;
        for (int l = 0; l < 32; l++)
            y[32 + l] = d2 * (float)((ql[l] >> 4) + ((qh[l] & u2) ? 16 : 0)) - m2;
        y += 64;
        ql += 32;
        is += 2;
        u1 = (u8)(u1 << 2);
        u2 = (u8)(u2 << 2);
    }
}

static void dq_q6_k(const u8 *b, float *y)
{
    const u8 *ql = b;
    const u8 *qh = b + 128;
    const i8 *sc = (const i8 *)(b + 192);
    float d = rdh(b + 208);
    for (int n = 0; n < QK_K; n += 128) {
        for (int l = 0; l < 32; l++) {
            int is = l / 16;
            int q1 = (int)((ql[l] & 0xf) | (((qh[l] >> 0) & 3) << 4)) - 32;
            int q2 = (int)((ql[l + 32] & 0xf) | (((qh[l] >> 2) & 3) << 4)) - 32;
            int q3 = (int)((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
            int q4 = (int)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
            y[l] = d * (float)sc[is] * (float)q1;
            y[l + 32] = d * (float)sc[is + 2] * (float)q2;
            y[l + 64] = d * (float)sc[is + 4] * (float)q3;
            y[l + 96] = d * (float)sc[is + 6] * (float)q4;
        }
        y += 128;
        ql += 64;
        qh += 32;
        sc += 8;
    }
}

void dequantize(int type, const void *src, float *dst, int n)
{
    const u8 *s = src;
    switch (type) {
    case GGML_TYPE_F32:
        memcpy(dst, s, (size_t)n * 4);
        return;
    case GGML_TYPE_F16:
#if defined(__aarch64__)
        {
            int i = 0;
            for (; i + 8 <= n; i += 8) {
                float16x8_t h = vld1q_f16((const float16_t *)(s + 2 * i));
                vst1q_f32(dst + i, vcvt_f32_f16(vget_low_f16(h)));
                vst1q_f32(dst + i + 4, vcvt_high_f32_f16(h));
            }
            for (; i < n; i++)
                dst[i] = rdh(s + 2 * i);
        }
#else
        for (int i = 0; i < n; i++)
            dst[i] = rdh(s + 2 * i);
#endif
        return;
    case GGML_TYPE_BF16:
        for (int i = 0; i < n; i++)
            dst[i] = bf16_to_f32(rd16(s + 2 * i));
        return;
    default:
        break;
    }
    const struct type_info *ti = info(type);
    void (*fn)(const u8 *, float *) = NULL;
    switch (type) {
    case GGML_TYPE_Q4_0: fn = dq_q4_0; break;
    case GGML_TYPE_Q4_1: fn = dq_q4_1; break;
    case GGML_TYPE_Q5_0: fn = dq_q5_0; break;
    case GGML_TYPE_Q5_1: fn = dq_q5_1; break;
    case GGML_TYPE_Q8_0: fn = dq_q8_0; break;
    case GGML_TYPE_Q4_K: fn = dq_q4_k; break;
    case GGML_TYPE_Q5_K: fn = dq_q5_k; break;
    case GGML_TYPE_Q6_K: fn = dq_q6_k; break;
    default:
        memset(dst, 0, (size_t)n * 4);
        return;
    }
    for (int i = 0; i < n; i += ti->blck) {
        fn(s, dst + i);
        s += ti->size;
    }
}

float vec_dot_f32(const float *a, const float *b, int n)
{
#if defined(__aarch64__)
    float32x4_t s0 = vdupq_n_f32(0), s1 = s0, s2 = s0, s3 = s0;
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        s0 = vfmaq_f32(s0, vld1q_f32(a + i), vld1q_f32(b + i));
        s1 = vfmaq_f32(s1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        s2 = vfmaq_f32(s2, vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
        s3 = vfmaq_f32(s3, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
    }
    for (; i + 4 <= n; i += 4)
        s0 = vfmaq_f32(s0, vld1q_f32(a + i), vld1q_f32(b + i));
    float r = vaddvq_f32(vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3)));
    for (; i < n; i++)
        r += a[i] * b[i];
    return r;
#else
    float r = 0;
    for (int i = 0; i < n; i++)
        r += a[i] * b[i];
    return r;
#endif
}

bool qmat_init(struct qmat *m, const void *data, int type, int cols, int rows)
{
    const struct type_info *ti = info(type);
    if (!ti || !ti->supported || cols <= 0 || rows <= 0 || cols % ti->blck)
        return false;
    m->data = data;
    m->type = type;
    m->rows = rows;
    m->cols = cols;
    m->row_bytes = (size_t)cols / ti->blck * ti->size;
    int ch = 256;
    while (ch > (int)ti->blck && cols % ch)
        ch /= 2;
    if (ch < (int)ti->blck || cols % ch)
        return false;
    m->chunk = ch;
    return true;
}

void qmat_get_row(const struct qmat *m, int r, float *out)
{
    dequantize(m->type, m->data + (size_t)r * m->row_bytes, out, m->cols);
}

struct mul_job {
    const struct qmat *m;
    const float *x;
    float *out;
    int xs, os, T;
};

static void mul_rows(void *ctx, int start, int end)
{
    const struct mul_job *j = ctx;
    const struct qmat *m = j->m;
    const int ch = m->chunk;
    const int T = j->T;
    const size_t chunk_bytes = (size_t)ch / ggml_blck_size(m->type) * ggml_type_size(m->type);
    float tmp[256] __attribute__((aligned(64)));
    float acc[AI_MAX_BATCH];
    for (int r = start; r < end; r++) {
        const u8 *row = m->data + (size_t)r * m->row_bytes;
        for (int t = 0; t < T; t++)
            acc[t] = 0.0f;
        for (int c0 = 0; c0 < m->cols; c0 += ch) {
            const float *w;
            if (m->type == GGML_TYPE_F32) {
                w = (const float *)(row) + c0;
            } else {
                dequantize(m->type, row + (size_t)(c0 / ch) * chunk_bytes, tmp, ch);
                w = tmp;
            }
            for (int t = 0; t < T; t++)
                acc[t] += vec_dot_f32(w, j->x + (size_t)t * j->xs + c0, ch);
        }
        for (int t = 0; t < T; t++)
            j->out[(size_t)t * j->os + r] = acc[t];
    }
}

static int qmode = QMODE_AUTO;

void qmat_set_mode(int mode) { qmode = mode; }

void qmat_mul(const struct qmat *m, const float *x, int x_stride, float *out, int out_stride,
              int T)
{
    for (int t0 = 0; t0 < T; t0 += AI_MAX_BATCH) {
        int n = T - t0 < AI_MAX_BATCH ? T - t0 : AI_MAX_BATCH;
        if (qmode != QMODE_FLOAT &&
            qdot_mul(m, x + (size_t)t0 * x_stride, x_stride, out + (size_t)t0 * out_stride,
                     out_stride, n, qmode))
            continue;
        struct mul_job j = { m, x + (size_t)t0 * x_stride, out + (size_t)t0 * out_stride,
                             x_stride, out_stride, n };
        int threads = ai_num_threads();
        int grain = m->rows / (threads * 8);
        if (grain < 4)
            grain = 4;
        ai_parallel_for(m->rows, grain, mul_rows, &j);
    }
}
