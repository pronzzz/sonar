/*
 * Kernel console: fans output out to the serial port and the framebuffer
 * console, and collects input from the serial port and keyboards into one
 * input queue.
 */
#include "arch.h"
#include "console.h"
#include "kernel.h"
#include "smp.h"
#include "string.h"

#define MAX_SINKS 4
static struct console_sink *sinks[MAX_SINKS];
static int nsinks;
static spinlock_t out_lock = SPINLOCK_INIT;

#define INBUF 1024
static volatile u8 inbuf[INBUF];
static volatile u32 in_head, in_tail;
static spinlock_t in_lock = SPINLOCK_INIT;

/* Everything shown on the console, kept so a display that appears late in
 * boot can show the messages before it, and for `dmesg`. */
#define LOGBUF (64 * 1024)
static char logbuf[LOGBUF];
static u64 log_total; /* bytes ever logged */

static void log_append(const char *s, size_t len)
{
    if (len >= LOGBUF) {
        s += len - LOGBUF;
        log_total += len - LOGBUF;
        len = LOGBUF;
    }
    size_t at = log_total % LOGBUF, first = MIN(len, LOGBUF - at);
    memcpy(logbuf + at, s, first);
    memcpy(logbuf, s + first, len - first);
    log_total += len;
}

/* The last min(size, 64 KiB) bytes of the log, oldest first. Caller holds
 * out_lock or accepts a racy copy. */
static size_t log_copy(char *dst, size_t size)
{
    size_t have = log_total < LOGBUF ? (size_t)log_total : LOGBUF, n = MIN(have, size);
    u64 start = log_total - n;
    size_t at = start % LOGBUF, first = MIN(n, LOGBUF - at);
    memcpy(dst, logbuf + at, first);
    memcpy(dst + first, logbuf, n - first);
    return n;
}

size_t console_log_read(char *dst, size_t size)
{
    u64 f = spin_lock_irqsave(&out_lock);
    size_t n = log_copy(dst, size);
    spin_unlock_irqrestore(&out_lock, f);
    return n;
}

u64 console_lock(void) { return spin_lock_irqsave(&out_lock); }
void console_unlock(u64 flags) { spin_unlock_irqrestore(&out_lock, flags); }
size_t console_log_copy_locked(char *dst, size_t size) { return log_copy(dst, size); }

void console_register(struct console_sink *s)
{
    u64 f = spin_lock_irqsave(&out_lock);
    if (nsinks < MAX_SINKS) {
        /* replay what was printed before this sink existed */
        size_t have = log_total < LOGBUF ? (size_t)log_total : LOGBUF;
        u64 start = log_total - have;
        size_t at = start % LOGBUF, first = MIN(have, LOGBUF - at);
        if (first)
            s->write(logbuf + at, first);
        if (have > first)
            s->write(logbuf, have - first);
        sinks[nsinks++] = s;
    }
    spin_unlock_irqrestore(&out_lock, f);
}

static void (*redirect)(const char *s, size_t len);

/* Send all output to fn instead of the sinks (NULL restores). Used for
 * shell redirection ("cmd > file"). */
void console_redirect(void (*fn)(const char *s, size_t len)) { redirect = fn; }
bool console_redirected(void) { return redirect != NULL; }

void console_write(const char *s, size_t len)
{
    u64 f = spin_lock_irqsave(&out_lock);
    if (redirect) {
        redirect(s, len);
    } else {
        log_append(s, len);
        for (int i = 0; i < nsinks; i++)
            sinks[i]->write(s, len);
    }
    spin_unlock_irqrestore(&out_lock, f);
}

void console_puts(const char *s) { console_write(s, strlen(s)); }

void console_flush(void)
{
    u64 f = spin_lock_irqsave(&out_lock);
    for (int i = 0; i < nsinks; i++)
        if (sinks[i]->flush)
            sinks[i]->flush();
    spin_unlock_irqrestore(&out_lock, f);
}

int vprintk(const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    console_write(buf, len);
    return n;
}

int printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprintk(fmt, ap);
    va_end(ap);
    return n;
}

void panic(const char *fmt, ...)
{
    irq_disable();
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* Avoid deadlocking on the output lock if we panicked while holding it. */
    out_lock.locked = 0;
    redirect = NULL;
    printk("\n\n*** KERNEL PANIC: %s\n", buf);
    printk("System halted.\n");
    console_flush();
    for (;;)
        wfi();
}

/* Called from interrupt handlers (serial RX, keyboard). */
void console_input(const char *s, size_t len)
{
    u64 f = spin_lock_irqsave(&in_lock);
    for (size_t i = 0; i < len; i++) {
        u32 next = (in_head + 1) % INBUF;
        if (next == in_tail)
            break; /* full: drop */
        inbuf[in_head] = (u8)s[i];
        in_head = next;
    }
    spin_unlock_irqrestore(&in_lock, f);
}

int console_getc_nonblock(void)
{
    int c = -1;
    u64 f = spin_lock_irqsave(&in_lock);
    if (in_tail != in_head) {
        c = inbuf[in_tail];
        in_tail = (in_tail + 1) % INBUF;
    }
    spin_unlock_irqrestore(&in_lock, f);
    return c;
}

/* True if Ctrl-C is waiting in the input queue. Input up to and including
 * the last Ctrl-C is discarded (it was typed to interrupt, not as text). */
bool console_interrupted(void)
{
    bool hit = false;
    u64 f = spin_lock_irqsave(&in_lock);
    for (u32 i = in_tail; i != in_head; i = (i + 1) % INBUF) {
        if (inbuf[i] == 0x03) {
            hit = true;
            in_tail = (i + 1) % INBUF;
        }
    }
    spin_unlock_irqrestore(&in_lock, f);
    return hit;
}

static void (*idle_hooks[8])(void);
static int nidle;

void console_add_idle_hook(void (*fn)(void))
{
    if (nidle < 8)
        idle_hooks[nidle++] = fn;
}

void console_run_idle(void)
{
    for (int i = 0; i < nidle; i++)
        idle_hooks[i]();
}

/* True while the shell (or a prompt) is blocked waiting for a key: no
 * command is in the middle of using the file system. */
static volatile bool waiting_input;
bool console_waiting_input(void) { return waiting_input; }

int console_getc(void)
{
    for (;;) {
        int c = console_getc_nonblock();
        if (c >= 0)
            return c;
        waiting_input = true;
        console_run_idle();
        waiting_input = false;
        console_flush();
        /* Sleep until the next interrupt (input, timer tick, network). */
        u64 f = irq_save();
        if (in_tail == in_head)
            wfi();
        irq_restore(f);
    }
}
