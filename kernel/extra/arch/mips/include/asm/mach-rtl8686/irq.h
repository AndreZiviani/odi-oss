/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ODI DFP-34X-2C2 (Realtek RTL9602C / RTL8686) IRQ numbering.
 *
 * MIPS_CPU_IRQ_BASE..+7: the 8 CPU-level interrupt pins, handled by the
 * generic drivers/irqchip/irq-mips-cpu.c (CONFIG_IRQ_MIPS_CPU, selected by
 * MACH_RTL8686) via mips_cpu_irq_init() in rtl8686/board.c.
 *
 * RTL8686_IRQ_BASE..+63: the board-level GIMR0/GIMR1 interrupt controller
 * (arch/mips/rtl8686/irq.c), chained off CPU pins IP2-IP7 per the
 * routing table programmed into IRR0..IRR6 (RTL8686_IRRn_VAL).
 */
#ifndef __ASM_MACH_RTL8686_IRQ_H
#define __ASM_MACH_RTL8686_IRQ_H

#define MIPS_CPU_IRQ_BASE	0

#define RTL8686_IRQ_BASE	(MIPS_CPU_IRQ_BASE + 8)
#define RTL8686_NR_IRQS	64

#define NR_IRQS		(RTL8686_IRQ_BASE + RTL8686_NR_IRQS)

#endif /* __ASM_MACH_RTL8686_IRQ_H */
