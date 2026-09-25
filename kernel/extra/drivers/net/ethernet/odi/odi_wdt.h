/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_wdt.h -- open, GPL, from-scratch watchdog kicker kthread for the
 * RTL9602C SoC watchdog, replacing the one the stock kernel carried (see
 * docs/LICENSING.md). This header states only hardware facts (SoC
 * addresses, bit positions, timing) and our own userland-deadline
 * addition, authored for this repository.
 *
 * Register ground truth (address and bit layout):
 *
 *   kick     0xb8003260   writing back the current value with bit 31 set
 *                         pulses a kick. Same bit position as the enable
 *                         bit below but a different register: 0xb8003260
 *                         bit 31 kicks, 0xb8003268 bit 31 enables.
 *   status   0xb8003264   status register, between the two above.
 *                         Not written by this driver; listed so the
 *                         allowlist documents the whole three-register
 *                         block rather than leaving a silent gap in it.
 *   control  0xb8003268   control register:
 *                           bit 31      ENABLE, 1 runs the timer
 *                           bits 30:29  PRESCALE, 2-bit clock scale
 *                           bits 26:22  TIMEOUT1, 5-bit stage-1 timeout
 *                           bits 19:15  TIMEOUT2, 5-bit stage-2 timeout
 *                           bits 1:0    RESET_MODE, 2-bit reset mode
 *
 * All three registers sit in the CPU system-controller window
 * (0xb8000000-0xb80fffff), already a KSEG1 (uncached, unmapped) virtual
 * address on this MIPS32 target -- no ioremap needed or possible, the
 * same way U-Boot's own `mw b8003268 ...` writes it (a plain 32-bit
 * pointer store). Unlike odi_switch.c's
 * switch-core window (physical 0x1b000000, ioremapped, its own separate
 * allowlist in odi_switch_sdkinit.c), this is a from-scratch driver in a
 * different address space entirely -- it keeps its own three-address
 * allowlist (odi_wdt.c) rather than borrowing that one.
 *
 * What U-Boot arms before every sw_tryactive trial boot
 * (en_wdt = `mw b8003268 e7c00000`) decodes
 * under the layout above to ENABLE=1, PRESCALE=3, TIMEOUT1=31, TIMEOUT2=0,
 * RESET_MODE=0 -- ODI_WDT_PRESCALE/ODI_WDT_TIMEOUT1/ODI_WDT_TIMEOUT2/
 * ODI_WDT_RESET_MODE below match it exactly, so odi_wdt_init() ends the register exactly where U-Boot
 * left it: not a shortened timeout, no window where the watchdog is
 * briefly disabled between the bootloader's own arm and the kernel's own
 * kicker starting.
 *
 * At PRESCALE=3, TIMEOUT1=31 the stage-1 timeout measures about 41.6 s,
 * observed on this hardware. Nothing here installs a stage-1 interrupt
 * handler: the file this replaces, on the 9602C, had one that turned the
 * watchdog off and spun forever, which meant a hung kernel never reset
 * -- this driver simply never adds it back, so a
 * stage-1 timeout with nothing servicing it runs on into the actual
 * hardware reset the same way it does today.
 *
 * Kicker kthread: sleeps TASK_INTERRUPTIBLE (a TASK_UNINTERRUPTIBLE
 * sleeper otherwise pins the load average at
 * 1.00) for ODI_WDT_KICK_INTERVAL_S
 * seconds, then kicks if enabled -- generalizing the outward behaviour
 * of the kicker thread it replaces, not its text.
 *
 * Userland deadline, heartbeat and kicker-stall report: our own addition,
 * carried over unchanged in effect. A trial boot
 * whose userland never confirms reachable must not be kept alive by the
 * kicker forever -- at ODI_WDT_USERLAND_DEADLINE_S (120) seconds of
 * uptime without a confirmation, the deadline timer forces the reset
 * itself: it calls the NIC quiesce hook (wdt_pre_reset_hook, exported by
 * this file, set by odi_nic.c under CONFIG_ODI_NIC -- an open DMA engine
 * that outlives a CPU reset needs to be stopped first, README.md), then
 * re-arms the control register with ENABLE alone (every other field back to 0,
 * i.e. TIMEOUT1=0 -- the fastest the hardware can time out) and spins,
 * waiting to be reset rather than returning to any code path that could
 * feed it again. Confirmation is rootfs/skeleton/etc/init.d/rcS writing
 * 1 to userland_ok once it has an ARP reply from the host side of the
 * link. Same heartbeat (free pages, every 60 s) and stall report
 * (dump_stack() if the kicker has not kicked for 15 s, at most three
 * times) as today.
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
 * quiesce function; called by the deadline timer below (odi_wdt.c) right
 * before it forces a reset, so an open DMA engine that outlives a CPU
 * reset is stopped first. Defined in odi_wdt.c; built-in only (both
 * CONFIG_ODI_NIC and CONFIG_ODI_WDT are bool), so a plain declaration is
 * enough, no EXPORT_SYMBOL.
 */
extern void (*wdt_pre_reset_hook)(void);
#endif

/* ---- SoC register addresses (system-controller window, KSEG1) --------- */

#define ODI_WDT_KICK_REG	0xb8003260U	/* kick */
#define ODI_WDT_STATUS_REG	0xb8003264U	/* status, unused, kept in the allowlist */
#define ODI_WDT_CTRL_REG	0xb8003268U	/* control */

/* ---- control register (0xb8003268) field layout ---------------------- */

#define ODI_WDT_CTRL_ENABLE_BIT		31U
#define ODI_WDT_CTRL_PRESCALE_SHIFT	29U
#define ODI_WDT_CTRL_PRESCALE_MASK	0x3U
#define ODI_WDT_CTRL_TIMEOUT1_SHIFT	22U
#define ODI_WDT_CTRL_TIMEOUT1_MASK	0x1fU
#define ODI_WDT_CTRL_TIMEOUT2_SHIFT	15U
#define ODI_WDT_CTRL_TIMEOUT2_MASK	0x1fU
#define ODI_WDT_CTRL_RESET_MODE_MASK	0x3U	/* bits 1:0, no shift */

/* Kick register (0xb8003260): writing it back with this bit set pulses a kick.
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
 * the fastest the hardware can time out): what the userland deadline
 * writes to the control register once it gives up waiting for a confirmation.
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
};

struct odi_wdt_deadline_state {
	unsigned int last_beat_s;	/* uptime at the last heartbeat printed */
	unsigned int last_kick_s;	/* uptime at the last observed kick, 0 = none yet */
	unsigned int stall_reports;	/* stall reports already issued, capped at ODI_WDT_STALL_MAX_REPORTS */
	int userland_ok;		/* set by odi_wdt_userland_confirm() */
	int watchdog_enabled;		/* mirrors odi_wdt_state.enabled -- a disabled watchdog never stalls or deadlines */
	int reset_signaled;		/* FORCE_RESET already returned once -- see odi_wdt_deadline_tick() below */
};

void odi_wdt_deadline_state_init(struct odi_wdt_deadline_state *st);

/* odi_wdt_note_kick() -- call once per observed kick (the kicker kthread
 * kicking, in the real driver) so the stall check has a last-kick time.
 */
void odi_wdt_note_kick(struct odi_wdt_deadline_state *st, unsigned int uptime_s);

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
