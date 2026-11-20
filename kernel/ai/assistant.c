/*
 * DeepSeek in the operating system: first-time setup, model loading, and
 * the assistant that answers questions and operates the shell.
 *
 * The assistant is an agent on Sonar's harness (harness.c, modeled on
 * DeepSeek Harness): it calls tools (shell commands, file reading and
 * editing, search, web fetch) with <tool_call> blocks. Read-only actions run
 * immediately; others follow the permission mode and may ask the user.
 * Every session is logged to /var/dsh/sessions and can be resumed.
 */
#include "ai.h"
#include "block.h"
#include "chat.h"
#include "config.h"
#include "console.h"
#include "fs.h"
#include "harness.h"
#include "irq.h"
#include "library.h"
#include "mm.h"
#include "net.h"
#include "shell.h"
#include "smp.h"

#define DIM "\x1b[2m"
#define BOLD "\x1b[1m"
#define CYAN "\x1b[36m"
#define YELLOW "\x1b[33m"
#define RESET "\x1b[0m"

#define MAX_TURNS 48
#define TOOL_OUT_MAX 3000

static struct model M;
static struct chat C;
static bool ready;
static char model_id[80];
static struct chat_turn turns[MAX_TURNS];
static int nturns;
static struct chat_stats last_stats;

bool ai_ready(void) { return ready; }
const char *ai_model_name(void) { return ready ? M.info.name : NULL; }

/* Generation speed of the last answer in tenths of a token per second. */
u32 ai_last_speed_x10(void)
{
    if (!last_stats.gen_ns)
        return 0;
    return (u32)((u64)last_stats.gen_tokens * 10000000000ULL / last_stats.gen_ns);
}

/* ------------------------------------------------------------ helpers */
static void fmt_gib(u64 bytes, char *out, size_t n)
{
    u64 tenths = (bytes * 10 + (1ULL << 29)) >> 30;
    snprintf(out, n, "%llu.%llu GiB", (unsigned long long)(tenths / 10),
             (unsigned long long)(tenths % 10));
}

static int ask(const char *prompt, char *buf, size_t n)
{
    int r = readline(prompt, buf, n);
    if (r < 0) {
        buf[0] = 0;
        return 0;
    }
    char *p = buf;
    while (*p == ' ')
        p++;
    if (p != buf)
        memmove(buf, p, strlen(p) + 1);
    return (int)strlen(buf);
}

static bool ask_yes(const char *prompt, bool def)
{
    char b[16];
    if (!ask(prompt, b, sizeof(b)))
        return def;
    return b[0] == 'y' || b[0] == 'Y';
}

static void reset_conversation(void)
{
    for (int i = 0; i < nturns; i++)
        kfree((void *)turns[i].text);
    nturns = 0;
}

static bool push_turn(int role, const char *text)
{
    if (nturns >= MAX_TURNS)
        return false;
    char *t = strdup(text);
    if (!t)
        return false;
    turns[nturns].role = role;
    turns[nturns].text = t;
    nturns++;
    return true;
}

static int load_pct = -1;
static void load_progress(void *ctx, u64 done, u64 total)
{
    int pct = total ? (int)(done * 100 / total) : 100;
    if (pct == load_pct)
        return;
    load_pct = pct;
    char bar[26];
    for (int i = 0; i < 25; i++)
        bar[i] = i < pct / 4 ? '#' : '.';
    bar[25] = 0;
    printk("\r  loading [%s] %3d%%  %llu / %llu MiB", bar, pct, (unsigned long long)(done >> 20),
           (unsigned long long)(total >> 20));
    if (done >= total)
        printk("\n");
}

static float cfg_float(const char *key, float def)
{
    const char *v = config_get(key, NULL);
    if (!v || !*v)
        return def;
    return (float)strtod(v, NULL);
}

static void apply_settings(void)
{
    long threads = config_get_int("ai.threads", 0);
    smp_set_workers(threads > 0 ? (int)threads : MAX_CPUS);
    if (ready) {
        C.smp.temperature = cfg_float("ai.temperature", 0.6f);
        C.smp.top_p = cfg_float("ai.top_p", 0.95f);
    }
}

static void unload(void)
{
    if (!ready)
        return;
    reset_conversation();
    chat_free(&C);
    model_free(&M);
    ready = false;
    model_id[0] = 0;
}

/* RAM a model still needs: one already in memory is used in place. */
static u64 ram_needed(const char *id, const struct model_info *in, int ctx)
{
    u64 need = model_ram_needed(in, ctx);
    if (library_in_memory(id) && need > in->weight_bytes)
        need -= in->weight_bytes;
    return need;
}

static bool load_model(const char *id)
{
    unload();
    struct ai_file f;
    if (!library_open(id, &f)) {
        printk("DeepSeek: model %s not found\n", id);
        return false;
    }
    char err[160];
    struct model_info info;
    if (model_probe(&f, &info, err, sizeof(err)) < 0) {
        printk("DeepSeek: %s: %s\n", id, err);
        library_close(&f);
        return false;
    }
    int ctx = (int)config_get_int("ai.ctx", 4096);
    if (ctx < 256)
        ctx = 256;
    if (info.ctx_train > 0 && ctx > info.ctx_train)
        ctx = info.ctx_train;
    u64 need = ram_needed(id, &info, ctx), avail = pmm_free_bytes();
    if (need > avail) {
        char a[24], b[24];
        fmt_gib(need, a, sizeof(a));
        fmt_gib(avail, b, sizeof(b));
        printk("DeepSeek: %s needs %s of RAM but only %s is free.\n"
               "          Choose a smaller model or context (run 'setup').\n",
               info.name, a, b);
        library_close(&f);
        return false;
    }
    printk("DeepSeek: loading %s (%s, %s) from %s\n", info.name, info.size_label[0] ? info.size_label : "?",
           info.quant, id);
    u64 t0 = time_ns();
    load_pct = -1;
    int r = model_load(&M, &f, ctx, load_progress, NULL, err, sizeof(err));
    library_close(&f);
    if (r < 0) {
        printk("\nDeepSeek: could not load %s: %s\n", id, err);
        return false;
    }
    if (chat_init(&C, &M) < 0) {
        model_free(&M);
        printk("DeepSeek: out of memory\n");
        return false;
    }
    ready = true;
    strlcpy(model_id, id, sizeof(model_id));
    apply_settings();
    u64 ms = (time_ns() - t0) / 1000000;
    printk("DeepSeek: ready in %llu.%llu s (%d-token context, %d CPUs). Ask with: ? <question>\n",
           (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000 / 100), M.n_ctx,
           smp_get_workers());
    return true;
}

