/*
 * The harness's built-in tools. Names and arguments follow DeepSeek
 * Harness where its documentation shows them (bash: command, description;
 * read: file_path; grep: pattern, path; web_fetch); write and edit are
 * Sonar's. Each tool is a plugin registered with HARNESS_TOOL.
 */
#include "console.h"
#include "fs.h"
#include "harness.h"
#include "kernel.h"
#include "mm.h"
#include "shell.h"
#include "string.h"

#define CYAN "\x1b[36m"
#define DIM "\x1b[2m"
#define RESET "\x1b[0m"

static bool need_str(const struct jval *args, const char *key, const char **out,
                     struct tool_ctx *t)
{
    *out = json_str(json_get(args, key));
    if (*out && **out)
        return true;
    tool_printf(t, "Error: missing string argument \"%s\".", key);
    t->error = true;
    return false;
}

static bool resolve(const char *path, char *abs, struct tool_ctx *t)
{
    if (fs_abspath(path, abs, FS_PATH_MAX) < 0) {
        tool_printf(t, "Error: bad path: %s", path);
        t->error = true;
        return false;
    }
    return true;
}

static size_t strip_ansi(char *s, size_t n)
{
    size_t w = 0;
    for (size_t r = 0; r < n; r++) {
        if (s[r] == 0x1b && r + 1 < n && s[r + 1] == '[') {
            r += 2;
            while (r < n && !(s[r] >= '@' && s[r] <= '~'))
                r++;
            continue;
        }
        s[w++] = s[r];
    }
    s[w] = 0;
    return w;
}

/* Run a shell command, capturing what it prints (shown on screen too). */
static int capture(const char *cmd, char *buf, size_t cap, size_t *len)
{
    shell_capture_begin(buf, cap);
    int rc = shell_exec(cmd);
    size_t n = shell_capture_end();
    *len = strip_ansi(buf, MIN(n, cap - 1));
    return rc;
}

/* ---- bash: a command in Sonar's shell ---- */
static void tool_bash(const struct jval *args, struct tool_ctx *t)
{
    const char *cmd;
    if (!need_str(args, "command", &cmd, t))
        return;
    char name[32];
    size_t k = 0;
    while (cmd[k] == ' ')
        cmd++;
    while (cmd[k] && cmd[k] != ' ' && k < sizeof(name) - 1) {
        name[k] = cmd[k];
        k++;
    }
    name[k] = 0;
    if (!shell_find(name) || harness_command_hidden(name)) {
        tool_printf(t, "Error: `%s` is not an available command. Sonar's shell is not a POSIX "
                       "shell; use the listed commands.", name);
        t->error = true;
        return;
    }
    if (!harness_allow_command(cmd)) {
        tool_printf(t, "The user did not allow `%s`.", cmd);
        t->error = true;
        return;
    }
    const char *why = json_str(json_get(args, "description"));
    printk(CYAN "● bash" RESET " %s%s%s%s\n", cmd, why ? DIM "  (" : "", why ? why : "",
           why ? ")" RESET : "");
    size_t room = t->cap - t->len;
    char *buf = kmalloc(room);
    if (!buf)
        return;
    size_t n;
    int rc = capture(cmd, buf, room, &n);
    tool_printf(t, "exit status %d\n%s%s", rc, buf, n + 1 >= room ? "\n(output truncated)" : "");
    kfree(buf);
    t->error = rc != 0;
}
HARNESS_TOOL(a_bash, "bash", "{\"command\": string, \"description\": string}",
             "run one command in Sonar's shell (listed below; not a POSIX shell: no pipes, "
             "but '> FILE' and '>> FILE' redirect output)", tool_bash);

