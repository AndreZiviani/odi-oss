/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_dlog.h -- log lines held back while odi_gpon_lock is taken in
 * the interrupt, printed after it is released.
 *
 * A console line costs milliseconds (the console is a UART); printed from
 * inside the interrupt, with interrupts off, it delays every PLOAM message
 * queued behind it, and the OLT's timers (Assign_ONU-ID answered within
 * its window, TO1) do not wait. The line is formatted into this buffer
 * under the lock, which takes microseconds, and the interrupt handler
 * copies the buffer out, drops the lock and prints. Nothing is lost on the
 * way: a line past the end is counted and the count printed as a line of
 * its own.
 *
 * Plain C so the host tests build it: odi_gpon.c is the only kernel user.
 */
#ifndef ODI_GPON_DLOG_H
#define ODI_GPON_DLOG_H

#ifdef __KERNEL__
#include <linux/stdarg.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#define ODI_DLOG_VSNPRINTF vscnprintf
#else
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ODI_DLOG_VSNPRINTF vsnprintf
#endif

/* A drain of 16 messages can log a transition per message and an alarm
 * edge or two; the lines are rate-limited, so this is generous. */
#define ODI_GPON_DLOG_LINES	8U
#define ODI_GPON_DLOG_LEN	192U

enum odi_gpon_dlog_level {
	ODI_GPON_DLOG_INFO,
	ODI_GPON_DLOG_NOTICE,
};

struct odi_gpon_dlog {
	struct {
		uint8_t level;
		char text[ODI_GPON_DLOG_LEN];
	} line[ODI_GPON_DLOG_LINES];
	unsigned int n;
	unsigned int lost;
	int active;	/* the interrupt holds the lock and will flush */
};

/* Format one line; returns 1 when it was kept, 0 when the buffer was full. */
static inline int odi_gpon_dlog_vadd(struct odi_gpon_dlog *d, enum odi_gpon_dlog_level level,
				     const char *fmt, va_list ap)
{
	if (d->n >= ODI_GPON_DLOG_LINES) {
		d->lost++;
		return 0;
	}
	d->line[d->n].level = (uint8_t)level;
	ODI_DLOG_VSNPRINTF(d->line[d->n].text, ODI_GPON_DLOG_LEN, fmt, ap);
	d->n++;
	return 1;
}

/* Move the held lines into *out and empty *d. *out is the caller's own
 * (the stack), so it can be printed with the lock released. The flag
 * `active` is not copied: it is the lock holder's.
 */
static inline void odi_gpon_dlog_take(struct odi_gpon_dlog *d, struct odi_gpon_dlog *out)
{
	unsigned int i;

	out->n = d->n;
	out->lost = d->lost;
	out->active = 0;
	for (i = 0; i < d->n; i++) {
		out->line[i].level = d->line[i].level;
		memcpy(out->line[i].text, d->line[i].text, ODI_GPON_DLOG_LEN);
	}
	d->n = 0;
	d->lost = 0;
}

#endif /* ODI_GPON_DLOG_H */
