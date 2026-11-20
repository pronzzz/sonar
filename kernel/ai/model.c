/*
 * Transformer forward pass (llama / qwen2 / qwen3 GGUF graphs).
 *
 * Per layer:  h = x + Wo * Attn(RoPE(norm_q(Wq*n(x)+bq)), RoPE(norm_k(Wk*n(x)+bk)), Wv*n(x)+bv)
 *             x = h + Wdown * (silu(Wgate*n(h)) * Wup*n(h))
 * with RMSNorm n(), grouped-query attention, an fp16 KV cache, and
 * attention computed with an online (streaming) softmax.
 */
#include "model.h"
#include "aimath.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define HEADER_MAX (512ULL << 20)

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* Read the GGUF header, growing the buffer until it parses. */
static int read_header(struct ai_file *f, struct gguf *g, char **bufout, char *err, size_t errlen)
{
    u64 want = 8ULL << 20;
    for (;;) {
        if (want > f->size)
            want = f->size;
        char *buf;
        if (f->mapped) {
            buf = (char *)f->mapped;
        } else {
            buf = ai_alloc(want);
            if (!buf) {
                seterr(err, errlen, "out of memory reading model header");
                return -1;
            }
            if (!f->read(f->ctx, 0, buf, want)) {
                ai_free(buf);
                seterr(err, errlen, "I/O error reading model header");
                return -1;
            }
        }
        int r = gguf_parse(g, (const u8 *)buf, f->mapped ? f->size : want, err, errlen);
        if (r == 0) {
            *bufout = f->mapped ? NULL : buf;
            return 0;
        }
        if (!f->mapped)
            ai_free(buf);
        if (r < 0 || f->mapped || want >= f->size || want >= HEADER_MAX) {
            if (r > 0)
                seterr(err, errlen, "model header is truncated or too large");
            return -1;
        }
        want *= 4;
    }
}

static bool kget_u32(const struct gguf *g, const char *arch, const char *key, u32 *out)
{
    char k[128];
    snprintf(k, sizeof(k), "%s.%s", arch, key);
    return gguf_get_u32(g, k, out);
}

static bool kget_f32(const struct gguf *g, const char *arch, const char *key, float *out)
{
    char k[128];
    snprintf(k, sizeof(k), "%s.%s", arch, key);
    return gguf_get_f32(g, k, out);
}

