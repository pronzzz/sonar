/* Interactive shell and command registry. */
#pragma once

#include "types.h"

#define SHELL_MAX_ARGS 32

struct shell_cmd {
    const char *name;
    const char *usage;
    const char *help;
    int (*fn)(int argc, char **argv);
};

/* Commands are collected from every subsystem through this linker section. */
#define SHELL_COMMAND(ident, cmd_name, cmd_usage, cmd_help, cmd_fn)                     \
    static const struct shell_cmd __shell_cmd_##ident                                   \
        __attribute__((used, section(".rodata.shellcmd." #ident))) = {                  \
            cmd_name, cmd_usage, cmd_help, cmd_fn }

void shell_run(void) __attribute__((noreturn));
int shell_exec(const char *line);
const struct shell_cmd *shell_find(const char *name);
int shell_split(char *line, char **argv, int max);
/* Splits "cmd > file" / "cmd >> file" in place at the first unquoted '>'. */
bool shell_split_redirect(char *line, char **target, bool *append);
void shell_list_commands(char *out, size_t size); /* compact "name - help" list */

/* Output capture, used to feed command output to the AI assistant. */
void shell_capture_begin(char *buf, size_t size);
size_t shell_capture_end(void);
