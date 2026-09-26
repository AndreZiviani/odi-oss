// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTL8686/RTL9602C board-level interrupt controller (ODI
 * DFP-34X-2C2), an irqdomain-based irqchip driver -- the modern idiom this
 * port uses throughout, rather than a direct irq_set_chip_and_handler() loop
 * and a hand-rolled plat_irq_dispatch().
 *
 * Two 32-bit mask/status register pairs (GIMR0/GISR0 for logical IRQ
 * 0-31, GIMR1/GISR1 for 32-63) cover all 64 board interrupt sources
 * directly, indexed by the Linux irq number with no translation. Seven
 * IRR routing registers (RTL8686_IRRn_VAL, arch/mips/include/asm/
 * mach-rtl8686/rtl8686regs.h) then route each source onto one of CPU
 * interrupt pins IP2-IP7; every one of those six pins is chained to the
 * same dispatch function here, so this driver does not need to track
 * which pin a given source is routed to.
 *
 * Simplification: bit 12 (PERIPHERAL) and bit 31 (TMO) are two-level
 * cascade summary bits for the second register bank rather than
 * independent sources. This driver reads GISR0/GISR1 masked by
 * GIMR0/GIMR1 as 64 flat, independently maskable sources (consistent with
 * how masking itself already works) and skips bits 12 and 31 as
 * self-referential summary bits with no independent consumer. This is a
 * real simplification, not re-verified against the two-level cascade
 * behaviour on hardware.
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>

#include <asm/mach-rtl8686/irq.h>
#include <asm/mach-rtl8686/rtl8686regs.h>

#include "rtl8686.h"

#ifdef CONFIG_ODI_EARLY_CRUMBS
#include <asm/mach-rtl8686/odi-early-crumb.h>
#endif

static void __iomem *rtl8686_irqregs;
static struct irq_domain *rtl8686_domain;

static inline void __iomem *gimr(unsigned int hwirq)
{
	return rtl8686_irqregs + ((hwirq < 32) ? (RTL8686_GIMR0 - RTL8686_GIMR0)
						: (RTL8686_GIMR1 - RTL8686_GIMR0));
}

static void rtl8686_irq_mask(struct irq_data *d)
{
	unsigned int hw = d->hwirq;
	void __iomem *reg = gimr(hw);
	u32 bit = 1u << (hw & 31);

	writel(readl(reg) & ~bit, reg);
}

static void rtl8686_irq_unmask(struct irq_data *d)
{
	unsigned int hw = d->hwirq;
	void __iomem *reg = gimr(hw);
	u32 bit = 1u << (hw & 31);

	writel(readl(reg) | bit, reg);
}

static struct irq_chip rtl8686_irq_chip = {
	.name		= "RTL8686-ICTL",
	.irq_ack	= rtl8686_irq_mask,
	.irq_mask	= rtl8686_irq_mask,
	.irq_unmask	= rtl8686_irq_unmask,
};

static int rtl8686_irq_map(struct irq_domain *d, unsigned int virq,
			    irq_hw_number_t hw)
{
	irq_set_chip_and_handler(virq, &rtl8686_irq_chip, handle_level_irq);
	return 0;
}

static const struct irq_domain_ops rtl8686_irq_domain_ops = {
	.map = rtl8686_irq_map,
	.xlate = irq_domain_xlate_onecell,
};

