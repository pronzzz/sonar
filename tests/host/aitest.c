/*
 * Host test driver for the Sonar AI engine.
 *
 *   aitest MODEL.gguf --info
 *   aitest MODEL.gguf --tokenize "text"
 *   aitest MODEL.gguf --chat "question" [--n 256] [--temp 0] [--no-think]
 *   aitest MODEL.gguf --raw "text to continue" [--n 32]
 *   aitest MODEL.gguf --ppl "text"            (perplexity check)
 *   common: --threads N  --ctx N  --read (no mmap: exercise the read path)
 */
#include "chat.h"
#include "aimath.h"

#include <unistd.h>

bool host_open(const char *path, struct ai_file *f, bool map);
void host_threads_init(int n);

static bool emit(void *ctx, const char *s, int len)
{
    (void)ctx;
    fwrite(s, 1, (size_t)len, stdout);
    fflush(stdout);
    return true;
}

static void progress(void *ctx, u64 done, u64 total)
{
    (void)ctx;
    if (done == total || (done >> 20) % 512 < 16)
        fprintf(stderr, "\rloading %llu / %llu MiB", (unsigned long long)(done >> 20),
                (unsigned long long)(total >> 20));
    if (done == total)
        fprintf(stderr, "\n");
}

/* Check the integer matmul path against the float reference path on every
 * quantized matrix of the model. */
static u64 rng = 0x1234567;
static float frand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (float)((rng >> 11) * (1.0 / 9007199254740992.0)) * 2.0f - 1.0f;
}

static int check_mat(const char *name, const struct qmat *q, int *bad)
{
    if (!qdot_type_supported(q->type))
        return 0;
    int cols = q->cols, rows = q->rows < 96 ? q->rows : 96, T = 3;
    float *a = malloc(sizeof(float) * cols), *b = malloc(sizeof(float) * cols);
    /* 1. unpacked int8 * scales must reproduce the float dequantizer */
    double max_ud = 0;
    for (int r = 0; r < rows; r++) {
        int rr = (int)((u64)r * (q->rows - 1) / (rows > 1 ? rows - 1 : 1));
        qmat_get_row(q, rr, a);
        qdot_unpack_row(q, rr, b);
        for (int i = 0; i < cols; i++) {
            double d = a[i] - b[i];
            if (d < 0)
                d = -d;
            double tol = 1e-6 * (a[i] < 0 ? -a[i] : a[i]) + 1e-12;
            if (d > tol && d > max_ud)
                max_ud = d;
        }
    }
    /* 2. integer matmul vs float matmul on activation-like inputs */
    struct qmat sub = *q;
    sub.rows = rows;
    float *x = malloc(sizeof(float) * cols * T), *yf = malloc(sizeof(float) * rows * T),
          *yi = malloc(sizeof(float) * rows * T), *y1 = malloc(sizeof(float) * rows);
    int t1_diff = 0;
    for (int i = 0; i < cols * T; i++)
        x[i] = frand() * (i % 97 == 0 ? 8.0f : 1.0f); /* a few outliers, like real activations */
    qmat_set_mode(QMODE_FLOAT);
    qmat_mul(&sub, x, cols, yf, rows, T);
    double num = 0, den = 0;
    for (int mode = QMODE_BASE; mode <= QMODE_DOTPROD; mode++) {
        qmat_set_mode(mode);
        qmat_mul(&sub, x, cols, yi, rows, T);
        for (int i = 0; i < rows * T; i++) {
            num += (double)(yi[i] - yf[i]) * (yi[i] - yf[i]);
            den += (double)yf[i] * yf[i];
        }
        /* 3. one row at a time (the generation kernels) must match exactly */
        for (int t = 0; t < T; t++) {
            qmat_mul(&sub, x + (size_t)t * cols, cols, y1, rows, 1);
            for (int r = 0; r < rows; r++)
                if (y1[r] != yi[(size_t)t * rows + r])
                    t1_diff++;
        }
    }
    qmat_set_mode(QMODE_AUTO);
    double rel = den > 0 ? __builtin_sqrt(num / den) : 0;
    bool ok = max_ud == 0 && rel < 0.02 && t1_diff == 0;
    if (!ok)
        (*bad)++;
    printf("  %-18s %-5s %6dx%-6d unpack-diff %.3g  int-vs-float rel err %.5f  1-row diffs %d  %s\n",
           name, ggml_type_name(q->type), q->rows, cols, max_ud, rel, t1_diff, ok ? "ok" : "FAIL");
    free(a); free(b); free(x); free(yf); free(yi); free(y1);
    return 1;
}

/* Random blocks of every integer-path format: float dequantizer vs int8
 * unpacker (two separately written decoders) must agree bit-for-bit. */
