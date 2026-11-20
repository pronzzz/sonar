/*
 * Platform layer for the AI engine. The engine (gguf, quant, tokenizer,
 * model, sampler, chat) is plain C that builds both inside the Sonar
 * kernel (SONAR_KERNEL) and on a development host for testing.
 */
#pragma once

#ifdef SONAR_KERNEL
#include "kernel.h"
#include "string.h"
#else
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
#endif

void *ai_alloc(size_t n);
void *ai_calloc(size_t n, size_t size);
void *ai_realloc(void *p, size_t n);
void ai_free(void *p);
void *ai_alloc_large(size_t n); /* page-granular, contents undefined */
void ai_free_large(void *p, size_t n);
/* false if a disk read cannot go straight into this large buffer */
bool ai_large_dma_ok(const void *p);
void ai_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

typedef void (*ai_par_fn)(void *ctx, int start, int end);
void ai_parallel_for(int n, int grain, ai_par_fn fn, void *ctx);
int ai_num_threads(void);
u64 ai_time_ns(void);
bool ai_should_abort(void); /* user pressed Ctrl-C */
bool ai_cpu_has_dotprod(void); /* ARMv8.2 SDOT/UDOT available */

/* Random-access reader for a model file (or a disk holding one). */
struct ai_file {
    void *ctx;
    u64 size;
    /* Read len bytes at off into buf; false on I/O error. */
    bool (*read)(void *ctx, u64 off, void *buf, u64 len);
    /* Optional: whole file already in memory (e.g. host mmap). */
    const u8 *mapped;
};

/* Progress callback for long operations: done/total in bytes. */
typedef void (*ai_progress_fn)(void *ctx, u64 done, u64 total);
