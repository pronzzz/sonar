/*
 * The agent harness (see harness.h): plugin registries, events,
 * permissions, the session log, tool-call parsing and execution, and
 * AGENTS.md instructions.
 */
#include "harness.h"
#include "config.h"
#include "console.h"
#include "fs.h"
#include "irq.h"
#include "kernel.h"
#include "mm.h"
#include "shell.h"
#include "string.h"

void epoch_to_date(u64 epoch, int *y, int *mo, int *d, int *h, int *mi, int *s);

#define CYAN "\x1b[36m"
#define YELLOW "\x1b[33m"
#define DIM "\x1b[2m"
#define BOLD "\x1b[1m"
#define RESET "\x1b[0m"

#define TOOL_OUT_MAX 4000

extern const struct harness_tool __harnesstool_start[], __harnesstool_end[];
extern const struct harness_listener __harnesslistener_start[], __harnesslistener_end[];

/* ------------------------------------------------------------ tools */
const struct harness_tool *harness_tool_find(const char *name)
{
    for (const struct harness_tool *t = __harnesstool_start; t < __harnesstool_end; t++)
        if (!strcmp(t->name, name))
            return t;
    return NULL;
}

void harness_tools_describe(char *out, size_t size)
{
    size_t n = 0;
    out[0] = 0;
    for (const struct harness_tool *t = __harnesstool_start; t < __harnesstool_end && n < size;
         t++)
        n += (size_t)snprintf(out + n, size - n, "- %s %s: %s\n", t->name, t->params,
                              t->description);
}

void tool_printf(struct tool_ctx *t, const char *fmt, ...)
{
    if (t->len + 1 >= t->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t->out + t->len, t->cap - t->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        t->len = MIN(t->len + (size_t)n, t->cap - 1);
}

/* ------------------------------------------------------------ events */
void harness_event(const char *event, const char *data_json)
{
    for (const struct harness_listener *l = __harnesslistener_start; l < __harnesslistener_end;
         l++)
        l->fn(event, data_json ? data_json : "{}");
}

/* Built-in listener: with "config ai.trace on" the agent loop's events are
 * shown as they happen. */
static void trace_listener(const char *event, const char *data)
{
    if (config_get_bool("ai.trace", false))
        printk(DIM "  [%s] %.160s" RESET "\n", event, data);
}
HARNESS_LISTENER(trace, trace_listener);

/* ------------------------------------------------------------ permissions */
static const char *perm_names[] = { "read-only", "workspace-write", "danger-full-access" };
static bool session_allow_writes, session_allow_commands;

int harness_permission(void)
{
    const char *m = config_get("ai.permission", "workspace-write");
    for (int i = 0; i < 3; i++)
        if (!strcmp(m, perm_names[i]))
            return i;
    return PERM_WORKSPACE_WRITE;
}

const char *harness_permission_name(int mode) { return perm_names[CLAMP(mode, 0, 2)]; }

const char *harness_workspace(void) { return config_get("ai.workspace", "/home"); }

static bool under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (n == 1 && dir[0] == '/')
        return true;
    return !strncmp(path, dir, n) && (path[n] == 0 || path[n] == '/');
}

/* y: once; a: this kind of action for the rest of the session */
static int approve(const char *what)
{
    if (console_redirected())
        return 0;
    printk(YELLOW "DeepSeek wants to %s" RESET "\n", what);
    char line[16];
    int n = readline("  Allow? [y]es / [N]o / [a]lways this session: ", line, sizeof(line));
    if (n <= 0)
        return 0;
    if (line[0] == 'y' || line[0] == 'Y')
        return 1;
    if (line[0] == 'a' || line[0] == 'A')
        return 2;
    return 0;
}

bool harness_allow_write(const char *path)
{
    int mode = harness_permission();
    if (mode == PERM_FULL_ACCESS)
        return true;
    if (mode == PERM_READ_ONLY) {
        printk(YELLOW "  (blocked: permission mode is read-only)" RESET "\n");
        return false;
    }
    if (under(path, harness_workspace()) || under(path, "/tmp") || session_allow_writes)
        return true;
    char what[FS_PATH_MAX + 64];
    snprintf(what, sizeof(what), "write %s (outside the workspace %s)", path,
             harness_workspace());
    int a = approve(what);
    if (a == 2)
        session_allow_writes = true;
    return a != 0;
}

static const char *hidden_cmds[] = { "gpustat", "chat", "dsh", "explain", "setup", "fstest", "smptest",
                                     "diskread", "clear", "ai", "desktop", "poweroff", "reboot",
                                     NULL };
