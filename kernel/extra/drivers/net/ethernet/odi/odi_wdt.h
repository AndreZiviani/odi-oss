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

/* wdt_cpu_rx_ops -- set by odi_switch.c, the one place that sees both the
 * switch MIB and odi_nic, for the CPU-port RX rule below. sample() returns
 * 0 and the two running counts, or nonzero when there is nothing to judge
 * (no NIC device open); report() logs the NIC state before the reset.
 */
struct odi_wdt_cpu_rx_ops {
	int (*sample)(u32 *offered, u32 *taken);
	void (*report)(void);
};
extern const struct odi_wdt_cpu_rx_ops *wdt_cpu_rx_ops;
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

/* ---- Per-client ping deadlines (docs/SETTINGS.md, "Watchdog rules") ------
 *
 * Replaces the v1.0.2 userland health-kicker (a separate process guessing
 * at omcid's liveness from /proc). Each client pings its OWN deadline from
 * its OWN main loop; the kernel is the only judge and the only thing that
 * still owns the hardware watchdog. A client is registered (name + deadline)
 * once, normally at boot from rcS; its first ping arms the deadline. Missing
 * an armed deadline stops the kicker, exactly like the boot confirmation
 * above.
 */
#define ODI_WDT_MAX_CLIENTS		4U
#define ODI_WDT_CLIENT_NAME_LEN		16U

/* omcid pings every few seconds (main.c); 60 s gives it ample margin over
 * ordinary scheduling jitter and the netlink poll cadence while still
 * catching a hang well inside a human's patience for "is it back yet".
 */
#define ODI_WDT_OMCID_DEADLINE_S	60U

/* ---- Kernel-side memory floor ---------------------------------------
 *
 * si_mem_available() (kernel/mm/util.c), sampled every ODI_WDT_TICK_INTERVAL_S,
 * converted to KB. Below the floor for ODI_WDT_MEM_FLOOR_CONSEC consecutive
 * samples (15 s at the default tick) stops the kicker -- the same floor and
 * the same "OOM took the box and nothing came back" case the v1.0.2 health
 * kicker existed to catch (docs/SETTINGS.md, "Resilience"), now judged by
 * the kernel itself instead of a userland process that can itself be a
 * casualty of the same OOM. Several consecutive samples, not one, so a
 * single allocation spike does not reset a box that is otherwise fine.
 */
#define ODI_WDT_MEM_FLOOR_KB		2048U
#define ODI_WDT_MEM_FLOOR_CONSEC	3U

/* ---- CPU-port RX liveness -------------------------------------------
 *
 * The management path is the switch CPU port (port 3) and the NIC behind
 * it. If the switch keeps offering it frames -- delivered (port 3
 * ifOut{Ucast,Multicast,Broadcast}Pkts), dropped for it (ifOutDiscards),
 * or held back by our own PAUSE (dot3InPauseFrames) -- while the NIC takes
 * no RX descriptor back, the NIC is wedged and nothing else notices: the
 * kernel and omcid are fine, only the stick is unreachable. That many
 * consecutive ticks with frames offered and none taken stops the kicker.
 * A tick with nothing offered neither counts nor clears (an idle port
 * never resets); any descriptor taken clears it.
 */
#define ODI_WDT_CPU_RX_STALL_TICKS	6U	/* 30 s at ODI_WDT_TICK_INTERVAL_S */

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
	ODI_WDT_ACTION_CLIENT_MISS	= 1U << 3,	/* always paired with FORCE_RESET; a registered client missed its deadline */
	ODI_WDT_ACTION_MEM_FLOOR	= 1U << 4,	/* always paired with FORCE_RESET; MemAvailable below floor, N consecutive checks */
	ODI_WDT_ACTION_CPU_RX_STALL	= 1U << 5,	/* always paired with FORCE_RESET; frames offered to port 3, none taken, N ticks */
};

struct odi_wdt_client {
	char name[ODI_WDT_CLIENT_NAME_LEN];
	unsigned int deadline_s;	/* 0 = slot unused */
	unsigned int last_ping_s;
	int armed;			/* set by the first ping; unarmed clients are never checked */
	int reset_signaled;		/* one-shot, same shape as odi_wdt_deadline_state.reset_signaled */
};

struct odi_wdt_deadline_state {
	unsigned int last_beat_s;	/* uptime at the last heartbeat printed */
	unsigned int last_kick_s;	/* uptime at the last observed kick, 0 = none yet */
	unsigned int stall_reports;	/* stall reports already issued, capped at ODI_WDT_STALL_MAX_REPORTS */
	int userland_ok;		/* set by odi_wdt_userland_confirm() */
	int watchdog_enabled;		/* mirrors odi_wdt_state.enabled -- a disabled watchdog never stalls or deadlines */
	int reset_signaled;		/* FORCE_RESET already returned once -- see odi_wdt_deadline_tick() below */
	struct odi_wdt_client clients[ODI_WDT_MAX_CLIENTS];
	unsigned int mem_low_streak;	/* consecutive ticks with free_kb below the floor */
	int mem_reset_signaled;	/* MEM_FLOOR already returned once, same one-shot shape */
	int cpu_rx_have;		/* a previous sample to diff against */
	uint32_t cpu_rx_offered;	/* last sample, running counts */
	uint32_t cpu_rx_taken;
	unsigned int cpu_rx_stall;	/* ticks with frames offered and none taken, since the last taken */
	int cpu_rx_reset_signaled;	/* CPU_RX_STALL already returned once */
};

