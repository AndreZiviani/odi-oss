/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kstub.h -- just enough of the kernel API for kernel/extra/arch/mips/
 * rtl8686/time.c to compile on the host, for rtl8686_time_test.c. The
 * register model lives in the test; this declares the hooks.
 */
#ifndef KSTUB_H
#define KSTUB_H
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

typedef uint32_t u32;
typedef uint64_t u64;
#define __iomem
#define __init
#define notrace
#define asmlinkage
#define HZ 250
#define BIT(n) (1u << (n))
#define GENMASK(h, l) ((~0u >> (31 - (h))) & (~0u << (l)))
#define CLOCKSOURCE_MASK(b) ((1ull << (b)) - 1)
#define CLOCK_SOURCE_IS_CONTINUOUS 1
#define RTL8686_IRQ_BASE 8
#define IRQF_TIMER 1
#define IRQF_PERCPU 2
#define IRQ_HANDLED 1
typedef int irqreturn_t;
#define cpumask_of(c) ((void *)0)

#define CLOCK_EVT_FEAT_PERIODIC 1
#define CLOCK_EVT_FEAT_ONESHOT 2

struct clock_event_device {
	const char *name;
	unsigned int features;
	int rating;
	int irq;
	void *cpumask;
	int (*set_state_periodic)(struct clock_event_device *);
	int (*set_state_oneshot)(struct clock_event_device *);
	int (*set_state_oneshot_stopped)(struct clock_event_device *);
	int (*set_state_shutdown)(struct clock_event_device *);
	int (*set_next_event)(unsigned long, struct clock_event_device *);
	void (*event_handler)(struct clock_event_device *);
};
struct clocksource {
	const char *name;
	int rating;
	u64 (*read)(struct clocksource *);
	u64 mask;
	unsigned long flags;
};

/* register access and kernel calls: provided by the test */
u32 readl(const void *addr);
void writel(u32 v, void *addr);
void *ioremap(unsigned long phys, unsigned long size);
void iounmap(void *p);
void clockevents_register_device(struct clock_event_device *d);
void clockevents_config_and_register(struct clock_event_device *d, u32 freq,
				     unsigned long min, unsigned long max);
int clocksource_register_hz(struct clocksource *cs, u32 hz);
void sched_clock_register(u64 (*rd)(void), int bits, unsigned long rate);
int request_irq(unsigned int irq, irqreturn_t (*h)(int, void *), unsigned long fl,
		const char *name, void *dev);
void panic(const char *fmt, ...);
void ktest_log(const char *level, const char *fmt, ...);
#define pr_info(...) ktest_log("info", __VA_ARGS__)
#define pr_warn(...) ktest_log("warn", __VA_ARGS__)
static inline u64 div_u64(u64 a, u32 b) { return a / b; }
extern unsigned int rtl8686_lx_hz;
#endif
