/*
 * Freestanding memory and string routines. All of these are safe to call
 * with the MMU off (no unaligned accesses: compiled with -mstrict-align and
 * written to align word copies explicitly).
 */
#include "kernel.h"
#include "string.h"
#include "mm.h"

#define NOBUILTIN __attribute__((no_builtin))

NOBUILTIN void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        u64 *dw = (u64 *)d;
        const u64 *sw = (const u64 *)s;
        while (n >= 32) {
            u64 a = sw[0], b = sw[1], c = sw[2], e = sw[3];
            dw[0] = a;
            dw[1] = b;
            dw[2] = c;
            dw[3] = e;
            dw += 4;
            sw += 4;
            n -= 32;
        }
        while (n >= 8) {
            *dw++ = *sw++;
            n -= 8;
        }
        d = (u8 *)dw;
        s = (const u8 *)sw;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

NOBUILTIN void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n)
        return memcpy(dst, src, n);
    /* Overlapping with dst after src: copy backwards. */
    d += n;
    s += n;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) {
            *--d = *--s;
            n--;
        }
        u64 *dw = (u64 *)d;
        const u64 *sw = (const u64 *)s;
        while (n >= 8) {
            *--dw = *--sw;
            n -= 8;
        }
        d = (u8 *)dw;
        s = (const u8 *)sw;
    }
    while (n--)
        *--d = *--s;
    return dst;
}

NOBUILTIN void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;
    u8 b = (u8)c;
    while (n && ((uintptr_t)d & 7)) {
        *d++ = b;
        n--;
    }
    u64 w = b;
    w |= w << 8;
    w |= w << 16;
    w |= w << 32;
    u64 *dw = (u64 *)d;
    while (n >= 32) {
        dw[0] = w;
        dw[1] = w;
        dw[2] = w;
        dw[3] = w;
        dw += 4;
        n -= 32;
    }
    while (n >= 8) {
        *dw++ = w;
        n -= 8;
    }
    d = (u8 *)dw;
    while (n--)
        *d++ = b;
    return dst;
}

NOBUILTIN int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

NOBUILTIN void *memchr(const void *s, int c, size_t n)
{
    const u8 *p = s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (u8)c)
            return (void *)(p + i);
    }
    return NULL;
}

NOBUILTIN size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

NOBUILTIN size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

NOBUILTIN int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

NOBUILTIN int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i])
            return (int)(u8)a[i] - (int)(u8)b[i];
    }
    return 0;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((u8)*a) == tolower((u8)*b)) {
        a++;
        b++;
    }
    return tolower((u8)*a) - tolower((u8)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((u8)a[i]), cb = tolower((u8)b[i]);
        if (ca != cb || !ca)
            return ca - cb;
    }
    return 0;
}

NOBUILTIN char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

NOBUILTIN char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

NOBUILTIN char *strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (!nl)
        return (char *)h;
    for (; *h; h++) {
        if (*h == *n && strncmp(h, n, nl) == 0)
            return (char *)h;
    }
    return NULL;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dl = strnlen(dst, size);
    if (dl == size)
        return size + strlen(src);
    return dl + strlcpy(dst + dl, src, size - dl);
}

char *strdup(const char *s)
{
    return strndup(s, strlen(s));
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *p = kmalloc(len + 1);
    if (!p)
        return NULL;
    memcpy(p, s, len);
    p[len] = 0;
    return p;
}

static int digit_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    return 99;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    const char *orig = s;
    while (isspace((u8)*s))
        s++;
    if (*s == '+')
        s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x' && isxdigit((u8)s[2])) {
        s += 2;
        base = 16;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    unsigned long long v = 0;
    const char *start = s;
    for (;; s++) {
        int d = digit_value((u8)*s);
        if (d >= base)
            break;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (end)
        *end = (char *)(s == start ? orig : s);
    return v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtoull(s, end, base);
}

long strtol(const char *s, char **end, int base)
{
    while (isspace((u8)*s))
        s++;
    bool neg = false;
    if (*s == '-') {
        neg = true;
        s++;
    }
    long v = (long)strtoull(s, end, base);
    return neg ? -v : v;
}

double strtod(const char *s, char **end)
{
    const char *orig = s;
    while (isspace((u8)*s))
        s++;
    double sign = 1.0;
    if (*s == '-') {
        sign = -1.0;
        s++;
    } else if (*s == '+') {
        s++;
    }
    double v = 0.0;
    const char *start = s;
    while (isdigit((u8)*s))
        v = v * 10.0 + (*s++ - '0');
    if (*s == '.') {
        s++;
        double scale = 0.1;
        while (isdigit((u8)*s)) {
            v += (*s++ - '0') * scale;
            scale *= 0.1;
        }
    }
    if ((*s | 32) == 'e' && s != start) {
        const char *e = s + 1;
        int esign = 1, ev = 0;
        if (*e == '-') {
            esign = -1;
            e++;
        } else if (*e == '+') {
            e++;
        }
        if (isdigit((u8)*e)) {
            while (isdigit((u8)*e))
                ev = ev * 10 + (*e++ - '0');
            s = e;
            double p = 1.0;
            while (ev--)
                p *= 10.0;
            v = esign > 0 ? v * p : v / p;
        }
    }
    if (end)
        *end = (char *)(s == start ? orig : s);
    return sign * v;
}