void odi_wdt_deadline_state_init(struct odi_wdt_deadline_state *st);

/* odi_wdt_note_kick() -- call once per observed kick (the kicker kthread
 * kicking, in the real driver) so the stall check has a last-kick time.
 */
void odi_wdt_note_kick(struct odi_wdt_deadline_state *st, unsigned int uptime_s);

/* odi_wdt_client_register() -- idempotent: a name already registered just
 * gets its deadline updated (rcS may be called more than once in a
 * development boot). Returns the client's slot index (>= 0), or -1 if every
 * slot is taken and the name is new.
 *
 * Arms the deadline immediately, counted from `uptime_s` (the moment of
 * registration), not from the client's own first ping. A client that never
 * pings at all used to go unnoticed forever -- unarmed clients were never
 * checked by odi_wdt_deadline_tick() -- which is exactly the shape a daemon
 * stuck before its first ping takes: registered, silent, and never reset.
 * Registration is the caller's promise that the client is about to run
 * (rcS only registers a client it is also about to start; see
 * docs/SETTINGS.md, "Watchdog rules"), so treating "never pinged" the same
 * as "missed its own ping" is safe -- there is no longer a legitimate
 * registered-but-never-started case to protect. A later ping still just
 * pushes the deadline out, same as any other.
 */
int odi_wdt_client_register(struct odi_wdt_deadline_state *st, const char *name,
			     unsigned int deadline_s, unsigned int uptime_s);

/* odi_wdt_client_ping() -- arms the client on its first call. Returns 0 on a
 * known (registered) client, -1 if no client of that name is registered --
 * a ping from an unregistered name is a configuration bug, not something to
 * silently create a client for.
 */
int odi_wdt_client_ping(struct odi_wdt_deadline_state *st, const char *name,
			 unsigned int uptime_s);

/* odi_wdt_deadline_tick() -- one evaluation at the given uptime, told the
 * current free memory in KB (si_mem_available() in the real driver; supplied
 * directly in the host test). Returns the OR of every action due this tick.
 * HEARTBEAT is due at most once per ODI_WDT_HEARTBEAT_INTERVAL_S;
 * STALL_REPORT only while the watchdog is enabled, a kick has been observed
 * at least once, more than ODI_WDT_STALL_THRESHOLD_S has passed since it,
 * and fewer than ODI_WDT_STALL_MAX_REPORTS have already been issued;
 * FORCE_RESET (alone) only while the watchdog is enabled, userland_ok is
 * still 0, and uptime_s has passed ODI_WDT_USERLAND_DEADLINE_S; CLIENT_MISS
 * (with FORCE_RESET) once any armed, registered client has gone more than
 * its own deadline_s since its last ping; MEM_FLOOR (with FORCE_RESET) once
 * free_kb has stayed below ODI_WDT_MEM_FLOOR_KB for ODI_WDT_MEM_FLOOR_CONSEC
 * consecutive ticks. Every FORCE_RESET-triggering condition is one-shot
 * (the caller is expected to act on it by resetting the board; a host test
 * instead sees it returned exactly once and never again on a state that
 * keeps ticking past the deadline, because the real hardware path never
 * reaches a second tick).
 */
unsigned int odi_wdt_deadline_tick(struct odi_wdt_deadline_state *st, unsigned int uptime_s,
				    unsigned long free_kb);

/* odi_wdt_cpu_rx_tick() -- the CPU-port RX rule, once per tick, given
 * whether there is a sample (`valid`, 0 while no NIC device is open, which
 * also forgets the previous one) and the two running counts. Returns
 * FORCE_RESET | CPU_RX_STALL, once, when ODI_WDT_CPU_RX_STALL_TICKS ticks
 * in a row (idle ticks skipped) saw frames offered and none taken, while
 * the watchdog is enabled; else ODI_WDT_ACTION_NONE. Counts wrap freely:
 * only differences are used.
 */
unsigned int odi_wdt_cpu_rx_tick(struct odi_wdt_deadline_state *st, int valid,
				  uint32_t offered, uint32_t taken);

/* odi_wdt_reset_reason() -- the ODI_RAMLOG_REASON_* code (odi_ramlog.h)
 * for the actions one odi_wdt_deadline_tick() returned, recorded in the
 * ramlog before the reset: ODI_RAMLOG_REASON_NONE without FORCE_RESET,
 * else WDT_MEM, WDT_CPU_RX, WDT_CLIENT or WDT_USERLAND, in that order
 * when several fired in the same tick -- memory first, as a starved box
 * also misses pings. For WDT_CLIENT, *client is set to the first client in slot order
 * that missed its deadline (odi_wdt.c logs every one of them).
 */
uint32_t odi_wdt_reset_reason(const struct odi_wdt_deadline_state *st, unsigned int actions,
			       unsigned int uptime_s, const char **client);

#endif /* ODI_WDT_H */