/* ------------------------------------------------------------ setup */
static void show_catalog(int ctx)
{
    u64 ram = pmm_total_bytes();
    printk("\n  DeepSeek-R1 models (4-bit Q4_K_M GGUF; RAM shown for a %d-token context):\n", ctx);
    for (int i = 0; i < ai_catalog_count; i++) {
        const struct catalog_entry *c = &ai_catalog[i];
        char sz[24];
        if (!c->supported) {
            printk("    %-32s  ~%.0f GB   not supported (mixture-of-experts, needs ~400 GB+)\n",
                   c->name, c->q4_gb);
            continue;
        }
        u64 need = catalog_ram(c, ctx);
        fmt_gib(need, sz, sizeof(sz));
        printk("    %-32s  %5.1f GB file, needs %9s  %s\n", c->name, c->q4_gb, sz,
               need < ram ? "fits" : "too big for this machine");
    }
    printk("\n  To install one: download its GGUF (e.g. from huggingface.co, search the model\n"
           "  name + \"GGUF\") and attach it as a disk, write it to a partition, or copy it\n"
           "  into /models. Then run 'setup' again.\n");
}

static void run_setup(bool first_boot)
{
    char line[64], gb[24], fb[24];
    fmt_gib(pmm_total_bytes(), gb, sizeof(gb));
    fmt_gib(pmm_free_bytes(), fb, sizeof(fb));
    printk("\n" BOLD "Sonar setup — DeepSeek" RESET "\n\n");
    printk("  This computer: %d CPUs%s, %s RAM (%s free)\n", smp_num_cpus(),
           ai_cpu_has_dotprod() ? " with int8 dot product" : "", gb, fb);
    if (fs_is_ram())
        printk("  " YELLOW "Note:" RESET " no system disk, so settings last until power-off.\n");
    if (ready) {
        printk("  Unloading the current model to free memory...\n");
        unload();
    }
    struct model_entry *lib = kmalloc(sizeof(*lib) * 16);
    int n = lib ? library_scan(lib, 16) : 0;
    int ctx = (int)config_get_int("ai.ctx", 4096);
    if (!n) {
        printk("\n  No model was found on the disks or in /models.\n");
        show_catalog(ctx);
        config_set("ai.model", "none");
        config_set("setup.done", "yes");
        config_save();
        kfree(lib);
        if (first_boot)
            ask("\n  Press Enter to continue without the assistant. ", line, sizeof(line));
        return;
    }
    printk("\n  Models found on this computer:\n");
    int rec = -1;
    u64 rec_size = 0;
    for (int i = 0; i < n; i++) {
        const struct model_info *in = &lib[i].info;
        u64 need = ram_needed(lib[i].id, in, ctx);
        bool fits = need < pmm_free_bytes();
        bool ds = strstr(in->name, "DeepSeek") || strstr(in->name, "deepseek");
        char nb[24];
        fmt_gib(need, nb, sizeof(nb));
        printk("    %d) %-34s %5.1fB  %-5s  %-14s needs %s%s\n", i + 1, in->name,
               (double)in->n_params / 1e9, in->quant, lib[i].id, nb,
               fits ? "" : "  (" YELLOW "not enough RAM" RESET ")");
        if (fits && (rec < 0 || (ds && (in->weight_bytes > rec_size ||
                                        !strstr(lib[rec].info.name, "DeepSeek"))))) {
            rec = i;
            rec_size = in->weight_bytes;
        }
    }
    if (ask_yes("\n  Show the DeepSeek-R1 model list with RAM requirements? [y/N] ", false))
        show_catalog(ctx);
    if (rec < 0)
        rec = 0;
    int choice = rec;
    for (;;) {
        char prompt[96];
        snprintf(prompt, sizeof(prompt), "\n  Choose a model [%d], or 0 for none: ", rec + 1);
        if (!ask(prompt, line, sizeof(line)))
            break;
        long v = strtol(line, NULL, 10);
        if (v >= 0 && v <= n) {
            choice = (int)v - 1;
            break;
        }
        printk("  Please enter a number from 0 to %d.\n", n);
    }
    if (choice < 0) {
        config_set("ai.model", "none");
    } else {
        const struct model_info *in = &lib[choice].info;
        int maxctx = in->ctx_train > 0 ? in->ctx_train : 4096;
        char prompt[96];
        snprintf(prompt, sizeof(prompt), "  Context length in tokens, 512-%d [%d]: ", maxctx,
                 ctx > maxctx ? maxctx : ctx);
        if (ask(prompt, line, sizeof(line))) {
            long v = strtol(line, NULL, 10);
            if (v >= 512 && v <= maxctx)
                ctx = (int)v;
        }
        if (ctx > maxctx)
            ctx = maxctx;
        while (ctx > 512 && ram_needed(lib[choice].id, in, ctx) > pmm_free_bytes())
            ctx /= 2;
        bool reason = ask_yes("  Let DeepSeek reason before answering (slower, better answers)? [Y/n] ",
                              true);
        bool show = reason && ask_yes("  Show its reasoning as it thinks? [y/N] ", false);
        char ctxs[16];
        snprintf(ctxs, sizeof(ctxs), "%d", ctx);
        config_set("ai.model", lib[choice].id);
        config_set("ai.ctx", ctxs);
        config_set("ai.reasoning", reason ? "on" : "off");
        config_set("ai.show_reasoning", show ? "yes" : "no");
        if (!config_get("ai.temperature", NULL))
            config_set("ai.temperature", "0.6");
        if (!config_get("ai.max_tokens", NULL))
            config_set("ai.max_tokens", "2048");
    }
    config_set("setup.done", "yes");
    if (!config_save() && !fs_is_ram())
        printk("  " YELLOW "Could not save %s." RESET "\n", CONFIG_PATH);
    kfree(lib);
    printk("\n");
    const char *id = config_get("ai.model", "none");
    if (strcmp(id, "none"))
        load_model(id);
}

