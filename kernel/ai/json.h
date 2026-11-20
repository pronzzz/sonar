/* Minimal JSON (RFC 8259) for the agent harness: parse tool-call arguments
 * written by the model, and write session log events. Numbers are kept as
 * integers (fractions are ignored); \u escapes outside ASCII become '?'. */
#pragma once
#include "types.h"

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct jval {
    int type;
    bool b;
    long num;
    char *str;            /* J_STR */
    struct jval *items;   /* J_ARR, J_OBJ: n values */
    char **keys;          /* J_OBJ: n keys */
    int n;
};

/* Parse one JSON value (leading/trailing space allowed). NULL on error. */
struct jval *json_parse(const char *s, size_t len);
void json_release(struct jval *v); /* free a json_parse result */
void json_free(struct jval *v);    /* free what v holds (v itself stays) */

const struct jval *json_get(const struct jval *obj, const char *key);
const char *json_str(const struct jval *v);       /* NULL unless a string */
long json_int(const struct jval *v, long def);

/* Growable output buffer for building JSON text. */
struct jbuf {
    char *p;
    size_t len, cap;
    bool oom;
};
void jb_init(struct jbuf *b);
void jb_raw(struct jbuf *b, const char *s);
void jb_str(struct jbuf *b, const char *s); /* quoted and escaped */
void jb_kstr(struct jbuf *b, const char *key, const char *val); /* "key":"val" with comma */
void jb_kint(struct jbuf *b, const char *key, long val);
void jb_kbool(struct jbuf *b, const char *key, bool val);
void jb_value(struct jbuf *b, const struct jval *v); /* serialize v */
void jb_free(struct jbuf *b);