static const char *safe_cmds[] = { "ls", "cat", "head", "pwd", "df", "disks", "info", "mem",
                                   "cpus", "uptime", "date", "stat", "wc", "grep", "find",
                                   "models", "help", "hexdump", "echo", "ifconfig", "ping",
                                   "host", "netstat", "arp", "dmesg", NULL };

static bool in_list(const char **list, const char *name)
{
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], name))
            return true;
    return false;
}

static void first_word(const char *cmd, char *name, size_t n)
{
    while (*cmd == ' ')
        cmd++;
    size_t k = 0;
    while (cmd[k] && cmd[k] != ' ' && k < n - 1) {
        name[k] = cmd[k];
        k++;
    }
    name[k] = 0;
}

bool harness_command_hidden(const char *name) { return in_list(hidden_cmds, name) || name[0] == '?'; }

bool harness_command_is_safe(const char *cmd)
{
    char name[32];
    first_word(cmd, name, sizeof(name));
    if (strchr(cmd, '>'))
        return false; /* writes a file */
    if (!strcmp(name, "config")) {
        int words = 0;
        for (const char *p = cmd; *p; p++)
            if (*p != ' ' && (p == cmd || p[-1] == ' '))
                words++;
        return words <= 2; /* reading a setting */
    }
    return in_list(safe_cmds, name);
}

static bool in_workspace(const char *path)
{
    char abs[FS_PATH_MAX];
    if (!*path || fs_abspath(path, abs, sizeof(abs)) < 0)
        return false;
    return under(abs, harness_workspace()) || under(abs, "/tmp");
}

/* File commands and the arguments they change: all from the first to the
 * last argument, except where noted. */
static const char *file_cmds[] = { "write", "append", "touch", "mkdir", "rm",
                                   "rmdir", "cp",     "mv",    NULL };

/* workspace-write lets the agent change files in the workspace without
 * asking, as in dsh: a read-only command whose output is redirected into
 * the workspace, or a file command whose paths are all in it. */
static bool workspace_only(const char *cmd)
{
    char buf[512];
    if (strlen(cmd) >= sizeof(buf))
        return false;
    strlcpy(buf, cmd, sizeof(buf));
    char *target = NULL;
    bool append;
    bool redir = shell_split_redirect(buf, &target, &append);
    if (redir && (strchr(target, '>') || !in_workspace(target)))
        return false;
    if (redir && harness_command_is_safe(buf))
        return true;
    char *argv[SHELL_MAX_ARGS];
    int argc = shell_split(buf, argv, SHELL_MAX_ARGS);
    if (argc < 2 || !in_list(file_cmds, argv[0]))
        return false;
    int first = 1, last = argc - 1;
    if (!strcmp(argv[0], "write") || !strcmp(argv[0], "append"))
        last = 1; /* write FILE TEXT... */
    else if (!strcmp(argv[0], "cp"))
        first = last; /* cp FROM TO: only TO changes */
    for (int i = first; i <= last; i++)
        if (argv[i][0] != '-' && !in_workspace(argv[i]))
            return false;
    return true;
}

bool harness_allow_command(const char *cmd)
{
    if (harness_command_is_safe(cmd))
        return true;
    int mode = harness_permission();
    if (mode == PERM_FULL_ACCESS)
        return true;
    if (mode == PERM_READ_ONLY) {
        printk(YELLOW "  (blocked: permission mode is read-only)" RESET "\n");
        return false;
    }
    if (workspace_only(cmd))
        return true;
    if (session_allow_commands)
        return true;
    char what[300];
    snprintf(what, sizeof(what), "run: %s", cmd);
    int a = approve(what);
    if (a == 2)
        session_allow_commands = true;
    return a != 0;
}

/* ------------------------------------------------------------ sessions */
#define SESSION_DIR "/var/dsh/sessions"
static char session_id[32];
static char session_path[96];
static long session_seq;
static bool session_ok;

static void mkdirs(const char *path)
{
    char p[FS_PATH_MAX];
    strlcpy(p, path, sizeof(p));
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            fs_mkdir(p);
            *s = '/';
        }
    fs_mkdir(p);
}

static u64 now_ms(void)
{
    u64 e = rtc_epoch();
    return e ? e * 1000 + time_ms() % 1000 : time_ms();
}

void harness_log(const char *type, const char *data_json)
{
    if (!session_ok)
        return;
    struct jbuf b;
    jb_init(&b);
    jb_raw(&b, "{");
    jb_kint(&b, "seq", session_seq++);
    jb_kint(&b, "time", (long)now_ms());
    jb_kstr(&b, "type", type);
    jb_raw(&b, ",\"data\":");
    jb_raw(&b, data_json && *data_json ? data_json : "{}");
    jb_raw(&b, "}\n");
    if (!b.oom && fs_append(session_path, b.p, b.len) < 0)
        session_ok = false; /* e.g. disk full: keep working without a log */
    jb_free(&b);
}

