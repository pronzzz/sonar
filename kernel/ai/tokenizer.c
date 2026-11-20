/*
 * Byte-level BPE tokenizer.
 *
 * Encoding = pre-tokenization (regex split) + BPE merges per piece:
 *   1. Split text with the model's pre-tokenizer pattern. Implemented as a
 *      hand-written matcher equivalent to the regex
 *        (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,D}
 *        | ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+
 *      with D = 1 (qwen2) or 3 (llama3).
 *   2. Map each byte of a piece to its GPT-2 "byte-level" unicode char.
 *   3. Repeatedly merge the adjacent pair with the lowest merge rank.
 */
#include "tokenizer.h"
#include "unicode_data.h"

/* ---------------- unicode helpers ---------------- */

static bool in_ranges(const u32 (*r)[2], int n, u32 cp)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < r[mid][0])
            hi = mid - 1;
        else if (cp > r[mid][1])
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

#define NLETTER ((int)(sizeof(ucd_letters) / sizeof(ucd_letters[0])))
#define NNUMBER ((int)(sizeof(ucd_numbers) / sizeof(ucd_numbers[0])))

static bool is_letter(u32 cp)
{
    if (cp < 128)
        return (cp | 32) >= 'a' && (cp | 32) <= 'z';
    return in_ranges(ucd_letters, NLETTER, cp);
}

static bool is_number(u32 cp)
{
    if (cp < 128)
        return cp >= '0' && cp <= '9';
    return in_ranges(ucd_numbers, NNUMBER, cp);
}

/* Whitespace as defined by llama.cpp's unicode tables. */
static bool is_space(u32 cp)
{
    return (cp >= 0x09 && cp <= 0x0d) || cp == 0x20 || cp == 0x85 || cp == 0xa0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202f || cp == 0x205f || cp == 0x3000;
}

/* Decode one UTF-8 sequence; invalid input yields U+FFFD consuming 1 byte. */
static u32 utf8_decode(const u8 *s, int len, int *adv)
{
    u8 c = s[0];
    if (c < 0x80) {
        *adv = 1;
        return c;
    }
    int n = (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
    if (n == 0 || n > len) {
        *adv = 1;
        return 0xfffd;
    }
    u32 cp = c & (0xff >> (n + 1));
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            *adv = 1;
            return 0xfffd;
        }
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    *adv = n;
    return cp;
}

static int utf8_encode(u32 cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* ---------------- hash tables ---------------- */

static u32 fnv1a(const char *s, u32 len)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < len; i++) {
        h ^= (u8)s[i];
        h *= 16777619u;
    }
    return h;
}

static u64 mix64(u64 x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static int lookup_n(const struct tokenizer *t, const char *s, u32 len)
{
    u32 i = fnv1a(s, len) & t->hash_mask;
    for (;;) {
        u32 v = t->hash[i];
        if (!v)
            return -1;
        int id = (int)v - 1;
        if (t->len[id] == len && memcmp(t->str[id], s, len) == 0)
            return id;
        i = (i + 1) & t->hash_mask;
    }
}

int tokenizer_find(const struct tokenizer *t, const char *s) { return lookup_n(t, s, (u32)strlen(s)); }

static const struct merge *merge_find(const struct tokenizer *t, int a, int b)
{
    u64 key = (u64)(u32)a << 32 | (u32)b;
    u32 i = (u32)mix64(key) & t->merge_mask;
    for (;;) {
        const struct merge *m = &t->merges[i];
        if (m->rank == 0xffffffffu)
            return NULL;
        if (m->key == key)
            return m;
        i = (i + 1) & t->merge_mask;
    }
}

/* ---------------- loading ---------------- */

static void seterr(char *err, size_t n, const char *m)
{
    if (err && n) {
        size_t l = strlen(m) < n - 1 ? strlen(m) : n - 1;
        memcpy(err, m, l);
        err[l] = 0;
    }
}

void tokenizer_free(struct tokenizer *t)
{
    ai_free(t->str);
    ai_free(t->len);
    ai_free(t->type);
    ai_free(t->pool);
    ai_free(t->hash);
    ai_free(t->merges);
    memset(t, 0, sizeof(*t));
}

static void build_byte_map(struct tokenizer *t)
{
    /* GPT-2 bytes_to_unicode(): printable bytes map to themselves, the rest
     * to 256, 257, ... in byte order. */
    int n = 0;
    for (int b = 0; b < 256; b++) {
        bool printable = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255);
        t->byte_cp[b] = printable ? (u16)b : (u16)(256 + n++);
    }
    for (int b = 0; b < 256; b++)
        t->cp_byte[t->byte_cp[b]] = (u8)b;
}