void ai_boot(void)
{
    apply_settings();
    if (!config_get_bool("setup.done", false)) {
        run_setup(true);
        return;
    }
    const char *id = config_get("ai.model", "none");
    if (strcmp(id, "none")) {
        if (!load_model(id))
            printk("DeepSeek: run 'setup' to choose another model.\n");
    } else {
        printk("DeepSeek: no model selected (run 'setup').\n");
    }
}

/* ------------------------------------------------------------ the assistant */
/*
 * The agent loop runs on the harness (harness.c, tools.c): a turn is up to
 * ai.max_steps steps; each step is one model request and at most one tool
 * call. Everything is recorded in the session log.
 */
static char *summary;      /* carried over by compaction */
static bool session_open;  /* the harness session logging this conversation */

static char *command_list(void)
{
    char *all = kmalloc(16384), *out = kmalloc(8192);
    if (!all || !out) {
        kfree(all);
        kfree(out);
        return NULL;
    }
    shell_list_commands(all, 16384);
    size_t n = 0;
    out[0] = 0;
    for (char *line = all; *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char name[32];
        size_t k = 0;
        while (line[k] && line[k] != ' ' && k < sizeof(name) - 1) {
            name[k] = line[k];
            k++;
        }
        name[k] = 0;
        if (!harness_command_hidden(name) && n + strlen(line) + 2 < 8192)
            n += (size_t)snprintf(out + n, 8192 - n, "%s\n", line);
        if (!nl)
            break;
        line = nl + 1;
    }
    kfree(all);
    return out;
}

static void system_state(char *out, size_t size)
{
    size_t n = 0;
    u64 t = rtc_epoch(), up = time_ms() / 1000;
    char tot[24], fre[24];
    fmt_gib(pmm_total_bytes(), tot, sizeof(tot));
    fmt_gib(pmm_free_bytes(), fre, sizeof(fre));
    n += (size_t)snprintf(out + n, size - n,
                          "- time: %llu seconds since 1970 (UTC); up %llu min\n"
                          "- %d CPUs; memory: %s free of %s\n- current directory: %s\n",
                          (unsigned long long)t, (unsigned long long)(up / 60), smp_num_cpus(), fre,
                          tot, fs_cwd());
    n += (size_t)snprintf(out + n, size - n, "- disks:");
    for (int i = 0; i < blk_count() && n < size; i++) {
        struct blkdev *d = blk_get(i);
        char sz[24];
        blk_format_size(d->nsectors * BLK_SECTOR, sz, sizeof(sz));
        n += (size_t)snprintf(out + n, size - n, "%s %s %s (%s)", i ? "," : "", d->name, sz,
                              blk_content_hint(d));
    }
    if (fs_mounted()) {
        u64 total, freeb;
        fs_usage(&total, &freeb, NULL, 0);
        fmt_gib(freeb, fre, sizeof(fre));
        n += (size_t)snprintf(out + n, size - n, "\n- root file system: %s, %s free%s\n",
                              fs_device(), fre, fs_is_ram() ? " (RAM disk, not saved)" : "");
    }
    char net[160];
    net_describe(net, sizeof(net));
    n += (size_t)snprintf(out + n, size - n, "- network: %s\n", net);
}

static char *first_turn_text(const char *question)
{
    char *cmds = command_list();
    char *state = kmalloc(2048);
    char *tools = kmalloc(4096);
    char *agents = harness_instructions();
    size_t cap = 16384 + strlen(question) + (agents ? strlen(agents) : 0) +
                 (summary ? strlen(summary) : 0);
    char *t = kmalloc(cap);
    if (!cmds || !state || !tools || !t) {
        kfree(cmds);
        kfree(state);
        kfree(tools);
        kfree(agents);
        kfree(t);
        return NULL;
    }
    system_state(state, 2048);
    harness_tools_describe(tools, 4096);
    int perm = harness_permission();
    /* Static instructions first so the KV cache can reuse them across
     * conversations; the live state and question follow. */
    snprintf(t, cap,
             "You are DeepSeek, the AI assistant built into Sonar, a small operating system for "
             "ARM computers. You run entirely on this machine.\n\n"
             "You can call tools to look at or change this computer. To call one, write "
             "exactly:\n<tool_call>\n{\"name\": \"TOOL\", \"arguments\": {...}}\n</tool_call>\n"
             "and stop. The system runs it and replies with <tool_response>...</tool_response>; "
             "then continue. Call one tool at a time. Use tools only when you need facts about "
             "this computer or the user asks you to do something on it. Never invent tool "
             "output. If no tool is needed, just answer. Keep the final answer short and "
             "clear.\n\nTools:\n%s\nCommands for bash (NAME USAGE - DESCRIPTION):\n%s\n"
             "Permission mode: %s (workspace %s)%s.\n\n"
             "%s%s%s%s%s%s"
             "Current state of this computer:\n%s\nQuestion: %s",
             tools, cmds, harness_permission_name(perm), harness_workspace(),
             perm == PERM_READ_ONLY ? "; you cannot change anything"
             : perm == PERM_WORKSPACE_WRITE
                 ? ": you may change files in the workspace and /tmp. For any other change, "
                   "still call the tool when the user asks for it; the system then asks the "
                   "user to approve or deny it, so do not refuse in advance"
                 : "",
             agents ? "Instructions from AGENTS.md:\n" : "", agents ? agents : "",
             agents ? "\n" : "", summary ? "Summary of this session so far:\n" : "",
             summary ? summary : "", summary ? "\n\n" : "", state, question);
    kfree(cmds);
    kfree(state);
    kfree(tools);
    kfree(agents);
    return t;
}

