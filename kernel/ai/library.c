/* Model discovery and the DeepSeek-R1 catalog. */
#include "library.h"
#include "block.h"
#include "fs.h"
#include "mm.h"

/*
 * The DeepSeek-R1 family as published by DeepSeek (huggingface.co/deepseek-ai).
 * Sizes are for 4-bit Q4_K_M GGUF builds; shapes are from the models'
 * config.json files and determine the KV cache size.
 */
const struct catalog_entry ai_catalog[] = {
    { "DeepSeek-R1-Distill-Qwen-1.5B", "deepseek-ai/DeepSeek-R1-Distill-Qwen-1.5B", "qwen2", 1.78,
      1.12, 28, 2, 128, true },
    { "DeepSeek-R1-Distill-Qwen-7B", "deepseek-ai/DeepSeek-R1-Distill-Qwen-7B", "qwen2", 7.62, 4.68,
      28, 4, 128, true },
    { "DeepSeek-R1-Distill-Llama-8B", "deepseek-ai/DeepSeek-R1-Distill-Llama-8B", "llama", 8.03,
      4.92, 32, 8, 128, true },
    { "DeepSeek-R1-0528-Qwen3-8B", "deepseek-ai/DeepSeek-R1-0528-Qwen3-8B", "qwen3", 8.19, 5.03, 36,
      8, 128, true },
    { "DeepSeek-R1-Distill-Qwen-14B", "deepseek-ai/DeepSeek-R1-Distill-Qwen-14B", "qwen2", 14.8,
      8.99, 48, 8, 128, true },
    { "DeepSeek-R1-Distill-Qwen-32B", "deepseek-ai/DeepSeek-R1-Distill-Qwen-32B", "qwen2", 32.8,
      19.85, 64, 8, 128, true },
    { "DeepSeek-R1-Distill-Llama-70B", "deepseek-ai/DeepSeek-R1-Distill-Llama-70B", "llama", 70.6,
      42.52, 80, 8, 128, true },
    { "DeepSeek-R1 (671B MoE)", "deepseek-ai/DeepSeek-R1", "deepseek2", 671, 404, 61, 128, 192,
      false },
};
const int ai_catalog_count = sizeof(ai_catalog) / sizeof(ai_catalog[0]);

u64 catalog_ram(const struct catalog_entry *c, int n_ctx)
{
    u64 weights = (u64)(c->q4_gb * 1e9);
    u64 kv = 2ULL * (u64)c->n_layer * (u64)n_ctx * (u64)(c->n_kv_head * c->head_dim) * 2;
    return weights + kv + (300ULL << 20);
}

/* ---- readers ---- */
static bool blk_reader(void *ctx, u64 off, void *buf, u64 len)
{
    return blk_read_bytes(ctx, off, buf, len);
}

static bool file_reader(void *ctx, u64 off, void *buf, u64 len)
{
    int fd = (int)(uintptr_t)ctx - 1;
    return fs_pread(fd, off, buf, len) == (ssize_t)len;
}

bool library_open(const char *id, struct ai_file *f)
{
    memset(f, 0, sizeof(*f));
    if (id[0] == '/') {
        int fd = fs_open(id, O_READ);
        if (fd < 0)
            return false;
        f->ctx = (void *)(uintptr_t)(fd + 1);
        f->size = fs_fsize(fd);
        f->read = file_reader;
        return true;
    }
    struct blkdev *d = blk_find(id);
    if (!d)
        return false;
    f->ctx = d;
    f->size = d->nsectors * BLK_SECTOR;
    f->read = blk_reader;
    f->mapped = d->mem; /* in memory already: the weights are used in place */
    return true;
}

bool library_in_memory(const char *id)
{
    struct blkdev *d = id[0] == '/' ? NULL : blk_find(id);
    return d && d->mem;
}

void library_close(struct ai_file *f)
{
    if (f->read == file_reader)
        fs_close((int)(uintptr_t)f->ctx - 1);
    memset(f, 0, sizeof(*f));
}

static bool probe(const char *id, bool is_file, struct model_entry *e)
{
    struct ai_file f;
    if (!library_open(id, &f))
        return false;
    u8 magic[4];
    bool ok = f.size >= 64 && f.read(f.ctx, 0, magic, 4) && !memcmp(magic, "GGUF", 4);
    if (ok) {
        char err[128];
        ok = model_probe(&f, &e->info, err, sizeof(err)) == 0;
        if (!ok)
            printk("  %s: GGUF file not usable: %s\n", id, err);
    }
    library_close(&f);
    if (ok) {
        strlcpy(e->id, id, sizeof(e->id));
        e->is_file = is_file;
    }
    return ok;
}

int library_scan(struct model_entry *out, int max)
{
    int n = 0;
    for (int i = 0; i < blk_count() && n < max; i++) {
        struct blkdev *d = blk_get(i);
        if (probe(d->name, false, &out[n]))
            n++;
    }
    struct fs_dirent *ents;
    int ne;
    if (fs_mounted() && fs_list("/models", &ents, &ne) == FS_OK) {
        for (int i = 0; i < ne && n < max; i++) {
            if (ents[i].type != FT_FILE)
                continue;
            char path[80];
            snprintf(path, sizeof(path), "/models/%s", ents[i].name);
            if (probe(path, true, &out[n]))
                n++;
        }
        kfree(ents);
    }
    return n;
}