static int fill_info(const struct gguf *g, struct model_info *in, char *err, size_t errlen)
{
    memset(in, 0, sizeof(*in));
    char arch[32] = "";
    gguf_get_str(g, "general.architecture", arch, sizeof(arch));
    if (strcmp(arch, "llama") == 0) {
        in->arch = ARCH_LLAMA;
        in->rope_neox = false;
    } else if (strcmp(arch, "qwen2") == 0) {
        in->arch = ARCH_QWEN2;
        in->rope_neox = true;
    } else if (strcmp(arch, "qwen3") == 0) {
        in->arch = ARCH_QWEN3;
        in->rope_neox = true;
    } else {
        seterr(err, errlen, "unsupported model architecture '%s'", arch);
        return -1;
    }
    snprintf(in->arch_name, sizeof(in->arch_name), "%s", arch);
    if (!gguf_get_str(g, "general.name", in->name, sizeof(in->name)))
        snprintf(in->name, sizeof(in->name), "unnamed model");
    gguf_get_str(g, "general.size_label", in->size_label, sizeof(in->size_label));
    u32 v;
    if (!kget_u32(g, arch, "block_count", &v)) goto missing;
    in->n_layer = (int)v;
    if (!kget_u32(g, arch, "embedding_length", &v)) goto missing;
    in->n_embd = (int)v;
    if (!kget_u32(g, arch, "feed_forward_length", &v)) goto missing;
    in->n_ff = (int)v;
    if (!kget_u32(g, arch, "attention.head_count", &v)) goto missing;
    in->n_head = (int)v;
    in->n_head_kv = kget_u32(g, arch, "attention.head_count_kv", &v) ? (int)v : in->n_head;
    in->head_dim = kget_u32(g, arch, "attention.key_length", &v) ? (int)v : in->n_embd / in->n_head;
    u32 vlen;
    if (kget_u32(g, arch, "attention.value_length", &vlen) && (int)vlen != in->head_dim) {
        seterr(err, errlen, "models with key_length != value_length are not supported");
        return -1;
    }
    in->n_rot = kget_u32(g, arch, "rope.dimension_count", &v) ? (int)v : in->head_dim;
    in->ctx_train = kget_u32(g, arch, "context_length", &v) ? (int)v : 4096;
    in->rope_base = 10000.0f;
    kget_f32(g, arch, "rope.freq_base", &in->rope_base);
    in->rms_eps = 1e-6f;
    kget_f32(g, arch, "attention.layer_norm_rms_epsilon", &in->rms_eps);
    char k[128], st[32] = "";
    snprintf(k, sizeof(k), "%s.rope.scaling.type", arch);
    gguf_get_str(g, k, st, sizeof(st));
    if (strcmp(st, "yarn") == 0) {
        float factor = 1.0f;
        kget_f32(g, arch, "rope.scaling.factor", &factor);
        u32 orig = 0;
        kget_u32(g, arch, "rope.scaling.original_context_length", &orig);
        if (factor > 1.0f && orig > 0) {
            in->yarn = true;
            in->yarn_factor = factor;
            in->yarn_orig_ctx = (int)orig;
        }
    } else if (st[0] && strcmp(st, "none") != 0) {
        seterr(err, errlen, "unsupported RoPE scaling '%s'", st);
        return -1;
    }
    if (in->n_layer <= 0 || in->n_embd <= 0 || in->n_head <= 0 || in->n_head_kv <= 0 ||
        in->n_head % in->n_head_kv || in->head_dim <= 0 || in->head_dim > 512 ||
        in->n_rot > in->head_dim || in->n_rot % 2) {
        seterr(err, errlen, "inconsistent model hyper-parameters");
        return -1;
    }
    const struct gguf_tensor *emb = gguf_find_tensor(g, "token_embd.weight");
    if (!emb) {
        seterr(err, errlen, "model has no token_embd.weight");
        return -1;
    }
    in->n_vocab = (int)emb->ne[1];
    /* Parameter count, weight bytes and dominant quantization. */
    u64 type_bytes[32] = { 0 };
    for (u64 i = 0; i < g->n_tensors; i++) {
        const struct gguf_tensor *t = &g->tensors[i];
        in->n_params += t->ne[0] * t->ne[1] * t->ne[2] * t->ne[3];
        in->weight_bytes += t->nbytes;
        if (t->type >= 0 && t->type < 32)
            type_bytes[t->type] += t->nbytes;
    }
    int best = 0;
    for (int i = 0; i < 32; i++)
        if (type_bytes[i] > type_bytes[best])
            best = i;
    snprintf(in->quant, sizeof(in->quant), "%s", ggml_type_name(best));
    return 0;
missing:
    seterr(err, errlen, "model is missing required hyper-parameters");
    return -1;
}

int model_probe(struct ai_file *f, struct model_info *info, char *err, size_t errlen)
{
    struct gguf g;
    char *buf = NULL;
    if (read_header(f, &g, &buf, err, errlen) < 0)
        return -1;
    int r = fill_info(&g, info, err, errlen);
    info->file_size = f->size;
    gguf_free(&g);
    ai_free(buf);
    return r;
}

u64 model_ram_needed(const struct model_info *in, int n_ctx)
{
    u64 kv = 2ULL * (u64)in->n_layer * (u64)n_ctx * (u64)(in->n_head_kv * in->head_dim) * 2;
    u64 act = (u64)AI_MAX_BATCH * 4 *
              (u64)(3 * in->n_embd + 2 * in->n_head * in->head_dim + 2 * in->n_ff);
    u64 tok = (u64)in->n_vocab * 64; /* vocab strings, hash, merges */
    return in->weight_bytes + kv + act + tok + (u64)in->n_vocab * 4 + (64ULL << 20);
}

