/* Chat sessions on top of the model: DeepSeek prompt template, streaming
 * generation with stop handling, and statistics. */
#pragma once

#include "model.h"
#include "sampler.h"

enum chat_role { ROLE_USER, ROLE_ASSISTANT };

struct chat_turn {
    int role;
    const char *text;
};

struct chat {
    struct model *m;
    struct sampler smp;
    int *tokens;
    int n_tokens, cap;
    int t_bos, t_eos, t_user, t_assistant, t_think, t_think_end;
    bool deepseek; /* model has DeepSeek chat special tokens */
    bool think_open; /* prompt ends inside an opened <think> block */
    /* Reasoning budget in tokens (0 = none). When reasoning reaches it, the
     * generator writes </think> itself so the model goes on to answer
     * ("budget forcing", Muennighoff et al. 2025, arXiv:2501.19393). */
    int think_budget;
};

struct chat_stats {
    int prompt_tokens;  /* tokens in the prompt */
    int prefill_tokens; /* tokens actually computed (rest reused from cache) */
    int gen_tokens;
    u64 prefill_ns, gen_ns;
    bool aborted, ctx_full, hit_limit;
    bool think_forced; /* the reasoning budget ended the reasoning */
};

/* Return false to stop generation. */
typedef bool (*chat_emit_fn)(void *ctx, const char *text, int len);

int chat_init(struct chat *c, struct model *m);
void chat_free(struct chat *c);

/* Build the prompt: system text, then alternating turns, ending with an
 * open assistant turn. With think == true the reply starts inside "<think>\n"
 * (as DeepSeek's R1 templates do, so reasoning always happens; think_open is
 * set); with think == false an empty <think></think> block is prefilled so
 * the model answers directly. Returns 0, or -1 if too long. */
int chat_build(struct chat *c, const char *system, const struct chat_turn *turns, int n,
               bool think);

/* Raw continuation prompt (no template), for testing. */
int chat_build_raw(struct chat *c, const char *text, bool bos);

/* Generate up to max_new tokens. The reply text (everything generated) is
 * stored NUL-terminated in reply. */
int chat_generate(struct chat *c, int max_new, chat_emit_fn emit, void *ctx, char *reply,
                  size_t reply_size, struct chat_stats *st);