void harness_log_text(const char *type, const char *key, const char *text)
{
    struct jbuf b;
    jb_init(&b);
    jb_raw(&b, "{");
    jb_kstr(&b, key, text);
    jb_raw(&b, "}");
    if (!b.oom)
        harness_log(type, b.p);
    jb_free(&b);
}

void harness_session_new(const char *model)
{
    session_allow_writes = session_allow_commands = false;
    harness_todos_clear();
    u64 e = rtc_epoch();
    int y = 1970, mo = 1, d = 1, h = 0, mi = 0, s = 0;
    if (e)
        epoch_to_date(e, &y, &mo, &d, &h, &mi, &s);
    snprintf(session_id, sizeof(session_id), "%04d%02d%02d-%02d%02d%02d-%03u", y, mo, d, h, mi, s,
             (unsigned)(time_ms() % 1000));
    char dir[80];
    snprintf(dir, sizeof(dir), SESSION_DIR "/%s", session_id);
    snprintf(session_path, sizeof(session_path), "%s/session.v1.jsonl", dir);
    session_seq = 0;
    session_ok = fs_mounted();
    if (session_ok) {
        mkdirs(dir);
        session_ok = fs_write_all(session_path, "", 0) >= 0;
    }
    struct jbuf b;
    jb_init(&b);
    jb_raw(&b, "{");
    jb_kstr(&b, "model", model);
    jb_kstr(&b, "cwd", fs_cwd());
    jb_kstr(&b, "permission", harness_permission_name(harness_permission()));
    jb_kstr(&b, "workspace", harness_workspace());
    jb_kstr(&b, "harness", "sonar-dsh/0.1");
    jb_raw(&b, "}");
    harness_log("session/start", b.p);
    jb_free(&b);
}

void harness_session_resume(const char *id, long next_seq)
{
    snprintf(session_id, sizeof(session_id), "%s", id);
    snprintf(session_path, sizeof(session_path), SESSION_DIR "/%s/session.v1.jsonl", id);
    session_seq = next_seq;
    session_ok = fs_mounted();
    session_allow_writes = session_allow_commands = false;
    harness_log_text("session/resume", "cwd", fs_cwd());
}

const char *harness_session_id(void) { return session_id[0] ? session_id : NULL; }

int harness_sessions_list(void)
{
    struct fs_dirent *ents;
    int n;
    if (fs_list(SESSION_DIR, &ents, &n) < 0 || !n) {
        printk("No saved sessions.\n");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        char p[FS_PATH_MAX];
        struct fs_stat st;
        snprintf(p, sizeof(p), SESSION_DIR "/%s/session.v1.jsonl", ents[i].name);
        long size = fs_stat(p, &st) == 0 ? (long)st.size : -1;
        printk("  %s%s  %ld bytes\n", ents[i].name,
               !strcmp(ents[i].name, session_id) ? " (current)" : "", size);
    }
    kfree(ents);
    return n;
}

/* ------------------------------------------------------------ tool calls */
void harness_call_free(struct tool_call *tc)
{
    kfree(tc->arguments);
    memset(tc, 0, sizeof(*tc));
}