static void rtl8686_irq_dispatch(struct irq_desc *desc)
{
	struct irq_chip *host_chip = irq_desc_get_chip(desc);
	u32 pend0, pend1;
	unsigned int hw;

	chained_irq_enter(host_chip, desc);

#ifdef CONFIG_ODI_EARLY_CRUMBS
	/* Diagnostic only -- see odi-early-crumb.h. Proves the chained
	 * dispatch itself ran (any of IP2-IP7 fired), distinct from the
	 * TIMER0 ISR's own crumb (time.c): this
	 * fires for any board source, that one only for TIMER0.
	 */
	{
		static unsigned int rtl8686_dispatch_count;

		odi_early_crumb("IRQD", ++rtl8686_dispatch_count);
	}
#endif

	pend0 = readl(rtl8686_irqregs + (RTL8686_GISR0 - RTL8686_GIMR0)) &
		readl(rtl8686_irqregs + (RTL8686_GIMR0 - RTL8686_GIMR0));
	pend1 = readl(rtl8686_irqregs + (RTL8686_GISR1 - RTL8686_GIMR0)) &
		readl(rtl8686_irqregs + (RTL8686_GIMR1 - RTL8686_GIMR0));

	/* bits 12 (PERIPHERAL) and 31 (TMO) are cascade-summary bits with
	 * no independent handler -- see the file header note.
	 */
	pend0 &= ~((1u << RTL8686_IRQ_PERIPHERAL) | (1u << RTL8686_IRQ_TMO));

	for (hw = 0; hw < 32; hw++) {
		if (pend0 & (1u << hw))
			generic_handle_domain_irq(rtl8686_domain, hw);
	}
	for (hw = 0; hw < 32; hw++) {
		if (pend1 & (1u << hw))
			generic_handle_domain_irq(rtl8686_domain, hw + 32);
	}

	chained_irq_exit(host_chip, desc);
}

void __init rtl8686_irq_init(void)
{
	int cpu_irq;

	rtl8686_irqregs = ioremap(RTL8686_GIMR0, RTL8686_IRQREGS_SIZE);
	if (!rtl8686_irqregs)
		panic("rtl8686-irq: ioremap failed");

	/* disable everything, then program the fixed IP2-IP7 routing */
	writel(0, rtl8686_irqregs + (RTL8686_GIMR0 - RTL8686_GIMR0));
	writel(0, rtl8686_irqregs + (RTL8686_GIMR1 - RTL8686_GIMR0));
	writel(RTL8686_IRR0_VAL, rtl8686_irqregs + (RTL8686_IRR0 - RTL8686_GIMR0));
	writel(RTL8686_IRR1_VAL, rtl8686_irqregs + (RTL8686_IRR1 - RTL8686_GIMR0));
	writel(RTL8686_IRR2_VAL, rtl8686_irqregs + (RTL8686_IRR2 - RTL8686_GIMR0));
	writel(RTL8686_IRR3_VAL, rtl8686_irqregs + (RTL8686_IRR3 - RTL8686_GIMR0));
	writel(RTL8686_IRR4_VAL, rtl8686_irqregs + (RTL8686_IRR4 - RTL8686_GIMR0));
	writel(RTL8686_IRR5_VAL, rtl8686_irqregs + (RTL8686_IRR5 - RTL8686_GIMR0));
	writel(RTL8686_IRR6_VAL, rtl8686_irqregs + (RTL8686_IRR6 - RTL8686_GIMR0));

	/* The sources of the second bank (GIMR1/GISR1, the TIMER0 among
	 * them) reach the CPU only through the summary bits of the first
	 * bank: PERIPHERAL (bit 12) and TMO (bit 31). Their mask bits in
	 * GIMR0 must be set or no second-bank interrupt is ever signalled to
	 * the CPU (observed on the device: TIMER0 pending in GISR1, GISR0 bit
	 * 12 set, CP0 Cause with no IP bit set). The dispatch below never
	 * hands them to a handler of their own.
	 */
	writel((1u << RTL8686_IRQ_PERIPHERAL) | (1u << RTL8686_IRQ_TMO),
	       rtl8686_irqregs + (RTL8686_GIMR0 - RTL8686_GIMR0));

	rtl8686_domain = irq_domain_create_legacy(NULL, RTL8686_NR_IRQS,
						   RTL8686_IRQ_BASE, 0,
						   &rtl8686_irq_domain_ops, NULL);
	if (!rtl8686_domain)
		panic("rtl8686-irq: irq_domain_create_legacy failed");

	/* Chain onto every CPU pin this SoC can route a board source to
	 * (IP2-IP7); mips_cpu_irq_init() has already brought up
	 * MIPS_CPU_IRQ_BASE..+7 by the time arch_init_irq() calls us.
	 */
	for (cpu_irq = 2; cpu_irq <= 7; cpu_irq++) {
		irq_set_chained_handler(MIPS_CPU_IRQ_BASE + cpu_irq,
					 rtl8686_irq_dispatch);
	}
}