/* The start of an answer is held back (HOLD_MAX bytes) until it is clear
 * whether a tool call follows: prose written before a call is usually a
 * guess at what the tool will show, so it is dropped rather than printed. */
#define HOLD_MAX 320

struct emit_state {
    bool in_think, show_think, answer_started, stop_for_tool;
    char hold[HOLD_MAX];
    int hold_len;
    bool released;    /* the held text has been printed; output streams now */
    int think_tokens;
    char line[256];
    int line_len;
    /* hiding <tool_call>...</tool_call> from the screen */
    char tag[16];
    int tag_len;
    bool in_call;
    char tail[16];
    int tail_len;
};

static const char CALL_OPEN[] = "<tool_call>", CALL_CLOSE[] = "</tool_call>";

static void release(struct emit_state *e)
{
    if (e->hold_len)
        console_write(e->hold, (size_t)e->hold_len);
    e->hold_len = 0;
    e->released = true;
}

static void show_char(struct emit_state *e, char c)
{
    if (e->released) {
        console_write(&c, 1);
    } else {
        e->hold[e->hold_len++] = c;
        if (e->hold_len == HOLD_MAX)
            release(e);
    }
    if (c == '\n') {
        e->line[e->line_len] = 0;
        e->line_len = 0;
    } else if (e->line_len < (int)sizeof(e->line) - 1) {
        e->line[e->line_len++] = c;
    }
}

static bool emit(void *ctx, const char *s, int len)
{
    struct emit_state *e = ctx;
    if (len == 7 && !memcmp(s, "<think>", 7)) {
        e->in_think = true;
        if (e->show_think)
            printk(DIM);
        return true;
    }
    if (len == 8 && !memcmp(s, "</think>", 8)) {
        e->in_think = false;
        printk(e->show_think ? RESET "\n" : "\r\x1b[K");
        return true;
    }
    if (e->in_think) {
        e->think_tokens++;
        if (e->show_think)
            console_write(s, (size_t)len);
        else if (e->think_tokens % 4 == 1)
            printk("\r" DIM "  thinking... %d" RESET, e->think_tokens);
        return true;
    }
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (!e->answer_started) {
            if (c == '\n' || c == ' ')
                continue;
            e->answer_started = true;
        }
        if (e->in_call) { /* collect silently until </tool_call> */
            if (e->tail_len == (int)sizeof(CALL_CLOSE) - 1) {
                memmove(e->tail, e->tail + 1, sizeof(CALL_CLOSE) - 2);
                e->tail_len--;
            }
            e->tail[e->tail_len++] = c;
            if (e->tail_len == (int)sizeof(CALL_CLOSE) - 1 &&
                !memcmp(e->tail, CALL_CLOSE, sizeof(CALL_CLOSE) - 1)) {
                e->stop_for_tool = true;
                return false; /* stop: the harness runs the tool now */
            }
            continue;
        }
        if (c == CALL_OPEN[e->tag_len]) {
            e->tag[e->tag_len++] = c;
            if (e->tag_len == (int)sizeof(CALL_OPEN) - 1) {
                e->in_call = true;
                e->tag_len = 0;
                e->hold_len = 0; /* prose before the call: not shown */
                e->line_len = 0;
            }
            continue;
        }
        if (e->tag_len) { /* not a tool call after all */
            for (int k = 0; k < e->tag_len; k++)
                show_char(e, e->tag[k]);
            e->tag_len = 0;
            if (c == '<') {
                e->tag[e->tag_len++] = c;
                continue;
            }
        }
        show_char(e, c);
        if (c == '\n' && !strncmp(e->line, "RUN:", 4)) {
            e->stop_for_tool = true;
            return false; /* older "RUN: command" protocol */
        }
    }
    return true;
}

static bool silent_emit(void *ctx, const char *s, int len) { return !console_interrupted(); }

static void ensure_session(void)
{
    if (!session_open) {
        harness_session_new(M.info.name);
        session_open = true;
    }
}

static void new_conversation(void)
{
    reset_conversation();
    kfree(summary);
    summary = NULL;
    session_open = false;
}

static void log_user(const char *text, bool first)
{
    struct jbuf b;
    jb_init(&b);
    jb_raw(&b, "{");
    jb_kstr(&b, "text", text);
    jb_kbool(&b, "first", first);
    jb_raw(&b, "}");
    if (!b.oom)
        harness_log("message/user", b.p);
    jb_free(&b);
}

/* Compaction (as in dsh): summarize the conversation, then continue from
 * the summary. Logged as compaction/start, compaction/summary,
 * compaction/end. */