static char *dup_range(const char *s, size_t n)
{
    char *d = kmalloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

/* <tool_call>{"name": ..., "arguments": {...}}</tool_call>, the format of
 * Qwen3-based models such as DeepSeek-R1-0528-Qwen3-8B; a bare JSON object
 * or a "RUN: command" line (Sonar's older protocol) are accepted too. */
bool harness_parse_call(const char *answer, struct tool_call *tc)
{
    memset(tc, 0, sizeof(*tc));
    const char *open = strstr(answer, "<tool_call>");
    if (open) {
        const char *s = open + 11;
        const char *e = strstr(s, "</tool_call>");
        size_t len = e ? (size_t)(e - s) : strlen(s);
        /* tolerate ```json fences */
        while (len && (*s == ' ' || *s == '\n' || *s == '`'))
            s++, len--;
        if (len >= 4 && !strncmp(s, "json", 4))
            s += 4, len -= 4;
        while (len && (s[len - 1] == ' ' || s[len - 1] == '\n' || s[len - 1] == '`'))
            len--;
        struct jval *v = json_parse(s, len);
        if (!v) {
            strlcpy(tc->name, "?", sizeof(tc->name));
            tc->arguments = dup_range(s, len);
            return true; /* reported back to the model as malformed */
        }
        const char *name = json_str(json_get(v, "name"));
        const struct jval *args = json_get(v, "arguments");
        if (!args)
            args = json_get(v, "parameters");
        strlcpy(tc->name, name ? name : "?", sizeof(tc->name));
        if (args && args->type == J_STR) {
            tc->arguments = strdup(args->str); /* OpenAI style: arguments as a string */
        } else {
            struct jbuf b;
            jb_init(&b);
            if (args)
                jb_value(&b, args);
            else
                jb_raw(&b, "{}");
            tc->arguments = b.p; /* re-serialized: always valid JSON */
        }
        json_release(v);
        return true;
    }
    for (const char *p = answer; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 4 && !strncmp(p, "RUN:", 4)) {
            const char *s = p + 4;
            while (*s == ' ' || *s == '`')
                s++;
            size_t l = (size_t)(p + len - s);
            while (l && (s[l - 1] == ' ' || s[l - 1] == '`' || s[l - 1] == '\r'))
                l--;
            char *cmd = dup_range(s, l);
            struct jbuf b;
            jb_init(&b);
            jb_raw(&b, "{");
            jb_kstr(&b, "command", cmd ? cmd : "");
            jb_raw(&b, "}");
            kfree(cmd);
            strlcpy(tc->name, "bash", sizeof(tc->name));
            tc->arguments = b.p;
            tc->legacy = true;
            return l > 0;
        }
        p = eol ? eol + 1 : NULL;
    }
    return false;
}

static long call_counter;

char *harness_execute(struct tool_call *tc)
{
    char call_id[24];
    snprintf(call_id, sizeof(call_id), "call_%ld", ++call_counter);
    const char *args_text = tc->arguments ? tc->arguments : "{}";

    struct jbuf ev;
    jb_init(&ev);
    jb_raw(&ev, "{");
    jb_kstr(&ev, "callId", call_id);
    jb_kstr(&ev, "name", tc->name);
    jb_kstr(&ev, "arguments", args_text); /* JSON text, as in dsh's tool/call events */
    jb_raw(&ev, "}");
    harness_log("tool/call", ev.p);
    harness_event("tools/pre-execute", ev.p);

    struct tool_ctx t = { kmalloc(TOOL_OUT_MAX + 1), TOOL_OUT_MAX + 1, 0, false };
    if (!t.out) {
        jb_free(&ev);
        return NULL;
    }
    t.out[0] = 0;
    const struct harness_tool *tool = harness_tool_find(tc->name);
    struct jval *args = json_parse(args_text, strlen(args_text));
    if (!strcmp(tc->name, "?")) {
        tool_printf(&t, "Error: the tool call was not valid JSON. Write "
                        "{\"name\": TOOL, \"arguments\": {...}} inside <tool_call>.");
        t.error = true;
    } else if (!tool && shell_find(tc->name) && !harness_command_hidden(tc->name) &&
               (tool = harness_tool_find("bash")) != NULL) {
        /* A shell command called as if it were a tool ({"name": "df"}): run it
         * through bash, with any string arguments as its operands. */
        char cmd[512];
        size_t n = (size_t)snprintf(cmd, sizeof(cmd), "%s", tc->name);
        if (args && args->type == J_OBJ)
            for (int i = 0; i < args->n && n + 2 < sizeof(cmd); i++) {
                const struct jval *v = &args->items[i];
                if (v->type == J_STR && v->str[0] && strcmp(args->keys[i], "description"))
                    n += (size_t)snprintf(cmd + n, sizeof(cmd) - n, " %s", v->str);
            }
        struct jbuf jb;
        jb_init(&jb);
        jb_raw(&jb, "{");
        jb_kstr(&jb, "command", cmd);
        jb_raw(&jb, "}");
        struct jval *bargs = json_parse(jb.p, strlen(jb.p));
        jb_free(&jb);
        harness_event("tools/execute", ev.p);
        if (bargs) {
            tool->run(bargs, &t);
            json_release(bargs);
        }
        tool_printf(&t, "\n(Note: %s is a shell command, not a tool; it was run with bash. "
                        "Next time call bash with {\"command\": \"%s\"}.)", tc->name, cmd);
    } else if (!tool) {
        tool_printf(&t, "Error: there is no tool named \"%s\".", tc->name);
        t.error = true;
        printk(YELLOW "  (DeepSeek asked for an unknown tool: %s)" RESET "\n", tc->name);
    } else if (!args || args->type != J_OBJ) {
        tool_printf(&t, "Error: the arguments for %s must be a JSON object.", tc->name);
        t.error = true;
    } else {
        harness_event("tools/execute", ev.p);
        tool->run(args, &t);
    }
    if (args)
        json_release(args);

    struct jbuf res;
    jb_init(&res);
    jb_raw(&res, "{");
    jb_kstr(&res, "callId", call_id);
    jb_kbool(&res, "ok", !t.error);
    jb_kstr(&res, "output", t.out);
    jb_raw(&res, "}");
    harness_log("tool/result", res.p);
    harness_event("tools/post-execute", res.p);
    jb_free(&res);
    jb_free(&ev);

    size_t cap = t.len + 160;
    char *reply = kmalloc(cap);
    if (reply) {
        if (tc->legacy) /* answer in the form the model used */
            snprintf(reply, cap, "Output:\n```\n%s\n```", t.out);
        else
            snprintf(reply, cap, "<tool_response>\n%s\n</tool_response>", t.out);
    }
    kfree(t.out);
    return reply;
}

