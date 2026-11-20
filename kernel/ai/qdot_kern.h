/*
 * Integer super-block kernel, included twice by qdot.c: once with the
 * ARMv8.2 dot-product instruction (SDOT) and once for plain ARMv8.0.
 * Expects QNAME(x), QATTR and QDOT(acc, a, b) to be defined.
 */

/* accv[t]/accs[t] += (super-block b) . (activation row t, 32-blocks blk0..) */
QATTR static void QNAME(sb_dot)(const struct wblk *b, int nsub, const struct qact *A, int blk0,
                                int T, float32x4_t *accv, float *accs)
{
    const int32x4_t z = vdupq_n_s32(0);
    const i8 *w = b->w;
    for (int t = 0; t < T; t++) {
        const i8 *aq = A->q + (size_t)t * A->cols + (size_t)blk0 * 32;
        const float *ad = A->d + (size_t)t * A->nb + blk0;
        float32x4_t v0 = accv[t], v1 = vdupq_n_f32(0.0f);
        if (nsub == 8 && b->uni) {
            /* one scale per 32 values: coefficient d32[j] * ad[j] */
            float32x4_t c0 = vmulq_f32(vld1q_f32(b->d32), vld1q_f32(ad));
            float32x4_t c1 = vmulq_f32(vld1q_f32(b->d32 + 4), vld1q_f32(ad + 4));
#define U(j, c, l, v)                                                                              \
    {                                                                                              \
        int32x4_t p = QDOT(z, vld1q_s8(w + 32 * (j)), vld1q_s8(aq + 32 * (j)));                    \
        p = QDOT(p, vld1q_s8(w + 32 * (j) + 16), vld1q_s8(aq + 32 * (j) + 16));                    \
        v = vmlaq_laneq_f32(v, vcvtq_f32_s32(p), c, l);                                            \
    }
            U(0, c0, 0, v0) U(1, c0, 1, v1) U(2, c0, 2, v0) U(3, c0, 3, v1)
            U(4, c1, 0, v0) U(5, c1, 1, v1) U(6, c1, 2, v0) U(7, c1, 3, v1)
#undef U
        } else if (nsub == 8) {
            /* one scale per 16 values: coefficient d16[g] * ad[g / 2] */
            float32x4_t a0 = vld1q_f32(ad), a1 = vld1q_f32(ad + 4);
            float32x4_t c0 = vmulq_f32(vld1q_f32(b->d16), vzip1q_f32(a0, a0));
            float32x4_t c1 = vmulq_f32(vld1q_f32(b->d16 + 4), vzip2q_f32(a0, a0));
            float32x4_t c2 = vmulq_f32(vld1q_f32(b->d16 + 8), vzip1q_f32(a1, a1));
            float32x4_t c3 = vmulq_f32(vld1q_f32(b->d16 + 12), vzip2q_f32(a1, a1));
#define H(g, c, l, v)                                                                              \
    {                                                                                              \
        int32x4_t p = QDOT(z, vld1q_s8(w + 16 * (g)), vld1q_s8(aq + 16 * (g)));                    \
        v = vmlaq_laneq_f32(v, vcvtq_f32_s32(p), c, l);                                            \
    }
            H(0, c0, 0, v0) H(1, c0, 1, v1) H(2, c0, 2, v0) H(3, c0, 3, v1)
            H(4, c1, 0, v0) H(5, c1, 1, v1) H(6, c1, 2, v0) H(7, c1, 3, v1)
            H(8, c2, 0, v0) H(9, c2, 1, v1) H(10, c2, 2, v0) H(11, c2, 3, v1)
            H(12, c3, 0, v0) H(13, c3, 1, v1) H(14, c3, 2, v0) H(15, c3, 3, v1)
#undef H
        } else {
            /* partial super-block (only 32-value block formats get here) */
            for (int j = 0; j < nsub; j++) {
                int32x4_t p0 = QDOT(z, vld1q_s8(w + 32 * j), vld1q_s8(aq + 32 * j));
                int32x4_t p1 = QDOT(z, vld1q_s8(w + 32 * j + 16), vld1q_s8(aq + 32 * j + 16));
                if (b->uni) {
                    v0 = vmlaq_n_f32(v0, vcvtq_f32_s32(vaddq_s32(p0, p1)), b->d32[j] * ad[j]);
                } else {
                    v0 = vmlaq_n_f32(v0, vcvtq_f32_s32(p0), b->d16[2 * j] * ad[j]);
                    v1 = vmlaq_n_f32(v1, vcvtq_f32_s32(p1), b->d16[2 * j + 1] * ad[j]);
                }
            }
        }
        accv[t] = vaddq_f32(v0, v1);
        if (b->has_mn) {
            const float *as = A->s + (size_t)t * A->nb + blk0;
            if (nsub == 8) {
                float32x4_t m = vmulq_f32(vld1q_f32(b->mn), vld1q_f32(as));
                m = vmlaq_f32(m, vld1q_f32(b->mn + 4), vld1q_f32(as + 4));
                accs[t] += vaddvq_f32(m);
            } else {
                float s = 0.0f;
                for (int j = 0; j < nsub; j++)
                    s += b->mn[j] * as[j];
                accs[t] += s;
            }
        }
    }
}