static bool compact(void)
{
    if (!nturns)
        return false;
    harness_log("compaction/start", NULL);
    harness_event("compaction/start", NULL);
    printk(DIM "(compacting the conversation...)" RESET);
    static const char ask_text[] =
        "Summarize this conversation for your own later use: the user's goals, facts learned "
        "from tool results, files changed, and what is still to do. At most 120 words. Reply "
        "with the summary only.";
    struct chat_turn tmp[MAX_TURNS + 1];
    int start;
    for (start = 0; start < nturns; start++) {
        if (turns[start].role != ROLE_USER)
            continue;
        int n = 0;
        for (int i = start; i < nturns; i++)
            tmp[n++] = turns[i];
        tmp[n++] = (struct chat_turn){ ROLE_USER, ask_text };
        if (chat_build(&C, NULL, tmp, n, false) == 0 && C.n_tokens + 300 < M.n_ctx)
            break;
    }
    static char out[4096];
    const char *text = "(The earlier conversation was too long to summarize.)";
    struct chat_stats st;
    if (start < nturns && chat_generate(&C, 280, silent_emit, NULL, out, sizeof(out), &st) >= 0) {
        const char *a = out;
        for (const char *p = out; (p = strstr(p, "</think>")) != NULL; p += 8)
            a = p + 8;
        while (*a == '\n' || *a == ' ')
            a++;
        if (*a)
            text = a;
    }
    char *s = strdup(text);
    kfree(summary);
    summary = s;
    harness_log_text("compaction/summary", "text", summary ? summary : "");
    reset_conversation();
    harness_log("compaction/end", NULL);
    harness_event("compaction/end", NULL);
    printk("\r\x1b[K" DIM "(conversation compacted)" RESET "\n");
    return true;
}

/* Does the question ask about this machine (a path, a file name, or its
 * state)? Then an answer given without running any tool is a guess. */
static bool about_system(const char *q)
{
    static const char *phrases[] = { "this computer", "this machine", "this system", "free space",
                                     "disk space", "how much memory", "how much ram",
                                     "ip address", "my files", "my disk", "uptime" };
    for (const char *p = q; *p; p++) {
        bool start = p == q || p[-1] == ' ' || p[-1] == '"' || p[-1] == '\'' || p[-1] == '(';
        if (start && (*p == '/' || *p == '~') && (isalnum((u8)p[1]) || p[1] == '.'))
            return true; /* a path */
        if (*p == '.' && p > q && isalnum((u8)p[-1])) { /* name.ext */
            int n = 0;
            while (isalpha((u8)p[1 + n]))
                n++;
            if (n >= 1 && n <= 4 && !isalnum((u8)p[1 + n]))
                return true;
        }
    }
    char low[512];
    size_t n = 0;
    for (; q[n] && n < sizeof(low) - 1; n++)
        low[n] = (char)tolower((u8)q[n]);
    low[n] = 0;
    for (unsigned i = 0; i < sizeof(phrases) / sizeof(phrases[0]); i++)
        if (strstr(low, phrases[i]))
            return true;
    return false;
}

/* The tool call that answers a question about the computer, for the nudge. */
static void system_hint(const char *q, char *out, size_t size)
{
    static const struct {
        const char *phrase, *call;
    } hints[] = {
        { "free space", "bash with {\"command\": \"df\"}" },
        { "disk space", "bash with {\"command\": \"df\"}" },
        { "my disk", "bash with {\"command\": \"disks\"}" },
        { "memory", "bash with {\"command\": \"mem\"}" },
        { " ram", "bash with {\"command\": \"mem\"}" },
        { "ip address", "bash with {\"command\": \"ifconfig\"}" },
        { "uptime", "bash with {\"command\": \"uptime\"}" },
    };
    char low[512];
    size_t n = 0;
    for (; q[n] && n < sizeof(low) - 1; n++)
        low[n] = (char)tolower((u8)q[n]);
    low[n] = 0;
    for (unsigned i = 0; i < sizeof(hints) / sizeof(hints[0]); i++)
        if (strstr(low, hints[i].phrase)) {
            snprintf(out, size, "call %s", hints[i].call);
            return;
        }
    for (const char *p = q; *p; p++) /* a path: read it */
        if ((p == q || p[-1] == ' ') && *p == '/' && isalnum((u8)p[1])) {
            size_t k = 0;
            while (p[k] && p[k] != ' ' && p[k] != '?' && p[k] != '"' && k < 200)
                k++;
            snprintf(out, size, "call read with {\"file_path\": \"%.*s\"}", (int)k, p);
            return;
        }
    snprintf(out, size, "call the tool that shows it");
}

/* The final answer of an assistant turn (after its reasoning). */
static const char *final_part(const char *reply)
{
    const char *a = reply;
    for (const char *p = reply; (p = strstr(p, "</think>")) != NULL; p += 8)
        a = p + 8;
    while (*a == ' ' || *a == '\n')
        a++;
    return a;
}

static bool same_text(const char *a, const char *b)
{
    for (;;) {
        while (*a == ' ' || *a == '\n' || *a == '\r')
            a++;
        while (*b == ' ' || *b == '\n' || *b == '\r')
            b++;
        if (!*a || !*b)
            return !*a && !*b;
        if (*a++ != *b++)
            return false;
    }
}

/* With reasoning off, a small model sometimes repeats an earlier answer
 * word for word instead of answering the new question. */
static bool repeats_earlier(const char *answer, int before, const char *question)
{
    if (strlen(answer) < 24)
        return false;
    for (int i = 0; i < before; i++)
        if (turns[i].role == ROLE_USER && strstr(turns[i].text, question))
            return false; /* the same question asked again: the same answer is fine */
    for (int i = 0; i < before; i++)
        if (turns[i].role == ROLE_ASSISTANT && !strstr(turns[i].text, CALL_OPEN) &&
            same_text(final_part(turns[i].text), answer))
            return true;
    return false;
}