/* ---------------- loading ---------------- */

struct loader {
    struct model *m;
    const struct gguf *g;
    char *err;
    size_t errlen;
    float *store;
    size_t store_used, store_cap;
    bool failed;
};

static const struct gguf_tensor *need_tensor(struct loader *L, const char *name, bool optional)
{
    const struct gguf_tensor *t = gguf_find_tensor(L->g, name);
    if (!t && !optional && !L->failed) {
        seterr(L->err, L->errlen, "model is missing tensor %s", name);
        L->failed = true;
    }
    return t;
}

static const u8 *tensor_data(struct loader *L, const struct gguf_tensor *t)
{
    return L->m->weights + t->offset;
}

static bool load_mat(struct loader *L, struct qmat *q, const char *name, int cols, int rows)
{
    const struct gguf_tensor *t = need_tensor(L, name, false);
    if (!t)
        return false;
    if ((int)t->ne[0] != cols || (int)t->ne[1] != rows) {
        seterr(L->err, L->errlen, "tensor %s has shape [%llu,%llu], expected [%d,%d]", name,
               (unsigned long long)t->ne[0], (unsigned long long)t->ne[1], cols, rows);
        L->failed = true;
        return false;
    }
    if (!qmat_init(q, tensor_data(L, t), t->type, cols, rows)) {
        seterr(L->err, L->errlen, "tensor %s uses unsupported format %s", name,
               ggml_type_name(t->type));
        L->failed = true;
        return false;
    }
    return true;
}

/* Small vectors (norm weights, biases) are converted to f32 copies. */
static float *load_vec(struct loader *L, const char *name, int n, bool optional)
{
    const struct gguf_tensor *t = need_tensor(L, name, optional);
    if (!t)
        return NULL;
    if ((int)(t->ne[0] * t->ne[1] * t->ne[2] * t->ne[3]) != n ||
        (t->type != GGML_TYPE_F32 && t->type != GGML_TYPE_F16 && t->type != GGML_TYPE_BF16)) {
        seterr(L->err, L->errlen, "tensor %s has unexpected size or type", name);
        L->failed = true;
        return NULL;
    }
    if (L->store_used + (size_t)n > L->store_cap) {
        seterr(L->err, L->errlen, "internal: vector store overflow");
        L->failed = true;
        return NULL;
    }
    float *dst = L->store + L->store_used;
    L->store_used += (size_t)n;
    dequantize(t->type, tensor_data(L, t), dst, n);
    return dst;
}

static void compute_rope(struct model *m)
{
    const struct model_info *in = &m->info;
    int half = in->n_rot / 2;
    double base = in->rope_base;
    m->rope_mscale = 1.0f;
    double low = 0, high = 0;
    if (in->yarn) {
        /* YaRN (as in llama.cpp ggml_rope_yarn_corr_dims / HF _compute_yarn_parameters) */
        const double beta_fast = 32.0, beta_slow = 1.0, pi = 3.14159265358979323846;
        double nd = in->n_rot;
        double lo = nd * ai_ln(in->yarn_orig_ctx / (beta_fast * 2 * pi)) / (2 * ai_ln(base));
        double hi = nd * ai_ln(in->yarn_orig_ctx / (beta_slow * 2 * pi)) / (2 * ai_ln(base));
        low = ai_floor(lo);
        high = ai_ceil(hi);
        if (low < 0)
            low = 0;
        if (high > nd - 1)
            high = nd - 1;
        m->rope_mscale = (float)(1.0 + 0.1 * ai_ln(in->yarn_factor));
    }
    for (int i = 0; i < half; i++) {
        double freq = ai_pow(base, -2.0 * i / in->n_rot);
        if (m->rope_freq_factors)
            freq /= m->rope_freq_factors[i];
        if (in->yarn) {
            double interp = freq / in->yarn_factor;
            double y = (i - low) / (high - low > 0.001 ? high - low : 0.001);
            double ramp = 1.0 - (y < 0 ? 0 : y > 1 ? 1 : y); /* 1 = extrapolate */
            freq = interp * (1.0 - ramp) + freq * ramp;
        }
        m->rope_inv_freq[i] = (float)freq;
    }
}