/* ---- read: lines of a text file ---- */
static void tool_read(const struct jval *args, struct tool_ctx *t)
{
    const char *path;
    char abs[FS_PATH_MAX];
    if (!need_str(args, "file_path", &path, t) || !resolve(path, abs, t))
        return;
    long offset = MAX(json_int(json_get(args, "offset"), 1), 1);
    long limit = CLAMP(json_int(json_get(args, "limit"), 200), 1, 2000);
    char *text;
    size_t len;
    int e = fs_read_all(abs, &text, &len);
    if (e < 0) {
        tool_printf(t, "Error: %s: %s", abs, fs_strerror(e));
        t->error = true;
        return;
    }
    printk(CYAN "● read" RESET " %s\n", abs);
    if (memchr(text, 0, len)) {
        tool_printf(t, "%s is a binary file (%zu bytes); use bash with hexdump.", abs, len);
        kfree(text);
        return;
    }
    long line = 1, shown = 0;
    for (char *p = text; *p && shown < limit && t->len + 2 < t->cap;) {
        char *eol = strchr(p, '\n');
        size_t l = eol ? (size_t)(eol - p) : strlen(p);
        if (line >= offset) {
            tool_printf(t, "%5ld\t%.*s\n", line, (int)MIN(l, 400), p);
            shown++;
        }
        line++;
        if (!eol)
            break;
        p = eol + 1;
    }
    if (!shown)
        tool_printf(t, "(no lines at offset %ld; the file has %ld lines)", offset, line - 1);
    else if (t->len + 2 >= t->cap)
        tool_printf(t, "(output truncated; use offset/limit)");
    kfree(text);
}
HARNESS_TOOL(b_read, "read", "{\"file_path\": string, \"offset\": int, \"limit\": int}",
             "read a text file with line numbers (offset = first line, default 1)", tool_read);

/* ---- grep: lines containing a string, in a file or under a directory ---- */
struct grep_state {
    const char *pattern;
    int matches, files;
    struct tool_ctx *t;
};

static void grep_file(struct grep_state *g, const char *path)
{
    char *text;
    size_t len;
    if (fs_read_all(path, &text, &len) < 0)
        return;
    g->files++;
    if (!memchr(text, 0, len)) {
        long line = 1;
        for (char *p = text; *p && g->matches < 60;) {
            char *eol = strchr(p, '\n');
            if (eol)
                *eol = 0;
            if (strstr(p, g->pattern)) {
                tool_printf(g->t, "%s:%ld: %.200s\n", path, line, p);
                g->matches++;
            }
            line++;
            if (!eol)
                break;
            p = eol + 1;
        }
    }
    kfree(text);
}

static void grep_walk(struct grep_state *g, const char *path, int depth)
{
    struct fs_stat st;
    if (fs_stat(path, &st) < 0 || g->matches >= 60)
        return;
    if (st.type == FT_FILE) {
        if (st.size <= (4u << 20)) /* skip huge files such as models */
            grep_file(g, path);
        return;
    }
    if (depth > 8)
        return;
    struct fs_dirent *ents;
    int n;
    if (fs_list(path, &ents, &n) < 0)
        return;
    for (int i = 0; i < n; i++) {
        char child[FS_PATH_MAX];
        snprintf(child, sizeof(child), "%s%s%s", path, strcmp(path, "/") ? "/" : "", ents[i].name);
        grep_walk(g, child, depth + 1);
    }
    kfree(ents);
}

static void tool_grep(const struct jval *args, struct tool_ctx *t)
{
    const char *pattern;
    if (!need_str(args, "pattern", &pattern, t))
        return;
    const char *path = json_str(json_get(args, "path"));
    char abs[FS_PATH_MAX];
    if (!resolve(path && *path ? path : ".", abs, t))
        return;
    printk(CYAN "● grep" RESET " \"%s\" in %s\n", pattern, abs);
    struct grep_state g = { pattern, 0, 0, t };
    grep_walk(&g, abs, 0);
    if (!g.matches)
        tool_printf(t, "No matches for \"%s\" in %s (%d files searched).", pattern, abs, g.files);
    else if (g.matches >= 60)
        tool_printf(t, "(stopped after 60 matches)");
}
HARNESS_TOOL(c_grep, "grep", "{\"pattern\": string, \"path\": string}",
             "find lines containing the text pattern in a file or under a directory", tool_grep);

