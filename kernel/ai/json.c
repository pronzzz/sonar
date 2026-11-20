/* Minimal JSON parser and writer (see json.h). */
#include "json.h"
#include "kernel.h"
#include "mm.h"
#include "string.h"

#define MAX_DEPTH 32

struct parser {
    const char *s, *end;
    int depth;
};

static void skip_ws(struct parser *p)
{
    while (p->s < p->end && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r'))
        p->s++;
}

static bool parse_value(struct parser *p, struct jval *v);

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static char *parse_string(struct parser *p)
{
    if (p->s >= p->end || *p->s != '"')
        return NULL;
    p->s++;
    /* the decoded string is never longer than the source */
    const char *q = p->s;
    while (q < p->end && *q != '"') {
        if (*q == '\\')
            q++;
        q++;
    }
    if (q >= p->end)
        return NULL;
    char *out = kmalloc((size_t)(q - p->s) + 1);
    if (!out)
        return NULL;
    size_t n = 0;
    while (p->s < p->end && *p->s != '"') {
        char c = *p->s++;
        if (c != '\\') {
            out[n++] = c;
            continue;
        }
        if (p->s >= p->end)
            break;
        c = *p->s++;
        switch (c) {
        case 'n': out[n++] = '\n'; break;
        case 't': out[n++] = '\t'; break;
        case 'r': out[n++] = '\r'; break;
        case 'b': out[n++] = '\b'; break;
        case 'f': out[n++] = '\f'; break;
        case 'u': {
            int cp = 0;
            for (int i = 0; i < 4; i++) {
                int h = p->s < p->end ? hexval(*p->s) : -1;
                if (h < 0) {
                    kfree(out);
                    return NULL;
                }
                cp = cp * 16 + h;
                p->s++;
            }
            out[n++] = cp < 128 ? (char)cp : '?';
            break;
        }
        default: out[n++] = c; break; /* \" \\ \/ */
        }
    }
    if (p->s >= p->end) {
        kfree(out);
        return NULL;
    }
    p->s++; /* closing quote */
    out[n] = 0;
    return out;
}

static bool push_item(struct jval *v, char *key, struct jval *item)
{
    if ((v->n & 7) == 0) {
        struct jval *ni = krealloc(v->items, sizeof(struct jval) * (size_t)(v->n + 8));
        if (!ni)
            return false;
        v->items = ni;
        if (v->type == J_OBJ) {
            char **nk = krealloc(v->keys, sizeof(char *) * (size_t)(v->n + 8));
            if (!nk)
                return false;
            v->keys = nk;
        }
    }
    v->items[v->n] = *item;
    if (v->type == J_OBJ)
        v->keys[v->n] = key;
    v->n++;
    return true;
}

static bool parse_container(struct parser *p, struct jval *v, char close)
{
    if (++p->depth > MAX_DEPTH)
        return false;
    p->s++;
    skip_ws(p);
    if (p->s < p->end && *p->s == close) {
        p->s++;
        p->depth--;
        return true;
    }
    for (;;) {
        char *key = NULL;
        skip_ws(p);
        if (close == '}') {
            key = parse_string(p);
            if (!key)
                return false;
            skip_ws(p);
            if (p->s >= p->end || *p->s != ':') {
                kfree(key);
                return false;
            }
            p->s++;
        }
        struct jval item;
        memset(&item, 0, sizeof(item));
        if (!parse_value(p, &item) || !push_item(v, key, &item)) {
            kfree(key);
            json_free(&item);
            return false;
        }
        skip_ws(p);
        if (p->s >= p->end)
            return false;
        if (*p->s == ',') {
            p->s++;
            continue;
        }
        if (*p->s == close) {
            p->s++;
            p->depth--;
            return true;
        }
        return false;
    }
}

static bool literal(struct parser *p, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(p->end - p->s) < n || memcmp(p->s, word, n))
        return false;
    p->s += n;
    return true;
}

static bool parse_value(struct parser *p, struct jval *v)
{
    skip_ws(p);
    if (p->s >= p->end)
        return false;
    char c = *p->s;
    if (c == '{') {
        v->type = J_OBJ;
        return parse_container(p, v, '}');
    }
    if (c == '[') {
        v->type = J_ARR;
        return parse_container(p, v, ']');
    }
    if (c == '"') {
        v->type = J_STR;
        v->str = parse_string(p);
        return v->str != NULL;
    }
    if (literal(p, "true")) {
        v->type = J_BOOL, v->b = true;
        return true;
    }
    if (literal(p, "false")) {
        v->type = J_BOOL;
        return true;
    }
    if (literal(p, "null")) {
        v->type = J_NULL;
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        bool neg = c == '-';
        if (neg)
            p->s++;
        long n = 0;
        bool digits = false;
        while (p->s < p->end && *p->s >= '0' && *p->s <= '9') {
            n = n * 10 + (*p->s++ - '0');
            digits = true;
        }
        /* fraction and exponent are accepted and ignored */
        while (p->s < p->end && (*p->s == '.' || *p->s == 'e' || *p->s == 'E' || *p->s == '+' ||
                                 *p->s == '-' || (*p->s >= '0' && *p->s <= '9')))
            p->s++;
        v->type = J_NUM, v->num = neg ? -n : n;
        return digits;
    }
    return false;
}

