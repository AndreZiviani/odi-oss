// SPDX-License-Identifier: GPL-2.0
/*
 * odi_gpon.c -- odi_gpon kernel-integration glue: the real binding between
 * the host-tested FSM/PLOAM/register core (odi_gpon_drv.h and its .c
 * files, 427/427 host replay against a captured re-activation, react.txt)
 * and the kernel boot path (the seven /proc/rtk_init verbs, the
 * odi_switch_cmd.c cmd 13/15 and odi_switch_dal.c encrypt-port call
 * sites).
 *
 * Three independent pieces live in this one file:
 *
 *  - The one struct odi_gpon_fsm this driver runs, one spinlock
 *    serialising every touch of it (the hard-IRQ ISR, the three kernel
 *    timers below, and every /proc/rtk_init verb -- see each site for why),
 *    and odi_gpon_irq_attach()'s own ISR registration and interrupt-mask
 *    calls, which run outside that lock (see that function's own comment).
 *    The GPON interrupt type is ordinal 10: bit 10 (0x400) of
 *    CHIP_IRQ_ENABLE (0x1d00c) and CHIP_IRQ_PENDING (0x1d010), the same
 *    bit-N-is-ordinal-N layout every one of the 19 types uses (odi_intr.h).
 *    Of the other ordinals this codebase only names 7 (ACL). Under
 *    CONFIG_ODI_INTR (depends on CONFIG_ODI_SDKINIT), odi_gpon_irq_attach()
 *    calls odi_intr_register()/odi_intr_enable() (drivers/net/ethernet/odi/
 *    odi_intr.c), our own generic demux, and odi_gpon_isr_entry() below no
 *    longer acks the GPON type itself (odi_intr's own dispatch loop does
 *    that uniformly for every type).
 *
 *  - Three kernel timers (TO1, TO2, the BER-interval/REI send) armed and
 *    disarmed reactively: after every call that can move the FSM (the ISR,
 *    gponact/gpondeact, and the timers' own expiry handlers), this file
 *    re-evaluates the current state and (re)arms exactly the timer(s) that
 *    state implies, rather than threading an arm/disarm call through
 *    struct odi_gpon_fsm_ops (whose own start_to1/stop_to1/etc leaves are
 *    deliberate no-ops, odi_gpon_hw.c's own comment -- arming/disarming a
 *    real kernel timer is this file's own concern, not a register leaf).
 *    The BER-interval period itself
 *    (frames, from the OLT's own BER_Interval PLOAM) and the REI send
 *    (odi_gpon_hw_send_us_ploam(), the BIP-error-count register read) are
 *    the two things odi_gpon_hw.c could not own itself without either
 *    depending on the kernel clock or losing its own host-testability --
 *    see odi_gpon_drv.h's own comment on the seam.
 *
 *  - /proc/odi_gpon: a read-only status file -- state, ONU-ID, serial
 *    number, EqD, PLOAM rx/tx totals AND per-type counts, the REI count,
 *    TO1/TO2 expiry counts, the interrupt count and the ISR-attach result
 *    (irq_attached/isr_rc/imr_rc, see odi_gpon_irq_attach()), the last DS
 *    LOS status-bit read, and the last 32 PLOAM messages in either
 *    direction with an uptime timestamp -- filled from odi_gpon_drv.h
 *    getters plus this file's own ring buffer (odi_gpon_hw_set_ploam_log(),
 *    since the core stays kernel-clock-independent, see above).
 *
 * odi_gpon_init() -- the driver one-time bring-up (FSM/PLOAM state and the
 * three timers only) -- runs once from this file's own module_init(), at
 * kernel boot, before rcS writes the first PON-step verb to
 * /proc/rtk_init. The ISR registration and the GPON interrupt-type enable
 * used to run here too, but a stock boot capture showed the "gpondrv" step
 * itself is what sets bit 10 (0x400) of the switch interrupt mask
 * register (0x1d00c) -- registering this early left this driver's own ISR
 * registered while the interrupt type stayed masked off at the switch
 * core, and the later "intr" boot verb may in any case reset the
 * interrupt registration table. Both calls move to odi_gpon_irq_attach(),
 * run from the "gpondrv" verb instead (odi_gpon_verb(), below).
 */
#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/hex.h>

#endif

#include "odi_gpon.h"
#include "odi_gpon_drv.h"
#include "odi_gpon_hw.h"
#include "odi_gpon_init.h"
#include "odi_replay_blob.h"
#include "odi_switch_reg.h"

#if defined(CONFIG_ODI_GPON) && defined(__KERNEL__)

