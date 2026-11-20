/*
 * Double-precision exp/log/sin/cos/pow for the AI engine, accurate to a
 * few ulps over the ranges the engine uses (RoPE angles up to ~1e6, YaRN
 * parameters). Range reduction follows the classic fdlibm approach.
 */
#include "aimath.h"

static const double LN2_HI = 6.93147180369123816490e-01;
static const double LN2_LO = 1.90821492927058770002e-10;
static const double INV_LN2 = 1.44269504088896338700e+00;
static const double PIO2_1 = 1.57079632673412561417e+00;  /* first 33 bits of pi/2 */
static const double PIO2_1T = 6.07710050650619224932e-11; /* pi/2 - PIO2_1 */
static const double INV_PIO2 = 6.36619772367581382433e-01;

static double scale_pow2(double x, int k)
{
    union { u64 u; double d; } s;
    while (k > 1000) {
        s.u = (u64)(1023 + 1000) << 52;
        x *= s.d;
        k -= 1000;
    }
    while (k < -1000) {
        s.u = (u64)(1023 - 1000) << 52;
        x *= s.d;
        k += 1000;
    }
    s.u = (u64)(1023 + k) << 52;
    return x * s.d;
}

double ai_floor(double x)
{
    if (x >= 9007199254740992.0 || x <= -9007199254740992.0)
        return x;
    i64 i = (i64)x;
    double d = (double)i;
    return d > x ? d - 1.0 : d;
}

double ai_ceil(double x)
{
    double f = ai_floor(x);
    return f < x ? f + 1.0 : f;
}

double ai_exp(double x)
{
    if (x > 709.0)
        return 1.0 / 0.0;
    if (x < -745.0)
        return 0.0;
    double kd = ai_floor(x * INV_LN2 + 0.5);
    int k = (int)kd;
    double r = (x - kd * LN2_HI) - kd * LN2_LO;
    /* Taylor series to r^13 on |r| <= 0.347: error < 2e-16 relative. */
    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 13; i++) {
        term *= r / i;
        sum += term;
    }
    return scale_pow2(sum, k);
}

double ai_ln(double x)
{
    if (x <= 0.0)
        return -1.0 / 0.0;
    union { u64 u; double d; } v = { .d = x };
    int e = (int)((v.u >> 52) & 0x7ff);
    if (e == 0) { /* subnormal: normalise */
        v.d *= 18014398509481984.0; /* 2^54 */
        e = (int)((v.u >> 52) & 0x7ff) - 54;
    }
    e -= 1023;
    v.u = (v.u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL; /* m in [1, 2) */
    double m = v.d;
    if (m > 1.41421356237309504880) {
        m *= 0.5;
        e++;
    }
    /* log(m) = 2 atanh(s), s = (m-1)/(m+1), |s| <= 0.1716 */
    double s = (m - 1.0) / (m + 1.0);
    double s2 = s * s, term = s, sum = 0.0;
    for (int i = 1; i <= 41; i += 2) {
        sum += term / i;
        term *= s2;
    }
    return 2.0 * sum + e * LN2_HI + e * LN2_LO;
}

/* sin and cos of r with |r| <= pi/4 (Taylor to r^17 / r^18). */
static double sin_poly(double r)
{
    double r2 = r * r, term = r, sum = r;
    for (int i = 1; i <= 8; i++) {
        term *= -r2 / ((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

static double cos_poly(double r)
{
    double r2 = r * r, term = 1.0, sum = 1.0;
    for (int i = 1; i <= 9; i++) {
        term *= -r2 / ((2 * i - 1) * (2 * i));
        sum += term;
    }
    return sum;
}

/* Reduce x to r in [-pi/4, pi/4] with x = k*pi/2 + r; valid for |x| < 2^28. */
static double reduce(double x, i64 *k)
{
    double kd = ai_floor(x * INV_PIO2 + 0.5);
    *k = (i64)kd;
    return (x - kd * PIO2_1) - kd * PIO2_1T;
}

double ai_sin(double x)
{
    i64 k;
    double r = reduce(x, &k);
    switch (k & 3) {
    case 0: return sin_poly(r);
    case 1: return cos_poly(r);
    case 2: return -sin_poly(r);
    default: return -cos_poly(r);
    }
}

double ai_cos(double x)
{
    i64 k;
    double r = reduce(x, &k);
    switch (k & 3) {
    case 0: return cos_poly(r);
    case 1: return -sin_poly(r);
    case 2: return -cos_poly(r);
    default: return sin_poly(r);
    }
}

double ai_pow(double base, double e)
{
    if (e == 0.0)
        return 1.0;
    if (base == 0.0)
        return 0.0;
    return ai_exp(e * ai_ln(base));
}