static void converse(const char *question)
{
    if (!ready) {
        printk("The assistant is not running. Run 'setup' to choose a DeepSeek model.\n");
        return;
    }
    ensure_session();
    int max_steps = (int)CLAMP(config_get_int("ai.max_steps", 10), 1, 20);
    if (nturns + 2 * max_steps + 1 > MAX_TURNS)
        compact();
    bool first = nturns == 0;
    char *text = first ? first_turn_text(question) : strdup(question);
    if (!text || !push_turn(ROLE_USER, text)) {
        kfree(text);
        printk("DeepSeek: out of memory\n");
        return;
    }
    kfree(text);
    log_user(question, first);
    int base = nturns - 1; /* turns before this question, for rolling back */
    bool reasoning = strcmp(config_get("ai.reasoning", "on"), "off") != 0;
    bool show = config_get_bool("ai.show_reasoning", false);
    int max_new = (int)config_get_int("ai.max_tokens", 2048);
    static char reply[16384];
    u64 gen_tokens = 0, gen_ns = 0;
    bool compacted = false;
    bool used_tool = false, nudged_ground = false, nudged_repeat = false;
    bool check_ground = config_get_bool("ai.verify", true) && about_system(question);
    char ev[64];
    for (int step = 0; step < max_steps; step++) {
        snprintf(ev, sizeof(ev), "{\"step\":%d}", step);
        harness_event("agent/pre-step", ev);
        if (chat_build(&C, NULL, turns, nturns, reasoning) < 0 || C.n_tokens + 64 >= M.n_ctx) {
            if (nturns > 1 && !compacted) {
                /* too long: compact, then continue with this request */
                compacted = true;
                compact();
                char *t = first_turn_text(question);
                if (!t || !push_turn(ROLE_USER, t)) {
                    kfree(t);
                    return;
                }
                kfree(t);
                base = 0;
                step--;
                continue;
            }
            printk("DeepSeek: the question is too long for the %d-token context.\n", M.n_ctx);
            reset_conversation();
            return;
        }
        snprintf(ev, sizeof(ev), "{\"step\":%d,\"prompt_tokens\":%d}", step, C.n_tokens);
        harness_event("agent/request", ev);
        struct emit_state es;
        memset(&es, 0, sizeof(es));
        es.show_think = show;
        es.in_think = C.think_open;
        if (es.in_think)
            printk(show ? DIM : DIM "  thinking..." RESET);
        int budget = max_new;
        if (C.n_tokens + budget > M.n_ctx)
            budget = M.n_ctx - C.n_tokens;
        /* Reasoning budget: fuller for the first step, short after a tool
         * result, so a multi-step turn does not reason for minutes. */
        C.think_budget = (int)(step == 0 ? config_get_int("ai.think_budget", 1024)
                                         : config_get_int("ai.think_budget_step", 384));
        int r = chat_generate(&C, budget, emit, &es, reply, sizeof(reply), &last_stats);
        C.think_budget = 0;
        gen_tokens += (u64)last_stats.gen_tokens;
        gen_ns += last_stats.gen_ns;
        if (es.in_think && !show)
            printk("\r\x1b[K");
        if (es.tag_len && !es.in_call) /* a '<' that started no tool call */
            for (int k = 0; k < es.tag_len; k++)
                show_char(&es, es.tag[k]);
        if (last_stats.aborted && !es.stop_for_tool) {
            printk(DIM "(stopped)" RESET "\n");
            while (nturns > base) /* forget the unanswered question */
                kfree((void *)turns[--nturns].text);
            harness_event("agent/turn-stopping", "{\"reason\":\"interrupted\"}");
            return;
        }
        if (r < 0 || last_stats.ctx_full) {
            printk(DIM "(context full)" RESET "\n");
            compact();
            harness_event("agent/turn-stopping", "{\"reason\":\"context_full\"}");
            return;
        }
        /* history keeps the reply; chat_build drops the reasoning part */
        const char *answer = final_part(reply);
        bool has_call = strstr(answer, CALL_OPEN) != NULL || !strncmp(es.line, "RUN:", 4);
        const char *nudge = NULL;
        char nudge_buf[384];
        if (!has_call && !es.in_think && !last_stats.aborted && r >= 0) {
            if (check_ground && !used_tool && !nudged_ground) {
                nudged_ground = true;
                char hint[256];
                system_hint(question, hint, sizeof(hint));
                snprintf(nudge_buf, sizeof(nudge_buf),
                         "(Harness: this question is about this computer. Before answering, %s, "
                         "then answer from its output. Make the call now, without apologizing.)",
                         hint);
                nudge = nudge_buf;
            } else if (!nudged_repeat && repeats_earlier(answer, base, question)) {
                nudged_repeat = true;
                nudge = "(Harness: that answer repeats one you gave to an earlier question. "
                        "Answer the latest question.)";
            }
        }
        if (nudge && !es.released) {
            es.hold_len = 0; /* never shown: try again */
        } else {
            if (nudge)
                printk(RESET "\n" DIM "(checking that...)" RESET);
            release(&es);
        }
        printk(RESET);
        if (es.line_len && (es.released || es.hold_len))
            printk("\n");
        if (es.in_think) {
            printk("(DeepSeek ran out of tokens while reasoning; raise the limit with: "
                   "config ai.max_tokens 4096)\n");
            reset_conversation();
            return;
        }
        char *call_at = strstr(answer, CALL_OPEN);
        if (call_at && call_at > answer) /* keep the call, not the guess before it */
            memmove((char *)answer, call_at, strlen(call_at) + 1);
        if (!push_turn(ROLE_ASSISTANT, reply)) {
            compact();
            return;
        }
        harness_log_text("message/assistant", "text", answer);
        if (nudge) {
            snprintf(ev, sizeof(ev), "{\"step\":%d,\"nudge\":\"%s\"}", step,
                     nudged_repeat ? "repeat" : "ground");
            harness_event("agent/nudge", ev);
            if (!push_turn(ROLE_USER, nudge)) {
                compact();
                return;
            }
            continue;
        }
        struct tool_call tc;
        if (!harness_parse_call(answer, &tc)) {
            harness_event("agent/turn-stopping", "{\"reason\":\"answered\"}");
            break;
        }
        char *out = harness_execute(&tc);
        harness_call_free(&tc);
        used_tool = true;
        if (!out || !push_turn(ROLE_USER, out)) {
            kfree(out);
            compact();
            return;
        }
        kfree(out);
        if (step == max_steps - 1) {
            printk(DIM "(stopping after %d steps; ask again to continue)" RESET "\n", max_steps);
            harness_event("agent/turn-stopping", "{\"reason\":\"max_steps\"}");
        }
    }
    if (gen_ns)
        printk(DIM "[%llu tokens, %llu.%llu tok/s]" RESET "\n", (unsigned long long)gen_tokens,
               (unsigned long long)(gen_tokens * 1000000000ULL / gen_ns),
               (unsigned long long)(gen_tokens * 10000000000ULL / gen_ns % 10));
}

