/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_wdt.h -- the SoC watchdog: a kernel thread kicks it, a userland
 * deadline resets the board if rcS never confirms, and a restart handler
 * resets through it (odi_wdt.c).
 *
 * The three registers, SoC-window offsets (odi_soc.h):
 *
 *   SOC_WDT_KICK    bit 31 written back into the current value kicks
 *   SOC_WDT_STATUS  not used; on the allowlist so the block has no gap
 *   SOC_WDT_CTRL    bit 31 ENABLE, bits 30:29 PRESCALE, 26:22 TIMEOUT1,
 *                   19:15 TIMEOUT2, 1:0 RESET_MODE
 *
 * U-Boot arms it before every boot with CTRL = 0xe7c00000 (ENABLE,
 * PRESCALE 3, TIMEOUT1 31): a reset about 41.6 s after the last kick,
 * measured. odi_wdt_init() writes the same value, so the timeout never
 * shortens and the watchdog is never off between the loader and the
 * kicker. No stage-1 interrupt handler is installed: a stage-1 timeout
 * runs on into the hardware reset.
 *
 * The userland deadline: unless something writes 1 to userland_ok within
 * ODI_WDT_USERLAND_DEADLINE_S of uptime, the deadline timer stops the NIC
 * DMA (wdt_pre_reset_hook) and forces the reset: CTRL = ENABLE alone
 * (TIMEOUT1 0, the shortest), then a kick, which restarts the count; the
 * board resets about 0.33 s later. The restart handler uses the same
 * sequence; halt and power-off re-arm the normal timeout and hang.
 */
#ifndef ODI_WDT_H
#define ODI_WDT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#ifdef __KERNEL__
/* wdt_pre_reset_hook -- set by odi_nic.c (CONFIG_ODI_NIC) to its own NIC
 * quiesce function; called by the deadline timer and the restart handler
 * (odi_wdt.c) right before either forces a reset, so an open DMA engine that outlives a CPU
 * reset is stopped first. Defined in odi_wdt.c; built-in only (both
 * CONFIG_ODI_NIC and CONFIG_ODI_WDT are bool), so a plain declaration is
 * enough, no EXPORT_SYMBOL.
 */
extern void (*wdt_pre_reset_hook)(void);
#endif

/* ---- SOC_WDT_CTRL fields --------------------------------------------- */

#define ODI_WDT_CTRL_ENABLE_BIT		31U
#define ODI_WDT_CTRL_PRESCALE_SHIFT	29U
#define ODI_WDT_CTRL_PRESCALE_MASK	0x3U
#define ODI_WDT_CTRL_TIMEOUT1_SHIFT	22U
#define ODI_WDT_CTRL_TIMEOUT1_MASK	0x1fU
#define ODI_WDT_CTRL_TIMEOUT2_SHIFT	15U
#define ODI_WDT_CTRL_TIMEOUT2_MASK	0x1fU
#define ODI_WDT_CTRL_RESET_MODE_MASK	0x3U	/* bits 1:0, no shift */

/* SOC_WDT_KICK: writing it back with this bit set pulses a kick.
 * Same bit position as ODI_WDT_CTRL_ENABLE_BIT, different register.
 */
#define ODI_WDT_KICK_BIT	31U

/*
 * ---- Operating point -- matches what U-Boot arms and the kernel
 * configuration sets today (see the header comment above).
 */

#define ODI_WDT_PRESCALE		3U
#define ODI_WDT_TIMEOUT1		31U
#define ODI_WDT_TIMEOUT2		0U
#define ODI_WDT_RESET_MODE	0U

#define ODI_WDT_KICK_INTERVAL_S		5U
#define ODI_WDT_HEARTBEAT_INTERVAL_S	60U
#define ODI_WDT_STALL_THRESHOLD_S	15U
#define ODI_WDT_STALL_MAX_REPORTS	3U
#define ODI_WDT_USERLAND_DEADLINE_S	120U

/* Periodic health-kick mode (docs/SETTINGS.md): off until the first write
 * to /proc/odi_wdt/health_kick, which arms it and seeds the period at this
 * default if none is set yet. Independent of the one-shot boot deadline
 * above: a stick that confirmed at boot and later stops reporting health
 * (the kicker died, or MemAvailable fell below its floor) still resets, on
 * its own deadline, once armed.
 */
#define ODI_WDT_HEALTH_PERIOD_S		30U

/* ---- Portable core (no __KERNEL__ dependency, host-testable) ---------- */

/* odi_wdt_ctrl_encode() -- packs the five the control register fields into one
 * register value. With prescale=ODI_WDT_PRESCALE, timeout1=ODI_WDT_TIMEOUT1,
 * timeout2=ODI_WDT_TIMEOUT2, reset_mode=ODI_WDT_RESET_MODE, enable=1 this
 * reproduces U-Boot's own en_wdt value (0xe7c00000) exactly.
 */