struct jval *json_parse(const char *s, size_t len)
{
    struct parser p = { s, s + len, 0 };
    struct jval *v = kzalloc(sizeof(*v));
    if (!v)
        return NULL;
    if (!parse_value(&p, v)) {
        json_free(v);
        kfree(v);
        return NULL;
    }
    skip_ws(&p);
    if (p.s != p.end) {
        json_free(v);
        kfree(v);
        return NULL;
    }
    return v;
}

/* Frees what v holds (and its children), not v itself, so it works for
 * embedded values. json_release frees a json_parse result. */
void json_free(struct jval *v)
{
    if (!v)
        return;
    for (int i = 0; i < v->n; i++) {
        json_free(&v->items[i]);
        if (v->keys)
            kfree(v->keys[i]);
    }
    kfree(v->items);
    kfree(v->keys);
    kfree(v->str);
    memset(v, 0, sizeof(*v));
}

const struct jval *json_get(const struct jval *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (int i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key))
            return &obj->items[i];
    return NULL;
}

const char *json_str(const struct jval *v) { return v && v->type == J_STR ? v->str : NULL; }

long json_int(const struct jval *v, long def)
{
    if (v && v->type == J_NUM)
        return v->num;
    if (v && v->type == J_STR && v->str[0])
        return strtol(v->str, NULL, 10);
    return def;
}

/* ---- writer ---- */
void jb_init(struct jbuf *b) { memset(b, 0, sizeof(*b)); }

static void jb_put(struct jbuf *b, const char *s, size_t n)
{
    if (b->oom)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = MAX(b->cap * 2, b->len + n + 256);
        char *np = krealloc(b->p, cap);
        if (!np) {
            b->oom = true;
            return;
        }
        b->p = np, b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void jb_raw(struct jbuf *b, const char *s) { jb_put(b, s, strlen(s)); }

void jb_str(struct jbuf *b, const char *s)
{
    jb_put(b, "\"", 1);
    const char *run = s;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        const char *esc = NULL;
        char u[8];
        if (c == '"')
            esc = "\\\"";
        else if (c == '\\')
            esc = "\\\\";
        else if (c == '\n')
            esc = "\\n";
        else if (c == '\t')
            esc = "\\t";
        else if (c == '\r')
            esc = "\\r";
        else if (c < 0x20) {
            snprintf(u, sizeof(u), "\\u%04x", c);
            esc = u;
        }
        if (esc) {
            jb_put(b, run, (size_t)(s - run));
            jb_raw(b, esc);
            run = s + 1;
        }
    }
    jb_put(b, run, (size_t)(s - run));
    jb_put(b, "\"", 1);
}

static void jb_key(struct jbuf *b, const char *key)
{
    if (b->len && b->p[b->len - 1] != '{' && b->p[b->len - 1] != '[')
        jb_put(b, ",", 1);
    jb_str(b, key);
    jb_put(b, ":", 1);
}

void jb_kstr(struct jbuf *b, const char *key, const char *val)
{
    jb_key(b, key);
    jb_str(b, val ? val : "");
}

void jb_kint(struct jbuf *b, const char *key, long val)
{
    char num[24];
    snprintf(num, sizeof(num), "%ld", val);
    jb_key(b, key);
    jb_raw(b, num);
}

void jb_kbool(struct jbuf *b, const char *key, bool val)
{
    jb_key(b, key);
    jb_raw(b, val ? "true" : "false");
}

void jb_free(struct jbuf *b)
{
    kfree(b->p);
    jb_init(b);
}

void json_release(struct jval *v)
{
    json_free(v);
    kfree(v);
}

void jb_value(struct jbuf *b, const struct jval *v)
{
    char num[24];
    switch (v->type) {
    case J_NULL: jb_raw(b, "null"); break;
    case J_BOOL: jb_raw(b, v->b ? "true" : "false"); break;
    case J_NUM:
        snprintf(num, sizeof(num), "%ld", v->num);
        jb_raw(b, num);
        break;
    case J_STR: jb_str(b, v->str); break;
    case J_ARR:
    case J_OBJ:
        jb_raw(b, v->type == J_ARR ? "[" : "{");
        for (int i = 0; i < v->n; i++) {
            if (i)
                jb_raw(b, ",");
            if (v->type == J_OBJ) {
                jb_str(b, v->keys[i]);
                jb_raw(b, ":");
            }
            jb_value(b, &v->items[i]);
        }
        jb_raw(b, v->type == J_ARR ? "]" : "}");
        break;
    }
}