/* Rebuild a conversation from its session log and keep logging to it. */
static void resume_session(const char *id)
{
    if (!ready) {
        printk("The assistant is not running. Run 'setup' first.\n");
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/var/dsh/sessions/%s/session.v1.jsonl", id);
    char *text;
    size_t len;
    if (fs_read_all(path, &text, &len) < 0) {
        printk("No session %s (see: ? /sessions)\n", id);
        return;
    }
    new_conversation();
    long last_seq = -1;
    int events = 0;
    for (char *p = text; *p;) {
        char *eol = strchr(p, '\n');
        size_t l = eol ? (size_t)(eol - p) : strlen(p);
        struct jval *v = json_parse(p, l);
        if (v) {
            const char *type = json_str(json_get(v, "type"));
            const struct jval *d = json_get(v, "data");
            last_seq = json_int(json_get(v, "seq"), last_seq);
            const char *t = json_str(json_get(d, "text"));
            const char *add = NULL;
            int role = ROLE_USER;
            char *owned = NULL;
            if (type && !strcmp(type, "message/user") && t) {
                const struct jval *f = json_get(d, "first");
                if (nturns == 0 || (f && f->type == J_BOOL && f->b))
                    reset_conversation(), add = owned = first_turn_text(t);
                else
                    add = t;
            } else if (type && !strcmp(type, "message/assistant") && t) {
                add = t, role = ROLE_ASSISTANT;
            } else if (type && !strcmp(type, "tool/result")) {
                const char *o = json_str(json_get(d, "output"));
                size_t n = (o ? strlen(o) : 0) + 48;
                owned = kmalloc(n);
                if (owned)
                    snprintf(owned, n, "<tool_response>\n%s\n</tool_response>", o ? o : "");
                add = owned;
            } else if (type && !strcmp(type, "compaction/summary") && t) {
                kfree(summary);
                summary = strdup(t);
                reset_conversation();
            }
            if (add && !push_turn(role, add)) {
                compact();
                push_turn(role, add);
            }
            kfree(owned);
            json_release(v);
            events++;
        }
        if (!eol)
            break;
        p = eol + 1;
    }
    kfree(text);
    harness_session_resume(id, last_seq + 1);
    session_open = true;
    printk("Resumed session %s (%d events, %d messages in context).\n", id, events, nturns);
}

static void show_tools(void)
{
    char *buf = kmalloc(4096);
    if (!buf)
        return;
    harness_tools_describe(buf, 4096);
    printk("Tools DeepSeek can call:\n%s", buf);
    kfree(buf);
    printk("Permission mode: %s, workspace %s (change with: ? /permission MODE)\n",
           harness_permission_name(harness_permission()), harness_workspace());
}

static void harness_help(void)
{
    printk("  /new               start a new session\n"
           "  /compact           summarize the conversation to free context\n"
           "  /sessions          list saved sessions (in /var/dsh/sessions)\n"
           "  /resume ID         continue a saved session\n"
           "  /tools             list the tools DeepSeek can use\n"
           "  /todos             show DeepSeek's current plan\n"
           "  /permission [MODE] read-only, workspace-write or danger-full-access\n");
}

void ai_ask_command(const char *text)
{
    while (*text == ' ')
        text++;
    if (!*text) {
        printk("usage: ? QUESTION   or one of:\n");
        harness_help();
        return;
    }
    if (text[0] != '/') {
        converse(text);
        return;
    }
    if (!strcmp(text, "/new")) {
        new_conversation();
        printk("Started a new session.\n");
    } else if (!strcmp(text, "/compact")) {
        if (!ready || !nturns)
            printk("Nothing to compact.\n");
        else
            compact();
    } else if (!strcmp(text, "/sessions")) {
        harness_sessions_list();
    } else if (!strncmp(text, "/resume ", 8)) {
        resume_session(text + 8);
    } else if (!strcmp(text, "/tools")) {
        show_tools();
    } else if (!strcmp(text, "/todos")) {
        harness_todos_print();
    } else if (!strncmp(text, "/permission", 11)) {
        const char *m = text + 11;
        while (*m == ' ')
            m++;
        if (*m) {
            if (strcmp(m, "read-only") && strcmp(m, "workspace-write") &&
                strcmp(m, "danger-full-access")) {
                printk("Modes: read-only, workspace-write, danger-full-access\n");
                return;
            }
            config_set("ai.permission", m);
            config_save();
            new_conversation(); /* the prompt states the mode */
        }
        printk("Permission mode: %s (workspace %s)\n",
               harness_permission_name(harness_permission()), harness_workspace());
    } else {
        harness_help();
    }
}

/* ------------------------------------------------------------ commands */
static bool interactive_ok(const char *name)
{
    if (!console_redirected())
        return true;
    printk("%s: output cannot be redirected\n", name);
    return false;
}

static int cmd_dsh(int argc, char **argv)
{
    if (!interactive_ok(argv[0]))
        return 1;
    if (!ready) {
        printk("The assistant is not running. Run 'setup' to choose a DeepSeek model.\n");
        return 1;
    }
    if (argc > 1) { /* one-shot: dsh QUESTION... */
        char q[1024];
        size_t n = 0;
        for (int i = 1; i < argc && n < sizeof(q); i++)
            n += (size_t)snprintf(q + n, sizeof(q) - n, i > 1 ? " %s" : "%s", argv[i]);
        ai_ask_command(q);
        return 0;
    }
    ensure_session();
    printk(BOLD "DeepSeek harness" RESET " - %s\n"
           "  session %s, permission %s, workspace %s\n"
           "  Type a request, /help for commands, exit to leave; Ctrl-C stops a reply.\n",
           M.info.name, harness_session_id() ? harness_session_id() : "(not saved)",
           harness_permission_name(harness_permission()), harness_workspace());
    char line[1024];
    for (;;) {
        int n = readline(BOLD "dsh> " RESET, line, sizeof(line));
        if (n < 0 || !strcmp(line, "exit") || !strcmp(line, "quit") || !strcmp(line, "/exit"))
            break;
        if (!n)
            continue;
        ai_ask_command(line);
    }
    return 0;
}
SHELL_COMMAND(dsh, "dsh", "dsh [QUESTION]",
              "DeepSeek agent harness: sessions, tools, approvals (interactive)", cmd_dsh);
SHELL_COMMAND(chat, "chat", "chat", "talk with DeepSeek (same as dsh)", cmd_dsh);

static int cmd_explain(int argc, char **argv)
{
    if (argc < 2) {
        printk("usage: explain COMMAND...   (runs it, then DeepSeek explains the output)\n");
        return 1;
    }
    if (!interactive_ok("explain"))
        return 1;
    char cmd[512];
    size_t n = 0;
    for (int i = 1; i < argc; i++)
        n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, i > 1 ? " %s" : "%s", argv[i]);
    char *cap = kmalloc(TOOL_OUT_MAX);
    shell_capture_begin(cap, TOOL_OUT_MAX);
    shell_exec(cmd);
    shell_capture_end();
    if (!ready) {
        kfree(cap);
        printk("(the assistant is not running; run 'setup')\n");
        return 1;
    }
    char *q = kmalloc(TOOL_OUT_MAX + 600);
    snprintf(q, TOOL_OUT_MAX + 600,
             "I ran `%s` and got this output:\n```\n%s\n```\nExplain briefly what it shows.", cmd,
             cap);
    reset_conversation();
    printk("\n");
    converse(q);
    kfree(q);
    kfree(cap);
    return 0;
}
SHELL_COMMAND(explain, "explain", "explain COMMAND...", "run a command and have DeepSeek explain it",
              cmd_explain);

