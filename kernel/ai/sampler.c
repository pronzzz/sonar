/* Sampling. Defaults follow DeepSeek's guidance for R1 models:
 * temperature 0.6, top-p 0.95. */
#include "sampler.h"
#include "aimath.h"

#define MAX_K 128

static u64 splitmix(u64 *x)
{
    u64 z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static inline u64 rotl(u64 x, int k) { return (x << k) | (x >> (64 - k)); }

u64 sampler_rand(struct sampler *s)
{
    u64 r = rotl(s->s[1] * 5, 7) * 9;
    u64 t = s->s[1] << 17;
    s->s[2] ^= s->s[0];
    s->s[3] ^= s->s[1];
    s->s[1] ^= s->s[2];
    s->s[0] ^= s->s[3];
    s->s[2] ^= t;
    s->s[3] = rotl(s->s[3], 45);
    return r;
}

void sampler_init(struct sampler *s, float temperature, float top_p, int top_k, u64 seed)
{
    s->temperature = temperature;
    s->top_p = top_p;
    s->min_p = 0.05f;
    s->top_k = top_k <= 0 || top_k > MAX_K ? 40 : top_k;
    u64 x = seed;
    for (int i = 0; i < 4; i++)
        s->s[i] = splitmix(&x);
}

int sampler_argmax(const float *logits, int n)
{
    int best = 0;
    for (int i = 1; i < n; i++)
        if (logits[i] > logits[best])
            best = i;
    return best;
}

int sampler_sample(struct sampler *s, const float *logits, int n)
{
    if (s->temperature <= 0.0f)
        return sampler_argmax(logits, n);
    /* top-k by insertion into a small sorted array */
    int k = s->top_k < n ? s->top_k : n;
    int ids[MAX_K];
    float val[MAX_K];
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        float v = logits[i];
        if (cnt == k && v <= val[k - 1])
            continue;
        int j = cnt < k ? cnt++ : k - 1;
        while (j > 0 && val[j - 1] < v) {
            val[j] = val[j - 1];
            ids[j] = ids[j - 1];
            j--;
        }
        val[j] = v;
        ids[j] = i;
    }
    /* softmax with temperature over the candidates */
    float p[MAX_K];
    double sum = 0.0;
    for (int i = 0; i < cnt; i++) {
        p[i] = ai_expf((val[i] - val[0]) / s->temperature);
        sum += p[i];
    }
    /* min-p, then top-p (candidates are sorted by probability) */
    int keep = cnt;
    for (int i = 1; i < cnt; i++) {
        if (p[i] < s->min_p * p[0]) {
            keep = i;
            break;
        }
    }
    double cum = 0.0;
    for (int i = 0; i < keep; i++) {
        cum += p[i] / sum;
        if (cum >= s->top_p) {
            keep = i + 1;
            break;
        }
    }
    double total = 0.0;
    for (int i = 0; i < keep; i++)
        total += p[i];
    double r = (double)(sampler_rand(s) >> 11) * (1.0 / 9007199254740992.0) * total;
    for (int i = 0; i < keep; i++) {
        r -= p[i];
        if (r < 0)
            return ids[i];
    }
    return ids[keep - 1];
}