/* ------------------------------------------------------------ AGENTS.md */
#define INSTRUCTIONS_MAX 65536

char *harness_instructions(void)
{
    char dir[FS_PATH_MAX];
    strlcpy(dir, fs_cwd(), sizeof(dir));
    /* collect directories from / down to the current one */
    char *dirs[16];
    int nd = 0;
    char cur[FS_PATH_MAX];
    strlcpy(cur, dir, sizeof(cur));
    for (;;) {
        if (nd < 16)
            dirs[nd++] = strdup(cur);
        if (!strcmp(cur, "/"))
            break;
        char *slash = strrchr(cur, '/');
        if (!slash)
            break;
        if (slash == cur)
            cur[1] = 0;
        else
            *slash = 0;
    }
    char *out = NULL;
    size_t len = 0;
    for (int i = nd - 1; i >= 0; i--) {
        static const char *names[] = { "AGENTS.md", "CLAUDE.md" };
        for (int k = 0; k < 2; k++) {
            char p[FS_PATH_MAX];
            snprintf(p, sizeof(p), "%s%s%s", dirs[i], strcmp(dirs[i], "/") ? "/" : "", names[k]);
            char *text;
            size_t n;
            if (fs_read_all(p, &text, &n) < 0)
                continue;
            size_t room = INSTRUCTIONS_MAX > len ? INSTRUCTIONS_MAX - len : 0;
            size_t take = MIN(n, room);
            char *grown = krealloc(out, len + take + strlen(p) + 32);
            if (grown && take) {
                out = grown;
                len += (size_t)snprintf(out + len, strlen(p) + 32, "From %s:\n", p);
                memcpy(out + len, text, take);
                len += take;
                out[len++] = '\n';
                out[len] = 0;
            }
            kfree(text);
            break; /* AGENTS.md wins over CLAUDE.md in the same directory */
        }
        kfree(dirs[i]);
    }
    return out;
}

/* ------------------------------------------------------------ todos */
#define MAX_TODOS 16
static struct {
    char content[96];
    char status[16];
} todos[MAX_TODOS];
static int ntodos;

void harness_todos_clear(void) { ntodos = 0; }

void harness_todos_print(void)
{
    for (int i = 0; i < ntodos; i++) {
        const char *s = todos[i].status;
        const char *mark = !strcmp(s, "completed") ? "[x]" : !strcmp(s, "in_progress") ? "[>]" : "[ ]";
        printk("  %s %s\n", mark, todos[i].content);
    }
}

static void tool_todo_write(const struct jval *args, struct tool_ctx *t)
{
    const struct jval *list = json_get(args, "todos");
    if (!list || list->type != J_ARR) {
        tool_printf(t, "Error: \"todos\" must be an array of {\"content\", \"status\"}.");
        t->error = true;
        return;
    }
    ntodos = 0;
    for (int i = 0; i < list->n && ntodos < MAX_TODOS; i++) {
        const char *c = json_str(json_get(&list->items[i], "content"));
        const char *s = json_str(json_get(&list->items[i], "status"));
        if (!c)
            continue;
        strlcpy(todos[ntodos].content, c, sizeof(todos[0].content));
        strlcpy(todos[ntodos].status, s ? s : "pending", sizeof(todos[0].status));
        ntodos++;
    }
    printk(CYAN "● todo_write" RESET "\n");
    harness_todos_print();
    tool_printf(t, "Todo list updated (%d items).", ntodos);
}
HARNESS_TOOL(todo_write, "todo_write", "{\"todos\": [{\"content\": string, \"status\": "
                                      "\"pending\"|\"in_progress\"|\"completed\"}]}",
             "keep a short plan for multi-step tasks", tool_todo_write);