uint32_t odi_wdt_ctrl_encode(unsigned int prescale, unsigned int timeout1,
			      unsigned int timeout2, unsigned int reset_mode,
			      int enable);

/* odi_wdt_kick_value() -- the read-modify-write kick: OR ODI_WDT_KICK_BIT
 * into whatever the kick register currently holds, the same "set the bit,
 * never blind-write the register" shape every other kick/kick-like
 * primitive in this codebase uses.
 */
uint32_t odi_wdt_kick_value(uint32_t cur_kick);

/* odi_wdt_force_reset_value() -- ENABLE alone, every other field 0 (TIMEOUT1=0,
 * the fastest the hardware can time out): what the userland deadline and
 * the restart handler write to the control register, followed by a kick.
 */
uint32_t odi_wdt_force_reset_value(void);

/* Deadline/heartbeat/stall-report state and the pure decision function,
 * split out from any I/O so test/odi_wdt_test.c can drive it with a fake
 * clock. odi_wdt_deadline_tick() is called every ODI_WDT_TICK_INTERVAL_S
 * seconds from the kernel timer (odi_wdt.c, __KERNEL__ half, re-arming
 * itself each time it fires); the host test calls it directly, uptime_s
 * supplied by hand.
 */
#define ODI_WDT_TICK_INTERVAL_S	5U

enum odi_wdt_action {
	ODI_WDT_ACTION_NONE		= 0,
	ODI_WDT_ACTION_HEARTBEAT	= 1U << 0,
	ODI_WDT_ACTION_STALL_REPORT	= 1U << 1,
	ODI_WDT_ACTION_FORCE_RESET	= 1U << 2,
	ODI_WDT_ACTION_HEALTH_MISS	= 1U << 3,	/* always paired with FORCE_RESET; distinguishes the log line */
};

struct odi_wdt_deadline_state {
	unsigned int last_beat_s;	/* uptime at the last heartbeat printed */
	unsigned int last_kick_s;	/* uptime at the last observed kick, 0 = none yet */
	unsigned int stall_reports;	/* stall reports already issued, capped at ODI_WDT_STALL_MAX_REPORTS */
	int userland_ok;		/* set by odi_wdt_userland_confirm() */
	int watchdog_enabled;		/* mirrors odi_wdt_state.enabled -- a disabled watchdog never stalls or deadlines */
	int reset_signaled;		/* FORCE_RESET already returned once -- see odi_wdt_deadline_tick() below */
	unsigned int health_period_s;	/* 0 = periodic health monitoring off (default) */
	unsigned int last_health_s;	/* uptime at the last health kick */
	int health_armed;		/* set by odi_wdt_note_health() on its first call */
	int health_reset_signaled;	/* HEALTH_MISS already returned once, same one-shot shape as reset_signaled */
};

void odi_wdt_deadline_state_init(struct odi_wdt_deadline_state *st);

/* odi_wdt_note_kick() -- call once per observed kick (the kicker kthread
 * kicking, in the real driver) so the stall check has a last-kick time.
 */
void odi_wdt_note_kick(struct odi_wdt_deadline_state *st, unsigned int uptime_s);

/* odi_wdt_note_health() -- call once per userland health confirmation (a
 * write to /proc/odi_wdt/health_kick). Arms periodic monitoring on its
 * first call, seeding health_period_s at ODI_WDT_HEALTH_PERIOD_S if the
 * period was never configured (still 0).
 */
void odi_wdt_note_health(struct odi_wdt_deadline_state *st, unsigned int uptime_s);

/* odi_wdt_deadline_tick() -- one evaluation at the given uptime. Returns
 * the OR of every action due this tick. HEARTBEAT is due at most once
 * per ODI_WDT_HEARTBEAT_INTERVAL_S; STALL_REPORT only while the watchdog
 * is enabled, a kick has been observed at least once, more than
 * ODI_WDT_STALL_THRESHOLD_S has passed since it, and fewer than
 * ODI_WDT_STALL_MAX_REPORTS have already been issued; FORCE_RESET only
 * while the watchdog is enabled, userland_ok is still 0, and uptime_s has
 * passed ODI_WDT_USERLAND_DEADLINE_S -- and only ONCE (the caller is
 * expected to act on it by resetting the board; a host test instead sees
 * it returned exactly once and never again on a state that keeps ticking
 * past the deadline, because the real hardware path never reaches a
 * second tick).
 */
unsigned int odi_wdt_deadline_tick(struct odi_wdt_deadline_state *st, unsigned int uptime_s);

#endif /* ODI_WDT_H */
