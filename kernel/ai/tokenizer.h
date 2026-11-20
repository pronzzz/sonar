/* Byte-level BPE tokenizer (GPT-2 family, as used by DeepSeek R1 / Qwen /
 * Llama 3 models), loaded from GGUF metadata. */
#pragma once

#include "gguf.h"

enum tok_pre {
    TOK_PRE_QWEN2,  /* \p{N} single digits */
    TOK_PRE_LLAMA3, /* \p{N}{1,3} */
};

enum tok_type {
    TOK_NORMAL = 1,
    TOK_UNKNOWN = 2,
    TOK_CONTROL = 3,
    TOK_USER_DEFINED = 4,
    TOK_UNUSED = 5,
    TOK_BYTE = 6,
};

struct tokenizer {
    int n_vocab;
    const char **str; /* token text (byte-level unicode form), NUL-terminated */
    u32 *len;
    u8 *type;
    char *pool;
    u32 *hash; /* open addressing: token id + 1, 0 = empty */
    u32 hash_mask;
    struct merge {
        u64 key;
        u32 rank;
        u32 merged;
    } *merges;
    u32 merge_mask;
    int n_merges;
    int pre;
    bool ignore_merges;
    int byte_tok[256];
    u16 byte_cp[256];  /* byte -> unicode codepoint */
    u8 cp_byte[324];   /* codepoint (< 324) -> byte */
    int bos, eos;
};

int tokenizer_load(struct tokenizer *t, const struct gguf *g, char *err, size_t errlen);
void tokenizer_free(struct tokenizer *t);

/* Encode plain text (special tokens are NOT parsed). Returns the number of
 * tokens written, or -1 if max is too small. */
int tokenizer_encode(const struct tokenizer *t, const char *text, int len, int *out, int max);

/* Token id for an exact token string, or -1. */
int tokenizer_find(const struct tokenizer *t, const char *s);

/* Bytes of a token's text as it should be shown. Returns length. */
int tokenizer_decode(const struct tokenizer *t, int id, char *out, int max);
