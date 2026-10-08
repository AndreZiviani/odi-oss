/* SPDX-License-Identifier: GPL-2.0 */
/*
 * rtl8686_time_test.c -- host-side test of the TIMER0 clockevent in
 * kernel/extra/arch/mips/rtl8686/time.c (unity build). A small model of the
 * two timer blocks stands in for the hardware: TIMER1 free-runs, TIMER0 in
 * counter mode counts DATA ticks from the enable write and latches the
 * pending bit; every register access costs two ticks of model time. The
 * model can lose the interrupt of short events, or never fire, to drive the
 * self-test and the lost-interrupt guard. It checks the register sequences
 * and the opt-in logic; it says nothing about the real silicon.
 */
#include "kstub.h"
#include <stdarg.h>
#include <string.h>

unsigned int rtl8686_lx_hz = 200000000u;

#include "../kernel/extra/arch/mips/rtl8686/time.c"

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static int failures;

/* ---- timer model ------------------------------------------------------ */
static u64 now;				/* model time, ticks of 3.125 MHz */
static unsigned char blk0[0x10], blk1[0x10];
static bool t1_moves, t1_down, want_down;
static unsigned long lose_below;	/* events shorter than this never latch */
static bool t0_dead;			/* TIMER0 never latches pending */

static u32 t0_data, t0_ctrl, t0_int;
static u64 t0_start;
static bool t0_fired;

static bool t0_pending(void)
{
	if ((t0_ctrl & RTL8686_TIMER_EN) && !(t0_ctrl & RTL8686_TIMER_PERIODIC) &&
	    !t0_fired && !t0_dead && t0_data >= lose_below &&
	    now - t0_start >= t0_data) {
		t0_fired = true;
		t0_int |= RTL8686_TIMER_IRQ_PEND;
	}
	return t0_int & RTL8686_TIMER_IRQ_PEND;
}

u32 readl(const void *a)
{
	const unsigned char *p = a;

	now += 2;
	if (p >= blk1 && p < blk1 + 0x10) {
		if (p - blk1 == RTL8686_TIMER_CNT) {
			u32 v = t1_moves ? (u32)now & GENMASK(27, 0) : 5;

			return t1_down ? GENMASK(27, 0) - v : v;
		}
		return 0;
	}
	switch (p - blk0) {
	case RTL8686_TIMER_DATA: return t0_data;
	case RTL8686_TIMER_CTRL: return t0_ctrl;
	case RTL8686_TIMER_INT: t0_pending(); return t0_int;
	}
	return 0;
}

void writel(u32 v, void *a)
{
	unsigned char *p = a;

	now += 2;
	if (p >= blk1 && p < blk1 + 0x10)
		return;			/* TIMER1 is programmed once, not modelled */
	switch (p - blk0) {
	case RTL8686_TIMER_DATA: t0_data = v & RTL8686_TIMER_DATA_MASK; break;
	case RTL8686_TIMER_CTRL:
		t0_ctrl = v;
		if (v & RTL8686_TIMER_EN) {
			t0_start = now;
			t0_fired = false;
		}
		break;
	case RTL8686_TIMER_INT:
		t0_pending();
		t0_int = (t0_int & ~RTL8686_TIMER_IRQ_EN) | (v & RTL8686_TIMER_IRQ_EN);
		if (v & RTL8686_TIMER_IRQ_PEND)
			t0_int &= ~RTL8686_TIMER_IRQ_PEND;
		break;
	}
}

void *ioremap(unsigned long phys, unsigned long size)
{
	return phys == RTL8686_TIMER1_BASE ? (void *)blk1 : (void *)blk0;
}
void iounmap(void *p) { }

/* ---- kernel calls ----------------------------------------------------- */
static int n_warn, n_info, n_registered, n_cfg_registered, n_irq;
static u32 cfg_freq;
static unsigned long cfg_min, cfg_max;

