// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTL8686/RTL9602C system timer (TIMER0), ODI DFP-34X-2C2.
 *
 * Clockevents driver for TIMER0 (modern idiom: clockevents, not an
 * irqaction/setup_irq pair). The hardware itself is not a general-purpose
 * comparator: TIMER0 is programmed once, here, for a fixed HZ-periodic
 * interrupt, with set_next_event() left unimplemented -- periodic-only is
 * what this SoC offers from Linux.
 *
 * No clocksource is registered: the RLX5281 has no CP0 Count/Compare, and
 * this SoC has no other free-running counter usable as a clocksource.
 * Timekeeping falls back to the jiffies clocksource built into the kernel --
 * coarse (~1/HZ resolution).
 *
 * The TIMER0_CTRL/TIMER0_PERIOD/TIMER0_IRQ layout used here is the RTL9602C one; other
 * chips of the same family place their timer registers differently and
 * are not supported.
 *
 * TIMER0_PERIOD scale: REG32(TIMER0_PERIOD) = MHz * (DIVISOR / HZ) -- MHz is the CPU
 * clock in MHz (not Hz), and DIVISOR/HZ (1000/HZ, integral for every HZ
 * this port uses) is computed before the multiply. Computing
 * (cpu_hz / HZ) * divisor with cpu_hz in raw Hz overflows u32 by six orders
 * of magnitude (e.g. HZ=250, 180 MHz: 720,000,000,000, silently wrapped to
 * a nonsense count) -- TIMER0 never reaches that count, so no timer interrupt
 * arrives and calibrate_delay() hangs waiting on jiffies that never
 * advance. Keep the MHz-first, divisor/HZ-before-multiply grouping exactly.
 */
#include <linux/clockchips.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/sched_clock.h>

#include <asm/mach-rtl8686/rtl8686regs.h>
#include <asm/time.h>

#include "rtl8686.h"

#ifdef CONFIG_ODI_EARLY_CRUMBS
#include <asm/mach-rtl8686/odi-early-crumb.h>
#endif

static void __iomem *rtl8686_timer0;

static int rtl8686_set_state_periodic(struct clock_event_device *evt)
{
	u32 intr = readl(rtl8686_timer0 + (RTL8686_TIMER0_IRQ - RTL8686_TIMER0_PERIOD));

	/* Write the whole control word: enable, timer mode and the prescaler
	 * in the low bits. The clockevent core calls the shutdown callback
	 * when the device registers, so a read-modify-write here finds the
	 * prescaler already cleared and the counter never advances (observed
	 * on the device: TIMER0_CTRL reads 0x11000000, TIMER0_COUNT stuck at 0, where a
	 * running timer holds 0x110003e8).
	 */
	writel(RTL8686_TIMER_EN | RTL8686_TIMER_PERIODIC | RTL8686_TIMER_PRESCALE,
	       rtl8686_timer0 + (RTL8686_TIMER0_CTRL - RTL8686_TIMER0_PERIOD));
	/* OR the enable bit into the register instead of overwriting it --
	 * see the read-modify-write note on the ack in
	 * rtl8686_timer_interrupt() below, same register, same reason.
	 */
	writel(intr | RTL8686_TIMER_IRQ_EN, rtl8686_timer0 + (RTL8686_TIMER0_IRQ - RTL8686_TIMER0_PERIOD));
	return 0;
}

static int rtl8686_set_state_shutdown(struct clock_event_device *evt)
{
	u32 ctl = readl(rtl8686_timer0 + (RTL8686_TIMER0_CTRL - RTL8686_TIMER0_PERIOD));

	/* Stop the counter only; keep the prescaler for the next start. */
	writel(ctl & ~RTL8686_TIMER_EN, rtl8686_timer0 + (RTL8686_TIMER0_CTRL - RTL8686_TIMER0_PERIOD));
	return 0;
}

static struct clock_event_device rtl8686_clockevent = {
	.name		= "rtl8686-timer0",
	.features	= CLOCK_EVT_FEAT_PERIODIC,
	.rating		= 100,
	.set_state_periodic	= rtl8686_set_state_periodic,
	.set_state_shutdown	= rtl8686_set_state_shutdown,
	.set_state_oneshot	= NULL, /* hardware cannot do it -- see file header */
};