void model_free(struct model *m)
{
    tokenizer_free(&m->tok);
    if (m->weights && m->weights_size) /* size 0: memory-mapped, not owned */
        ai_free_large(m->weights, m->weights_size);
    ai_free(m->rope_cos);
    ai_free(m->rope_sin);
    ai_free(m->f32_store);
    ai_free(m->layers);
    if (m->k_cache)
        ai_free_large(m->k_cache, (size_t)m->info.n_layer * m->n_ctx * m->info.n_head_kv * m->info.head_dim * 2);
    if (m->v_cache)
        ai_free_large(m->v_cache, (size_t)m->info.n_layer * m->n_ctx * m->info.n_head_kv * m->info.head_dim * 2);
    ai_free(m->rope_inv_freq);
    ai_free(m->x);
    ai_free(m->xb);
    ai_free(m->q);
    ai_free(m->k);
    ai_free(m->v);
    ai_free(m->att);
    ai_free(m->hb);
    ai_free(m->hb2);
    ai_free(m->logits);
    ai_free(m->cached);
    ai_free(m->header);
    memset(m, 0, sizeof(*m));
}

int model_load(struct model *m, struct ai_file *f, int n_ctx, ai_progress_fn progress, void *pctx,
               char *err, size_t errlen)
{
    memset(m, 0, sizeof(*m));
    struct gguf g;
    if (read_header(f, &g, &m->header, err, errlen) < 0)
        return -1;
    struct model_info *in = &m->info;
    if (fill_info(&g, in, err, errlen) < 0)
        goto fail;
    in->file_size = f->size;
    if (n_ctx <= 0 || n_ctx > in->ctx_train)
        n_ctx = n_ctx <= 0 ? 4096 : in->ctx_train;
    m->n_ctx = n_ctx;
    if (tokenizer_load(&m->tok, &g, err, errlen) < 0)
        goto fail;

    /* Check every tensor's format and extent before reading gigabytes. */
    u64 avail = f->size > g.data_offset ? f->size - g.data_offset : 0;
    u64 data_size = 0; /* end of the last tensor: a raw disk may hold more */
    for (u64 i = 0; i < g.n_tensors; i++) {
        const struct gguf_tensor *t = &g.tensors[i];
        if (!t->nbytes) {
            seterr(err, errlen, "tensor %s uses unsupported format %s (type %d)", t->name,
                   ggml_type_name(t->type), t->type);
            goto fail;
        }
        if (t->offset > avail || t->nbytes > avail - t->offset) {
            seterr(err, errlen, "tensor %s lies outside the file (truncated download?)", t->name);
            goto fail;
        }
        if (t->offset + t->nbytes > data_size)
            data_size = t->offset + t->nbytes;
    }

    /* Weights: one contiguous region holding the whole tensor data section. */
    m->weights_size = data_size;
    if (f->mapped) {
        m->weights = (u8 *)f->mapped + g.data_offset;
        m->weights_size = 0; /* not owned */
    } else {
        m->weights = ai_alloc_large(data_size);
        if (!m->weights) {
            seterr(err, errlen, "not enough memory for %llu MiB of weights",
                   (unsigned long long)(data_size >> 20));
            goto fail;
        }
        const u64 step = 16ULL << 20;
        /* Weights in mapped chunks are read through a contiguous buffer,
         * since disk drivers DMA to physical addresses. */
        u8 *bounce = ai_large_dma_ok(m->weights) ? NULL : ai_alloc_large(step);
        if (!ai_large_dma_ok(m->weights) && !bounce) {
            seterr(err, errlen, "out of memory");
            goto fail;
        }
        for (u64 off = 0; off < data_size; off += step) {
            u64 n = data_size - off < step ? data_size - off : step;
            bool ok = f->read(f->ctx, g.data_offset + off, bounce ? bounce : m->weights + off, n);
            if (ok && bounce)
                memcpy(m->weights + off, bounce, n);
            if (!ok || ai_should_abort()) {
                ai_free_large(bounce, step);
                bounce = NULL;
            }
            if (!ok) {
                seterr(err, errlen, "I/O error reading weights at offset %llu",
                       (unsigned long long)(g.data_offset + off));
                goto fail;
            }
            if (progress)
                progress(pctx, off + n, data_size);
            if (ai_should_abort()) {
                seterr(err, errlen, "cancelled");
                goto fail;
            }
        }
        ai_free_large(bounce, step);
    }

    struct loader L = { m, &g, err, errlen, NULL, 0, 0, false };
    const int E = in->n_embd, H = in->n_head, KV = in->n_head_kv, D = in->head_dim;
    L.store_cap = (size_t)E * (1 + 2 * in->n_layer) + (size_t)in->n_layer * (2 * D + H * D + 2 * KV * D) +
                  (size_t)in->n_rot;
    L.store = ai_alloc(L.store_cap * sizeof(float));
    m->f32_store = L.store;
    m->layers = ai_calloc((size_t)in->n_layer, sizeof(struct layer_w));
    if (!L.store || !m->layers) {
        seterr(err, errlen, "out of memory");
        goto fail;
    }
    load_mat(&L, &m->tok_embd, "token_embd.weight", E, in->n_vocab);
    m->output_norm = load_vec(&L, "output_norm.weight", E, false);
    if (gguf_find_tensor(&g, "output.weight"))
        load_mat(&L, &m->output, "output.weight", E, in->n_vocab);
    else
        m->output = m->tok_embd; /* tied embeddings */
    m->rope_freq_factors = load_vec(&L, "rope_freqs.weight", in->n_rot / 2, true);
    char name[96];
    for (int l = 0; l < in->n_layer && !L.failed; l++) {
        struct layer_w *w = &m->layers[l];
#define T(suffix) (snprintf(name, sizeof(name), "blk.%d.%s", l, suffix), name)
        w->attn_norm = load_vec(&L, T("attn_norm.weight"), E, false);
        w->ffn_norm = load_vec(&L, T("ffn_norm.weight"), E, false);
        load_mat(&L, &w->wq, T("attn_q.weight"), E, H * D);
        load_mat(&L, &w->wk, T("attn_k.weight"), E, KV * D);
        load_mat(&L, &w->wv, T("attn_v.weight"), E, KV * D);
        load_mat(&L, &w->wo, T("attn_output.weight"), H * D, E);
        load_mat(&L, &w->w_gate, T("ffn_gate.weight"), E, in->n_ff);
        load_mat(&L, &w->w_up, T("ffn_up.weight"), E, in->n_ff);
        load_mat(&L, &w->w_down, T("ffn_down.weight"), in->n_ff, E);
        w->bq = load_vec(&L, T("attn_q.bias"), H * D, true);
        w->bk = load_vec(&L, T("attn_k.bias"), KV * D, true);
        w->bv = load_vec(&L, T("attn_v.bias"), KV * D, true);
        if (in->arch == ARCH_QWEN3) {
            w->q_norm = load_vec(&L, T("attn_q_norm.weight"), D, false);
            w->k_norm = load_vec(&L, T("attn_k_norm.weight"), D, false);
        }
#undef T
    }
    if (L.failed)
        goto fail;

    /* Runtime buffers. */
    size_t kv_elems = (size_t)in->n_layer * (size_t)n_ctx * (size_t)(KV * D);
    m->k_cache = ai_alloc_large(kv_elems * 2);
    m->v_cache = ai_alloc_large(kv_elems * 2);
    const size_t B = AI_MAX_BATCH;
    m->rope_inv_freq = ai_alloc(sizeof(float) * (size_t)(in->n_rot / 2 + 1));
    m->rope_cos = ai_alloc(sizeof(float) * B * (size_t)(in->n_rot / 2 + 1));
    m->rope_sin = ai_alloc(sizeof(float) * B * (size_t)(in->n_rot / 2 + 1));
    m->x = ai_alloc(sizeof(float) * B * E);
    m->xb = ai_alloc(sizeof(float) * B * (size_t)(E > H * D ? E : H * D));
    m->q = ai_alloc(sizeof(float) * B * (size_t)(H * D));
    m->k = ai_alloc(sizeof(float) * B * (size_t)(KV * D));
    m->v = ai_alloc(sizeof(float) * B * (size_t)(KV * D));
    m->att = ai_alloc(sizeof(float) * B * (size_t)(H * D));
    m->hb = ai_alloc(sizeof(float) * B * (size_t)in->n_ff);
    m->hb2 = ai_alloc(sizeof(float) * B * (size_t)in->n_ff);
    m->logits = ai_alloc(sizeof(float) * (size_t)in->n_vocab);
    m->cached = ai_alloc(sizeof(int) * (size_t)n_ctx);
    if (!m->k_cache || !m->v_cache || !m->rope_inv_freq || !m->rope_cos || !m->rope_sin || !m->x || !m->xb || !m->q || !m->k ||
        !m->v || !m->att || !m->hb || !m->hb2 || !m->logits || !m->cached) {
        seterr(err, errlen, "not enough memory for a %d-token context", n_ctx);
        goto fail;
    }
    compute_rope(m);
    gguf_free(&g);
    ai_free(m->header);
    m->header = NULL;
    return 0;
fail:
    gguf_free(&g);
    model_free(m);
    return -1;
}

