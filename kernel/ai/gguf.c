/*
 * GGUF parser. Every read is bounds-checked: model files come from outside
 * the system and must not be able to crash the kernel.
 */
#include "gguf.h"
#include "quant.h"

struct rd {
    const u8 *p, *end;
    bool short_read;
};

static bool need(struct rd *r, u64 n)
{
    if ((u64)(r->end - r->p) < n) {
        r->short_read = true;
        return false;
    }
    return true;
}

static u64 rd_u64(struct rd *r)
{
    if (!need(r, 8))
        return 0;
    u64 v;
    memcpy(&v, r->p, 8);
    r->p += 8;
    return v;
}

static u32 rd_u32(struct rd *r)
{
    if (!need(r, 4))
        return 0;
    u32 v;
    memcpy(&v, r->p, 4);
    r->p += 4;
    return v;
}

static const char *rd_str(struct rd *r, u64 *len)
{
    *len = rd_u64(r);
    if (r->short_read || !need(r, *len))
        return NULL;
    const char *s = (const char *)r->p;
    r->p += *len;
    return s;
}

static u64 scalar_size(int t)
{
    switch (t) {
    case GGUF_U8: case GGUF_I8: case GGUF_BOOL: return 1;
    case GGUF_U16: case GGUF_I16: return 2;
    case GGUF_U32: case GGUF_I32: case GGUF_F32: return 4;
    case GGUF_U64: case GGUF_I64: case GGUF_F64: return 8;
    default: return 0;
    }
}

static char *dup_str(const char *s, u64 len)
{
    char *d = ai_alloc(len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = 0;
    }
    return d;
}

static bool read_scalar(struct rd *r, int t, struct gguf_kv *kv)
{
    u64 sz = scalar_size(t);
    if (!sz || !need(r, sz))
        return false;
    u64 raw = 0;
    memcpy(&raw, r->p, sz);
    r->p += sz;
    switch (t) {
    case GGUF_I8: kv->v.i = (i8)raw; break;
    case GGUF_I16: kv->v.i = (i16)raw; break;
    case GGUF_I32: kv->v.i = (i32)raw; break;
    case GGUF_I64: kv->v.i = (i64)raw; break;
    case GGUF_F32: {
        float f;
        u32 r32 = (u32)raw;
        memcpy(&f, &r32, 4);
        kv->v.f = f;
        break;
    }
    case GGUF_F64: {
        double d;
        memcpy(&d, &raw, 8);
        kv->v.f = d;
        break;
    }
    default: kv->v.u = raw; break;
    }
    return true;
}

static void set_err(char *err, size_t n, const char *msg)
{
    if (err && n) {
        size_t l = strlen(msg);
        if (l >= n)
            l = n - 1;
        memcpy(err, msg, l);
        err[l] = 0;
    }
}

void gguf_free(struct gguf *g)
{
    if (g->kv) {
        for (u64 i = 0; i < g->n_kv; i++)
            ai_free(g->kv[i].key);
        ai_free(g->kv);
    }
    if (g->tensors) {
        for (u64 i = 0; i < g->n_tensors; i++)
            ai_free(g->tensors[i].name);
        ai_free(g->tensors);
    }
    memset(g, 0, sizeof(*g));
}