#define ODI_GPON_LOG(fmt, ...) pr_info_ratelimited("odi_gpon: " fmt, ##__VA_ARGS__)

/* The GPON interrupt ordinal is ODI_INTR_TYPE_GPON (odi_intr.h) -- no
 * second name for the same number here.
 */

#ifdef CONFIG_ODI_INTR
/* odi_intr.c's own generic demux (drivers/net/ethernet/odi/odi_intr.h)
 * registers and unmasks this ordinal: odi_intr_register() installs the
 * handler, odi_intr_enable() sets bit 10 of CHIP_IRQ_ENABLE (0x1d00c).
 * There is no status clear to make from here any more -- odi_intr's own
 * dispatch loop W1C-acks the bit itself, AFTER calling this file's own
 * odi_gpon_isr_entry() below, so this file must NOT also ack it
 * (odi_gpon_isr_entry()'s own tail comment has the guard).
 */
#include "odi_intr.h"
#else
#error "CONFIG_ODI_GPON needs CONFIG_ODI_INTR: no other switch interrupt path exists on this kernel"
#endif /* CONFIG_ODI_INTR */

/* ---- Driver state: one FSM, one lock, three timers -------------------- */

static struct odi_gpon_fsm odi_gpon_fsm_inst;
static DEFINE_SPINLOCK(odi_gpon_lock);

/* True once the boot-time gponact call has replayed gpon_init.bin
 * (odi_gpon_init.h) -- every LATER gponact (an interactive/coordinator-
 * driven re-activation, after an explicit gpondeact) goes through
 * odi_gpon_verb_activate() instead, the react.txt-derived sequence that
 * function's own header documents. The two are genuinely different
 * register preconditions (a cold block vs. one gpondeact just tore down),
 * so replaying the fixed boot capture a second time would not match either
 * a stock re-activation or this driver's own tested react.txt scenario.
 */
static bool odi_gpon_booted;

static struct timer_list odi_gpon_to1_timer;
static struct timer_list odi_gpon_to2_timer;
static struct timer_list odi_gpon_ber_timer;

#define ODI_GPON_TO1_MS	10000U	/* G.984.3 clause 10.2.1 recommended initial value */
#define ODI_GPON_TO2_MS	100U	/* G.984.3 clause 10.2.1 recommended initial value */

/* 0 = BER timer not armed (no BER_Interval received yet this activation, or
 * not in O5) -- distinct from odi_gpon_hw_get_ber_interval_frames()'s own
 * "0 = none yet" so a change in the OLT-configured interval re-arms at the
 * new period instead of leaving the old one running.
 */
static uint32_t odi_gpon_ber_period_ms;
static uint8_t odi_gpon_rei_seq;

/* ---- Counters and the PLOAM ring for /proc/odi_gpon -------------------
 *
 * odi_gpon_hw.c stays kernel-clock-independent (its own header contract),
 * so every field below that needs a clock or is purely observational is
 * kept here instead, fed by odi_gpon_hw_set_ploam_log() (PLOAM traffic),
 * the ISR wrapper (interrupt count, LOS status), and the timer callbacks
 * (TO1/TO2 expiry counts) -- all under odi_gpon_lock.
 */
static unsigned int odi_gpon_irq_count;
static unsigned int odi_gpon_to1_expiries;
static unsigned int odi_gpon_to2_expiries;
static unsigned int odi_gpon_rei_count;
static unsigned long odi_gpon_last_los_ms;	/* 0 = never observed since init */
static int odi_gpon_last_los_state;		/* the DS LOS status bit, last read */

/* Spurious-interrupt and per-sub-block last-nonzero-STS bookkeeping,
 * odi_gpon_isr_poll()'s own struct odi_gpon_isr_poll_status out-parameter
 * fed straight into /proc/odi_gpon -- IRQ-storm diagnosis (is the switch
 * core raising this interrupt with nothing actually pending, and if not,
 * which sub-block was busy last).
 */
static unsigned int odi_gpon_irq_spurious_count;	/* top_sts == 0 on this poll */
static uint32_t odi_gpon_last_top_sts_nonzero;
static uint32_t odi_gpon_last_ds_dlt_nonzero;
static uint32_t odi_gpon_last_us_sts_nonzero;

/* Set once by odi_gpon_irq_attach() (below); isr_rc/imr_rc are the raw
 * ISR-registration and interrupt-unmask return codes, kept for
 * /proc/odi_gpon so a failed attach is visible without a kernel log.
 */
static bool odi_gpon_irq_attached;
static int odi_gpon_irq_isr_rc;
static int odi_gpon_irq_imr_rc;

