/* Console: output sinks (serial, framebuffer) and a shared input queue. */
#pragma once

#include "types.h"

struct console_sink {
    const char *name;
    void (*write)(const char *s, size_t len);
    void (*flush)(void);
};

void console_register(struct console_sink *s);
void console_flush(void);
void console_redirect(void (*fn)(const char *s, size_t len));
bool console_redirected(void); /* output going to a file: no colors */
void console_input(const char *s, size_t len);
/* Copy the most recent console output (up to 64 KiB, oldest first). */
size_t console_log_read(char *dst, size_t size);
int console_getc(void);            /* blocking; runs idle hooks while waiting */
int console_getc_nonblock(void);   /* -1 when empty */
bool console_interrupted(void);    /* Ctrl-C pending? (consumes it) */
void console_add_idle_hook(void (*fn)(void));
/* Hold the output lock (interrupts off) to change what the sinks draw on. */
u64 console_lock(void);
void console_unlock(u64 flags);
size_t console_log_copy_locked(char *dst, size_t size);
void console_run_idle(void);
bool console_waiting_input(void);  /* inside console_getc's idle loop */

/* Line editor with history (kernel/shell/readline.c). Returns length, or -1
 * on Ctrl-D at an empty line. */
int readline(const char *prompt, char *buf, size_t size);

/* Serial port */
void pl011_init(void);
void pl011_init_irq(void);
