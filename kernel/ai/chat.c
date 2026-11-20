/*
 * DeepSeek chat template (from the model's own tokenizer.chat_template):
 *
 *   <｜begin▁of▁sentence｜>{system}<｜User｜>{q1}<｜Assistant｜>{a1}<｜end▁of▁sentence｜>
 *   <｜User｜>{q2}<｜Assistant｜>
 *
 * Earlier assistant turns are included without their <think> reasoning,
 * exactly as the template does (content.split('</think>')[-1]).
 */
#include "chat.h"

static bool push(struct chat *c, int tok)
{
    if (tok < 0)
        return true;
    if (c->n_tokens >= c->cap)
        return false;
    c->tokens[c->n_tokens++] = tok;
    return true;
}

static bool push_text(struct chat *c, const char *text)
{
    int len = (int)strlen(text);
    if (!len)
        return true;
    int n = tokenizer_encode(&c->m->tok, text, len, c->tokens + c->n_tokens, c->cap - c->n_tokens);
    if (n < 0)
        return false;
    c->n_tokens += n;
    return true;
}

int chat_init(struct chat *c, struct model *m)
{
    memset(c, 0, sizeof(*c));
    c->m = m;
    c->cap = m->n_ctx;
    c->tokens = ai_alloc(sizeof(int) * (size_t)c->cap);
    if (!c->tokens)
        return -1;
    const struct tokenizer *t = &m->tok;
    c->t_bos = tokenizer_find(t, "<｜begin▁of▁sentence｜>");
    c->t_eos = tokenizer_find(t, "<｜end▁of▁sentence｜>");
    c->t_user = tokenizer_find(t, "<｜User｜>");
    c->t_assistant = tokenizer_find(t, "<｜Assistant｜>");
    c->t_think = tokenizer_find(t, "<think>");
    c->t_think_end = tokenizer_find(t, "</think>");
    c->deepseek = c->t_user >= 0 && c->t_assistant >= 0 && c->t_eos >= 0;
    if (c->t_bos < 0)
        c->t_bos = t->bos;
    if (c->t_eos < 0)
        c->t_eos = t->eos;
    sampler_init(&c->smp, 0.6f, 0.95f, 40, ai_time_ns());
    return 0;
}

void chat_free(struct chat *c)
{
    ai_free(c->tokens);
    memset(c, 0, sizeof(*c));
}

/* Text after the last </think> (what the template keeps for history). */
static const char *strip_thinking(const char *s)
{
    const char *last = NULL;
    for (const char *p = s; (p = strstr(p, "</think>")) != NULL; p += 8)
        last = p + 8;
    if (!last)
        return s;
    while (*last == '\n' || *last == ' ')
        last++;
    return last;
}

int chat_build(struct chat *c, const char *system, const struct chat_turn *turns, int n, bool think)
{
    c->n_tokens = 0;
    if (!c->deepseek) {
        /* Not a DeepSeek chat model: plain transcript. */
        bool ok = push(c, c->t_bos) && (!system || push_text(c, system)) && push_text(c, "\n\n");
        for (int i = 0; ok && i < n; i++) {
            ok = push_text(c, turns[i].role == ROLE_USER ? "User: " : "Assistant: ") &&
                 push_text(c, turns[i].text) && push_text(c, "\n\n");
        }
        ok = ok && push_text(c, "Assistant:");
        return ok ? 0 : -1;
    }
    bool ok = push(c, c->t_bos);
    if (ok && system && *system)
        ok = push_text(c, system);
    for (int i = 0; ok && i < n; i++) {
        if (turns[i].role == ROLE_USER) {
            ok = push(c, c->t_user) && push_text(c, turns[i].text) && push(c, c->t_assistant);
        } else {
            ok = push_text(c, strip_thinking(turns[i].text)) && push(c, c->t_eos);
        }
    }
    c->think_open = false;
    if (ok && n > 0 && turns[n - 1].role == ROLE_USER && c->t_think >= 0 && c->t_think_end >= 0) {
        if (think) {
            /* Start the reply inside the reasoning block. */
            ok = push(c, c->t_think) && push_text(c, "\n");
            c->think_open = true;
        } else {
            /* Skip reasoning: prefill an empty think block. */
            ok = push(c, c->t_think) && push_text(c, "\n\n") && push(c, c->t_think_end) &&
                 push_text(c, "\n\n");
        }
    }
    return ok ? 0 : -1;
}

int chat_build_raw(struct chat *c, const char *text, bool bos)
{
    c->n_tokens = 0;
    bool ok = (!bos || push(c, c->t_bos)) && push_text(c, text);
    return ok ? 0 : -1;
}

static bool is_stop(const struct chat *c, int tok)
{
    return tok == c->t_eos || tok == c->m->tok.eos ||
           (c->deepseek && (tok == c->t_user || tok == c->t_assistant || tok == c->t_bos));
}

int chat_generate(struct chat *c, int max_new, chat_emit_fn emit, void *ctx, char *reply,
                  size_t reply_size, struct chat_stats *st)
{
    struct model *m = c->m;
    memset(st, 0, sizeof(*st));
    size_t rlen = 0;
    if (reply_size)
        reply[0] = 0;
    st->prompt_tokens = c->n_tokens;
    int before = m->n_past;
    u64 t0 = ai_time_ns();
    int reuse = 0;
    while (reuse < before && reuse < c->n_tokens && m->cached[reuse] == c->tokens[reuse])
        reuse++;
    float *logits = model_feed(m, c->tokens, c->n_tokens);
    st->prefill_tokens = c->n_tokens - (reuse < c->n_tokens ? reuse : c->n_tokens - 1);
    st->prefill_ns = ai_time_ns() - t0;
    if (!logits) {
        if (m->n_past + 1 >= m->n_ctx || c->n_tokens >= m->n_ctx)
            st->ctx_full = true;
        else
            st->aborted = true;
        return -1;
    }
    u64 t1 = ai_time_ns();
    bool thinking = c->think_open && c->t_think_end >= 0;
    int think_n = 0;
    for (int i = 0; i < max_new; i++) {
        int tok;
        if (thinking && c->think_budget > 0 && think_n >= c->think_budget) {
            tok = c->t_think_end; /* budget reached: end the reasoning */
            st->think_forced = true;
        } else {
            tok = sampler_sample(&c->smp, logits, m->info.n_vocab);
            if (is_stop(c, tok))
                break;
        }
        if (thinking) {
            if (tok == c->t_think_end)
                thinking = false;
            else
                think_n++;
        }
        char piece[256];
        int pl = tokenizer_decode(&m->tok, tok, piece, sizeof(piece));
        st->gen_tokens++;
        if (rlen + (size_t)pl + 1 < reply_size) {
            memcpy(reply + rlen, piece, (size_t)pl);
            rlen += (size_t)pl;
            reply[rlen] = 0;
        }
        if (emit && pl > 0 && !emit(ctx, piece, pl)) {
            st->aborted = true;
            break;
        }
        if (ai_should_abort()) {
            st->aborted = true;
            break;
        }
        if (m->n_past >= m->n_ctx) {
            st->ctx_full = true;
            break;
        }
        logits = model_forward(m, &tok, 1);
        if (!logits) {
            st->ctx_full = true;
            break;
        }
        if (i == max_new - 1)
            st->hit_limit = true;
    }
    st->gen_ns = ai_time_ns() - t1;
    return 0;
}