int tokenizer_load(struct tokenizer *t, const struct gguf *g, char *err, size_t errlen)
{
    memset(t, 0, sizeof(*t));
    char model[32] = "", pre[64] = "";
    gguf_get_str(g, "tokenizer.ggml.model", model, sizeof(model));
    if (strcmp(model, "gpt2") != 0) {
        seterr(err, errlen, "unsupported tokenizer (need byte-level BPE 'gpt2')");
        return -1;
    }
    gguf_get_str(g, "tokenizer.ggml.pre", pre, sizeof(pre));
    if (strcmp(pre, "qwen2") == 0 || strcmp(pre, "deepseek-r1-qwen") == 0) {
        t->pre = TOK_PRE_QWEN2;
    } else if (strcmp(pre, "llama-bpe") == 0 || strcmp(pre, "llama3") == 0 ||
               strcmp(pre, "default") == 0 || pre[0] == 0) {
        t->pre = TOK_PRE_LLAMA3;
        t->ignore_merges = true;
    } else {
        ai_log("tokenizer: unknown pre-tokenizer '%s', using llama3 rules\n", pre);
        t->pre = TOK_PRE_LLAMA3;
        t->ignore_merges = true;
    }

    const struct gguf_kv *toks = gguf_find(g, "tokenizer.ggml.tokens");
    const struct gguf_kv *types = gguf_find(g, "tokenizer.ggml.token_type");
    const struct gguf_kv *merges = gguf_find(g, "tokenizer.ggml.merges");
    if (!toks || toks->type != GGUF_ARR || toks->v.arr.type != GGUF_STR || !merges ||
        merges->type != GGUF_ARR || merges->v.arr.type != GGUF_STR) {
        seterr(err, errlen, "tokenizer vocabulary or merges missing");
        return -1;
    }
    int n = (int)toks->v.arr.n;
    t->n_vocab = n;
    const u8 *end = g->buf + g->buf_len;

    /* Copy token strings into one pool. */
    size_t total = 0;
    const u8 *p = toks->v.arr.data;
    for (int i = 0; i < n; i++) {
        u64 l;
        if (!gguf_arr_next_str(&p, end, &l)) {
            seterr(err, errlen, "corrupt token list");
            return -1;
        }
        total += l + 1;
    }
    t->str = ai_calloc((size_t)n, sizeof(char *));
    t->len = ai_calloc((size_t)n, sizeof(u32));
    t->type = ai_calloc((size_t)n, 1);
    t->pool = ai_alloc(total);
    u32 cap = 1;
    while (cap < (u32)n * 2)
        cap <<= 1;
    t->hash = ai_calloc(cap, sizeof(u32));
    t->hash_mask = cap - 1;
    if (!t->str || !t->len || !t->type || !t->pool || !t->hash) {
        seterr(err, errlen, "out of memory");
        tokenizer_free(t);
        return -1;
    }
    p = toks->v.arr.data;
    char *w = t->pool;
    for (int i = 0; i < n; i++) {
        u64 l;
        const char *s = gguf_arr_next_str(&p, end, &l);
        memcpy(w, s, l);
        w[l] = 0;
        t->str[i] = w;
        t->len[i] = (u32)l;
        w += l + 1;
        u32 h = fnv1a(t->str[i], t->len[i]) & t->hash_mask;
        while (t->hash[h])
            h = (h + 1) & t->hash_mask;
        t->hash[h] = (u32)i + 1;
        t->type[i] = TOK_NORMAL;
    }
    if (types && types->type == GGUF_ARR && (types->v.arr.type == GGUF_I32 ||
                                              types->v.arr.type == GGUF_U32) &&
        types->v.arr.n == (u64)n) {
        for (int i = 0; i < n; i++) {
            i32 v;
            memcpy(&v, types->v.arr.data + 4 * (size_t)i, 4);
            t->type[i] = (u8)v;
        }
    }

    /* Merge table: (left id, right id) -> (rank, merged id). */
    int nm = (int)merges->v.arr.n;
    u32 mcap = 1;
    while (mcap < (u32)nm * 2 + 2)
        mcap <<= 1;
    t->merges = ai_alloc(mcap * sizeof(struct merge));
    if (!t->merges) {
        seterr(err, errlen, "out of memory");
        tokenizer_free(t);
        return -1;
    }
    for (u32 i = 0; i < mcap; i++)
        t->merges[i].rank = 0xffffffffu;
    t->merge_mask = mcap - 1;
    p = merges->v.arr.data;
    char buf[512];
    int skipped = 0;
    for (int i = 0; i < nm; i++) {
        u64 l;
        const char *s = gguf_arr_next_str(&p, end, &l);
        if (!s) {
            seterr(err, errlen, "corrupt merge list");
            tokenizer_free(t);
            return -1;
        }
        const char *sp = memchr(s, ' ', l);
        if (!sp || l >= sizeof(buf)) {
            skipped++;
            continue;
        }
        u32 la = (u32)(sp - s), lb = (u32)(l - la - 1);
        int a = lookup_n(t, s, la);
        int b = lookup_n(t, sp + 1, lb);
        memcpy(buf, s, la);
        memcpy(buf + la, sp + 1, lb);
        int m = lookup_n(t, buf, la + lb);
        if (a < 0 || b < 0 || m < 0) {
            skipped++;
            continue;
        }
        u64 key = (u64)(u32)a << 32 | (u32)b;
        u32 h = (u32)mix64(key) & t->merge_mask;
        while (t->merges[h].rank != 0xffffffffu && t->merges[h].key != key)
            h = (h + 1) & t->merge_mask;
        if (t->merges[h].rank == 0xffffffffu) { /* keep the first (lowest) rank */
            t->merges[h].key = key;
            t->merges[h].rank = (u32)i;
            t->merges[h].merged = (u32)m;
            t->n_merges++;
        }
    }
    if (skipped)
        ai_log("tokenizer: %d merges skipped (tokens not in vocabulary)\n", skipped);

    build_byte_map(t);
    for (int b = 0; b < 256; b++) {
        char u[4];
        int ul = utf8_encode(t->byte_cp[b], u);
        t->byte_tok[b] = lookup_n(t, u, (u32)ul);
        if (t->byte_tok[b] < 0) {
            seterr(err, errlen, "vocabulary lacks byte-level tokens");
            tokenizer_free(t);
            return -1;
        }
    }
    u32 v;
    t->bos = gguf_get_u32(g, "tokenizer.ggml.bos_token_id", &v) ? (int)v : -1;
    t->eos = gguf_get_u32(g, "tokenizer.ggml.eos_token_id", &v) ? (int)v : -1;
    return 0;
}