QATTR static void QNAME(mul_rows)(void *ctx, int start, int end)
{
    const struct qjob *j = ctx;
    const struct qmat *m = j->m;
    const int T = j->T;
    const int blck = ggml_blck_size(m->type);
    const size_t bsize = ggml_type_size(m->type);
    struct wblk wb;
    float32x4_t accv[AI_MAX_BATCH];
    float accs[AI_MAX_BATCH];
    for (int r = start; r < end; r++) {
        const u8 *row = m->data + (size_t)r * m->row_bytes;
        for (int t = 0; t < T; t++) {
            accv[t] = vdupq_n_f32(0.0f);
            accs[t] = 0.0f;
        }
        for (int c0 = 0; c0 < m->cols; c0 += 256) {
            int nsub = (m->cols - c0 < 256 ? m->cols - c0 : 256) / 32;
            unpack(m->type, row + (size_t)(c0 / blck) * bsize, nsub, &wb);
            QNAME(sb_dot)(&wb, nsub, j->a, c0 / 32, T, accv, accs);
        }
        for (int t = 0; t < T; t++)
            j->out[(size_t)t * j->os + r] = vaddvq_f32(accv[t]) + accs[t];
    }
}

/* Generation (one activation row): Q4_K straight from the packed nibbles,
 * without the unpacked block in memory. Same products, same order of
 * accumulation as mul_rows, so the results are bit-identical. */
QATTR static void QNAME(mul_rows_q4k1)(void *ctx, int start, int end)
{
    const struct qjob *j = ctx;
    const struct qmat *m = j->m;
    const struct qact *A = j->a;
    const int32x4_t z = vdupq_n_s32(0);
    const uint8x16_t m4 = vdupq_n_u8(0x0f);
    struct wblk wb; /* scales only */
    for (int r = start; r < end; r++) {
        const u8 *row = m->data + (size_t)r * m->row_bytes;
        float32x4_t acc = vdupq_n_f32(0.0f);
        float accs = 0.0f;
        for (int c0 = 0; c0 < m->cols; c0 += 256) {
            const u8 *p = row + (size_t)(c0 / 256) * 144;
            __builtin_prefetch(p + 144 * 4);
            k_scales(p, &wb);
            const int blk0 = c0 / 32;
            const i8 *aq = A->q + (size_t)blk0 * 32;
            const float *ad = A->d + blk0;
            float32x4_t c0v = vmulq_f32(vld1q_f32(wb.d32), vld1q_f32(ad));
            float32x4_t c1v = vmulq_f32(vld1q_f32(wb.d32 + 4), vld1q_f32(ad + 4));
            float32x4_t v0 = acc, v1 = vdupq_n_f32(0.0f);
            const u8 *q = p + 16;
#define Q4(c, cv, l0, l1)                                                                          \
    {                                                                                              \
        uint8x16_t x0 = vld1q_u8(q + 32 * (c)), x1 = vld1q_u8(q + 32 * (c) + 16);                  \
        int32x4_t pe = QDOT(z, vreinterpretq_s8_u8(vandq_u8(x0, m4)), vld1q_s8(aq + 64 * (c)));   \
        pe = QDOT(pe, vreinterpretq_s8_u8(vandq_u8(x1, m4)), vld1q_s8(aq + 64 * (c) + 16));       \
        v0 = vmlaq_laneq_f32(v0, vcvtq_f32_s32(pe), cv, l0);                                      \
        int32x4_t po = QDOT(z, vreinterpretq_s8_u8(vshrq_n_u8(x0, 4)), vld1q_s8(aq + 64 * (c) + 32)); \
        po = QDOT(po, vreinterpretq_s8_u8(vshrq_n_u8(x1, 4)), vld1q_s8(aq + 64 * (c) + 48));      \
        v1 = vmlaq_laneq_f32(v1, vcvtq_f32_s32(po), cv, l1);                                      \
    }
            Q4(0, c0v, 0, 1) Q4(1, c0v, 2, 3) Q4(2, c1v, 0, 1) Q4(3, c1v, 2, 3)
#undef Q4
            acc = vaddq_f32(v0, v1);
            const float *as = A->s + blk0;
            float32x4_t mm = vmulq_f32(vld1q_f32(wb.mn), vld1q_f32(as));
            mm = vmlaq_f32(mm, vld1q_f32(wb.mn + 4), vld1q_f32(as + 4));
            accs += vaddvq_f32(mm);
        }
        j->out[r] = vaddvq_f32(acc) + accs;
    }
}

