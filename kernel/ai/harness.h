/*
 * Sonar's agent harness for DeepSeek, modeled on DeepSeek Harness (dsh,
 * github.com/deepseek-ai/deepseek-harness): tools and event listeners are
 * plugins, an agent turn is a series of steps (one model request plus its
 * tool call), every session is an append-only JSONL event log, long
 * sessions are compacted, permission presets decide what needs approval,
 * and AGENTS.md files carry project instructions. dsh itself is a Node.js
 * program; this is a native implementation of the same design for Sonar.
 */
#pragma once
#include "json.h"
#include "types.h"

/* ---- tools (plugins collected from a linker section) ---- */
struct tool_ctx {
    char *out;      /* text returned to the model */
    size_t cap, len;
    bool error;
};
void tool_printf(struct tool_ctx *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

struct harness_tool {
    const char *name;
    const char *params;      /* argument summary shown to the model */
    const char *description;
    void (*run)(const struct jval *args, struct tool_ctx *t);
};

#define HARNESS_TOOL(ident, tname, tparams, tdesc, tfn)                                       \
    static const struct harness_tool __harness_tool_##ident                                   \
        __attribute__((used, section(".rodata.harnesstool." #ident))) = { tname, tparams,     \
                                                                          tdesc, tfn }

const struct harness_tool *harness_tool_find(const char *name);
void harness_tools_describe(char *out, size_t size);

/* ---- events (dsh names: agent/pre-step, agent/request, tools/pre-execute,
 * tools/execute, tools/post-execute, agent/turn-stopping) ---- */
struct harness_listener {
    const char *name;
    void (*fn)(const char *event, const char *data_json);
};
#define HARNESS_LISTENER(ident, lfn)                                                           \
    static const struct harness_listener __harness_listener_##ident                           \
        __attribute__((used, section(".rodata.harnesslistener." #ident))) = { #ident, lfn }
void harness_event(const char *event, const char *data_json);

/* ---- permissions (dsh presets) ---- */
enum { PERM_READ_ONLY, PERM_WORKSPACE_WRITE, PERM_FULL_ACCESS };
int harness_permission(void);
const char *harness_permission_name(int mode);
const char *harness_workspace(void);
/* May the agent write path / run an unlisted command? Asks the user when
 * the preset requires it. */
bool harness_allow_write(const char *abs_path);
bool harness_allow_command(const char *cmd);
bool harness_command_is_safe(const char *cmd); /* read-only command */
bool harness_command_hidden(const char *name);  /* never offered to the model */

/* ---- sessions: /var/dsh/sessions/ID/session.v1.jsonl ---- */
void harness_session_new(const char *model);
void harness_session_resume(const char *id, long next_seq);
const char *harness_session_id(void);
void harness_log(const char *type, const char *data_json); /* append one event */
void harness_log_text(const char *type, const char *key, const char *text);
int harness_sessions_list(void); /* prints them */

/* ---- tool calls ---- */
struct tool_call {
    char name[32];
    char *arguments; /* raw JSON text as the model wrote it (kmalloc'd) */
    bool legacy;     /* came from a "RUN: command" line */
};
/* Finds the model's tool call in its answer. */
bool harness_parse_call(const char *answer, struct tool_call *tc);
/* Runs it (with events, approval and logging); returns the text for the
 * model, wrapped in <tool_response>. */
char *harness_execute(struct tool_call *tc);
void harness_call_free(struct tool_call *tc);

/* AGENTS.md (or CLAUDE.md) from / down to the current directory, up to
 * 65,536 bytes as in dsh. Returns kmalloc'd text or NULL. */
char *harness_instructions(void);

/* Todo list kept by the todo_write tool, for display. */
void harness_todos_print(void);
void harness_todos_clear(void);
