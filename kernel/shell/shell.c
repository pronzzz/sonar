/* The Sonar shell: command parsing, dispatch and output capture. */
#include "ai.h"
#include "console.h"
#include "fs.h"
#include "kernel.h"
#include "mm.h"
#include "shell.h"
#include "string.h"

extern const struct shell_cmd __shellcmd_start[], __shellcmd_end[];

/* ---- output capture (lets the assistant read command output) ---- */
static char *cap_buf;
static size_t cap_size, cap_len;

static void cap_write(const char *s, size_t len)
{
    if (!cap_buf)
        return;
    for (size_t i = 0; i < len && cap_len + 1 < cap_size; i++)
        cap_buf[cap_len++] = s[i];
    cap_buf[cap_len] = 0;
}

static struct console_sink cap_sink = { "capture", cap_write, NULL };
static bool cap_registered;

void shell_capture_begin(char *buf, size_t size)
{
    if (!cap_registered) {
        console_register(&cap_sink);
        cap_registered = true;
    }
    cap_len = 0;
    cap_size = size;
    if (size)
        buf[0] = 0;
    cap_buf = buf;
}

size_t shell_capture_end(void)
{
    cap_buf = NULL;
    return cap_len;
}

/* ---- parsing ---- */

/* Split in place on whitespace, honouring "double" and 'single' quotes. */
int shell_split(char *line, char **argv, int max)
{
    int argc = 0;
    char *r = line, *w = line;
    while (*r) {
        while (isspace((u8)*r))
            r++;
        if (!*r || argc >= max - 1)
            break;
        argv[argc++] = w;
        char quote = 0;
        while (*r && (quote || !isspace((u8)*r))) {
            if (!quote && (*r == '"' || *r == '\'')) {
                quote = *r++;
                continue;
            }
            if (quote && *r == quote) {
                quote = 0;
                r++;
                continue;
            }
            /* backslash escapes quotes, spaces and itself; other sequences
             * (like \n for `write`) are kept for the command to interpret */
            if (*r == '\\' && (r[1] == '"' || r[1] == '\'' || r[1] == '\\' || isspace((u8)r[1])))
                r++;
            *w++ = *r++;
        }
        if (*r)
            r++;
        *w++ = 0;
    }
    argv[argc] = NULL;
    return argc;
}