/* ---------------- forward pass ---------------- */

static void rmsnorm(float *out, const float *x, const float *w, int n, float eps)
{
    double ss = 0.0;
    for (int i = 0; i < n; i++)
        ss += (double)x[i] * (double)x[i];
    float scale = (float)(1.0 / ai_sqrt(ss / n + eps));
    for (int i = 0; i < n; i++)
        out[i] = x[i] * scale * w[i];
}

static void rope_apply(float *v, int n_heads, int head_dim, int n_rot, bool neox, const float *cs,
                       const float *sn)
{
    int half = n_rot / 2;
    for (int h = 0; h < n_heads; h++) {
        float *x = v + h * head_dim;
        if (neox) {
            for (int i = 0; i < half; i++) {
                float a = x[i], b = x[i + half];
                x[i] = a * cs[i] - b * sn[i];
                x[i + half] = a * sn[i] + b * cs[i];
            }
        } else {
            for (int i = 0; i < half; i++) {
                float a = x[2 * i], b = x[2 * i + 1];
                x[2 * i] = a * cs[i] - b * sn[i];
                x[2 * i + 1] = a * sn[i] + b * cs[i];
            }
        }
    }
}

static inline void store_f16(u16 *dst, const float *src, int n)
{
#if defined(__aarch64__)
    int i = 0;
    for (; i + 4 <= n; i += 4)
        vst1_f16((float16_t *)(dst + i), vcvt_f16_f32(vld1q_f32(src + i)));
    for (; i < n; i++)
        dst[i] = f32_to_fp16(src[i]);
#else
    for (int i = 0; i < n; i++)
        dst[i] = f32_to_fp16(src[i]);
#endif
}