/* ---------------- encoding ---------------- */

/* BPE-encode one pre-tokenized piece of raw bytes. */
static int bpe_piece(const struct tokenizer *t, const u8 *s, int len, int *out, int max)
{
    if (len <= 0)
        return 0;
    /* Whole piece as byte-level text (for ignore_merges and short-cuts). */
    if (t->ignore_merges && len <= 128) {
        char buf[512];
        int bl = 0;
        for (int i = 0; i < len; i++)
            bl += utf8_encode(t->byte_cp[s[i]], buf + bl);
        int id = lookup_n(t, buf, (u32)bl);
        if (id >= 0) {
            if (max < 1)
                return -1;
            out[0] = id;
            return 1;
        }
    }
    int sym_small[256];
    int *sym = len <= 256 ? sym_small : ai_alloc((size_t)len * sizeof(int));
    if (!sym)
        return -1;
    int n = len;
    for (int i = 0; i < len; i++)
        sym[i] = t->byte_tok[s[i]];
    for (;;) {
        u32 best = 0xffffffffu;
        int bi = -1;
        int merged = -1;
        for (int i = 0; i + 1 < n; i++) {
            const struct merge *m = merge_find(t, sym[i], sym[i + 1]);
            if (m && m->rank < best) {
                best = m->rank;
                bi = i;
                merged = (int)m->merged;
            }
        }
        if (bi < 0)
            break;
        sym[bi] = merged;
        memmove(&sym[bi + 1], &sym[bi + 2], (size_t)(n - bi - 2) * sizeof(int));
        n--;
    }
    int r = n <= max ? n : -1;
    if (r > 0)
        memcpy(out, sym, (size_t)n * sizeof(int));
    if (sym != sym_small)
        ai_free(sym);
    return r;
}

struct cps {
    u32 *cp;
    int *off; /* byte offset of each codepoint, plus one past the end */
    int n;
};

static inline u32 cp_at(const struct cps *c, int i) { return i < c->n ? c->cp[i] : 0xffffffffu; }

#define F_LETTER 1
#define F_NUMBER 2
#define F_SPACE 4
#define F_VALID 8

static inline int flags_at(const struct cps *c, int i)
{
    if (i >= c->n)
        return 0;
    u32 cp = c->cp[i];
    return F_VALID | (is_letter(cp) ? F_LETTER : 0) | (is_number(cp) ? F_NUMBER : 0) |
           (is_space(cp) ? F_SPACE : 0);
}