static int cmd_setup(int argc, char **argv)
{
    if (!interactive_ok("setup"))
        return 1;
    run_setup(false);
    return 0;
}
SHELL_COMMAND(setup, "setup", "setup", "choose the DeepSeek model and its settings", cmd_setup);

static int cmd_models(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "use")) {
        if (!load_model(argv[2]))
            return 1;
        config_set("ai.model", argv[2]);
        config_save();
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "catalog")) {
        show_catalog((int)config_get_int("ai.ctx", 4096));
        return 0;
    }
    struct model_entry *lib = kmalloc(sizeof(*lib) * 16);
    int n = lib ? library_scan(lib, 16) : 0;
    if (!n)
        printk("No models found. Attach a GGUF model disk or copy one into /models.\n");
    for (int i = 0; i < n; i++) {
        const struct model_info *in = &lib[i].info;
        char nb[24];
        fmt_gib(in->weight_bytes, nb, sizeof(nb));
        printk("%c %-14s %-34s %-6s %5.1fB  %-5s %s\n",
               ready && !strcmp(model_id, lib[i].id) ? '*' : ' ', lib[i].id, in->name,
               in->arch_name, (double)in->n_params / 1e9, in->quant, nb);
    }
    kfree(lib);
    if (n)
        printk("(* = loaded)  'models use ID' to switch, 'models catalog' for the DeepSeek list\n");
    return 0;
}
SHELL_COMMAND(models, "models", "models [use ID | catalog]", "list or switch AI models", cmd_models);

static int cmd_ai(int argc, char **argv)
{
    if (!ready) {
        printk("assistant: not running (run 'setup')\n");
        return 1;
    }
    const struct model_info *in = &M.info;
    char wb[24];
    fmt_gib(in->weight_bytes, wb, sizeof(wb));
    printk("model:      %s (%s, %s, %s weights) from %s\n", in->name, in->arch_name, in->quant, wb,
           model_id);
    printk("context:    %d tokens (%d in use)\n", M.n_ctx, M.n_past);
    printk("reasoning:  %s, shown: %s, temperature %s, max %s tokens\n",
           config_get("ai.reasoning", "on"), config_get("ai.show_reasoning", "no"),
           config_get("ai.temperature", "0.6"), config_get("ai.max_tokens", "2048"));
    printk("compute:    %d CPUs, %s\n", smp_get_workers(),
           ai_cpu_has_dotprod() ? "int8 dot product (SDOT)" : "int8 multiply-accumulate");
    if (last_stats.gen_ns)
        printk("last reply: %d tokens at %llu tok/s (prompt: %d tokens, %d computed)\n",
               last_stats.gen_tokens,
               (unsigned long long)(last_stats.gen_tokens * 1000000000ULL / last_stats.gen_ns),
               last_stats.prompt_tokens, last_stats.prefill_tokens);
    return 0;
}
SHELL_COMMAND(ai, "ai", "ai", "show the assistant's model and settings", cmd_ai);
