/*
 * Line editor for the console: cursor movement, history, and the usual
 * Emacs-style control keys. Output uses a small ANSI subset understood by
 * both serial terminals and the framebuffer console.
 */
#include "console.h"
#include "kernel.h"
#include "string.h"

#define HISTORY 32
#define LINE_MAX 1024

static char history[HISTORY][LINE_MAX];
static int hist_count;

static void out(const char *s) { console_write(s, strlen(s)); }

static void redraw(const char *prompt, const char *buf, size_t len, size_t cur)
{
    char tmp[32];
    out("\r");
    out(prompt);
    console_write(buf, len);
    out("\x1b[K");
    if (len > cur) {
        snprintf(tmp, sizeof(tmp), "\x1b[%zuD", len - cur);
        out(tmp);
    }
}

static void history_add(const char *line)
{
    if (!line[0])
        return;
    if (hist_count && strcmp(history[(hist_count - 1) % HISTORY], line) == 0)
        return;
    strlcpy(history[hist_count % HISTORY], line, LINE_MAX);
    hist_count++;
}

int readline(const char *prompt, char *buf, size_t size)
{
    size_t len = 0, cur = 0;
    int hist_pos = hist_count;
    int esc = 0; /* 0 none, 1 got ESC, 2 got ESC [ */
    char escnum = 0;
    if (size > LINE_MAX)
        size = LINE_MAX;
    buf[0] = 0;
    out(prompt);
    console_flush();
    for (;;) {
        int c = console_getc();
        if (esc == 1) {
            esc = (c == '[' || c == 'O') ? 2 : 0;
            escnum = 0;
            continue;
        }
        if (esc == 2) {
            if (c >= '0' && c <= '9') {
                escnum = (char)c;
                continue;
            }
            esc = 0;
            if (c == 'A' || c == 'B') { /* history up / down */
                int oldest = hist_count > HISTORY ? hist_count - HISTORY : 0;
                if (c == 'A' && hist_pos > oldest)
                    hist_pos--;
                else if (c == 'B' && hist_pos < hist_count)
                    hist_pos++;
                else
                    continue;
                if (hist_pos == hist_count)
                    buf[0] = 0;
                else
                    strlcpy(buf, history[hist_pos % HISTORY], size);
                len = cur = strlen(buf);
                redraw(prompt, buf, len, cur);
            } else if (c == 'C' && cur < len) {
                cur++;
                out("\x1b[C");
            } else if (c == 'D' && cur > 0) {
                cur--;
                out("\x1b[D");
            } else if (c == 'H' || (c == '~' && (escnum == '1' || escnum == '7'))) {
                cur = 0;
                redraw(prompt, buf, len, cur);
            } else if (c == 'F' || (c == '~' && (escnum == '4' || escnum == '8'))) {
                cur = len;
                redraw(prompt, buf, len, cur);
            } else if (c == '~' && escnum == '3' && cur < len) { /* Delete */
                memmove(buf + cur, buf + cur + 1, len - cur - 1);
                len--;
                buf[len] = 0;
                redraw(prompt, buf, len, cur);
            }
            console_flush();
            continue;
        }
        switch (c) {
        case 0x1b:
            esc = 1;
            break;
        case '\r':
        case '\n':
            buf[len] = 0;
            out("\n");
            history_add(buf);
            console_flush();
            return (int)len;
        case 0x03: /* Ctrl-C */
            out("^C\n");
            buf[0] = 0;
            console_flush();
            return 0;
        case 0x04: /* Ctrl-D */
            if (len == 0) {
                out("\n");
                return -1;
            }
            break;
        case 0x7f:
        case 0x08:
            if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, len - cur);
                cur--;
                len--;
                buf[len] = 0;
                redraw(prompt, buf, len, cur);
            }
            break;
        case 0x01: /* Ctrl-A */
            cur = 0;
            redraw(prompt, buf, len, cur);
            break;
        case 0x05: /* Ctrl-E */
            cur = len;
            redraw(prompt, buf, len, cur);
            break;
        case 0x0b: /* Ctrl-K */
            len = cur;
            buf[len] = 0;
            redraw(prompt, buf, len, cur);
            break;
        case 0x15: /* Ctrl-U */
            memmove(buf, buf + cur, len - cur);
            len -= cur;
            cur = 0;
            buf[len] = 0;
            redraw(prompt, buf, len, cur);
            break;
        case 0x0c: /* Ctrl-L */
            out("\x1b[2J\x1b[H");
            redraw(prompt, buf, len, cur);
            break;
        case '\t':
            c = ' ';
            /* fall through */
        default:
            if (c >= 0x20 && c != 0x7f && len + 1 < size) {
                memmove(buf + cur + 1, buf + cur, len - cur);
                buf[cur] = (char)c;
                len++;
                cur++;
                buf[len] = 0;
                if (cur == len) {
                    char ch = (char)c;
                    console_write(&ch, 1);
                } else {
                    redraw(prompt, buf, len, cur);
                }
            }
            break;
        }
        console_flush();
    }
}
