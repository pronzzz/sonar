/* Interrupt controller abstraction (GICv2 / GICv3). */
#pragma once

#include "types.h"

typedef void (*irq_handler_t)(int irq, void *ctx);

void gic_init(void);
void gic_init_cpu(void);         /* per-CPU interface setup (all CPUs) */
const char *gic_name(void);
void irq_register(int irq, irq_handler_t handler, void *ctx);
void irq_unmask(int irq);
void irq_mask(int irq);
void irq_set_edge(int irq, bool edge);
void gic_send_sgi(int sgi, u64 target_mpidr);
void gic_ack_pending(void); /* ack + EOI all pending, no handlers */
u64 irq_count(int irq);

/* Timer (ARM generic timer, virtual counter). */
void timer_init(void);
void timer_init_cpu(void);
u64 time_ns(void);          /* monotonic since boot */
u64 time_ms(void);
u64 time_ticks(void);       /* scheduler ticks since boot */
void delay_us(u64 us);
void delay_ms(u64 ms);
#define TIMER_HZ 100

/* Wall clock (PL031 RTC). Returns seconds since the Unix epoch, 0 if none. */
void rtc_init(void);
u64 rtc_epoch(void);