static int qtest_synth(void)
{
    const int types[] = { GGML_TYPE_Q4_0, GGML_TYPE_Q4_1, GGML_TYPE_Q5_0, GGML_TYPE_Q5_1,
                          GGML_TYPE_Q8_0, GGML_TYPE_Q4_K, GGML_TYPE_Q5_K, GGML_TYPE_Q6_K };
    int bad = 0;
    for (unsigned k = 0; k < sizeof(types) / sizeof(types[0]); k++) {
        int type = types[k], cols = 1024 + (ggml_blck_size(type) == 32 ? 96 : 0), rows = 64;
        size_t rb = (size_t)cols / ggml_blck_size(type) * ggml_type_size(type);
        u8 *data = malloc(rb * rows);
        for (size_t i = 0; i < rb * rows; i++)
            data[i] = (u8)(frand() * 128 + 128);
        /* make every fp16 scale field a sane value: positions per format */
        size_t bs = ggml_type_size(type);
        for (size_t b = 0; b < rb * rows / bs; b++) {
            u8 *p = data + b * bs;
            int nscale = (type == GGML_TYPE_Q4_1 || type == GGML_TYPE_Q5_1 ||
                          type == GGML_TYPE_Q4_K || type == GGML_TYPE_Q5_K) ? 2 : 1;
            int off = type == GGML_TYPE_Q6_K ? 208 : 0;
            for (int s = 0; s < nscale; s++) {
                u16 h = (u16)(0x2000 + (u16)((frand() + 1) * 0x0fff)); /* ~0.008 .. 0.06 */
                if (frand() < 0)
                    h |= 0x8000;
                p[off + 2 * s] = (u8)h;
                p[off + 2 * s + 1] = (u8)(h >> 8);
            }
        }
        struct qmat q;
        if (!qmat_init(&q, data, type, cols, rows)) {
            printf("  synth %-5s qmat_init failed\n", ggml_type_name(type));
            bad++;
            continue;
        }
        float *a = malloc(sizeof(float) * cols), *b = malloc(sizeof(float) * cols);
        int mism = 0;
        for (int r = 0; r < rows; r++) {
            qmat_get_row(&q, r, a);
            qdot_unpack_row(&q, r, b);
            for (int i = 0; i < cols; i++)
                if (a[i] != b[i])
                    mism++;
        }
        printf("  synth %-5s %d values, %d mismatches  %s\n", ggml_type_name(type), rows * cols, mism,
               mism ? "FAIL" : "ok");
        bad += mism != 0;
        free(a); free(b); free(data);
    }
    return bad;
}

