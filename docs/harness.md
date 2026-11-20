# The DeepSeek harness

Sonar's AI assistant is an agent: DeepSeek can use tools to look at and
change the computer, with your approval where needed. The agent loop follows
the design of **DeepSeek Harness** (`dsh`), DeepSeek's open-source agent
harness ([repository](https://github.com/deepseek-ai/deepseek-harness),
[announcement](https://www.deepseek.com/en/harness)).

dsh itself is a Node.js/TypeScript program built on the Cordis plugin
framework. Sonar has no JavaScript runtime and no POSIX userland, so it
cannot run dsh. Sonar's harness is **a native C implementation of dsh's
design inside the kernel**, not dsh itself. It is in `kernel/ai/harness.c`
(the loop's plumbing), `kernel/ai/tools.c` (the tools) and
`kernel/ai/assistant.c` (the conversation).

## Using it

| Command | What it does |
| --- | --- |
| `? QUESTION` | ask DeepSeek; it may call tools before answering |
| `dsh` | interactive harness prompt (`dsh>`); `exit` leaves |
| `dsh QUESTION` | one request, same as `?` |
| `chat` | the same as `dsh` |

In the desktop, the dock's **Ask DeepSeek** button types `? ` into the
Terminal for you.

Slash commands, typed after `?` or at the `dsh>` prompt:

| Command | |
| --- | --- |
| `/new` | start a new session |
| `/compact` | summarize the conversation so far to free context |
| `/sessions` | list saved sessions |
| `/resume ID` | continue a saved session |
| `/tools` | list the tools DeepSeek can call |
| `/todos` | show DeepSeek's current plan (from `todo_write`) |
| `/permission [MODE]` | show or set the permission mode |
| `/help` | list these commands |

Ctrl-C stops a reply.

## How a turn works

As in dsh, a turn is a series of **steps**. Each step is one model request.
When the reply contains a tool call, the harness runs the tool, adds the
result to the conversation and starts the next step. The turn ends when the
model answers without calling a tool, or after `ai.max_steps` steps
(default 10).

With reasoning on, DeepSeek-R1 models reason inside `<think>…</think>`
before every step. To keep multi-step turns from taking many minutes, the
reasoning has a budget: `ai.think_budget` tokens on the first step (default
1024) and `ai.think_budget_step` after a tool result (default 384); `0`
means no limit. When the budget runs out, the harness writes `</think>`
itself and the model goes on to answer. This is the "budget forcing"
technique from [s1: Simple test-time scaling](https://arxiv.org/abs/2501.19393).

### Checks on the model's answers

A small model sometimes states what a file or command will show before it
has looked, or (with reasoning off) repeats an earlier answer word for
word. The harness guards against both:

- **Prose before a tool call is dropped.** The start of each answer (up to
  320 bytes) is held back until it is clear whether a tool call follows. If
  one does, that text is neither shown nor kept in the conversation, so the
  next step does not build on the guess.
- **Grounding.** When the question names a path or a file (`/home/notes.txt`,
  `sonar.conf`) or asks about this computer (free space, memory, uptime, IP
  address) and the model answers without having run any tool in this turn,
  the harness discards the answer and asks it once to call the tool that
  shows it. `config ai.verify off` turns this off.
- **Repetition.** An answer identical to one given earlier in the
  conversation, to a different question, is discarded once with a note to
  answer the latest question.

Each check retries at most once per turn and is logged as an `agent/nudge`
event. If the discarded answer was long enough to have been shown already,
`(checking that...)` marks the second answer.

The model calls a tool by writing:

```
<tool_call>
{"name": "bash", "arguments": {"command": "df -h"}}
</tool_call>
```

This is the tool-call format of Qwen3, the base model of
DeepSeek-R1-0528-Qwen3-8B. The harness replies with
`<tool_response>…</tool_response>`. It also accepts arguments written as a
JSON string, JSON in a Markdown code fence, and a plain `RUN: command`
line, all of which small models sometimes produce. Tool calls are hidden
from the screen; you see one line per call (`● bash df -h`), and the output
of `bash` commands.

Along the way the harness fires events with dsh's names: `agent/pre-step`,
`agent/request`, `tools/pre-execute`, `tools/execute`, `tools/post-execute`
and `agent/turn-stopping`. `config ai.trace on` prints them.

## Tools

| Tool | Arguments | |
| --- | --- | --- |
| `bash` | `command`, `description` | runs one command in Sonar's shell (Sonar's shell is not a POSIX shell: no pipes, but `>` and `>>` work) |
| `read` | `file_path`, `offset`, `limit` | a text file with line numbers |
| `grep` | `pattern`, `path` | lines containing the text, in a file or a directory tree |
| `write` | `file_path`, `content` | create or replace a file |
| `edit` | `file_path`, `old_string`, `new_string` | replace text that occurs exactly once |
| `web_fetch` | `url` | an `http://` page (no HTTPS yet) |
| `todo_write` | `todos` | DeepSeek's plan for a longer task |

`bash`, `read`, `grep` and `web_fetch` follow dsh's tool names. Tool output
is cut at 4,000 characters. Some shell commands are never offered to the
model, such as `setup` and the assistant's own commands.

Tools are plugins, as in dsh: each is declared with `HARNESS_TOOL(...)` in
any source file and collected by the linker (section `.harness`), so adding
a tool does not mean editing a central list. Event listeners are declared
the same way with `HARNESS_LISTENER(...)`.

## Permissions

dsh's three presets, set with `? /permission MODE` (config `ai.permission`):

| Mode | Reading | Writing files | Other commands |
| --- | --- | --- | --- |
| `read-only` | yes | no | read-only commands only |
| `workspace-write` (default) | yes | in the workspace and `/tmp`; elsewhere asks you | file commands (`write`, `mkdir`, `cp`, `mv`, `rm`, ...) and `>` redirects that only touch the workspace run; others ask you |
| `danger-full-access` | yes | yes | yes |

The workspace is `/home` (config `ai.workspace`). When the harness asks, you
answer `y` (once), `n` (deny; the model is told) or `a` (allow that kind of
action, writes or commands, for the rest of the session). Commands that only read (`ls`, `cat`, `df`, `ifconfig` and
others) never need approval.

## Sessions

Every session is saved as an append-only JSON Lines file, as in dsh:

```
/var/dsh/sessions/20261118-031915-455/session.v1.jsonl
```

Each line is one event `{"seq": N, "time": ..., "type": ..., "data": {...}}`.
Sonar writes `session/start` (model, cwd, permission mode, workspace),
`message/user`, `message/assistant`, `tool/call`, `tool/result`,
`compaction/start`, `compaction/summary`, `compaction/end` and
`session/resume`. `/resume ID` rebuilds the conversation from the file.

The event names follow dsh's session design
([docs/subsystems/session.md](https://github.com/deepseek-ai/deepseek-harness/blob/main/docs/subsystems/session.md)),
but the file is not guaranteed to be readable by dsh.

## Compaction

When the conversation no longer fits in the model's context (`ai.ctx`
tokens), the harness asks the model for a short summary of the session so
far, starts a fresh context with that summary and logs `compaction/*`
events. `/compact` does this on demand.

## AGENTS.md

As in dsh, project instructions come from `AGENTS.md` (or `CLAUDE.md`)
files. The harness reads them from `/` down to the current directory, up to
65,536 bytes in total, and adds them to the model's instructions.

## Limits

- The model runs on the CPU. DeepSeek-R1-0528-Qwen3-8B at Q4_K produced
  4–6.5 tokens per second with 4 cores in Apple's Virtualization framework
  on an M1 MacBook Pro (about 5–6 in QEMU), so a turn with tool calls takes
  from several seconds to minutes. Single-token generation uses fused Q4_K
  and Q6_K dot-product kernels.
- An 8B model makes mistakes with tools. The checks above catch an answer
  about the computer given without a tool call and an answer that repeats
  an earlier one, and a shell command called as if it were a tool (`df`
  instead of `bash` with `df`) is run through `bash`. It can still pick
  the wrong tool or ramble, and with reasoning off it tends to write its
  reasoning into the answer.
- No HTTPS, so `web_fetch` only reaches `http://` sites.
- No subagents, MCP servers or model API providers, which dsh has.