static inline u32 ascii_lower(u32 c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Length (in codepoints) of the next pre-token starting at pos. */
static int next_piece(const struct cps *c, int pos, int max_digits)
{
    u32 cpt = cp_at(c, pos);
    int flags = flags_at(c, pos);
    int start = pos;

    /* (?i:'s|'t|'re|'ve|'m|'ll|'d) */
    if (cpt == '\'' && pos + 1 < c->n) {
        u32 c1 = ascii_lower(cp_at(c, pos + 1));
        if (c1 == 's' || c1 == 't' || c1 == 'm' || c1 == 'd')
            return 2;
        if (pos + 2 < c->n) {
            u32 c2 = ascii_lower(cp_at(c, pos + 2));
            if ((c1 == 'r' && c2 == 'e') || (c1 == 'v' && c2 == 'e') || (c1 == 'l' && c2 == 'l'))
                return 3;
        }
    }
    /* [^\r\n\p{L}\p{N}]?\p{L}+ */
    if (!(cpt == '\r' || cpt == '\n' || (flags & F_NUMBER))) {
        if ((flags & F_LETTER) || (flags_at(c, pos + 1) & F_LETTER)) {
            pos++;
            while (flags_at(c, pos) & F_LETTER)
                pos++;
            return pos - start;
        }
    }
    /* \p{N}{1,max_digits} */
    if (flags & F_NUMBER) {
        while ((flags_at(c, pos) & F_NUMBER) && pos - start < max_digits)
            pos++;
        return pos - start;
    }
    /* <space>?[^\s\p{L}\p{N}]+[\r\n]* */
    int f2 = cpt == ' ' ? flags_at(c, pos + 1) : flags;
    if (!(f2 & (F_SPACE | F_LETTER | F_NUMBER)) && (f2 & F_VALID)) {
        pos += cpt == ' ';
        for (;;) {
            f2 = flags_at(c, pos);
            if ((f2 & (F_SPACE | F_LETTER | F_NUMBER)) || !(f2 & F_VALID))
                break;
            pos++;
        }
        while (cp_at(c, pos) == '\r' || cp_at(c, pos) == '\n')
            pos++;
        return pos - start;
    }
    int nws = 0, last_nl_end = 0;
    while (flags_at(c, pos + nws) & F_SPACE) {
        u32 w = cp_at(c, pos + nws);
        if (w == '\r' || w == '\n')
            last_nl_end = pos + nws + 1;
        nws++;
    }
    /* \s*[\r\n]+ */
    if (last_nl_end > 0)
        return last_nl_end - start;
    /* \s+(?!\S) */
    if (nws > 1 && pos + nws < c->n)
        return nws - 1;
    /* \s+ */
    if (nws > 0)
        return nws;
    return 1;
}

int tokenizer_encode(const struct tokenizer *t, const char *text, int len, int *out, int max)
{
    struct cps c;
    c.cp = ai_alloc((size_t)(len + 1) * sizeof(u32));
    c.off = ai_alloc((size_t)(len + 2) * sizeof(int));
    if (!c.cp || !c.off) {
        ai_free(c.cp);
        ai_free(c.off);
        return -1;
    }
    c.n = 0;
    const u8 *s = (const u8 *)text;
    for (int i = 0; i < len;) {
        int adv;
        u32 cp = utf8_decode(s + i, len - i, &adv);
        c.cp[c.n] = cp;
        c.off[c.n] = i;
        c.n++;
        i += adv;
    }
    c.off[c.n] = len;
    int max_digits = t->pre == TOK_PRE_QWEN2 ? 1 : 3;
    int count = 0;
    for (int pos = 0; pos < c.n;) {
        int k = next_piece(&c, pos, max_digits);
        int b0 = c.off[pos], b1 = c.off[pos + k];
        int r = bpe_piece(t, s + b0, b1 - b0, out + count, max - count);
        if (r < 0) {
            count = -1;
            break;
        }
        count += r;
        pos += k;
    }
    ai_free(c.cp);
    ai_free(c.off);
    return count;
}

int tokenizer_decode(const struct tokenizer *t, int id, char *out, int max)
{
    if (id < 0 || id >= t->n_vocab || max <= 0)
        return 0;
    int ty = t->type[id];
    if (ty == TOK_UNUSED)
        return 0;
    const char *s = t->str[id];
    int len = (int)t->len[id];
    if (ty == TOK_CONTROL || ty == TOK_USER_DEFINED) {
        int n = len < max ? len : max;
        memcpy(out, s, (size_t)n);
        return n;
    }
    int o = 0;
    for (int i = 0; i < len && o < max;) {
        int adv;
        u32 cp = utf8_decode((const u8 *)s + i, len - i, &adv);
        i += adv;
        if (cp < 324)
            out[o++] = (char)t->cp_byte[cp];
        else
            o += 0; /* not a byte-level char: cannot occur in a valid vocab */
    }
    return o;
}
