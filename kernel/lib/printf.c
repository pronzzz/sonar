/*
 * vsnprintf: the kernel's formatted output engine.
 * Supports flags (-, 0, +, space, #), width and precision (incl. '*'),
 * length modifiers (hh, h, l, ll, z, t, j) and conversions
 * d i u x X o c s p f %.
 */
#include "kernel.h"
#include "string.h"

struct outbuf {
    char *buf;
    size_t size;
    size_t pos;
};

static void out_char(struct outbuf *o, char c)
{
    if (o->pos + 1 < o->size)
        o->buf[o->pos] = c;
    o->pos++;
}

static void out_pad(struct outbuf *o, char c, int n)
{
    while (n-- > 0)
        out_char(o, c);
}

enum { F_LEFT = 1, F_ZERO = 2, F_PLUS = 4, F_SPACE = 8, F_ALT = 16, F_UPPER = 32 };

static void out_number(struct outbuf *o, unsigned long long v, bool neg, int base,
                       int width, int prec, int flags)
{
    char tmp[32];
    int len = 0;
    const char *digits = (flags & F_UPPER) ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0 && prec != 0)
        tmp[len++] = '0';
    while (v) {
        tmp[len++] = digits[v % (unsigned)base];
        v /= (unsigned)base;
    }
    char sign = 0;
    if (neg)
        sign = '-';
    else if (flags & F_PLUS)
        sign = '+';
    else if (flags & F_SPACE)
        sign = ' ';
    const char *prefix = "";
    if ((flags & F_ALT) && base == 16)
        prefix = (flags & F_UPPER) ? "0X" : "0x";
    else if ((flags & F_ALT) && base == 8)
        prefix = "0";
    int plen = (int)strlen(prefix);
    int zeros = prec > len ? prec - len : 0;
    int total = len + zeros + plen + (sign ? 1 : 0);
    if (prec < 0 && (flags & F_ZERO) && !(flags & F_LEFT) && width > total) {
        zeros += width - total;
        total = width;
    }
    if (!(flags & F_LEFT))
        out_pad(o, ' ', width - total);
    if (sign)
        out_char(o, sign);
    for (int i = 0; i < plen; i++)
        out_char(o, prefix[i]);
    out_pad(o, '0', zeros);
    while (len)
        out_char(o, tmp[--len]);
    if (flags & F_LEFT)
        out_pad(o, ' ', width - total);
}

static void out_string(struct outbuf *o, const char *s, int width, int prec, int flags)
{
    if (!s)
        s = "(null)";
    int len = (int)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
    if (!(flags & F_LEFT))
        out_pad(o, ' ', width - len);
    for (int i = 0; i < len; i++)
        out_char(o, s[i]);
    if (flags & F_LEFT)
        out_pad(o, ' ', width - len);
}

static void out_double(struct outbuf *o, double v, int width, int prec, int flags)
{
    char tmp[64];
    int n = 0;
    if (prec < 0)
        prec = 6;
    if (prec > 9)
        prec = 9;
    bool neg = v < 0;
    if (neg)
        v = -v;
    if (v != v) {
        out_string(o, "nan", width, -1, flags);
        return;
    }
    if (v > 1.8e19) {
        out_string(o, neg ? "-inf" : "inf", width, -1, flags);
        return;
    }
    unsigned long long scale = 1;
    for (int i = 0; i < prec; i++)
        scale *= 10;
    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;
    unsigned long long fp = (unsigned long long)(frac * (double)scale + 0.5);
    if (fp >= scale) {
        ip++;
        fp -= scale;
    }
    /* fractional digits, reversed */
    for (int i = 0; i < prec; i++) {
        tmp[n++] = (char)('0' + fp % 10);
        fp /= 10;
    }
    if (prec > 0 || (flags & F_ALT))
        tmp[n++] = '.';
    if (ip == 0)
        tmp[n++] = '0';
    while (ip) {
        tmp[n++] = (char)('0' + ip % 10);
        ip /= 10;
    }
    char sign = neg ? '-' : (flags & F_PLUS) ? '+' : (flags & F_SPACE) ? ' ' : 0;
    int total = n + (sign ? 1 : 0);
    if (!(flags & F_LEFT) && !(flags & F_ZERO))
        out_pad(o, ' ', width - total);
    if (sign)
        out_char(o, sign);
    if (!(flags & F_LEFT) && (flags & F_ZERO))
        out_pad(o, '0', width - total);
    while (n)
        out_char(o, tmp[--n]);
    if (flags & F_LEFT)
        out_pad(o, ' ', width - total);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct outbuf o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_char(&o, *fmt);
            continue;
        }
        fmt++;
        int flags = 0;
        for (;; fmt++) {
            if (*fmt == '-')
                flags |= F_LEFT;
            else if (*fmt == '0')
                flags |= F_ZERO;
            else if (*fmt == '+')
                flags |= F_PLUS;
            else if (*fmt == ' ')
                flags |= F_SPACE;
            else if (*fmt == '#')
                flags |= F_ALT;
            else
                break;
        }
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= F_LEFT;
                width = -width;
            }
            fmt++;
        } else {
            while (isdigit((u8)*fmt))
                width = width * 10 + (*fmt++ - '0');
        }
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (isdigit((u8)*fmt))
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        int lng = 0; /* 0=int, 1=long, 2=long long, -1=short, -2=char */
        for (;; fmt++) {
            if (*fmt == 'l')
                lng = lng >= 1 ? 2 : 1;
            else if (*fmt == 'h')
                lng = lng <= -1 ? -2 : -1;
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j')
                lng = 2;
            else
                break;
        }
        char c = *fmt;
        if (!c)
            break;
        switch (c) {
        case 'd':
        case 'i': {
            long long v;
            if (lng == 2)
                v = va_arg(ap, long long);
            else if (lng == 1)
                v = va_arg(ap, long);
            else
                v = va_arg(ap, int);
            if (lng == -1)
                v = (short)v;
            else if (lng == -2)
                v = (signed char)v;
            bool neg = v < 0;
            unsigned long long u = neg ? 0ULL - (unsigned long long)v : (unsigned long long)v;
            out_number(&o, u, neg, 10, width, prec, flags);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long v;
            if (lng == 2)
                v = va_arg(ap, unsigned long long);
            else if (lng == 1)
                v = va_arg(ap, unsigned long);
            else
                v = va_arg(ap, unsigned int);
            if (lng == -1)
                v = (unsigned short)v;
            else if (lng == -2)
                v = (unsigned char)v;
            int base = c == 'u' ? 10 : c == 'o' ? 8 : 16;
            if (c == 'X')
                flags |= F_UPPER;
            out_number(&o, v, false, base, width, prec, flags);
            break;
        }
        case 'p':
            out_number(&o, (uintptr_t)va_arg(ap, void *), false, 16, width, prec,
                       flags | F_ALT);
            break;
        case 'c':
            if (!(flags & F_LEFT))
                out_pad(&o, ' ', width - 1);
            out_char(&o, (char)va_arg(ap, int));
            if (flags & F_LEFT)
                out_pad(&o, ' ', width - 1);
            break;
        case 's':
            out_string(&o, va_arg(ap, const char *), width, prec, flags);
            break;
        case 'f':
        case 'F':
        case 'g':
            out_double(&o, va_arg(ap, double), width, prec, flags);
            break;
        case '%':
            out_char(&o, '%');
            break;
        default:
            out_char(&o, '%');
            out_char(&o, c);
            break;
        }
    }
    if (size)
        buf[o.pos < size ? o.pos : size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