const struct shell_cmd *shell_find(const char *name)
{
    for (const struct shell_cmd *c = __shellcmd_start; c < __shellcmd_end; c++)
        if (strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

void shell_list_commands(char *out, size_t size)
{
    size_t n = 0;
    out[0] = 0;
    for (const struct shell_cmd *c = __shellcmd_start; c < __shellcmd_end && n < size; c++)
        n += (size_t)snprintf(out + n, size - n, "%s - %s\n", c->usage[0] ? c->usage : c->name,
                              c->help); /* usage starts with the name */
}

/* ---- redirection: "cmd > file" and "cmd >> file" ---- */
static char *redir_buf;
static size_t redir_len, redir_cap;
static bool redir_overflow;

static void redir_write(const char *s, size_t len)
{
    if (redir_len + len > redir_cap) {
        size_t nc = redir_cap ? redir_cap * 2 : 4096;
        while (nc < redir_len + len)
            nc *= 2;
        if (nc > (16u << 20)) {
            redir_overflow = true;
            return;
        }
        char *nb = krealloc(redir_buf, nc);
        if (!nb) {
            redir_overflow = true;
            return;
        }
        redir_buf = nb;
        redir_cap = nc;
    }
    memcpy(redir_buf + redir_len, s, len);
    redir_len += len;
}

/* Find an unquoted '>' and split "cmd > target". */
bool shell_split_redirect(char *line, char **target, bool *append)
{
    char quote = 0;
    for (char *p = line; *p; p++) {
        if (quote) {
            if (*p == quote)
                quote = 0;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            quote = *p;
            continue;
        }
        if (*p == '\\' && p[1]) {
            p++;
            continue;
        }
        if (*p == '>') {
            *append = p[1] == '>';
            *p = 0;
            char *t = p + (*append ? 2 : 1);
            while (isspace((u8)*t))
                t++;
            char *e = t + strlen(t);
            while (e > t && isspace((u8)e[-1]))
                *--e = 0;
            if (e > t && (*t == '"' || *t == '\'') && e[-1] == *t) {
                e[-1] = 0;
                t++;
            }
            *target = t;
            return true;
        }
    }
    return false;
}

static int run_argv(int argc, char **argv)
{
    const struct shell_cmd *c = shell_find(argv[0]);
    if (!c) {
        printk("%s: command not found. Type 'help', or ask the AI: ? <question>\n", argv[0]);
        return 127;
    }
    int r = c->fn(argc, argv);
    ai_done(); /* whatever DeepSeek computed for it is finished */
    return r;
}

int shell_exec(const char *line)
{
    char buf[1024];
    char *argv[SHELL_MAX_ARGS];
    strlcpy(buf, line, sizeof(buf));
    char *target = NULL;
    bool append = false;
    bool redir = shell_split_redirect(buf, &target, &append);
    int argc = shell_split(buf, argv, SHELL_MAX_ARGS);
    if (argc == 0)
        return 0;
    if (!redir)
        return run_argv(argc, argv);
    if (!target || !*target) {
        printk("shell: missing file name after '>'\n");
        return 2;
    }
    if (console_redirected()) {
        printk("shell: nested redirection is not supported\n");
        return 2;
    }
    redir_len = 0;
    redir_overflow = false;
    console_redirect(redir_write);
    int rc = run_argv(argc, argv);
    console_redirect(NULL);
    int r = append ? fs_append(target, redir_buf, redir_len)
                   : fs_write_all(target, redir_buf, redir_len);
    if (r < 0) {
        printk("shell: %s: %s\n", target, fs_strerror(r));
        rc = 1;
    } else if (redir_overflow) {
        printk("shell: output truncated at 16 MiB\n");
    }
    kfree(redir_buf);
    redir_buf = NULL;
    redir_cap = 0;
    return rc;
}

void shell_run(void)
{
    char line[1024];
    char prompt[FS_PATH_MAX + 48];
    printk("\nType 'help' for commands. Ask DeepSeek anything with: ? <question>\n\n");
    for (;;) {
        snprintf(prompt, sizeof(prompt), "\x1b[1;36msonar\x1b[0m:\x1b[1;34m%s\x1b[0m$ ",
                 fs_cwd());
        int n = readline(prompt, line, sizeof(line));
        if (n <= 0)
            continue;
        char *p = line;
        while (isspace((u8)*p))
            p++;
        if (*p == '?') {
            ai_ask_command(p + 1);
            continue;
        }
        shell_exec(p);
    }
}

/* ---- built-in commands ---- */

static int cmd_help(int argc, char **argv)
{
    if (argc > 1) {
        const struct shell_cmd *c = shell_find(argv[1]);
        if (!c) {
            printk("help: no such command '%s'\n", argv[1]);
            return 1;
        }
        printk("usage: %s %s\n  %s\n", c->name, c->usage, c->help);
        return 0;
    }
    printk("Commands:\n");
    for (const struct shell_cmd *c = __shellcmd_start; c < __shellcmd_end; c++)
        printk("  %-10s %s\n", c->name, c->help);
    printk("\n  ? <text>   ask the DeepSeek assistant (it can run commands for you)\n");
    return 0;
}
SHELL_COMMAND(help, "help", "[command]", "list commands or show help for one", cmd_help);

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        printk("%s%s", argv[i], i + 1 < argc ? " " : "");
    printk("\n");
    return 0;
}
SHELL_COMMAND(echo, "echo", "[text...]", "print text", cmd_echo);

static int cmd_clear(int argc, char **argv)
{
    printk("\x1b[2J\x1b[H");
    return 0;
}
SHELL_COMMAND(clear, "clear", "", "clear the screen", cmd_clear);
