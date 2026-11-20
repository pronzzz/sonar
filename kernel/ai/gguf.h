/* GGUF model container parser (versions 2 and 3). */
#pragma once

#include "platform.h"

enum gguf_vtype {
    GGUF_U8 = 0, GGUF_I8 = 1, GGUF_U16 = 2, GGUF_I16 = 3, GGUF_U32 = 4, GGUF_I32 = 5,
    GGUF_F32 = 6, GGUF_BOOL = 7, GGUF_STR = 8, GGUF_ARR = 9, GGUF_U64 = 10, GGUF_I64 = 11,
    GGUF_F64 = 12,
};

struct gguf_kv {
    char *key;
    int type;
    union {
        u64 u;
        i64 i;
        double f;
        struct {
            const char *s; /* not NUL-terminated */
            u64 len;
        } str;
        struct {
            int type;
            u64 n;
            const u8 *data; /* raw element data inside the header buffer */
        } arr;
    } v;
};

struct gguf_tensor {
    char *name;
    int n_dims;
    u64 ne[4];
    int type;
    u64 offset; /* relative to the data section */
    u64 nbytes;
};

struct gguf {
    u32 version;
    u64 n_kv;
    struct gguf_kv *kv;
    u64 n_tensors;
    struct gguf_tensor *tensors;
    u64 alignment;
    u64 data_offset; /* absolute file offset of the tensor data section */
    const u8 *buf;   /* header bytes (owned by caller) */
    u64 buf_len;
};

/* Returns 0 on success, 1 if buf is too short (retry with more of the file),
 * or -1 if the file is invalid (message in err). */
int gguf_parse(struct gguf *g, const u8 *buf, u64 len, char *err, size_t errlen);
void gguf_free(struct gguf *g);

const struct gguf_kv *gguf_find(const struct gguf *g, const char *key);
bool gguf_get_u64(const struct gguf *g, const char *key, u64 *out);
bool gguf_get_u32(const struct gguf *g, const char *key, u32 *out);
bool gguf_get_f32(const struct gguf *g, const char *key, float *out);
bool gguf_get_bool(const struct gguf *g, const char *key, bool *out);
/* Copies a string value into out (NUL-terminated); false if missing. */
bool gguf_get_str(const struct gguf *g, const char *key, char *out, size_t size);
const struct gguf_tensor *gguf_find_tensor(const struct gguf *g, const char *name);

/* Iterate a string array: returns pointer to the next string, advancing *p. */
const char *gguf_arr_next_str(const u8 **p, const u8 *end, u64 *len);