/* ---- write: create or replace a file ---- */
static void tool_write(const struct jval *args, struct tool_ctx *t)
{
    const char *path;
    char abs[FS_PATH_MAX];
    if (!need_str(args, "file_path", &path, t) || !resolve(path, abs, t))
        return;
    const char *content = json_str(json_get(args, "content"));
    if (!content) {
        tool_printf(t, "Error: missing string argument \"content\".");
        t->error = true;
        return;
    }
    if (!harness_allow_write(abs)) {
        tool_printf(t, "The user did not allow writing %s.", abs);
        t->error = true;
        return;
    }
    int e = fs_write_all(abs, content, strlen(content));
    if (e < 0) {
        tool_printf(t, "Error: %s: %s", abs, fs_strerror(e));
        t->error = true;
        return;
    }
    printk(CYAN "● write" RESET " %s (%zu bytes)\n", abs, strlen(content));
    tool_printf(t, "Wrote %zu bytes to %s.", strlen(content), abs);
}
HARNESS_TOOL(d_write, "write", "{\"file_path\": string, \"content\": string}",
             "create or overwrite a text file", tool_write);

/* ---- edit: replace one exact occurrence of a string in a file ---- */
static void tool_edit(const struct jval *args, struct tool_ctx *t)
{
    const char *path, *old;
    char abs[FS_PATH_MAX];
    if (!need_str(args, "file_path", &path, t) || !resolve(path, abs, t) ||
        !need_str(args, "old_string", &old, t))
        return;
    const char *repl = json_str(json_get(args, "new_string"));
    if (!repl) {
        tool_printf(t, "Error: missing string argument \"new_string\".");
        t->error = true;
        return;
    }
    char *text;
    size_t len;
    int e = fs_read_all(abs, &text, &len);
    if (e < 0) {
        tool_printf(t, "Error: %s: %s", abs, fs_strerror(e));
        t->error = true;
        return;
    }
    char *at = strstr(text, old);
    if (!at || strstr(at + 1, old)) {
        tool_printf(t, "Error: old_string must occur exactly once in %s (found %s).", abs,
                    at ? "more than once" : "no match");
        t->error = true;
        kfree(text);
        return;
    }
    if (!harness_allow_write(abs)) {
        tool_printf(t, "The user did not allow editing %s.", abs);
        t->error = true;
        kfree(text);
        return;
    }
    size_t ol = strlen(old), nl = strlen(repl), pre = (size_t)(at - text);
    char *out = kmalloc(len - ol + nl + 1);
    if (!out) {
        kfree(text);
        return;
    }
    memcpy(out, text, pre);
    memcpy(out + pre, repl, nl);
    memcpy(out + pre + nl, at + ol, len - pre - ol);
    out[len - ol + nl] = 0;
    e = fs_write_all(abs, out, len - ol + nl);
    if (e < 0) {
        tool_printf(t, "Error: %s: %s", abs, fs_strerror(e));
        t->error = true;
    } else {
        printk(CYAN "● edit" RESET " %s\n", abs);
        tool_printf(t, "Edited %s.", abs);
    }
    kfree(out);
    kfree(text);
}
HARNESS_TOOL(e_edit, "edit", "{\"file_path\": string, \"old_string\": string, "
                             "\"new_string\": string}",
             "replace text that occurs exactly once in a file", tool_edit);

/* ---- web_fetch: an http:// page (Sonar has no TLS yet) ---- */
static void tool_web_fetch(const struct jval *args, struct tool_ctx *t)
{
    const char *url;
    if (!need_str(args, "url", &url, t))
        return;
    if (strncmp(url, "http://", 7)) {
        tool_printf(t, "Error: only http:// URLs can be fetched (Sonar has no TLS for "
                       "https:// yet).");
        t->error = true;
        return;
    }
    if (strchr(url, ' ') || strchr(url, '>')) {
        tool_printf(t, "Error: invalid URL.");
        t->error = true;
        return;
    }
    printk(CYAN "● web_fetch" RESET " %s\n", url);
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "fetch %s", url);
    size_t room = t->cap - t->len;
    char *buf = kmalloc(room);
    if (!buf)
        return;
    size_t n;
    int rc = capture(cmd, buf, room, &n);
    tool_printf(t, "%s%s", buf, n + 1 >= room ? "\n(truncated)" : "");
    t->error = rc != 0;
    kfree(buf);
}
HARNESS_TOOL(f_web_fetch, "web_fetch", "{\"url\": string}",
             "download an http:// URL and return its text", tool_web_fetch);