int gguf_parse(struct gguf *g, const u8 *buf, u64 len, char *err, size_t errlen)
{
    memset(g, 0, sizeof(*g));
    struct rd r = { buf, buf + len, false };
    if (len < 24) {
        set_err(err, errlen, "file too small");
        return len < 4 ? -1 : 1;
    }
    if (memcmp(buf, "GGUF", 4) != 0) {
        set_err(err, errlen, "not a GGUF file");
        return -1;
    }
    r.p += 4;
    g->version = rd_u32(&r);
    if (g->version != 2 && g->version != 3) {
        set_err(err, errlen, "unsupported GGUF version");
        return -1;
    }
    g->n_tensors = rd_u64(&r);
    g->n_kv = rd_u64(&r);
    if (g->n_tensors > 100000 || g->n_kv > 100000) {
        set_err(err, errlen, "implausible tensor/key count");
        return -1;
    }
    g->kv = ai_calloc(g->n_kv ? g->n_kv : 1, sizeof(struct gguf_kv));
    g->tensors = ai_calloc(g->n_tensors ? g->n_tensors : 1, sizeof(struct gguf_tensor));
    if (!g->kv || !g->tensors) {
        set_err(err, errlen, "out of memory");
        gguf_free(g);
        return -1;
    }
    g->alignment = 32;
    for (u64 i = 0; i < g->n_kv; i++) {
        struct gguf_kv *kv = &g->kv[i];
        u64 klen;
        const char *k = rd_str(&r, &klen);
        if (!k)
            goto short_or_bad;
        kv->key = dup_str(k, klen);
        kv->type = (int)rd_u32(&r);
        if (r.short_read)
            goto short_or_bad;
        if (kv->type == GGUF_STR) {
            kv->v.str.s = rd_str(&r, &kv->v.str.len);
            if (!kv->v.str.s)
                goto short_or_bad;
        } else if (kv->type == GGUF_ARR) {
            kv->v.arr.type = (int)rd_u32(&r);
            kv->v.arr.n = rd_u64(&r);
            kv->v.arr.data = r.p;
            if (r.short_read)
                goto short_or_bad;
            if (kv->v.arr.type == GGUF_STR) {
                for (u64 j = 0; j < kv->v.arr.n; j++) {
                    u64 sl;
                    if (!rd_str(&r, &sl))
                        goto short_or_bad;
                }
            } else {
                u64 es = scalar_size(kv->v.arr.type);
                if (!es || kv->v.arr.n > (u64)1 << 40) {
                    set_err(err, errlen, "unsupported metadata array type");
                    gguf_free(g);
                    return -1;
                }
                if (!need(&r, es * kv->v.arr.n))
                    goto short_or_bad;
                r.p += es * kv->v.arr.n;
            }
        } else if (!read_scalar(&r, kv->type, kv)) {
            if (r.short_read)
                goto short_or_bad;
            set_err(err, errlen, "unknown metadata value type");
            gguf_free(g);
            return -1;
        }
        if (strcmp(kv->key, "general.alignment") == 0 && kv->type == GGUF_U32 && kv->v.u &&
            (kv->v.u & (kv->v.u - 1)) == 0)
            g->alignment = kv->v.u;
    }
    for (u64 i = 0; i < g->n_tensors; i++) {
        struct gguf_tensor *t = &g->tensors[i];
        u64 nlen;
        const char *n = rd_str(&r, &nlen);
        if (!n)
            goto short_or_bad;
        t->name = dup_str(n, nlen);
        t->n_dims = (int)rd_u32(&r);
        if (r.short_read)
            goto short_or_bad;
        if (t->n_dims < 1 || t->n_dims > 4) {
            set_err(err, errlen, "tensor with invalid dimension count");
            gguf_free(g);
            return -1;
        }
        for (int d = 0; d < 4; d++)
            t->ne[d] = 1;
        for (int d = 0; d < t->n_dims; d++)
            t->ne[d] = rd_u64(&r);
        t->type = (int)rd_u32(&r);
        t->offset = rd_u64(&r);
        if (r.short_read)
            goto short_or_bad;
        u64 nel = t->ne[0] * t->ne[1] * t->ne[2] * t->ne[3];
        size_t bs = ggml_blck_size(t->type);
        if (bs && t->ne[0] % bs == 0)
            t->nbytes = nel / bs * ggml_type_size(t->type);
        else
            t->nbytes = 0; /* unknown/unsupported type: checked by the loader */
    }
    g->data_offset = ((u64)(r.p - buf) + g->alignment - 1) & ~(g->alignment - 1);
    g->buf = buf;
    g->buf_len = len;
    return 0;

short_or_bad:
    gguf_free(g);
    set_err(err, errlen, "truncated header");
    return 1;
}

const struct gguf_kv *gguf_find(const struct gguf *g, const char *key)
{
    for (u64 i = 0; i < g->n_kv; i++)
        if (g->kv[i].key && strcmp(g->kv[i].key, key) == 0)
            return &g->kv[i];
    return NULL;
}

bool gguf_get_u64(const struct gguf *g, const char *key, u64 *out)
{
    const struct gguf_kv *kv = gguf_find(g, key);
    if (!kv)
        return false;
    switch (kv->type) {
    case GGUF_U8: case GGUF_U16: case GGUF_U32: case GGUF_U64: case GGUF_BOOL:
        *out = kv->v.u;
        return true;
    case GGUF_I8: case GGUF_I16: case GGUF_I32: case GGUF_I64:
        if (kv->v.i < 0)
            return false;
        *out = (u64)kv->v.i;
        return true;
    default:
        return false;
    }
}

bool gguf_get_u32(const struct gguf *g, const char *key, u32 *out)
{
    u64 v;
    if (!gguf_get_u64(g, key, &v) || v > 0xffffffffULL)
        return false;
    *out = (u32)v;
    return true;
}

bool gguf_get_f32(const struct gguf *g, const char *key, float *out)
{
    const struct gguf_kv *kv = gguf_find(g, key);
    if (!kv)
        return false;
    if (kv->type == GGUF_F32 || kv->type == GGUF_F64) {
        *out = (float)kv->v.f;
        return true;
    }
    u64 u;
    if (gguf_get_u64(g, key, &u)) {
        *out = (float)u;
        return true;
    }
    return false;
}

bool gguf_get_bool(const struct gguf *g, const char *key, bool *out)
{
    const struct gguf_kv *kv = gguf_find(g, key);
    if (!kv || kv->type != GGUF_BOOL)
        return false;
    *out = kv->v.u != 0;
    return true;
}

bool gguf_get_str(const struct gguf *g, const char *key, char *out, size_t size)
{
    const struct gguf_kv *kv = gguf_find(g, key);
    if (!kv || kv->type != GGUF_STR || !size)
        return false;
    u64 n = kv->v.str.len < size - 1 ? kv->v.str.len : size - 1;
    memcpy(out, kv->v.str.s, n);
    out[n] = 0;
    return true;
}

const struct gguf_tensor *gguf_find_tensor(const struct gguf *g, const char *name)
{
    for (u64 i = 0; i < g->n_tensors; i++)
        if (strcmp(g->tensors[i].name, name) == 0)
            return &g->tensors[i];
    return NULL;
}

const char *gguf_arr_next_str(const u8 **p, const u8 *end, u64 *len)
{
    if ((u64)(end - *p) < 8)
        return NULL;
    u64 l;
    memcpy(&l, *p, 8);
    if ((u64)(end - *p) - 8 < l)
        return NULL;
    const char *s = (const char *)*p + 8;
    *p += 8 + l;
    *len = l;
    return s;
}