static inline float dot_f32_f16(const float *a, const u16 *b, int n)
{
#if defined(__aarch64__)
    float32x4_t s0 = vdupq_n_f32(0), s1 = s0;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        float16x8_t h = vld1q_f16((const float16_t *)(b + i));
        s0 = vfmaq_f32(s0, vld1q_f32(a + i), vcvt_f32_f16(vget_low_f16(h)));
        s1 = vfmaq_f32(s1, vld1q_f32(a + i + 4), vcvt_high_f32_f16(h));
    }
    float r = vaddvq_f32(vaddq_f32(s0, s1));
    for (; i < n; i++)
        r += a[i] * fp16_to_f32(b[i]);
    return r;
#else
    float r = 0;
    for (int i = 0; i < n; i++)
        r += a[i] * fp16_to_f32(b[i]);
    return r;
#endif
}

static inline void axpy_f16(float *acc, float w, const u16 *v, int n)
{
#if defined(__aarch64__)
    float32x4_t wv = vdupq_n_f32(w);
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        float16x8_t h = vld1q_f16((const float16_t *)(v + i));
        vst1q_f32(acc + i, vfmaq_f32(vld1q_f32(acc + i), wv, vcvt_f32_f16(vget_low_f16(h))));
        vst1q_f32(acc + i + 4, vfmaq_f32(vld1q_f32(acc + i + 4), wv, vcvt_high_f32_f16(h)));
    }
    for (; i < n; i++)
        acc[i] += w * fp16_to_f32(v[i]);