static int qtest(struct model *m)
{
    int sb = qtest_synth();
    int bad = 0, n = 0;
    n += check_mat("output", &m->output, &bad);
    n += check_mat("token_embd", &m->tok_embd, &bad);
    int ls[3] = { 0, m->info.n_layer / 2, m->info.n_layer - 1 };
    for (int k = 0; k < 3; k++) {
        struct layer_w *l = &m->layers[ls[k]];
        char nm[32];
        const struct qmat *mats[7] = { &l->wq, &l->wk, &l->wv, &l->wo, &l->w_gate, &l->w_up, &l->w_down };
        const char *names[7] = { "attn_q", "attn_k", "attn_v", "attn_out", "ffn_gate", "ffn_up", "ffn_down" };
        for (int i = 0; i < 7; i++) {
            snprintf(nm, sizeof(nm), "blk.%d.%s", ls[k], names[i]);
            n += check_mat(nm, mats[i], &bad);
        }
    }
    printf("%d matrices checked, %d failed; synthetic formats failed: %d\n", n, bad, sb);
    return bad || sb ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s MODEL.gguf --info|--tokenize T|--chat T|--raw T|--ppl T\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    const char *mode = NULL, *text = NULL;
    int n_gen = 256, threads = (int)sysconf(_SC_NPROCESSORS_ONLN), ctx = 4096;
    float temp = 0.0f;
    bool think = true, map = true;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--info"))
            mode = "info";
        else if (!strcmp(argv[i], "--tokenize") || !strcmp(argv[i], "--chat") ||
                 !strcmp(argv[i], "--raw") || !strcmp(argv[i], "--ppl")) {
            mode = argv[i] + 2;
            text = argv[++i];
        } else if (!strcmp(argv[i], "--n"))
            n_gen = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--temp"))
            temp = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--threads"))
            threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ctx"))
            ctx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-think"))
            think = false;
        else if (!strcmp(argv[i], "--read"))
            map = false;
        else if (!strcmp(argv[i], "--qtest"))
            mode = "qtest";
        else if (!strcmp(argv[i], "--qmode"))
            qmat_set_mode(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--tokfile")) {
            mode = "tokfile";
            text = argv[++i];
        }
    }
    if (!mode) {
        fprintf(stderr, "no mode given\n");
        return 2;
    }
    host_threads_init(threads);
    struct ai_file f;
    if (!host_open(path, &f, map)) {
        perror(path);
        return 1;
    }
    char err[256];
    struct model_info info;
    if (model_probe(&f, &info, err, sizeof(err)) < 0) {
        fprintf(stderr, "probe failed: %s\n", err);
        return 1;
    }
    fprintf(stderr,
            "model: %s (%s, %s) arch=%s layers=%d embd=%d ff=%d heads=%d/%d hd=%d vocab=%d\n"
            "       rope base=%.0f neox=%d yarn=%d(x%.1f, orig %d) params=%.2fB weights=%.2f GiB, "
            "needs %.2f GiB @ ctx %d\n",
            info.name, info.size_label, info.quant, info.arch_name, info.n_layer, info.n_embd,
            info.n_ff, info.n_head, info.n_head_kv, info.head_dim, info.n_vocab, info.rope_base,
            info.rope_neox, info.yarn, info.yarn_factor, info.yarn_orig_ctx,
            info.n_params / 1e9, info.weight_bytes / 1073741824.0,
            model_ram_needed(&info, ctx) / 1073741824.0, ctx);
    if (!strcmp(mode, "info"))
        return 0;

    static struct model m;
    u64 t0 = ai_time_ns();
    if (model_load(&m, &f, ctx, progress, NULL, err, sizeof(err)) < 0) {
        fprintf(stderr, "load failed: %s\n", err);
        return 1;
    }
    fprintf(stderr, "loaded in %.2f s (%d threads)\n", (ai_time_ns() - t0) / 1e9, threads);

    if (!strcmp(mode, "qtest"))
        return qtest(&m);
    if (!strcmp(mode, "tokfile")) {
        /* NUL-separated test strings in; one line of token ids per string out. */
        FILE *tf = fopen(text, "rb");
        if (!tf) {
            perror(text);
            return 1;
        }
        static char data[1 << 22];
        size_t len = fread(data, 1, sizeof(data) - 1, tf);
        fclose(tf);
        data[len] = 0;
        static int toks[65536];
        for (size_t p = 0; p <= len;) {
            size_t e = p;
            while (e < len && data[e])
                e++;
            int n = tokenizer_encode(&m.tok, data + p, (int)(e - p), toks, 65536);
            for (int i = 0; i < n; i++)
                printf(i ? " %d" : "%d", toks[i]);
            printf("\n");
            p = e + 1;
        }
        return 0;
    }
    if (!strcmp(mode, "tokenize")) {
        static int toks[65536];
        int n = tokenizer_encode(&m.tok, text, (int)strlen(text), toks, 65536);
        printf("%d tokens:", n);
        for (int i = 0; i < n; i++)
            printf(" %d", toks[i]);
        printf("\n");
        for (int i = 0; i < n; i++) {
            char buf[256];
            int l = tokenizer_decode(&m.tok, toks[i], buf, 255);
            buf[l] = 0;
            printf("  %6d  [%s]\n", toks[i], buf);
        }
        return 0;
    }

    struct chat c;
    chat_init(&c, &m);
    c.smp.temperature = temp;
    if (!strcmp(mode, "ppl")) {
        chat_build_raw(&c, text, true);
        int n = c.n_tokens;
        double nll = 0;
        m.n_past = 0;
        for (int i = 0; i + 1 < n; i++) {
            float *lg = model_forward(&m, &c.tokens[i], 1);
            float mx = lg[0];
            for (int j = 1; j < m.info.n_vocab; j++)
                if (lg[j] > mx)
                    mx = lg[j];
            double sum = 0;
            for (int j = 0; j < m.info.n_vocab; j++)
                sum += ai_exp(lg[j] - mx);
            double lp = lg[c.tokens[i + 1]] - mx - ai_ln(sum);
            nll -= lp;
        }
        printf("tokens=%d  avg nll=%.4f  perplexity=%.3f\n", n, nll / (n - 1),
               ai_exp(nll / (n - 1)));
        return 0;
    }
    if (!strcmp(mode, "raw")) {
        chat_build_raw(&c, text, true);
    } else {
        struct chat_turn t = { ROLE_USER, text };
        if (chat_build(&c, NULL, &t, 1, think) < 0) {
            fprintf(stderr, "prompt too long\n");
            return 1;
        }
    }
    fprintf(stderr, "prompt: %d tokens\n", c.n_tokens);
    static char reply[1 << 16];
    struct chat_stats st;
    printf("%s", strcmp(mode, "raw") ? "" : text);
    chat_generate(&c, n_gen, emit, NULL, reply, sizeof(reply), &st);
    printf("\n");
    fprintf(stderr, "\nprefill: %d tok in %.2f s (%.1f tok/s)  gen: %d tok in %.2f s (%.2f tok/s)%s\n",
            st.prefill_tokens, st.prefill_ns / 1e9, st.prefill_tokens / (st.prefill_ns / 1e9 + 1e-9),
            st.gen_tokens, st.gen_ns / 1e9, st.gen_tokens / (st.gen_ns / 1e9 + 1e-9),
            st.hit_limit ? " [hit limit]" : "");
    return 0;
}