void ktest_log(const char *level, const char *fmt, ...)
{
	va_list ap;

	if (!strcmp(level, "warn"))
		n_warn++;
	else
		n_info++;
	va_start(ap, fmt);
	if (getenv("KTEST_VERBOSE")) {
		printf("[%s] ", level);
		vprintf(fmt, ap);
	}
	va_end(ap);
}
void clockevents_register_device(struct clock_event_device *d) { n_registered++; }
void clockevents_config_and_register(struct clock_event_device *d, u32 f,
				     unsigned long mn, unsigned long mx)
{
	n_cfg_registered++;
	cfg_freq = f;
	cfg_min = mn;
	cfg_max = mx;
}
int clocksource_register_hz(struct clocksource *cs, u32 hz) { return 0; }
void sched_clock_register(u64 (*rd)(void), int bits, unsigned long rate) { }
int request_irq(unsigned int irq, irqreturn_t (*h)(int, void *), unsigned long fl,
		const char *name, void *dev)
{
	n_irq++;
	return 0;
}
void panic(const char *fmt, ...) { fprintf(stderr, "panic\n"); exit(2); }

/* ---- scenarios -------------------------------------------------------- */
static void boot(bool t1ok, bool dead, unsigned long lose)
{
	memset(blk0, 0, sizeof(blk0));
	memset(blk1, 0, sizeof(blk1));
	t0_data = t0_ctrl = t0_int = 0;
	t0_fired = false;
	now = 1000;
	t1_moves = t1ok;
	t1_down = want_down;
	t0_dead = dead;
	lose_below = lose;
	n_warn = n_info = n_registered = n_cfg_registered = n_irq = 0;
	rtl8686_clockevent.features = CLOCK_EVT_FEAT_PERIODIC;
	rtl8686_clockevent_init();
}

static void test_selftest_passes_and_advertises_oneshot(void)
{
	boot(true, false, 0);
	CHECK(n_warn == 0, "no warning when the self-test passes");
	CHECK(n_info >= 1, "the self-test reports itself");
	CHECK(rtl8686_clockevent.features == (CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT),
	      "periodic and oneshot advertised");
	CHECK(n_cfg_registered == 1 && n_registered == 0, "registered with a delta range");
	CHECK(cfg_freq == 3125000, "oneshot tick is 3.125 MHz");
	CHECK(cfg_min == RTL8686_TIMER0_MIN_DELTA && cfg_max == 0x0fffffffu, "delta range");
	CHECK(n_irq == 1, "irq requested once");
	/* init ends in periodic mode, the HZ tick as before */
	CHECK(t0_ctrl == (RTL8686_TIMER_EN | RTL8686_TIMER_PERIODIC | 1000),
	      "periodic control word unchanged: enable, periodic, divisor 1000");
	CHECK(t0_data == 200 * (1000 / HZ), "periodic reload is HZ=250 on the 200 kHz tick");
	CHECK(t0_int & RTL8686_TIMER_IRQ_EN, "interrupt enabled");
}

static void test_set_next_event_programs_counter_mode(void)
{
	struct clock_event_device *cd = &rtl8686_clockevent;

	boot(true, false, 0);
	CHECK(cd->set_state_oneshot(cd) == 0, "oneshot state");
	CHECK(!(t0_ctrl & RTL8686_TIMER_EN), "oneshot state leaves the counter stopped");
	CHECK(cd->set_next_event(3125, cd) == 0, "1 ms event accepted");
	CHECK(t0_data == 3125, "DATA is the delta in ticks");
	CHECK(t0_ctrl == (RTL8686_TIMER_EN | 64), "counter mode (periodic bit clear), divisor 64, enabled");
	CHECK(!(t0_int & RTL8686_TIMER_IRQ_PEND), "not pending before it is due");
	now += 3125;
	CHECK(readl(blk0 + RTL8686_TIMER_INT) & RTL8686_TIMER_IRQ_PEND, "pending once the delta has elapsed");

	/* reprogramming a running timer: stale pending cleared, new delta */
	CHECK(cd->set_next_event(6250, cd) == 0, "reprogram while pending");
	CHECK(!(t0_int & RTL8686_TIMER_IRQ_PEND), "stale pending cleared by the reprogram");
	CHECK(t0_data == 6250, "new delta");

	CHECK(cd->set_state_oneshot_stopped(cd) == 0 && !(t0_ctrl & RTL8686_TIMER_EN), "stopped state disables the counter");
	CHECK(cd->set_state_periodic(cd) == 0 && t0_ctrl == (RTL8686_TIMER_EN | RTL8686_TIMER_PERIODIC | 1000),
	      "back to periodic restores the periodic divisor");
}

