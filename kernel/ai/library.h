/* Model library: GGUF models found on disks and in /models, plus a catalog
 * of the DeepSeek-R1 family for choosing what to install. */
#pragma once
#include "model.h"

struct model_entry {
    char id[80]; /* block device ("vdb", "vda2") or file path ("/models/x.gguf") */
    bool is_file;
    struct model_info info;
};

int library_scan(struct model_entry *out, int max);
bool library_open(const char *id, struct ai_file *f);
void library_close(struct ai_file *f);
/* True if the model's bytes are already in memory (loaded by the UEFI loader). */
bool library_in_memory(const char *id);

struct catalog_entry {
    const char *name;     /* model name */
    const char *repo;     /* where GGUF builds are published (Hugging Face) */
    const char *arch;     /* engine architecture */
    double params_b;      /* parameters, billions */
    double q4_gb;         /* approx. Q4_K_M GGUF size, GB (10^9 bytes) */
    int n_layer, n_kv_head, head_dim; /* for KV-cache sizing */
    bool supported;
};

extern const struct catalog_entry ai_catalog[];
extern const int ai_catalog_count;

/* Estimated RAM (bytes) to run a catalog model at n_ctx tokens. */
u64 catalog_ram(const struct catalog_entry *c, int n_ctx);