/* Per-message-type counters, both directions -- one array wide enough for
 * every documented DS/US type byte (odi_gpon_ploam.h) plus the 0x81 ZTE-
 * interop variant, indexed directly by the raw type byte.
 */
static unsigned int odi_gpon_ds_type_count[256];
static unsigned int odi_gpon_us_type_count[256];

static bool odi_gpon_ds_type_known(uint8_t type)
{
	switch (type) {
	case ODI_GPON_DS_UPSTREAM_OVERHEAD:
	case ODI_GPON_DS_SERIAL_NUMBER_MASK:
	case ODI_GPON_DS_ASSIGN_ONU_ID:
	case ODI_GPON_DS_RANGING_TIME:
	case ODI_GPON_DS_DEACTIVATE_ONU_ID:
	case ODI_GPON_DS_DISABLE_SERIAL_NUMBER:
	case ODI_GPON_DS_CONFIGURE_VP_VC:
	case ODI_GPON_DS_ENCRYPTED_PORT_ID:
	case ODI_GPON_DS_REQUEST_PASSWORD:
	case ODI_GPON_DS_ASSIGN_ALLOC_ID:
	case ODI_GPON_DS_NO_MESSAGE:
	case ODI_GPON_DS_POPUP:
	case ODI_GPON_DS_REQUEST_KEY:
	case ODI_GPON_DS_CONFIGURE_PORT_ID:
	case ODI_GPON_DS_PHYSICAL_EQUIPMENT_ERROR:
	case ODI_GPON_DS_CHANGE_POWER_LEVEL:
	case ODI_GPON_DS_PST:
	case ODI_GPON_DS_BER_INTERVAL:
	case ODI_GPON_DS_KEY_SWITCHING_TIME:
	case ODI_GPON_DS_EXT_BURST_LENGTH:
	case ODI_GPON_DS_DISABLE_SERIAL_NUMBER_ZTE:
		return true;
	default:
		return false;
	}
}

static struct odi_gpon_ploam_entry odi_gpon_ring[ODI_GPON_PLOAM_RING_LEN];
static unsigned int odi_gpon_ring_head;
static unsigned int odi_gpon_ring_count;

/* odi_gpon_hw_set_ploam_log()'s own callback -- called from inside
 * odi_gpon_hw.c/odi_gpon_isr.c while odi_gpon_lock is already held by this
 * file's own ISR wrapper or verb dispatch, so no locking of its own beyond
 * that (the "one spinlock serialises every touch" rule in the file header).
 */
static void odi_gpon_ploam_log(unsigned int dir, const struct odi_gpon_ploam *msg)
{
	struct odi_gpon_ploam_entry *e = &odi_gpon_ring[odi_gpon_ring_head];

	e->timestamp_ms = (u32)jiffies_to_msecs(jiffies);
	e->direction = (u8)dir;
	e->type = msg->type;
	memcpy(e->content, msg->content, sizeof(e->content));
	odi_gpon_ring_head = (odi_gpon_ring_head + 1U) % ODI_GPON_PLOAM_RING_LEN;
	if (odi_gpon_ring_count < ODI_GPON_PLOAM_RING_LEN)
		odi_gpon_ring_count++;

	if (dir == 0U) {
		odi_gpon_ds_type_count[msg->type]++;
		if (!odi_gpon_ds_type_known(msg->type))
			ODI_GPON_LOG("unexpected downstream PLOAM type 0x%02x\n", msg->type);
	} else {
		odi_gpon_us_type_count[msg->type]++;
		if (msg->type == ODI_GPON_US_REI)
			odi_gpon_rei_count++;
	}
}

/* ---- Timer (re)arming, called with odi_gpon_lock held ----------------- */

static void odi_gpon_timers_sync(void)
{
	enum odi_gpon_state st = odi_gpon_fsm_inst.state;

	if (st == ODI_GPON_STATE_O3 || st == ODI_GPON_STATE_O4)
		mod_timer(&odi_gpon_to1_timer, jiffies + msecs_to_jiffies(ODI_GPON_TO1_MS));
	else
		timer_delete(&odi_gpon_to1_timer);

	if (st == ODI_GPON_STATE_O6)
		mod_timer(&odi_gpon_to2_timer, jiffies + msecs_to_jiffies(ODI_GPON_TO2_MS));
	else
		timer_delete(&odi_gpon_to2_timer);

	if (st == ODI_GPON_STATE_O5) {
		uint32_t frames = odi_gpon_hw_get_ber_interval_frames();

		if (frames) {
			/* One downstream frame is 125 us (G.984.3 clause 3.1
			 * frame period; cross-check against the react.txt
			 * capture: 80000 frames * 125 us = 10.000 s, matching
			 * the observed 10.02 s REI cadence exactly) -- frames *
			 * 125 us = frames / 8 ms exactly, no rounding needed for
			 * every interval this driver has seen (all multiples of
			 * 8 frames).
			 */
			uint32_t period_ms = frames / 8U;

			if (!period_ms)
				period_ms = 1U;
			if (period_ms != odi_gpon_ber_period_ms || !timer_pending(&odi_gpon_ber_timer)) {
				odi_gpon_ber_period_ms = period_ms;
				mod_timer(&odi_gpon_ber_timer, jiffies + msecs_to_jiffies(period_ms));
			}
		} else {
			odi_gpon_ber_period_ms = 0U;
			timer_delete(&odi_gpon_ber_timer);
		}
	} else {
		odi_gpon_ber_period_ms = 0U;
		timer_delete(&odi_gpon_ber_timer);
	}
}