#else
    for (int i = 0; i < n; i++)
        acc[i] += w * fp16_to_f32(v[i]);
#endif
}

static inline void scale_vec(float *a, float s, int n)
{
    for (int i = 0; i < n; i++)
        a[i] *= s;
}

struct attn_job {
    struct model *m;
    int layer, T, pos0;
};

/* One (token, head) pair per index: streaming softmax over the cache. */
static void attn_task(void *ctx, int start, int end)
{
    struct attn_job *j = ctx;
    struct model *m = j->m;
    const struct model_info *in = &m->info;
    const int D = in->head_dim, H = in->n_head, KV = in->n_head_kv;
    const int kvd = KV * D, group = H / KV;
    const float scale = 1.0f / ai_sqrtf((float)D);
    const u16 *kc = m->k_cache + (size_t)j->layer * m->n_ctx * kvd;
    const u16 *vc = m->v_cache + (size_t)j->layer * m->n_ctx * kvd;
    float acc[512];
    for (int idx = start; idx < end; idx++) {
        int t = idx / H, h = idx % H, g = h / group;
        const float *q = m->q + (size_t)t * H * D + (size_t)h * D;
        int npos = j->pos0 + t + 1; /* causal: positions 0 .. pos0+t */
        float mx = -1e30f, sum = 0.0f;
        for (int d = 0; d < D; d++)
            acc[d] = 0.0f;
        for (int p = 0; p < npos; p++) {
            float s = dot_f32_f16(q, kc + (size_t)p * kvd + (size_t)g * D, D) * scale;
            if (s > mx) {
                float c = ai_expf(mx - s);
                sum *= c;
                scale_vec(acc, c, D);
                mx = s;
            }
            float w = ai_expf(s - mx);
            sum += w;
            axpy_f16(acc, w, vc + (size_t)p * kvd + (size_t)g * D, D);
        }
        float *out = m->att + (size_t)t * H * D + (size_t)h * D;
        float inv = 1.0f / sum;
        for (int d = 0; d < D; d++)
            out[d] = acc[d] * inv;
    }
}

static float silu(float x) { return x / (1.0f + ai_expf(-x)); }