static irqreturn_t rtl8686_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *cd = &rtl8686_clockevent;
	u32 intr = readl(rtl8686_timer0 + (RTL8686_TIMER0_IRQ - RTL8686_TIMER0_PERIOD));

	/*
	 * TCIE (enable, bit 20) and TCIP (pending, bit 16) live in the same
	 * register. Overwriting it with TCIP alone, as this ack used to,
	 * also writes 0 into the TCIE bit position -- silently disabling
	 * the timer interrupt right after the first tick it ever delivers.
	 * OR the ack bit in instead, so TCIE survives.
	 */
	writel(intr | RTL8686_TIMER_IRQ_PEND, rtl8686_timer0 + (RTL8686_TIMER0_IRQ - RTL8686_TIMER0_PERIOD));

#ifdef CONFIG_ODI_EARLY_CRUMBS
	/* Diagnostic only -- see odi-early-crumb.h. Proves the TIMER0 ISR
	 * itself ran, distinct from the irqchip dispatch crumb (irq.c)
	 * which only proves *some* board source reached the chained
	 * handler. Same page B +8/+12 slot as
	 * every other crumb, not a separate counter word: whichever of
	 * IRQE/IRQD/TICK ran last is what a revert reads back.
	 */
	{
		static unsigned int rtl8686_tick_count;

		odi_early_crumb("TICK", ++rtl8686_tick_count);
	}
#endif

	cd->event_handler(cd);
	return IRQ_HANDLED;
}

void __init rtl8686_clockevent_init(void)
{
	unsigned int cpu_hz = rtl8686_cpu_hz;
	unsigned int divisor = RTL8686_TIMER_PRESCALE;
	unsigned int irq = RTL8686_IRQ_BASE + RTL8686_IRQ_TIMER0;
	int ret;

	rtl8686_timer0 = ioremap(RTL8686_TIMER0_PERIOD, RTL8686_TIMER0_IRQ - RTL8686_TIMER0_PERIOD + 4);
	if (!rtl8686_timer0)
		panic("rtl8686-timer: ioremap failed");

	/* ack + disable before reprogramming. */
	if (readl(rtl8686_timer0 + (RTL8686_TIMER0_IRQ - RTL8686_TIMER0_PERIOD)) & RTL8686_TIMER_IRQ_PEND)
		writel(RTL8686_TIMER_IRQ_PEND, rtl8686_timer0 + (RTL8686_TIMER0_IRQ - RTL8686_TIMER0_PERIOD));
	writel(0, rtl8686_timer0 + (RTL8686_TIMER0_CTRL - RTL8686_TIMER0_PERIOD));

	writel(divisor, rtl8686_timer0 + (RTL8686_TIMER0_CTRL - RTL8686_TIMER0_PERIOD));
	/* MHz first, then (divisor/HZ) -- see the file header for why this
	 * is not (cpu_hz / HZ) * divisor.
	 */
	writel((cpu_hz / 1000000) * (divisor / HZ),
	       rtl8686_timer0 + (RTL8686_TIMER0_PERIOD - RTL8686_TIMER0_PERIOD));

	/*
	 * PERIODIC-only device (no set_next_event -- the hardware cannot do
	 * oneshot): clockevents_register_
	 * device() directly rather than clockevents_config_and_register(),
	 * which assumes a programmable delta this hardware does not have.
	 * mult/shift are left at their zero default, same as upstream
	 * periodic-only clockevent drivers (e.g. the old ds1287 one)
	 * -- nothing in
	 * the periodic path calls into the delta<->ns conversion these
	 * fields exist for.
	 */
	rtl8686_clockevent.cpumask = cpumask_of(0);
	rtl8686_clockevent.irq = irq;
	clockevents_register_device(&rtl8686_clockevent);

	ret = request_irq(irq, rtl8686_timer_interrupt, IRQF_TIMER | IRQF_PERCPU,
			   "rtl8686-timer0", NULL);
	if (ret)
		panic("rtl8686-timer: request_irq failed (%d)", ret);

	rtl8686_set_state_periodic(&rtl8686_clockevent);
}
