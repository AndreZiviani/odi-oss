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
 * The clocksource is TIMER1, a second block of the same layout, run as a
 * free-running 28-bit counter with no interrupt (the RLX5281 has no CP0
 * Count/Compare). Verified on hardware: it is the current clocksource and
 * keeps time exactly across a counter wrap. If TIMER1 does not count
 * at init, no clocksource is registered and timekeeping stays on the
 * kernel jiffies clocksource (~1/HZ resolution), advanced by the TIMER0
 * tick.
 *
 * The TIMER0_CTRL/TIMER0_PERIOD/TIMER0_IRQ layout used here is the RTL9602C one; other
 * chips of the same family place their timer registers differently and
 * are not supported.
 *
 * TIMER0_PERIOD scale: REG32(TIMER0_PERIOD) = MHz * (DIVISOR / HZ) -- MHz is the LX
 * bus clock in MHz (not Hz), and DIVISOR/HZ (1000/HZ, integral for every HZ
 * this port uses) is computed before the multiply. Computing
 * (lx_hz / HZ) * divisor with lx_hz in raw Hz overflows u32 by six orders
 * of magnitude (e.g. HZ=250, 180 MHz: 720,000,000,000, silently wrapped to
 * a nonsense count) -- TIMER0 never reaches that count, so no timer interrupt
 * arrives and calibrate_delay() hangs waiting on jiffies that never
 * advance. Keep the MHz-first, divisor/HZ-before-multiply grouping exactly.
 */
#include <linux/clockchips.h>
#include <linux/clocksource.h>
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

/*
 * TIMER1 clocksource: DATA = 0x0fffffff, CTRL = divisor | enable |
 * periodic, so it counts the LX clock / 64 and wraps in about 85.9 s. The
 * mask matches the 28-bit DATA field. Whether CNT counts up or down is
 * decided at init (rtl8686_timer1_probe), and read inverts a down-counter.
 */
#define RTL8686_TIMER1_MASK	GENMASK(27, 0)

static void __iomem *rtl8686_timer1;
static bool rtl8686_timer1_down;

static inline u32 rtl8686_timer1_raw(void)
{
	return readl(rtl8686_timer1 + RTL8686_TIMER_CNT) & RTL8686_TIMER1_MASK;
}

static u64 rtl8686_timer1_read(struct clocksource *cs)
{
	u32 v = rtl8686_timer1_raw();

	return rtl8686_timer1_down ? RTL8686_TIMER1_MASK - v : v;
}

static u64 notrace rtl8686_timer1_sched_read(void)
{
	return rtl8686_timer1_read(NULL);
}

static struct clocksource rtl8686_clocksource = {
	.name	= "rtl8686-timer1",
	.rating	= 300,
	.read	= rtl8686_timer1_read,
	.mask	= CLOCKSOURCE_MASK(28),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

/*
 * Check that TIMER1 moves and which way. A bounded poll of the register
 * itself (up to 1000 reads), not udelay(): plat_time_init() runs before
 * calibrate_delay(), so udelay() has no calibrated loop count here. A
 * 3.125 MHz counter advances every 320 ns, far less than one MMIO read.
 * Returns true and sets rtl8686_timer1_down when it counts. A step of more
 * than half the range means it went backwards.
 */
static bool __init rtl8686_timer1_probe(void)
{
	u32 first = rtl8686_timer1_raw();
	int i;

	for (i = 0; i < 1000; i++) {
		u32 step = (rtl8686_timer1_raw() - first) & RTL8686_TIMER1_MASK;

		if (!step)
			continue;
		rtl8686_timer1_down = step > RTL8686_TIMER1_MASK / 2;
		return true;
	}
	return false;
}

static void __init rtl8686_clocksource_init(unsigned int lx_hz)
{
	unsigned long rate = lx_hz / RTL8686_TIMER1_DIV;

	rtl8686_timer1 = ioremap(RTL8686_TIMER1_BASE, RTL8686_TIMER_BLOCK_SIZE);
	if (!rtl8686_timer1) {
		pr_warn("rtl8686-timer1: ioremap failed, no clocksource\n");
		return;
	}

	/* Counter off, interrupt off, then reload value and the whole
	 * control word in one write (see the note in
	 * rtl8686_set_state_periodic()).
	 */
	writel(0, rtl8686_timer1 + RTL8686_TIMER_CTRL);
	writel(0, rtl8686_timer1 + RTL8686_TIMER_INT);
	writel(RTL8686_TIMER1_MASK, rtl8686_timer1 + RTL8686_TIMER_DATA);
	writel(RTL8686_TIMER_EN | RTL8686_TIMER_PERIODIC | RTL8686_TIMER1_DIV,
	       rtl8686_timer1 + RTL8686_TIMER_CTRL);

	if (!rtl8686_timer1_probe()) {
		pr_warn("rtl8686-timer1: counter does not move, no clocksource (jiffies stays)\n");
		writel(0, rtl8686_timer1 + RTL8686_TIMER_CTRL);
		iounmap(rtl8686_timer1);
		rtl8686_timer1 = NULL;
		return;
	}

	sched_clock_register(rtl8686_timer1_sched_read, 28, rate);
	clocksource_register_hz(&rtl8686_clocksource, rate);
	pr_info("rtl8686-timer1: clocksource at %lu Hz, counts %s\n", rate,
		rtl8686_timer1_down ? "down" : "up");
}

void __init rtl8686_clockevent_init(void)
{
	unsigned int lx_hz = rtl8686_lx_hz;
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
	 * is not (lx_hz / HZ) * divisor.
	 */
	writel((lx_hz / 1000000) * (divisor / HZ),
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

	rtl8686_clocksource_init(lx_hz);
}