static void forward_batch(struct model *m, const int *tokens, int T, int pos0, bool want_logits)
{
    const struct model_info *in = &m->info;
    const int E = in->n_embd, H = in->n_head, KV = in->n_head_kv, D = in->head_dim;
    const int QD = H * D, KD = KV * D, F = in->n_ff;
    const int half = in->n_rot / 2;

    for (int t = 0; t < T; t++) {
        qmat_get_row(&m->tok_embd, tokens[t], m->x + (size_t)t * E);
        double pos = pos0 + t;
        float *cs = m->rope_cos + (size_t)t * half, *sn = m->rope_sin + (size_t)t * half;
        for (int i = 0; i < half; i++) {
            double th = pos * (double)m->rope_inv_freq[i];
            cs[i] = (float)ai_cos(th) * m->rope_mscale;
            sn[i] = (float)ai_sin(th) * m->rope_mscale;
        }
    }
    for (int l = 0; l < in->n_layer; l++) {
        const struct layer_w *w = &m->layers[l];
        for (int t = 0; t < T; t++)
            rmsnorm(m->xb + (size_t)t * E, m->x + (size_t)t * E, w->attn_norm, E, in->rms_eps);
        qmat_mul(&w->wq, m->xb, E, m->q, QD, T);
        qmat_mul(&w->wk, m->xb, E, m->k, KD, T);
        qmat_mul(&w->wv, m->xb, E, m->v, KD, T);
        for (int t = 0; t < T; t++) {
            float *q = m->q + (size_t)t * QD, *k = m->k + (size_t)t * KD, *v = m->v + (size_t)t * KD;
            if (w->bq)
                for (int i = 0; i < QD; i++)
                    q[i] += w->bq[i];
            if (w->bk)
                for (int i = 0; i < KD; i++)
                    k[i] += w->bk[i];
            if (w->bv)
                for (int i = 0; i < KD; i++)
                    v[i] += w->bv[i];
            if (w->q_norm) {
                for (int h = 0; h < H; h++)
                    rmsnorm(q + h * D, q + h * D, w->q_norm, D, in->rms_eps);
                for (int h = 0; h < KV; h++)
                    rmsnorm(k + h * D, k + h * D, w->k_norm, D, in->rms_eps);
            }
            const float *cs = m->rope_cos + (size_t)t * half, *sn = m->rope_sin + (size_t)t * half;
            rope_apply(q, H, D, in->n_rot, in->rope_neox, cs, sn);
            rope_apply(k, KV, D, in->n_rot, in->rope_neox, cs, sn);
            size_t off = ((size_t)l * m->n_ctx + (size_t)(pos0 + t)) * KD;
            store_f16(m->k_cache + off, k, KD);
            store_f16(m->v_cache + off, v, KD);
        }
        struct attn_job aj = { m, l, T, pos0 };
        ai_parallel_for(T * H, 1, attn_task, &aj);
        qmat_mul(&w->wo, m->att, QD, m->xb, E, T);
        for (size_t i = 0; i < (size_t)T * E; i++)
            m->x[i] += m->xb[i];

        for (int t = 0; t < T; t++)
            rmsnorm(m->xb + (size_t)t * E, m->x + (size_t)t * E, w->ffn_norm, E, in->rms_eps);
        qmat_mul(&w->w_gate, m->xb, E, m->hb, F, T);
        qmat_mul(&w->w_up, m->xb, E, m->hb2, F, T);
        for (size_t i = 0; i < (size_t)T * F; i++)
            m->hb[i] = silu(m->hb[i]) * m->hb2[i];
        qmat_mul(&w->w_down, m->hb, F, m->xb, E, T);
        for (size_t i = 0; i < (size_t)T * E; i++)
            m->x[i] += m->xb[i];
    }
    if (want_logits) {
        float *last = m->x + (size_t)(T - 1) * E;
        rmsnorm(m->xb, last, m->output_norm, E, in->rms_eps);
        qmat_mul(&m->output, m->xb, E, m->logits, in->n_vocab, 1);
    }
}

float *model_forward(struct model *m, const int *tokens, int n)
{
    if (n <= 0 || m->n_past + n > m->n_ctx)
        return NULL;
    for (int i = 0; i < n; i++)
        if (tokens[i] < 0 || tokens[i] >= m->info.n_vocab)
            return NULL;
    for (int done = 0; done < n;) {
        int T = n - done < AI_MAX_BATCH ? n - done : AI_MAX_BATCH;
        forward_batch(m, tokens + done, T, m->n_past, done + T == n);
        memcpy(m->cached + m->n_past, tokens + done, sizeof(int) * (size_t)T);
        m->n_past += T;
        done += T;
        if (done < n && ai_should_abort())
            return NULL;
    }
    return m->logits;
}

float *model_feed(struct model *m, const int *tokens, int n)
{
    if (n <= 0)
        return NULL;
    int common = 0;
    while (common < m->n_past && common < n && m->cached[common] == tokens[common])
        common++;
    if (common == n) /* need logits for the last token: recompute it */
        common = n - 1;
    m->n_past = common;
    return model_forward(m, tokens + common, n - common);
}
