/* SPDX-License-Identifier: GPL-2.0 */
/*
 * One-shot dump of the interrupt path state, for trial images that stop
 * before the first timer tick. Called through late_time_init (set by
 * plat_time_init() in arch/mips/rtl8686/board.c), which start_kernel()
 * runs right before calibrate_delay(), with interrupts already enabled. All reads go
 * through fixed KSEG0/KSEG1 addresses so nothing here depends on ioremap
 * or on any driver state.
 */
#ifndef _ASM_MACH_RTL8686_ODI_IRQ_DEBUG_H
#define _ASM_MACH_RTL8686_ODI_IRQ_DEBUG_H

#include <linux/compiler.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <asm/mipsregs.h>
#include <asm/barrier.h>
#include <asm/mach-rtl8686/rtl8686-barrier.h>

#define ODI_DBG_RD(addr)	(*(volatile unsigned int *)(unsigned long)(addr))

/* Interrupt controller (GIMR, GISR, IRR) and timer TIMER0, KSEG1 addresses. */
#define ODI_DBG_GIMR0		0xb8003000u
#define ODI_DBG_GIMR1		0xb8003004u
#define ODI_DBG_GISR0		0xb8003008u
#define ODI_DBG_GISR1		0xb800300cu
#define ODI_DBG_IRR4		0xb8003020u
#define ODI_DBG_TIMER0_PERIOD		0xb8003200u
#define ODI_DBG_TIMER0_COUNT		0xb8003204u
#define ODI_DBG_TIMER0_CTRL		0xb8003208u
#define ODI_DBG_TIMER0_IRQ		0xb800320cu

static inline void odi_irq_debug_vec(const char *name, unsigned long cached)
{
	unsigned long uncached = cached + 0x20000000UL;

	pr_info("odi_irqdbg: %s uncached %08x %08x %08x %08x cached %08x %08x %08x %08x\n",
		name,
		ODI_DBG_RD(uncached), ODI_DBG_RD(uncached + 4),
		ODI_DBG_RD(uncached + 8), ODI_DBG_RD(uncached + 12),
		ODI_DBG_RD(cached), ODI_DBG_RD(cached + 4),
		ODI_DBG_RD(cached + 8), ODI_DBG_RD(cached + 12));
}

static inline void odi_irq_debug_dump(void)
{
	unsigned int i, j;

	pr_info("odi_irqdbg: status %08x cause %08x jiffies %lu\n",
		read_c0_status(), read_c0_cause(), jiffies);
	pr_info("odi_irqdbg: gimr0 %08x gimr1 %08x gisr0 %08x gisr1 %08x irr4 %08x\n",
		ODI_DBG_RD(ODI_DBG_GIMR0), ODI_DBG_RD(ODI_DBG_GIMR1),
		ODI_DBG_RD(ODI_DBG_GISR0), ODI_DBG_RD(ODI_DBG_GISR1),
		ODI_DBG_RD(ODI_DBG_IRR4));
	pr_info("odi_irqdbg: timer0 period %08x count %08x ctrl %08x irq %08x\n",
		ODI_DBG_RD(ODI_DBG_TIMER0_PERIOD), ODI_DBG_RD(ODI_DBG_TIMER0_COUNT),
		ODI_DBG_RD(ODI_DBG_TIMER0_CTRL), ODI_DBG_RD(ODI_DBG_TIMER0_IRQ));
	rtl8686_sync();
	pr_info("odi_irqdbg: early sync ok\n");
	__fast_iob();
	pr_info("odi_irqdbg: early uncached load ok\n");
	odi_irq_debug_vec("vec80", 0x80000080UL);
	odi_irq_debug_vec("vec00", 0x80000000UL);

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 2000000; j++)
			barrier();
		pr_info("odi_irqdbg: t%u cnt %08x int %08x gisr1 %08x status %08x cause %08x jiffies %lu\n",
			i, ODI_DBG_RD(ODI_DBG_TIMER0_COUNT), ODI_DBG_RD(ODI_DBG_TIMER0_IRQ),
			ODI_DBG_RD(ODI_DBG_GISR1), read_c0_status(),
			read_c0_cause(), jiffies);
	}
}

#endif /* _ASM_MACH_RTL8686_ODI_IRQ_DEBUG_H */
