/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_compat.h -- this tree builds against exactly one kernel API shape
 * (mainline 6.18, CONFIG_MACH_RTL8686), so every odi_*.c file uses the
 * plain 6.18 APIs directly (struct proc_ops, ioremap(), timer_setup(),
 * strscpy(), netif_napi_add_weight(), eth_hw_addr_set(),
 * timer_delete[_sync](), <linux/unaligned.h>, <linux/panic_notifier.h>).
 * What is left here is the one thing that is not a plain kernel API: the
 * Linux irq numbers for the two board interrupt lines this driver set
 * requests, derived from the RTL8686_IRQ_BASE virq offset
 * (asm/mach-rtl8686/irq.h) plus the hardware bit position of each line
 * (asm/mach-rtl8686/rtl8686regs.h). Every ODI_* Kconfig symbol depends on
 * MACH_RTL8686 (odi Kconfig, top of the file), so this is never built for
 * another board.
 *
 * __KERNEL__ only: every odi_*.c file keeps its host-test half free of
 * kernel headers entirely (see odi_wdt.c's own file header for the
 * three-tier split this repeats), so nothing here needs a host-side
 * definition.
 */
#ifndef ODI_COMPAT_H
#define ODI_COMPAT_H

#ifdef __KERNEL__

#include <asm/mach-rtl8686/irq.h>
#include <asm/mach-rtl8686/rtl8686regs.h>

/* apl_sw (switch core) interrupt: odi_intr.c's demux for hardware bit
 * RTL8686_IRQ_SWITCH.
 */
#define ODI_INTR_IRQ_NUM	(RTL8686_IRQ_BASE + RTL8686_IRQ_SWITCH)

/* GMAC (odi_nic) interrupt: hardware bit ODI_NIC_HWIRQ (odi_nic_hw.h, 26).
 * ISP1 trial 618w requested the bare 26 with no RTL8686_IRQ_BASE offset,
 * which is hardware bit 18 on this controller, and the NIC never saw one
 * interrupt.
 */
#define ODI_NIC_IRQ_NUM		(RTL8686_IRQ_BASE + ODI_NIC_HWIRQ)

#endif /* __KERNEL__ */
#endif /* ODI_COMPAT_H */
