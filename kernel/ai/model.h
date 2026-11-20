/* Decoder-only transformer inference for the DeepSeek-R1 model family:
 * Qwen2 (R1-Distill-Qwen 1.5B/7B/14B/32B), Qwen3 (R1-0528-Qwen3-8B) and
 * Llama (R1-Distill-Llama 8B/70B) architectures, from GGUF files. */
#pragma once

#include "gguf.h"
#include "quant.h"
#include "tokenizer.h"

enum model_arch { ARCH_LLAMA, ARCH_QWEN2, ARCH_QWEN3 };

struct model_info {
    char name[96];
    char arch_name[16];
    char size_label[16];
    int arch;
    int n_layer, n_embd, n_ff, n_head, n_head_kv, head_dim, n_vocab, n_rot;
    int ctx_train;
    float rope_base, rms_eps;
    bool rope_neox;
    bool yarn;
    float yarn_factor;
    int yarn_orig_ctx;
    u64 n_params;
    u64 weight_bytes;
    u64 file_size;
    char quant[16]; /* dominant quantization type */
};

/* Read only the GGUF header and describe the model (no weights loaded). */
int model_probe(struct ai_file *f, struct model_info *info, char *err, size_t errlen);

/* Bytes of RAM needed to run with the given context length. */
u64 model_ram_needed(const struct model_info *info, int n_ctx);

struct layer_w {
    float *attn_norm, *ffn_norm, *q_norm, *k_norm;
    float *bq, *bk, *bv;
    struct qmat wq, wk, wv, wo, w_gate, w_up, w_down;
};

struct model {
    struct model_info info;
    struct tokenizer tok;
    u8 *weights;
    u64 weights_size;
    float *f32_store; /* norms/biases converted to f32 */
    struct qmat tok_embd, output;
    float *output_norm;
    float *rope_freq_factors;
    struct layer_w *layers;

    /* runtime */
    int n_ctx;
    u16 *k_cache, *v_cache; /* fp16 [layer][pos][kv_dim] */
    float *rope_inv_freq;   /* [n_rot/2] */
    float *rope_cos, *rope_sin; /* [AI_MAX_BATCH][n_rot/2] scratch */
    float rope_mscale;
    float *x, *xb, *q, *k, *v, *att, *hb, *hb2; /* activations, AI_MAX_BATCH rows */
    float *logits;
    int n_past;
    int *cached;   /* tokens currently in the KV cache */
    char *header;  /* GGUF header buffer (owned) */
};

int model_load(struct model *m, struct ai_file *f, int n_ctx, ai_progress_fn progress,
               void *pctx, char *err, size_t errlen);
void model_free(struct model *m);

/* Run the network over tokens[0..n) at positions n_past.. ; returns logits
 * for the last token (n_vocab floats, valid until the next call), or NULL
 * if the context is full or aborted. */
float *model_forward(struct model *m, const int *tokens, int n);

/* Make the KV cache hold exactly `tokens` (reusing the common prefix with
 * what is cached) and return logits for the last token. */
float *model_feed(struct model *m, const int *tokens, int n);