/* Generation with Q6_K weights, the same way (groups of 16 values, taken in
 * the order mul_rows takes them: g = 8n + h + 2k). */
QATTR static void QNAME(mul_rows_q6k1)(void *ctx, int start, int end)
{
    const struct qjob *j = ctx;
    const struct qmat *m = j->m;
    const struct qact *A = j->a;
    const int32x4_t z = vdupq_n_s32(0);
    const uint8x16_t m4 = vdupq_n_u8(0x0f), m3 = vdupq_n_u8(3);
    const int8x16_t o32 = vdupq_n_s8(32);
    for (int r = start; r < end; r++) {
        const u8 *row = m->data + (size_t)r * m->row_bytes;
        float32x4_t acc = vdupq_n_f32(0.0f);
        for (int c0 = 0; c0 < m->cols; c0 += 256) {
            const u8 *p = row + (size_t)(c0 / 256) * 210;
            __builtin_prefetch(p + 210 * 4);
            const u8 *ql = p, *qh = p + 128;
            float d = rdh(p + 208);
            int8x16_t s8 = vld1q_s8((const i8 *)(p + 192));
            int16x8_t slo = vmovl_s8(vget_low_s8(s8)), shi = vmovl_high_s8(s8);
            const int blk0 = c0 / 32;
            const i8 *aq = A->q + (size_t)blk0 * 32;
            const float *ad = A->d + blk0;
            float32x4_t a0 = vld1q_f32(ad), a1 = vld1q_f32(ad + 4);
            float32x4_t cv[4];
            cv[0] = vmulq_f32(vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(slo))), d), vzip1q_f32(a0, a0));
            cv[1] = vmulq_f32(vmulq_n_f32(vcvtq_f32_s32(vmovl_high_s16(slo)), d), vzip2q_f32(a0, a0));
            cv[2] = vmulq_f32(vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(shi))), d), vzip1q_f32(a1, a1));
            cv[3] = vmulq_f32(vmulq_n_f32(vcvtq_f32_s32(vmovl_high_s16(shi)), d), vzip2q_f32(a1, a1));
            float32x4_t v0 = acc, v1 = vdupq_n_f32(0.0f);
#define G6(g, qv, v)                                                                               \
    {                                                                                              \
        int32x4_t pp = QDOT(z, vsubq_s8(vreinterpretq_s8_u8(qv), o32), vld1q_s8(aq + 16 * (g)));  \
        v = vmlaq_laneq_f32(v, vcvtq_f32_s32(pp), cv[(g) / 4], (g) % 4);                          \
    }
#define NH(n, h, v)                                                                                \
    {                                                                                              \
        uint8x16_t l0 = vld1q_u8(ql + 64 * (n) + 16 * (h));                                        \
        uint8x16_t l1 = vld1q_u8(ql + 64 * (n) + 32 + 16 * (h));                                   \
        uint8x16_t hb = vld1q_u8(qh + 32 * (n) + 16 * (h));                                        \
        G6(8 * (n) + (h) + 0, vorrq_u8(vandq_u8(l0, m4), vshlq_n_u8(vandq_u8(hb, m3), 4)), v)      \
        G6(8 * (n) + (h) + 2,                                                                      \
           vorrq_u8(vandq_u8(l1, m4), vshlq_n_u8(vandq_u8(vshrq_n_u8(hb, 2), m3), 4)), v)          \
        G6(8 * (n) + (h) + 4,                                                                      \
           vorrq_u8(vshrq_n_u8(l0, 4), vshlq_n_u8(vandq_u8(vshrq_n_u8(hb, 4), m3), 4)), v)         \
        G6(8 * (n) + (h) + 6, vorrq_u8(vshrq_n_u8(l1, 4), vshlq_n_u8(vshrq_n_u8(hb, 6), 4)), v)    \
    }
            NH(0, 0, v0) NH(0, 1, v1) NH(1, 0, v0) NH(1, 1, v1)
#undef NH
#undef G6
            acc = vaddq_f32(v0, v1);
        }
        j->out[r] = vaddvq_f32(acc);
    }
}