/* Logs every state transition, rate limited (ODI_GPON_LOG() above) --
 * called by every entry point below that can move the FSM, before and
 * after the call, under the lock.
 */
static const char *odi_gpon_state_name(enum odi_gpon_state st)
{
	switch (st) {
	case ODI_GPON_STATE_O1: return "O1";
	case ODI_GPON_STATE_O2: return "O2";
	case ODI_GPON_STATE_O3: return "O3";
	case ODI_GPON_STATE_O4: return "O4";
	case ODI_GPON_STATE_O5: return "O5";
	case ODI_GPON_STATE_O6: return "O6";
	case ODI_GPON_STATE_O7: return "O7";
	default: return "?";
	}
}

static void odi_gpon_note_transition(enum odi_gpon_state prev)
{
	if (odi_gpon_fsm_inst.state != prev)
		ODI_GPON_LOG("state %s -> %s (onu_id %u)\n",
			     odi_gpon_state_name(prev), odi_gpon_state_name(odi_gpon_fsm_inst.state),
			     (unsigned int)odi_gpon_fsm_inst.onu_id);
}

/* ---- Timer callbacks --------------------------------------------------- */

static void odi_gpon_to1_fn(struct timer_list *odi_timer_arg)
{
	unsigned long flags;
	enum odi_gpon_state prev;

	(void)odi_timer_arg;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	prev = odi_gpon_fsm_inst.state;
	odi_gpon_to1_expiries++;
	odi_gpon_fsm_handle_event(&odi_gpon_fsm_inst, odi_gpon_hw_ops(), NULL,
				   ODI_GPON_EVENT_TO1_EXPIRE, NULL);
	odi_gpon_note_transition(prev);
	odi_gpon_timers_sync();
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

static void odi_gpon_to2_fn(struct timer_list *odi_timer_arg)
{
	unsigned long flags;
	enum odi_gpon_state prev;

	(void)odi_timer_arg;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	prev = odi_gpon_fsm_inst.state;
	odi_gpon_to2_expiries++;
	odi_gpon_fsm_handle_event(&odi_gpon_fsm_inst, odi_gpon_hw_ops(), NULL,
				   ODI_GPON_EVENT_TO2_EXPIRE, NULL);
	odi_gpon_note_transition(prev);
	odi_gpon_timers_sync();
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

static void odi_gpon_ber_fn(struct timer_list *odi_timer_arg)
{
	unsigned long flags;
	uint32_t bip_err;
	struct odi_gpon_ploam rei;

	(void)odi_timer_arg;
	spin_lock_irqsave(&odi_gpon_lock, flags);
	if (odi_gpon_fsm_inst.state != ODI_GPON_STATE_O5) {
		spin_unlock_irqrestore(&odi_gpon_lock, flags);
		return;
	}

	/* The one register the react capture reads immediately before every
	 * REI send -- used directly as the REI's own error count, matching
	 * that one capture exactly; whether this counter is clear-on-read
	 * (making every read already the correct "since the last REI" delta)
	 * or free-running (which would need this driver to track its own
	 * baseline and subtract) is UNVERIFIED with only one sample.
	 */
	bip_err = odi_reg_read(ODI_GPON_DSF_BIP_ERR_BLOCKS_OFF);
	odi_gpon_rei_seq = (odi_gpon_rei_seq + 1U) & 0x0fU;
	odi_gpon_encode_rei(odi_gpon_fsm_inst.onu_id, bip_err, odi_gpon_rei_seq, &rei);
	odi_gpon_hw_send_us_ploam(&rei);

	odi_gpon_timers_sync();	/* re-arms this same timer for the next interval */
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

/* ---- ISR ---------------------------------------------------------------
 *
 * Registered for interrupt ordinal ODI_INTR_TYPE_GPON (10): no
 * arguments, no return value, runs in hard-IRQ context on the shared
 * switch-core IRQ (odi_gpon.h's own header comment). The GPON interrupt TYPE
 * is NOT already enabled at the generic interrupt module by the time this
 * driver runs: a boot capture taken across the full switch interrupt-mask
 * register range showed the stock "gpondrv" step itself setting bit 10
 * (0x400, the GPON type) of that register (0x1d00c: 0x80 -> 0x480) --
 * without this driver setting the same bit, the switch core never raises
 * this interrupt at all and this handler, however it is registered, is
 * simply never invoked.
 * odi_gpon_irq_attach() makes both the registration and the unmask,
 * once, from the "gpondrv" verb (odi_gpon_verb(), below). This function
 * itself only has to mask/unmask the GPON block OWN PONMAC_IRQ_ENABLE
 * register, which odi_gpon_isr_poll() (the core function, odi_gpon_isr.c)
 * already does.
 */
static void odi_gpon_isr_entry(void)
{
	unsigned long flags;
	enum odi_gpon_state prev;
	uint32_t ds_sts;
	struct odi_gpon_isr_poll_status poll_status;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	odi_gpon_irq_count++;

	/* Read-only DS LOS status-bit snapshot for /proc/odi_gpon's own
	 * "last_los" field -- this does NOT drive an FSM LOS/LOS_CLEAR event
	 * (odi_gpon_fsm.h defines those events, but no capture ever exercised
	 * the real DSF_ALARM_STATE/DLT LOS/LOF sequence -- wiring a
	 * made-up sequence with zero evidence would be worse than leaving
	 * O6/POPUP unautomated). A plain register read, side-effect free.
	 */
	ds_sts = odi_reg_read(ODI_GPON_DSF_ALARM_STATE_OFF);
	if (ODI_GPON_DSF_ALARM_STATE_LOS_NOW & ds_sts) {
		if (!odi_gpon_last_los_state)
			odi_gpon_last_los_ms = jiffies_to_msecs(jiffies);
		odi_gpon_last_los_state = 1;
	} else {
		odi_gpon_last_los_state = 0;
	}

	prev = odi_gpon_fsm_inst.state;
	odi_gpon_isr_poll(&odi_gpon_fsm_inst, &poll_status);
	odi_gpon_note_transition(prev);
	odi_gpon_timers_sync();

	if (poll_status.top_sts == 0U)
		odi_gpon_irq_spurious_count++;
	else
		odi_gpon_last_top_sts_nonzero = poll_status.top_sts;
	if (poll_status.ds_dlt)
		odi_gpon_last_ds_dlt_nonzero = poll_status.ds_dlt;
	if (poll_status.us_sts)
		odi_gpon_last_us_sts_nonzero = poll_status.us_sts;

	spin_unlock_irqrestore(&odi_gpon_lock, flags);

	/* No status clear here: odi_intr (odi_intr.c) W1C-acks every
	 * pending and enabled bit itself, right after calling the handler
	 * registered for it (this function, for ordinal 10).
	 */
}

/* ---- gponsn/gponpw argument parsing ------------------------------------
 *
 * odi_gpon owns this parsing now (odi_gpon.h's own header comment) -- the
 * /proc/rtk_init dispatch hands this file the raw
 * verb argument text unmodified, the same text rcS already trims. Formats
 * unchanged from the pre-CONFIG_ODI_GPON /proc/rtk_init parsing this
 * replaces: gponsn takes a 12-character argument (4 literal ASCII vendor-ID
 * bytes + 8 hex characters for the 4 specific bytes); gponpw takes either
 * 20 hex characters or 1-10 literal ASCII characters, or no argument at all
 * (this board's own config never captured a gponpw argument).
 */
static int odi_gpon_parse_sn(const char *arg, uint8_t sn[8])
{
	if (!arg || strlen(arg) != 12U)
		return -EINVAL;
	memcpy(sn, arg, 4U);
	if (hex2bin(&sn[4U], &arg[4U], 4U))
		return -EINVAL;
	return 0;
}

static int odi_gpon_parse_pw(const char *arg, uint8_t pw[10])
{
	size_t len;

	memset(pw, 0, 10U);
	if (!arg)
		return 0;
	len = strlen(arg);
	if (len == 20U)
		return hex2bin(pw, arg, 10U) ? -EINVAL : 0;
	if (len >= 1U && len <= 10U) {
		memcpy(pw, arg, len);
		return 0;
	}
	return -EINVAL;
}

/* ---- odi_gpon_drv.h-facing entry points -------------------------------- */

int odi_gpon_init(void)
{
	odi_gpon_hw_reset();
	odi_gpon_fsm_init(&odi_gpon_fsm_inst);
	odi_gpon_hw_set_ploam_log(odi_gpon_ploam_log);
	odi_gpon_booted = false;
	odi_gpon_ber_period_ms = 0U;

	timer_setup(&odi_gpon_to1_timer, odi_gpon_to1_fn, 0);
	timer_setup(&odi_gpon_to2_timer, odi_gpon_to2_fn, 0);
	timer_setup(&odi_gpon_ber_timer, odi_gpon_ber_fn, 0);

	/* ISR registration and the GPON interrupt-type enable no longer
	 * happen here -- see odi_gpon_irq_attach() below and the file header
	 * for why.
	 */
	return 0;
}

/* Attaches this driver's own ISR to the switch core and unmasks the GPON
 * interrupt type (bit 10 of 0x1d00c) -- once only (odi_gpon_irq_attached
 * guards it). Called from odi_gpon_verb()'s own "gpondrv" case, below, BEFORE
 * odi_gpon_lock is taken: both calls leave this file, and
 * nothing establishes that either is safe to make with this driver's own
 * spinlock held (IRQs off) -- unlike odi_gpon_init(), which used to make
 * the first of the two from module_init(), before rcS reaches the "gpondrv"
 * verb at all, this call site matches the point the boot capture shows the
 * stock firmware making the equivalent IMR write, so it also survives the
 * later "intr" boot verb potentially resetting the interrupt registration
 * table (this call runs after "intr", not before it).
 */
static void odi_gpon_irq_attach(void)
{
	unsigned long flags;
	int isr_rc, imr_rc;

	if (odi_gpon_irq_attached)
		return;

	isr_rc = odi_intr_register(ODI_INTR_TYPE_GPON, odi_gpon_isr_entry);
	pr_info("odi_gpon: odi_intr_register(ODI_INTR_TYPE_GPON) rc=%d\n", isr_rc);

	imr_rc = odi_intr_enable(ODI_INTR_TYPE_GPON, 1);
	pr_info("odi_gpon: odi_intr_enable(ODI_INTR_TYPE_GPON, 1) rc=%d\n", imr_rc);

	spin_lock_irqsave(&odi_gpon_lock, flags);
	odi_gpon_irq_attached = true;
	odi_gpon_irq_isr_rc = isr_rc;
	odi_gpon_irq_imr_rc = imr_rc;
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

int odi_gpon_verb(const char *verb, const char *arg)
{
	struct odi_replay_fw init_fw = { };
	int init_fw_rc = -ENOENT;
	unsigned long flags;
	enum odi_gpon_state prev;
	int rc = 0;

	if (!verb)
		return -EINVAL;

	/* odi_gpon_irq_attach() runs here, before odi_gpon_lock is taken --
	 * see that function's own comment for why "gpondrv" is the right call
	 * site and why it cannot run under this driver's own spinlock.
	 */
	if (!strcmp(verb, "gpondrv"))
		odi_gpon_irq_attach();

	/* The boot gponact replays gpon_init.bin, and loading it sleeps, so it
	 * is loaded here, in process context (the /proc/rtk_init write),
	 * before the spinlock below; the replay under the lock only reads it.
	 * Only the first gponact needs it (odi_gpon_booted), and it is
	 * released again at the end of this call, so nothing stays resident.
	 * An unlocked read is enough to decide whether to load: every verb
	 * runs under odi_switch_lock (odi_rtk_init.c), and the flag is
	 * checked again under odi_gpon_lock.
	 */
	if (!strcmp(verb, "gponact") && !READ_ONCE(odi_gpon_booted))
		init_fw_rc = odi_replay_fw_load(ODI_REPLAY_TABLE_GPON_INIT, &init_fw);

	spin_lock_irqsave(&odi_gpon_lock, flags);
	prev = odi_gpon_fsm_inst.state;

	if (!strcmp(verb, "gpondrv") || !strcmp(verb, "gpondev")) {
		/* No-op beyond the irq_attach() above: gpon_init.bin
		 * (odi_gpon_init.h) is one flat table covering
		 * gpondrv+gpondev+gponsn+gponact's own boot-time writes in the
		 * real capture's own order (odi_gpon_init.h's own header) --
		 * applied once, at "gponact" below, once the real serial
		 * number (from "gponsn") is known.
		 */
	} else if (!strcmp(verb, "gponsn")) {
		uint8_t sn[8];

		rc = odi_gpon_parse_sn(arg, sn);
		if (rc == 0)
			odi_gpon_fsm_set_serial_number(&odi_gpon_fsm_inst, sn);
	} else if (!strcmp(verb, "gponpw")) {
		uint8_t pw[10];

		rc = odi_gpon_parse_pw(arg, pw);
		if (rc == 0)
			odi_gpon_fsm_set_password(&odi_gpon_fsm_inst, pw);
	} else if (!strcmp(verb, "gponact")) {
		if (!odi_gpon_booted && init_fw_rc) {
			/* No boot table (odi_replay_fw_load() logged why):
			 * nothing is written and the ONU is not activated. The
			 * flag stays clear, so the next gponact tries again.
			 */
			pr_err("odi_gpon: gponact: no boot init table (%d), not activating\n",
			       init_fw_rc);
			rc = init_fw_rc;
		} else if (!odi_gpon_booted) {
			odi_gpon_init_apply(&init_fw.blob, odi_gpon_fsm_inst.serial_number,
					    odi_gpon_fsm_inst.password);
			/* The captured boot sequence ends with the FSM's own
			 * O1->O2 self-transition inside this same rtk_init
			 * call -- the registers are already written by the
			 * replay above; this poke brings this driver's own
			 * software state in line with them without re-issuing
			 * those writes through the ops table a second time
			 * (the same direct fsm->state poke
			 * odi_gpon_verb_deactivate() already uses).
			 */
			odi_gpon_fsm_inst.state = ODI_GPON_STATE_O2;
			odi_gpon_fsm_inst.onu_id = 0U;
			odi_gpon_booted = true;
		} else {
			odi_gpon_verb_activate(&odi_gpon_fsm_inst);
		}
	} else if (!strcmp(verb, "gpondeact")) {
		odi_gpon_verb_deactivate(&odi_gpon_fsm_inst);
	} else if (!strcmp(verb, "gponstat")) {
		/* A trigger only -- odi_gpon owns its own return convention
		 * now (odi_gpon.h's own header comment), not the stock
		 * error-code-then-state-code range /proc/rtk_init used to read
		 * back. /proc/odi_gpon is the real status surface.
		 */
		rc = 0;
	} else {
		rc = -ENOENT;
	}

	odi_gpon_note_transition(prev);
	odi_gpon_timers_sync();
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	if (!init_fw_rc)
		odi_replay_fw_release(&init_fw);
	return rc;
}

int odi_gpon_onu_state(void)
{
	unsigned long flags;
	int st;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	st = (int)odi_gpon_fsm_inst.state;
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	return st;
}

int odi_gpon_onu_id_get(void)
{
	unsigned long flags;
	int id;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	id = odi_gpon_fsm_inst.onu_id ? (int)odi_gpon_fsm_inst.onu_id : -1;
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
	return id;
}

void odi_gpon_sn_get(u8 sn[8])
{
	unsigned long flags;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	memcpy(sn, odi_gpon_fsm_inst.serial_number, 8);
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

void odi_gpon_eqd_get(struct odi_gpon_eqd *eqd)
{
	uint32_t mf, inf;

	odi_gpon_get_eqd(&mf, &inf);
	eqd->multiframe = mf;
	eqd->inframe = inf;
}

void odi_gpon_ploam_counts_get(struct odi_gpon_ploam_counts *counts)
{
	unsigned int rx, tx;

	odi_gpon_get_ploam_counts(&rx, &tx);
	counts->ds_rx = rx;
	counts->us_tx = tx;
}

void odi_gpon_ploam_ring_get(struct odi_gpon_ploam_entry *ring, unsigned int *count)
{
	unsigned long flags;
	unsigned int i, start;

	spin_lock_irqsave(&odi_gpon_lock, flags);
	*count = odi_gpon_ring_count;
	start = (odi_gpon_ring_head + ODI_GPON_PLOAM_RING_LEN - odi_gpon_ring_count) % ODI_GPON_PLOAM_RING_LEN;
	for (i = 0; i < odi_gpon_ring_count; i++)
		ring[i] = odi_gpon_ring[(start + i) % ODI_GPON_PLOAM_RING_LEN];
	spin_unlock_irqrestore(&odi_gpon_lock, flags);
}

/* ---- /proc/odi_gpon ---------------------------------------------------- */

static int odi_gpon_proc_show(struct seq_file *seq, void *v)
{
	struct odi_gpon_eqd eqd;
	struct odi_gpon_ploam_counts counts;
	struct odi_gpon_ploam_entry ring[ODI_GPON_PLOAM_RING_LEN];
	unsigned int count = 0, i;
	u8 sn[8];
	unsigned long flags;
	unsigned int irq_count, to1_exp, to2_exp, rei_count;
	unsigned long last_los;
	int last_los_state;
	bool irq_attached;
	int irq_isr_rc, irq_imr_rc;
	unsigned int irq_spurious;
	uint32_t last_top_sts, last_ds_dlt, last_us_sts;

	(void)v;

	odi_gpon_eqd_get(&eqd);
	odi_gpon_ploam_counts_get(&counts);
	odi_gpon_ploam_ring_get(ring, &count);
	odi_gpon_sn_get(sn);

	spin_lock_irqsave(&odi_gpon_lock, flags);
	irq_count = odi_gpon_irq_count;
	to1_exp = odi_gpon_to1_expiries;
	to2_exp = odi_gpon_to2_expiries;
	rei_count = odi_gpon_rei_count;
	last_los = odi_gpon_last_los_ms;
	last_los_state = odi_gpon_last_los_state;
	irq_attached = odi_gpon_irq_attached;
	irq_isr_rc = odi_gpon_irq_isr_rc;
	irq_imr_rc = odi_gpon_irq_imr_rc;
	irq_spurious = odi_gpon_irq_spurious_count;
	last_top_sts = odi_gpon_last_top_sts_nonzero;
	last_ds_dlt = odi_gpon_last_ds_dlt_nonzero;
	last_us_sts = odi_gpon_last_us_sts_nonzero;
	spin_unlock_irqrestore(&odi_gpon_lock, flags);

	{
		int state = odi_gpon_onu_state();

		seq_printf(seq, "state %d (%s)\n", state, odi_gpon_state_name((enum odi_gpon_state)state));
	}
	seq_printf(seq, "onu_id %d\n", odi_gpon_onu_id_get());
	seq_printf(seq, "sn %02x%02x%02x%02x%02x%02x%02x%02x\n",
		   sn[0], sn[1], sn[2], sn[3], sn[4], sn[5], sn[6], sn[7]);
	seq_printf(seq, "eqd multiframe %u inframe %u\n", eqd.multiframe, eqd.inframe);
	seq_printf(seq, "ploam ds_rx %u us_tx %u\n", counts.ds_rx, counts.us_tx);

	for (i = 0; i < 256U; i++) {
		if (odi_gpon_ds_type_count[i])
			seq_printf(seq, "ploam_type ds 0x%02x %u\n", i, odi_gpon_ds_type_count[i]);
	}
	for (i = 0; i < 256U; i++) {
		if (odi_gpon_us_type_count[i])
			seq_printf(seq, "ploam_type us 0x%02x %u\n", i, odi_gpon_us_type_count[i]);
	}

	seq_printf(seq, "rei_count %u\n", rei_count);
	seq_printf(seq, "to1_expiries %u\n", to1_exp);
	seq_printf(seq, "to2_expiries %u\n", to2_exp);
	seq_printf(seq, "irq_count %u\n", irq_count);
	seq_printf(seq, "irq_spurious %u\n", irq_spurious);
	seq_printf(seq, "irq_last_top_sts 0x%08x irq_last_ds_dlt 0x%08x irq_last_us_sts 0x%08x\n",
		   last_top_sts, last_ds_dlt, last_us_sts);
	seq_printf(seq, "irq_attached %d isr_rc %d imr_rc %d\n",
		   irq_attached ? 1 : 0, irq_isr_rc, irq_imr_rc);
	seq_printf(seq, "last_los_ms %lu state %d\n", last_los, last_los_state);

	seq_printf(seq, "ploam_ring %u entries\n", count);
	for (i = 0; i < count && i < ODI_GPON_PLOAM_RING_LEN; i++) {
		seq_printf(seq, "  %u %s type 0x%02x\n",
			   ring[i].timestamp_ms,
			   ring[i].direction == ODI_GPON_PLOAM_US ? "us" : "ds",
			   ring[i].type);
	}

	return 0;
}

static int odi_gpon_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, odi_gpon_proc_show, NULL);
}

static const struct proc_ops odi_gpon_proc_fops = {
	.proc_open = odi_gpon_proc_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
};

static int __init odi_gpon_module_init(void)
{
	int rc = odi_gpon_init();

	if (rc)
		pr_info("odi_gpon: init failed, rc=%d\n", rc);
	if (!proc_create("odi_gpon", 0444, NULL, &odi_gpon_proc_fops))
		pr_info("odi_gpon: /proc/odi_gpon not created\n");
	return 0; /* never fail module_init over odi_gpon_init()'s own result */
}
module_init(odi_gpon_module_init);

#endif /* CONFIG_ODI_GPON && __KERNEL__ */