static void test_minimum_delta_event_fires(void)
{
	struct clock_event_device *cd = &rtl8686_clockevent;
	int i;

	boot(true, false, 0);
	for (i = 0; i < 100; i++) {
		CHECK(cd->set_next_event(RTL8686_TIMER0_MIN_DELTA, cd) == 0, "min delta accepted");
		now += RTL8686_TIMER0_MIN_DELTA;
		CHECK(readl(blk0 + RTL8686_TIMER_INT) & RTL8686_TIMER_IRQ_PEND, "min delta event fires");
	}
}

static void test_lost_interrupt_is_reported(void)
{
	struct clock_event_device *cd = &rtl8686_clockevent;

	/* short events never latch: the guard must say -ETIME, the self-test
	 * of the minimum delta must fail, and the device stays periodic. */
	boot(true, false, 100);
	CHECK(n_warn == 1, "one warning when the minimum-delta self-test fails");
	CHECK(rtl8686_clockevent.features == CLOCK_EVT_FEAT_PERIODIC, "oneshot not advertised");
	CHECK(n_registered == 1 && n_cfg_registered == 0, "registered as periodic-only");
	CHECK(t0_ctrl == (RTL8686_TIMER_EN | RTL8686_TIMER_PERIODIC | 1000), "still the periodic tick");
	CHECK(cd->set_next_event(4, cd) == -ETIME, "an event that would be lost returns -ETIME");
}

static void test_dead_timer_stays_periodic(void)
{
	boot(true, true, 0);
	CHECK(n_warn == 1, "exactly one warning");
	CHECK(rtl8686_clockevent.features == CLOCK_EVT_FEAT_PERIODIC, "oneshot not advertised");
	CHECK(t0_data == 200 * (1000 / HZ) && t0_ctrl == (RTL8686_TIMER_EN | RTL8686_TIMER_PERIODIC | 1000),
	      "periodic tick programmed as before");
	CHECK(n_irq == 1, "irq still requested");
}

static void test_no_timer1_stays_periodic(void)
{
	boot(false, false, 0);
	CHECK(n_warn >= 1, "warns");
	CHECK(rtl8686_clockevent.features == CLOCK_EVT_FEAT_PERIODIC, "oneshot not advertised without TIMER1");
	CHECK(n_registered == 1 && n_irq == 1, "periodic device still registered");
}

static void test_timer1_down_counter(void)
{
	struct clock_event_device *cd = &rtl8686_clockevent;

	want_down = true;
	boot(true, false, 0);
	CHECK(rtl8686_timer1_down, "probe sees the down-counter");
	CHECK(n_warn == 0 && (rtl8686_clockevent.features & CLOCK_EVT_FEAT_ONESHOT), "self-test passes on a down-counting TIMER1");
	CHECK(cd->set_next_event(3125, cd) == 0, "event accepted with a down-counting TIMER1");
	want_down = false;
}

int main(void)
{
	test_selftest_passes_and_advertises_oneshot();
	test_set_next_event_programs_counter_mode();
	test_minimum_delta_event_fires();
	test_lost_interrupt_is_reported();
	test_dead_timer_stays_periodic();
	test_no_timer1_stays_periodic();
	test_timer1_down_counter();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("rtl8686_time_test: ok\n");
	return 0;
}
