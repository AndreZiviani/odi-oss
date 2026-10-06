// SPDX-License-Identifier: GPL-2.0
/*
 * odi_gpon_hw.c -- the real register leaves behind struct odi_gpon_fsm_ops
 * (odi_gpon_fsm.h), plus the GEM-port/Alloc-ID/Acknowledge/verb-level
 * driver logic odi_gpon_fsm.c deliberately leaves out of the pure FSM
 * (its own header comment: GEM/T-CONT/Alloc-ID/Acknowledge are not an FSM
 * state concern). odi_gpon_isr.c is the only caller of the non-ops-table
 * functions this file declares (odi_gpon_drv.h); the ops table itself is
 * also handed straight to odi_gpon_fsm_handle_event() by odi_gpon_isr.c.
 *
 * Register access is through odi_reg_read()/odi_reg_write() -- the
 * odi_switch MMIO primitive, real MMIO under __KERNEL__ (odi_switch.c)
 * and the host register-model mock otherwise (test/odi_switch_mock.h,
 * included ahead of this file in one unity-build translation unit by
 * every host test, exactly like odi_switch_tbl.c and the switch-core leaves already
 * do) -- and, for the two indirect CAM tables this block shares with the
 * rest of the switch core, through odi_switch_gpon_ds_port_write()/
 * odi_switch_gpon_alloc_write() (odi_switch_tbl.c), already implemented
 * and tested -- this file does not re-derive that handshake.
 */
#include "odi_gpon_drv.h"
#include "odi_gpon_hw.h"
#include "odi_switch_hw.h"
#include "odi_switch_tbl.h"
#include "odi_switch_dal.h"
#include "odi_switch_reg.h"

#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/printk.h>
#define ODI_GPON_HW_LOG(fmt, ...) pr_info_once("odi_gpon_hw: " fmt, ##__VA_ARGS__)
#else
#include <string.h>
#include <stdio.h>
#define ODI_GPON_HW_LOG(fmt, ...) printf("odi_gpon_hw: " fmt, ##__VA_ARGS__)
#endif

/* ---- Small hardware-shadow state this leaf layer keeps, documented at
 * each field -- none of it is FSM state (struct odi_gpon_fsm has that);
 * this is CAM-row bookkeeping and a couple of values one register write
 * needs that a later one supplies (odi_gpon_fsm.h's own ops contract has
 * no channel for that, so the two calls share it here instead).
 */
struct odi_gpon_hw_state {
	/* Cached from the last set_upstream_overhead() call, reused by
	 * set_eqd()'s own "ranged" burst-overhead reconfigure: the USF_BURST_HDR_BYTE
	 * bytes written at Ranging_Time are the OLT's own type3_pattern/
	 * delimiter repeated, not new values.
	 */
	uint8_t boh_pattern;
	uint8_t boh_delimiter[3];
	int boh_valid;
	/* Bytes of each burst that are not type 3 preamble: guard plus type 1
	 * and type 2 preamble (Upstream_Overhead bit counts, rounded to bytes)
	 * plus the 3 delimiter bytes. USF_BURST_HDR_SETUP.LENGTH = this + type 3 bytes.
	 */
	uint8_t boh_fixed_bytes;
	/* USF_BURST_HDR_SETUP.LENGTH for the ranged (O5) burst: Extended_Burst_Length ranged
	 * count once the OLT sent one, else the default below.
	 */
	uint8_t boh_ranged_len;
	int eqd_applied;	/* set_eqd() ran: bursts use the ranged length */

	/* Alloc-ID (T-CONT) CAM row bookkeeping (odi_gpon_hw.c's own
	 * "sequential pool from row 0" policy, confirmed exactly against
	 * react.txt's own five Assign_Alloc-ID rows 0..4, in arrival
	 * order) -- bit i set means row i is currently allocated. The
	 * default/OMCC row (ODI_GPON_HW_ALLOC_ROW_DEFAULT) is tracked in the
	 * same bitmap so odi_gpon_verb_deactivate() clears everything with
	 * one loop.
	 */
	uint32_t alloc_used;
	uint16_t alloc_id_values[32];	/* row -> assigned Alloc-ID (32 = ODI_GPON_HW_ALLOC_ROWS, the
					 * table's own 5-bit index width) -- for the dedup below
					 */

	/* The one downstream GEM port row implemented here (the port
	 * Configure_Port-ID names, always ODI_GPON_HW_GEM_ROW_PRIMARY --
	 * row 64 is the only one ever observed in use). Additional GEM ports an
	 * OMCI-driven Encrypted_Port-ID names for a DIFFERENT Port-ID are a
	 * documented gap (see odi_gpon_hw_gem_port_encrypted() below) --
	 * those never touch gem_port_id/gem_port_active.
	 */
	uint16_t gem_port_id;
	int gem_port_active;

	unsigned int ploam_rx_count;
	unsigned int ploam_tx_count;

	uint32_t eqd_multframe;
	uint32_t eqd_inframe;

	/* USF_MIN_RESP_DELAY as read by the last USF_EQ_DELAY write, and the
	 * snapshot of the last write a Ranging_Time caused (debug only,
	 * /proc/odi_gpon): the delay bits it carried, that register value and
	 * the MULTFRAME/INFRAME written.
	 */
	uint32_t eqd_min_resp;
	struct odi_gpon_ranging_dbg rng;

	/* The last DSK_SWITCH_FRAME value armed, and whether one has
	 * been armed at all yet -- hw_switch_key()'s own dedup guard.
	 */
	uint32_t key_switch_superframe;
	int key_switch_armed;

	/* The downstream frame count the last BER_Interval PLOAM carried
	 * (G.984.3 clause 9.2.3.18) -- odi_gpon_isr.c decodes and stores it
	 * here; odi_gpon.c's own kernel timer reads it back to (re)arm the
	 * REI-send period. 0 = none received yet this activation.
	 */
	uint32_t ber_interval_frames;

	/* Optional PLOAM observability hook, odi_gpon.c's own /proc/odi_gpon
	 * ring buffer (odi_gpon_hw_set_ploam_log()). NULL by default and after
	 * odi_gpon_hw_reset() -- a plain call-if-set, no locking of its own
	 * (the caller, odi_gpon.c, already holds its own lock around every
	 * odi_gpon_isr()/odi_gpon_verb_*() call this fires from).
	 */
	odi_gpon_ploam_log_fn ploam_log;
};

static struct odi_gpon_hw_state odi_gpon_hw;

/* Fixed CAM rows/indices this driver uses -- single-sample constants from
 * the one activation capture available; a box that ever needs a second
 * GEM port row or a different default Alloc-ID row would need this
 * generalized -- not attempted here.
 */
#define ODI_GPON_HW_GEM_ROW_PRIMARY	64U
#define ODI_GPON_HW_ALLOC_ROW_DEFAULT	16U
#define ODI_GPON_HW_ALLOC_ROWS		32U

/* One upstream GTC frame is 155520 bits at 1.24416 Gb/s over 125 us
 * (1.24416e9 * 125e-6 = 155520) -- a given constant of the GPON upstream
 * rate, not derived here.
 */
#define ODI_GPON_HW_US_FRAME_BITS	155520UL

/* Configure_Port-ID's own traffic-cfg byte, the one value this capture
 * shows (react.txt, DSF_GEM_FLOW_TYPE[64] = 0x04) -- the packed
 * fields (is_eth/is_omci/is_mcast/aes_en) are named elsewhere but
 * not their bit positions; this driver does not decode this byte further,
 * it only replays the one value evidence gives it. A later pass with more
 * captures (different port types) would need to actually decode it.
 */
#define ODI_GPON_HW_TRAFFIC_CFG_PRIMARY	0x04U

/* Fixed switch-core PON stream index (odi_switch_hw.h ODI_SW_PONQ_STREAM_VALID
 * own comment has the evidence and the single-PON-port caveat).
 */
#define ODI_GPON_HW_PON_SID_INDEX	2U

void odi_gpon_hw_reset(void)
{
	memset(&odi_gpon_hw, 0, sizeof(odi_gpon_hw));
}

#ifndef __KERNEL__
/* Test/replay-harness seam only: not compiled into vmlinux. Seeds this
 * leaf layer's own bookkeeping to look like an ONU that has already been
 * through activation once -- a FORCED re-activation of an already-active
 * stick, not a fresh boot, so odi_gpon_verb_deactivate() Alloc-ID/GEM-port
 * teardown has something real to tear down. Not part of the
 * odi_gpon.c-facing surface (odi_gpon_drv.h has it under its own
 * "internal" heading, same as the PLOAM-dispatch helpers).
 */
void odi_gpon_hw_test_seed_active(uint16_t gem_port_id, const unsigned int *alloc_rows,
				   unsigned int n_alloc_rows)
{
	unsigned int i;

	odi_gpon_hw.gem_port_id = gem_port_id;
	odi_gpon_hw.gem_port_active = 1;
	for (i = 0; i < n_alloc_rows; i++) {
		odi_gpon_hw.alloc_used |= (1UL << alloc_rows[i]);
		odi_gpon_hw.alloc_id_values[alloc_rows[i]] = 0;	/* value irrelevant to a delete */
	}
}
#endif /* !__KERNEL__ */

/* ---- struct odi_gpon_fsm_ops leaves ---- */

static void hw_set_state(void *ctx, enum odi_gpon_state state)
{
	uint32_t reg = odi_reg_read(ODI_GPON_DSF_ONU_STATE_OFF);

	(void)ctx;
	reg = ODI_GPON_DSF_ONU_STATE_ACTIVATION_STATE_SET(reg, (uint32_t)state);
	odi_reg_write(ODI_GPON_DSF_ONU_STATE_OFF, reg);
}

/* Both hardware ONU-ID registers get the ONU-ID value written before the
 * FSM state transition is written -- this leaf, called before set_state()
 * by the O3 handler in odi_gpon_fsm.c, matches that ordering by
 * construction -- plus the default/OMCC Alloc-ID
 * CAM row: react.txt L330 shows the same instant the
 * ONU-ID is assigned, row 16 of the Alloc-ID table is written with that
 * same ONU-ID value (the G.984.3 convention that the default/OMCC
 * Alloc-ID equals the ONU-ID). Only for a real (nonzero) assignment --
 * the O1/O2 reset paths call this with 0 and do not touch the CAM (no
 * capture evidence either way for a protocol-driven Deactivate_ONU-ID
 * own CAM footprint; the administrative gpondeact verb's own teardown,
 * which IS evidenced, goes through odi_gpon_verb_deactivate() instead).
 */
static void hw_set_onu_id(void *ctx, uint8_t onu_id)
{
	uint32_t reg;

	(void)ctx;

	reg = odi_reg_read(ODI_GPON_DSF_ONU_STATE_OFF);
	reg = ODI_GPON_DSF_ONU_STATE_ASSIGNED_ONU_ID_SET(reg, onu_id);
	odi_reg_write(ODI_GPON_DSF_ONU_STATE_OFF, reg);

	reg = odi_reg_read(ODI_GPON_USF_ONU_ID_OFF);
	reg = ODI_GPON_USF_ONU_ID_TX_ONU_ID_SET(reg, onu_id);
	odi_reg_write(ODI_GPON_USF_ONU_ID_OFF, reg);

	if (onu_id != ODI_GPON_ONU_ID_BROADCAST) {
		odi_switch_gpon_alloc_write(ODI_GPON_HW_ALLOC_ROW_DEFAULT, onu_id);
		odi_gpon_hw.alloc_used |= (1UL << ODI_GPON_HW_ALLOC_ROW_DEFAULT);
	}
}

/* The EqD formula, derived against TWO confirming data points, not one --
 * both from react.txt:
 *
 *   1. At Ranging_Time (both activation captures, identical -- same
 *      fibre): delay = 227172 bits (0x00037764). One upstream frame =
 *      155520 bits (1.24416 Gb/s * 125 us). 227172 / 155520 = 1 remainder
 *      71652. USF_MIN_RESP_DELAY read just before the EQD write =
 *      0x9132 -> DELAY_HI (bits 15:7, 9-bit) = 0x9132 >> 7 = 0x122 = 290.
 *      71652 + (290 << 7) = 71652 + 37120 = 108772 = 0x1a8e4 -- EXACT
 *      match to the captured EQD field.
 *   2. At Upstream_Overhead, BEFORE any ranging: USF_EQ_DELAY is
 *      ALSO written, with no Ranging_Time
 *      delay yet to apply -- captured
 *      value 0x00009100 = MULTFRAME 0, INFRAME 0x9100 = 37120 = exactly
 *      (290 << 7) again, with the delay term at zero. This is a second,
 *      independent confirmation of the "+ DELAY_HI << 7" additive term
 *      specifically (a different trigger point, same constant), though it
 *      says nothing new about the MULTFRAME/frame-division half of the
 *      formula (delay is 0 here, so that term does not exercise it).
 *
 * So: total = delay + (DELAY_HI << 7), MULTFRAME = total / frame_bits,
 * INFRAME = total % frame_bits, delay = 0 before ranging (or the
 * pre-assigned delay, see hw_set_upstream_overhead()). This reproduces
 * both observed register values bit-for-bit from quantities the capture
 * itself gives (frame_bits is a given constant, not fit to the data).
 *
 * The additive term goes in BEFORE the division. G.984.3 defines the
 * equalization delay as one total delay; splitting it into
 * whole frames and a remainder is only a way to write it into two fields,
 * so the remainder must stay below one frame. Dividing first and adding
 * DELAY_HI << 7 to the remainder afterwards
 * gives the same total but leaves INFRAME above one frame, with FRAMES one
 * short, whenever the remainder is within DELAY_HI << 7 of a frame (about a
 * quarter of all fibre lengths). Both captures have a remainder far from
 * that edge, so they cannot tell the two orders apart; the total is the
 * same, the register encoding is not.
 *
 * FLAGGED: only one distinct NONZERO delay value exists
 * across both captures (same fibre length) -- the DELAY_HI additive
 * term is now confirmed at two
 * different points, but the frame-division half of the formula is still
 * exact for only that one sample; nothing here proves MULTFRAME is really
 * a plain integer division rather than, say, always 1 on this box's own
 * typical distance. A hardware trial at a DIFFERENT fibre length (or a
 * second real OLT) is what would turn that half from "reproduces the one
 * sample exactly" into "confirmed formula".
 */
static void hw_apply_eqd(uint32_t eqd_bits)
{
	uint32_t min_delay, min_delay1;
	uint32_t total, multframe, inframe;
	uint32_t reg;

	min_delay = odi_reg_read(ODI_GPON_USF_MIN_RESP_DELAY_OFF);
	min_delay1 = ODI_GPON_USF_MIN_RESP_DELAY_DELAY_HI_GET(min_delay);

	total = eqd_bits + (min_delay1 << 7);
	multframe = total / (uint32_t)ODI_GPON_HW_US_FRAME_BITS;
	inframe = total % (uint32_t)ODI_GPON_HW_US_FRAME_BITS;

	odi_gpon_hw.eqd_multframe = multframe;
	odi_gpon_hw.eqd_inframe = inframe;
	odi_gpon_hw.eqd_min_resp = min_delay;

	reg = 0;
	reg = ODI_GPON_USF_EQ_DELAY_FRAMES_SET(reg, multframe);
	reg = ODI_GPON_USF_EQ_DELAY_EQD_SET(reg, inframe);
	odi_reg_write(ODI_GPON_USF_EQ_DELAY_OFF, reg);
}

/* Burst overhead: USF_BURST_HDR_SETUP.LENGTH bytes per burst, the OLT pattern byte then its
 * 3 delimiter bytes in USF_BURST_HDR_BYTE. Checked against two OLTs on the device:
 * guard 32 bits, no type 1/2 preamble, Extended_Burst_Length 0x29/0x12 gave
 * USF_BURST_HDR_SETUP.LENGTH 0x30 (pre-ranged) and 0x19 (ranged) = count + 4 + 3; the other
 * OLT pre-ranged count gave 0x39. Before any Extended_Burst_Length the
 * length is 0x0c on both.
 */
#define ODI_GPON_HW_BOH_LEN_WIDE	0x0cU	/* pre-ranging, no Extended_Burst_Length yet */
#define ODI_GPON_HW_BOH_LEN_RANGED	0x19U	/* ranged, no Extended_Burst_Length seen */
#define ODI_GPON_HW_BOH_LEN_MAX	0x3fU	/* USF_BURST_HDR_SETUP.LENGTH field width guard */

static void hw_write_boh(uint32_t len)
{
	uint32_t boh_cfg = 0;
	unsigned int i;

	if (len > ODI_GPON_HW_BOH_LEN_MAX)
		len = ODI_GPON_HW_BOH_LEN_MAX;
	boh_cfg = ODI_GPON_USF_BURST_HDR_SETUP_REPEAT_SET(boh_cfg, 8U);
	boh_cfg = ODI_GPON_USF_BURST_HDR_SETUP_LENGTH_SET(boh_cfg, len);
	odi_reg_write(ODI_GPON_USF_BURST_HDR_SETUP_OFF, boh_cfg);
	for (i = 0; i < 9U; i++)
		odi_reg_write(ODI_GPON_USF_BURST_HDR_BYTE(i), odi_gpon_hw.boh_pattern);
	for (i = 0; i < 3U; i++)
		odi_reg_write(ODI_GPON_USF_BURST_HDR_BYTE(9U + i), odi_gpon_hw.boh_delimiter[i]);
}

static void hw_set_ext_burst_length(uint8_t pre_ranged_bytes, uint8_t ranged_bytes)
{
	if (!odi_gpon_hw.boh_valid)
		return;	/* no Upstream_Overhead yet: nothing to extend */
	odi_gpon_hw.boh_ranged_len = (uint8_t)(odi_gpon_hw.boh_fixed_bytes + ranged_bytes);
	if (odi_gpon_hw.eqd_applied)
		hw_write_boh(odi_gpon_hw.boh_ranged_len);
	else
		hw_write_boh((uint32_t)odi_gpon_hw.boh_fixed_bytes + pre_ranged_bytes);
}

/* hw_apply_eqd() for an EqD that came from a Ranging_Time message, keeping
 * what it wrote for /proc/odi_gpon.
 */
static void hw_apply_ranging_eqd(uint32_t eqd_bits)
{
	hw_apply_eqd(eqd_bits);
	odi_gpon_hw.rng.valid = 1U;
	odi_gpon_hw.rng.eqd_bits = eqd_bits;
	odi_gpon_hw.rng.min_resp_delay = odi_gpon_hw.eqd_min_resp;
	odi_gpon_hw.rng.multframe = odi_gpon_hw.eqd_multframe;
	odi_gpon_hw.rng.inframe = odi_gpon_hw.eqd_inframe;
}

static void hw_set_eqd(void *ctx, uint32_t eqd_bits)
{
	(void)ctx;

	hw_apply_ranging_eqd(eqd_bits);

	/* Ranged bursts: the Extended_Burst_Length ranged count when the OLT
	 * sent one, else the default length (see hw_write_boh()).
	 */
	odi_gpon_hw.eqd_applied = 1;
	if (odi_gpon_hw.boh_valid)
		hw_write_boh(odi_gpon_hw.boh_ranged_len);
}

/* Caches the OLT pattern/delimiter bytes and the fixed part of each burst
 * (guard, type 1/2 preamble, delimiter), then writes the pre-ranging burst
 * overhead, BEFORE the O2->O3 transition (odi_gpon_fsm.c calls
 * set_upstream_overhead() before set_state()). An Extended_Burst_Length
 * from the OLT lengthens it later (hw_set_ext_burst_length()).
 */
static void hw_set_upstream_overhead(void *ctx, uint8_t guard_bits, uint8_t type1_preamble_bits,
				      uint8_t type2_preamble_bits, uint8_t type3_pattern,
				      const uint8_t delimiter[3], uint16_t preassigned_delay,
				      uint8_t power_level_mode)
{
	(void)ctx;
	(void)power_level_mode;

	odi_gpon_hw.boh_pattern = type3_pattern;
	memcpy(odi_gpon_hw.boh_delimiter, delimiter, 3U);
	odi_gpon_hw.boh_valid = 1;
	odi_gpon_hw.boh_fixed_bytes = (uint8_t)((guard_bits + type1_preamble_bits +
						 type2_preamble_bits + 7U) / 8U + 3U);
	odi_gpon_hw.boh_ranged_len = ODI_GPON_HW_BOH_LEN_RANGED;
	odi_gpon_hw.eqd_applied = 0;

	hw_write_boh(ODI_GPON_HW_BOH_LEN_WIDE);

	odi_reg_write(ODI_GPON_DSF_SETUP_OFF, odi_reg_read(ODI_GPON_DSF_SETUP_OFF));

	/* USF_EQ_DELAY is ALSO written here, before any ranging. G.984.3
	 * clause 9.2.3.1 lets the OLT pre-assign the equalization delay in
	 * Upstream_Overhead (units of 32 bytes, so 256 bits each); the FSM
	 * hands over 0 when the OLT did not enable it. The OLT then computes
	 * the Ranging_Time delay on the assumption that this ONU already
	 * applied the pre-assigned value, so ignoring it shifts every ranged
	 * burst by that amount. With no pre-assigned delay only the formula's
	 * own additive DELAY_HI term lands in the register (hw_apply_eqd()'s
	 * own comment has the evidence and why this is a second confirming
	 * data point, not a coincidence).
	 */
	hw_apply_eqd((uint32_t)preassigned_delay * 32U * 8U);
}

static void hw_send_us_ploam(void *ctx, const struct odi_gpon_ploam *msg)
{
	uint16_t words[6];
	uint32_t ind;
	unsigned int i;

	(void)ctx;

	odi_gpon_ploam_pack_words(msg, words);

	/* ODI_GPON_PLOAM_TYPE_REPLY (odi_gpon_hw.h) is the one MSG_TYPE value
	 * every software-driven reply this driver sends at run time uses
	 * (Acknowledge, Encryption_Key, Password) -- the three autonomous-
	 * hardware messages (Serial_Number_ONU, Dying_Gasp, No_Message) are
	 * pre-armed once by odi_gpon_init_apply(), never through this path.
	 */
	ind = 0;
	ind = ODI_GPON_USF_PLOAM_TX_CTL_MSG_TYPE_SET(ind, ODI_GPON_PLOAM_TYPE_REPLY);
	odi_reg_write(ODI_GPON_USF_PLOAM_TX_CTL_OFF, ind);

	for (i = 0; i < ODI_GPON_US_PLOAM_WORDS; i++)
		odi_reg_write(ODI_GPON_USF_PLOAM_TX_WORD(i), words[i]);

	ind = ODI_GPON_USF_PLOAM_TX_CTL_PUSH_SET(ind, 1);
	odi_reg_write(ODI_GPON_USF_PLOAM_TX_CTL_OFF, ind);

	odi_gpon_hw.ploam_tx_count++;
	if (odi_gpon_hw.ploam_log)
		odi_gpon_hw.ploam_log(1U, msg);
}

/* TO1/TO2 are software timers, not GPON MAC block registers -- arming/
 * disarming a real kernel timer for them is odi_gpon.c's own concern, not
 * a register leaf; these are deliberate no-ops here.
 */
static void hw_start_to1(void *ctx) { (void)ctx; }
static void hw_stop_to1(void *ctx) { (void)ctx; }
static void hw_start_to2(void *ctx) { (void)ctx; }
static void hw_stop_to2(void *ctx) { (void)ctx; }

/* Laser on/off gating is outside the 0x700000-0x706fff GPON MAC block --
 * USF_LASER_MARGIN (odi_gpon_hw.h) has timing fields, but not evidence for
 * exactly when/what this driver itself writes to them versus a fixed
 * hardware default; not reproduced, a deliberate no-op, not a
 * silently-forgotten leaf.
 */
static void hw_laser_enable(void *ctx, int enable) { (void)ctx; (void)enable; }

/* link_force_up: named for odi_gpon_fsm.c's own design (its header comment:
 * "forcing the PON port MAC link up entering O5, down leaving it"), but the
 * two switch-core registers this capture actually shows written at the
 * first O4->O5 transition are frame-length-accept registers, not a PHY/MAC
 * link-force bit -- MAX_FRAME_LEN_1 (0x011018, a global accepted-frame-
 * length profile) and PORT_MAX_FRAME_SEL port 2 (0x011010, the PON port --
 * odi_switch_init_platform()'s own port-numbering comment: UNI 0, PON 2, CPU
 * 3). Sequence exactly as captured (act capture, first activation): R
 * MAX_FRAME_LEN_1, W MAX_FRAME_LEN_1=0x7ef (2031 bytes), then
 * PORT_MAX_FRAME_SEL(2) read-then-written-1 twice with both accept bits
 * set, then a trailing MAX_FRAME_LEN_1 read -- replayed in that order, not
 * simplified to blind writes, since the reads carry no side effect this
 * driver depends on either way. MAX_FRAME_LEN_1 at its power-on 0 makes
 * every non-empty frame checked against that profile look oversize, so
 * the MAC drops it (odi_switch_hw.h).
 *
 * Leaving O5 (up == 0): MAX_FRAME_LEN_1 back to 0, undoing the write above.
 * The one activation capture available starts already active, so it never
 * shows a real "leaving O5" moment -- this half is inferred as the natural
 * undo, not itself captured at that exact transition. A same-shaped
 * MAX_FRAME_LEN_1=0 write does appear elsewhere on this board (the boot
 * capture, immediately after an identical up-side write during early
 * bring-up rather than at a protocol-driven O5 exit) -- independent
 * evidence this register is one the stock firmware toggles, not proof of
 * the exact down-side trigger point.
 */
static void hw_link_force_up(void *ctx, int up)
{
	uint32_t accept_max_len_ctrl2 = 0;

	(void)ctx;

	if (!up) {
		odi_reg_write(ODI_SW_MAX_FRAME_LEN_1_OFF, 0U);
		return;
	}

	accept_max_len_ctrl2 = ODI_SW_PORT_MAX_FRAME_SEL_GIGA_PROFILE_SET(accept_max_len_ctrl2, 1U);
	accept_max_len_ctrl2 = ODI_SW_PORT_MAX_FRAME_SEL_FE_PROFILE_SET(accept_max_len_ctrl2, 1U);

	(void)odi_reg_read(ODI_SW_MAX_FRAME_LEN_1_OFF);
	odi_reg_write(ODI_SW_MAX_FRAME_LEN_1_OFF,
		      ODI_SW_MAX_FRAME_LEN_1_BYTES_SET(0, 0x7efU));

	(void)odi_reg_read(ODI_SW_PORT_MAX_FRAME_SEL(2U));
	odi_reg_write(ODI_SW_PORT_MAX_FRAME_SEL(2U), accept_max_len_ctrl2);
	(void)odi_reg_read(ODI_SW_PORT_MAX_FRAME_SEL(2U));
	odi_reg_write(ODI_SW_PORT_MAX_FRAME_SEL(2U), accept_max_len_ctrl2);

	(void)odi_reg_read(ODI_SW_MAX_FRAME_LEN_1_OFF);
}

/* AES key word load (odi_gpon_hw.h's own 8-word/128-bit protocol) -- register
 * sequence exactly as captured (react.txt L791+): a
 * DSK_SWITCH_ARM bracket (ARM 0->1, ACTIVE_SLOT set
 * only when loading the CURRENT slot, clear when loading NEXT -- both
 * captures show this bit as the inverse of the target slot: ACTIVE_SLOT
 * = 1 for the CURRENT-slot zero-out at gpondev/gponact bring-up,
 * ACTIVE_SLOT = 0 for the real-key load into the NEXT slot at
 * Request_Key time) then, per word 0..7, data written before the index/
 * req pair that commits it. DSK_KEY_LOAD's own SLOT bit (bit 7) is
 * NEVER set by software in either capture, despite the header field
 * existing for it -- both the current-slot zero-out and the real key
 * load left it 0; slot selection happens entirely through the
 * ACTIVE_SLOT bracket above, not this bit. Documented here since it
 * contradicts what odi_gpon_hw.h's own field comment would suggest.
 */
static void hw_load_key(void *ctx, unsigned int slot, const uint8_t key[16])
{
	uint32_t reg;
	unsigned int i;

	(void)ctx;

	reg = 0;
	if (slot == ODI_GPON_DSK_SLOT_CURRENT)
		reg |= (1U << 14);	/* ACTIVE_SLOT, DSK_SWITCH_ARM -- see comment above */
	odi_reg_write(ODI_GPON_DSK_SWITCH_ARM_OFF, reg);
	reg = ODI_GPON_DSK_SWITCH_ARM_ARM_SET(reg, 1);
	odi_reg_write(ODI_GPON_DSK_SWITCH_ARM_OFF, reg);

	for (i = 0; i < ODI_GPON_DSK_KEY_WORDS; i++) {
		uint16_t word = ((uint16_t)key[2U * i] << 8) | (uint16_t)key[2U * i + 1U];
		uint32_t ind = 0;

		odi_reg_write(ODI_GPON_DSK_KEY_WORD_OFF,
			      ODI_GPON_DSK_KEY_WORD_KEY_BITS_SET(0, word));

		ind = ODI_GPON_DSK_KEY_LOAD_WORD_INDEX_SET(ind, i);
		odi_reg_write(ODI_GPON_DSK_KEY_LOAD_OFF, ind);
		ind = ODI_GPON_DSK_KEY_LOAD_WRITE_REQ_SET(ind, 1);
		odi_reg_write(ODI_GPON_DSK_KEY_LOAD_OFF, ind);
	}
}

/* Key_switching_time -- one plain write, the frame-count value straight
 * from the PLOAM content (react.txt L850ish: no
 * separate DSK_SWITCH_ARM write at this point -- that bracket is
 * load_key()'s own, above, already run for this key generation).
 *
 * Confirmed only written when the value actually changes:
 * Key_Switching_Time is read three times in react.txt like every other
 * downstream message, but DSK_SWITCH_FRAME itself is written only
 * ONCE (a direct grep of the capture shows exactly one W to this
 * address) -- the second and third dequeues send another Acknowledge
 * (odi_gpon_isr.c's own call) without re-arming an unchanged switch time.
 * Same shape as odi_gpon_hw_alloc_id_assign()'s own dedup, for the same
 * evidenced reason.
 */
static void hw_switch_key(void *ctx, uint32_t switch_superframe)
{
	(void)ctx;

	if (odi_gpon_hw.key_switch_armed && odi_gpon_hw.key_switch_superframe == switch_superframe)
		return;

	odi_reg_write(ODI_GPON_DSK_SWITCH_FRAME_OFF,
		      ODI_GPON_DSK_SWITCH_FRAME_SUPERFRAME_SET(0, switch_superframe));
	odi_gpon_hw.key_switch_superframe = switch_superframe;
	odi_gpon_hw.key_switch_armed = 1;
}

static void hw_flush_us_ploam_buf(void *ctx)
{
	uint32_t reg;

	(void)ctx;

	/* FLUSH toggled off then back on --
	 * react.txt: R 0x13 -> W 0x03 -> R 0x03 -> W 0x13 at Ranging_Time,
	 * i.e. this bit (and only this bit) cleared then set again
	 * (usPloamBuf_flush()).
	 */
	reg = odi_reg_read(ODI_GPON_USF_PLOAM_TX_SETUP_OFF);
	odi_reg_write(ODI_GPON_USF_PLOAM_TX_SETUP_OFF, ODI_GPON_USF_PLOAM_TX_SETUP_FLUSH_SET(reg, 0));
	reg = odi_reg_read(ODI_GPON_USF_PLOAM_TX_SETUP_OFF);
	odi_reg_write(ODI_GPON_USF_PLOAM_TX_SETUP_OFF, ODI_GPON_USF_PLOAM_TX_SETUP_FLUSH_SET(reg, 1));
}

static const struct odi_gpon_fsm_ops odi_gpon_hw_ops_table = {
	.set_state = hw_set_state,
	.set_onu_id = hw_set_onu_id,
	.set_eqd = hw_set_eqd,
	.set_upstream_overhead = hw_set_upstream_overhead,
	.send_us_ploam = hw_send_us_ploam,
	.start_to1 = hw_start_to1,
	.stop_to1 = hw_stop_to1,
	.start_to2 = hw_start_to2,
	.stop_to2 = hw_stop_to2,
	.laser_enable = hw_laser_enable,
	.load_key = hw_load_key,
	.switch_key = hw_switch_key,
	.link_force_up = hw_link_force_up,
	.flush_us_ploam_buf = hw_flush_us_ploam_buf,
};

const struct odi_gpon_fsm_ops *odi_gpon_hw_ops(void)
{
	return &odi_gpon_hw_ops_table;
}

/* Ranging_Time is read three times like every other downstream message,
 * and unlike Assign_Alloc-ID/Key_Switching_Time (whose repeats are pure
 * no-ops once already applied), react.txt shows the SECOND and THIRD
 * reads each re-write USF_EQ_DELAY with the SAME value -- but do NOT
 * repeat the USF_BURST_HDR_SETUP/USF_BURST_HDR_BYTE reconfigure, the USF_PLOAM_TX_SETUP flush bracket, or
 * the O4->O5 state write (all three only ever appear once, on the first
 * read). odi_gpon_fsm.c's own state gating already makes ops->set_eqd()
 * (which writes EQD *and* the BOH/flush/state side effects together, see
 * its own definition) run exactly once, on the transition -- this
 * function is the plain, no-side-effects EQD-only rewrite odi_gpon_isr.c
 * calls for the second and third reads, once the FSM is already in O5.
 */
void odi_gpon_hw_eqd_rewrite(uint32_t eqd_bits)
{
	hw_apply_ranging_eqd(eqd_bits);
}

/* ---- GEM port / Alloc-ID / Acknowledge -- not FSM ops, called directly
 * by odi_gpon_isr.c's own PLOAM dispatch for the message types
 * odi_gpon_fsm.c documents as out of its own scope.
 */

void odi_gpon_hw_ack_send(uint8_t onu_id, const struct odi_gpon_ploam *acked)
{
	uint8_t content9[9];
	struct odi_gpon_ploam reply;

	/* DMBYTE1..9 = the acknowledged message's own first 9 wire bytes
	 * (onu_id, type, content[0..6] -- G.984.3 clause 9.2.4.9, confirmed
	 * byte-for-byte against react.txt).
	 */
	content9[0] = acked->onu_id;
	content9[1] = acked->type;
	memcpy(&content9[2], acked->content, 7U);

	odi_gpon_encode_acknowledge(onu_id, acked->type, content9, &reply);
	hw_send_us_ploam(NULL, &reply);
}

/* Configure_Port-ID: the one GEM port row implemented here,
 * always ODI_GPON_HW_GEM_ROW_PRIMARY -- odi_switch_gpon_ds_
 * port_write() does the CAM add/TRAFFIC_CFG write (odi_switch_tbl.c,
 * already tested), then this leaf adds the two things that primitive
 * does not cover: the upstream mirror (US_GEM_PORT_MAP, a plain
 * array write, no CAM handshake) and the switch-core
 * stream-valid gate (0xf02144, odi_switch_hw.h ODI_SW_PONQ_STREAM_VALID) --
 * both written 1:1 with `activate` in the capture (react.txt L479-489 for
 * the on side, L34-37 for the off side at deactivate).
 */
void odi_gpon_hw_gem_port_configure(uint16_t gem_port_id, int activate)
{
	int already = odi_gpon_hw.gem_port_active && odi_gpon_hw.gem_port_id == gem_port_id;

	/* The port-table CAM add/TRAFFIC_CFG write repeats identically on
	 * all three reads of the SAME Configure_Port-ID (react.txt
	 * confirms it) -- unconditional, every call.
	 */
	odi_switch_gpon_ds_port_write(ODI_GPON_HW_GEM_ROW_PRIMARY, gem_port_id,
				       activate ? ODI_GPON_HW_TRAFFIC_CFG_PRIMARY : 0U);

	/* GEM_US_PORT_MAP and the switch-core stream-valid gate do NOT
	 * repeat, though -- react.txt shows each written exactly once, on
	 * the FIRST of the three reads only. Same asymmetry as odi_gpon_hw_
	 * alloc_id_assign()'s own CAM-vs-Acknowledge split, a different pair
	 * of registers this time.
	 */
	if (!already || !activate) {
		odi_reg_write(ODI_SW_US_GEM_PORT_MAP(ODI_GPON_HW_GEM_ROW_PRIMARY),
			       ODI_SW_US_GEM_PORT_MAP_GEM_PORT_SET(0, activate ? gem_port_id : 0U));
		odi_reg_write(ODI_SW_PONQ_STREAM_VALID(ODI_GPON_HW_PON_SID_INDEX),
			       ODI_SW_PONQ_STREAM_VALID_STREAM_ON_SET(0, activate ? 1U : 0U));
	}

	odi_gpon_hw.gem_port_id = gem_port_id;
	odi_gpon_hw.gem_port_active = activate;
}

/* Encrypted_Port-ID for the SAME Port-ID Configure_Port-ID already placed
 * at the primary row (the OMCC port, whose aes_en bit is left clear --
 * message says not-encrypted) is a plain re-write of TRAFFIC_CFG
 * at that row, handled below. Every OTHER GEM Port-ID -- the six ports
 * the OLT brings up right after O5 (react.txt
 * L1395-1812, one Encrypted_Port-ID each) -- is odi_switch's own DS slot,
 * populated at Configure_Port-ID time through
 * odi_switch_gpon_ds_port_write() (odi_switch_tbl.c) the same way the
 * OMCC row is, but this leaf never allocates a second GPON-block CAM row
 * for it (documented gap, no row-allocation policy the one available
 * capture determines -- see this function's own header history above);
 * its AES-enable bit instead goes through odi_switch_gpon_encrypt_port()
 * (odi_switch_ds_gem.c), the same DS-slot-indexed aes_en rewrite the automatic-
 * AES-enable PLOAM hook already uses on a SWITCH=vendor image, called
 * directly here instead of through that now-gone stock callback. No
 * captured evidence exists for `encrypted=1` on any port on this box
 * (every Encrypted_Port-ID this driver has ever seen sets the a-flag to
 * 0), so this path is forward-looking against the spec, not a
 * reproduction of a captured sequence. Returns
 * odi_switch_gpon_encrypt_port()'s own convention (0 =
 * applied, -1 = no DS slot recorded yet for this Port-ID) for that path,
 * or the OMCC-row convention below (1 = applied, 0 = not the OMCC row --
 * impossible now that the OMCC row falls through to the branch above).
 */
int odi_gpon_hw_gem_port_encrypted(uint16_t gem_port_id, int encrypted)
{
	unsigned long flags;
	uint32_t ind = 0;

	if (!odi_gpon_hw.gem_port_active || gem_port_id != odi_gpon_hw.gem_port_id)
		return odi_switch_gpon_encrypt_port(gem_port_id, encrypted);

	/* A search (MODE=2, no DSF_GEM_CAM_WDATA write) precedes the
	 * write every one of the three reads -- unlike Configure_Port-ID's own
	 * blind MODE=1-only write, react.txt shows this search step
	 * ahead of every Encrypted_Port-ID CAM touch (a real difference
	 * between how the stock firmware handles the two messages). The write
	 * itself DOES re-supply DSF_GEM_CAM_WDATA (unlike the search), so
	 * it is odi_switch_gpon_ds_port_write() again, the same primitive
	 * Configure_Port-ID uses. Search and write are one sequence on the
	 * GEM CAM, so odi_switch_dsf_lock (odi_switch.c) covers both.
	 */
	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	ind = ODI_SW_DSF_GEM_CAM_CTL_OP_SET(ind, 2);
	ind = ODI_SW_DSF_GEM_CAM_CTL_CAM_ROW_SET(ind, ODI_GPON_HW_GEM_ROW_PRIMARY);
	odi_reg_write(ODI_SW_DSF_GEM_CAM_CTL_OFF, ind);
	ind = ODI_SW_DSF_GEM_CAM_CTL_REQ_SET(ind, 1);
	odi_reg_write(ODI_SW_DSF_GEM_CAM_CTL_OFF, ind);

	(void)encrypted;	/* the one capture available never sets it -- nothing to OR in yet */
	__odi_switch_gpon_ds_port_write(ODI_GPON_HW_GEM_ROW_PRIMARY, gem_port_id,
					ODI_GPON_HW_TRAFFIC_CFG_PRIMARY);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
	return 1;
}

/* Assign_Alloc-ID: sequential pool from row 0,
 * confirmed exactly against the five rows react.txt shows (0..4, in
 * arrival order) -- skips ODI_GPON_HW_ALLOC_ROW_DEFAULT (the ONU-ID's own
 * fixed row, hw_set_onu_id() above). Idempotent per Alloc-ID -- confirmed
 * by a direct read of the capture: each Assign_Alloc-ID message is
 * read three times same as every other downstream message (G.984.3's own
 * "sent 3x" rule), but the CAM write only appears on the FIRST of the
 * three reads for a given Alloc-ID; the second and third re-dequeue the
 * same content and send another Acknowledge (odi_gpon_isr.c's own call,
 * not this function) without touching the CAM again. This differs from
 * Configure_Port-ID (odi_gpon_hw_gem_port_configure() above), whose own
 * CAM write DOES repeat identically on all three reads in the same
 * capture -- a real difference in how the stock firmware handles the two, not
 * something to paper over into one shared rule. Returns the row used
 * (existing or newly allocated), or -1 if the 32-row table (minus the one
 * reserved row) is exhausted.
 */
int odi_gpon_hw_alloc_id_assign(uint16_t alloc_id)
{
	unsigned int row;

	for (row = 0; row < ODI_GPON_HW_ALLOC_ROWS; row++) {
		if ((odi_gpon_hw.alloc_used & (1UL << row)) && odi_gpon_hw.alloc_id_values[row] == alloc_id)
			return (int)row;	/* already assigned -- no repeat CAM write */
	}

	for (row = 0; row < ODI_GPON_HW_ALLOC_ROWS; row++) {
		if (row == ODI_GPON_HW_ALLOC_ROW_DEFAULT)
			continue;
		if (!(odi_gpon_hw.alloc_used & (1UL << row)))
			break;
	}
	if (row == ODI_GPON_HW_ALLOC_ROWS)
		return -1;

	odi_switch_gpon_alloc_write(row, alloc_id);
	odi_gpon_hw.alloc_used |= (1UL << row);
	odi_gpon_hw.alloc_id_values[row] = alloc_id;
	return (int)row;
}

/* gponact verb (odi_gpon_drv.h's own comment has the full sequence this
 * replays). Order matches react.txt exactly (an earlier gponact table
 * built from the fresh-boot capture listed items
 * 1-3 -- the ONU-ID/state writes -- before items 4-8 -- AES/mask -- but
 * ALSO showed no separate top-mask write; the re-activation
 * capture shows no such write either; both corrections come from that
 * capture, not a disagreement with that earlier table, which did not have
 * this second capture detail when it was written):
 *
 *   1. ODI_GPON_EVENT_ACTIVATE then ODI_GPON_EVENT_LOS_CLEAR (the ONU-ID/
 *      state writes -- odi_gpon_fsm.c's own do_activate()/do_los_clear()
 *      produce the DS/US/DS write triple react.txt shows, unchanged
 *      values included, since gpondeact already left the registers at
 *      the values these calls re-assert).
 *   2. AES current-slot zero + key-switch-time clear.
 *   3. DS and US interrupt-mask ramps -- confirmed to be
 *      read-modify-write OR operations, one bit at a time, not fixed
 *      absolute values: the fresh-boot capture's own 0x00->0x04->0x84->
 *      0xa4 US-mask sequence and this capture's own three IDENTICAL
 *      0xa4 writes (the US mask was never touched by gpondeact, so it
 *      was already 0xa4 going in) only both make sense as the same
 *      OR-one-bit-in operation running from a different starting value
 *      -- a fixed-value-sequence model would have written 0x04/0x84/0xa4
 *      here too, which react.txt does not show.
 *
 * No PONMAC_IRQ_ENABLE (top mask) write happens anywhere in this sequence in
 * this capture -- gpondeact's own closing bracket already left it at
 * 0x22, and gponact re-run here never touches it again (confirmed by a
 * direct grep of the capture window: the only 0x700040 access in it is a
 * READ, at the first post-activation interrupt pass).
 */
void odi_gpon_verb_activate(struct odi_gpon_fsm *fsm)
{
	static const unsigned int ds_mask_bits[] = { 10U, 9U, 8U, 3U, 2U, 1U, 0U };
	static const unsigned int us_mask_bits[] = { 2U, 7U, 5U };
	unsigned int i;
	uint32_t reg;

	odi_gpon_fsm_handle_event(fsm, &odi_gpon_hw_ops_table, NULL, ODI_GPON_EVENT_ACTIVATE, NULL);
	odi_gpon_fsm_handle_event(fsm, &odi_gpon_hw_ops_table, NULL, ODI_GPON_EVENT_LOS_CLEAR, NULL);

	{
		uint8_t zero_key[16];

		memset(zero_key, 0, sizeof(zero_key));
		hw_load_key(NULL, ODI_GPON_DSK_SLOT_CURRENT, zero_key);
	}
	odi_reg_write(ODI_GPON_DSK_SWITCH_FRAME_OFF, 0U);

	reg = odi_reg_read(ODI_GPON_DSF_IRQ_ENABLE_OFF);
	for (i = 0; i < sizeof(ds_mask_bits) / sizeof(ds_mask_bits[0]); i++) {
		reg |= (1U << ds_mask_bits[i]);
		odi_reg_write(ODI_GPON_DSF_IRQ_ENABLE_OFF, reg);
	}

	reg = odi_reg_read(ODI_GPON_USF_IRQ_ENABLE_OFF);
	for (i = 0; i < sizeof(us_mask_bits) / sizeof(us_mask_bits[0]); i++) {
		reg |= (1U << us_mask_bits[i]);
		odi_reg_write(ODI_GPON_USF_IRQ_ENABLE_OFF, reg);
	}
}

/* gpondeact verb (odi_gpon_drv.h's own comment has the full sequence). Goes
 * straight to O1 -- not through odi_gpon_fsm.c's own ODI_GPON_EVENT_
 * DEACTIVATE (which lands in O2, the correct effect for a real Deactivate_
 * ONU-ID PLOAM, clause 9.2.3.5) -- because this is the administrative
 * "local force-deactivate" path, genuinely different from the
 * protocol-driven one, so it manipulates
 * fsm->state/onu_id directly rather than through an FSM event.
 */
/* Alloc-ID/T-CONT CAM row delete -- MODE=3 (react.txt L10-27: IND values
 * 0x300+idx / 0x8300+idx, never MODE=1 as odi_switch_gpon_alloc_write()
 * uses for an add/write -- a genuinely different op code, not that
 * primitive reused backwards) and, unlike an add, no ALLOC_WR data write
 * at all. odi_switch_tbl.c has no primitive for this mode (it was only
 * ever exercised for the write side up to now), so this is its own
 * small leaf rather than a misuse of odi_switch_gpon_alloc_write().
 */
static void hw_alloc_id_delete(unsigned int row)
{
	uint32_t ind = 0;

	ind = ODI_SW_DSF_ALLOC_CAM_CTL_OP_SET(ind, 3);
	ind = ODI_SW_DSF_ALLOC_CAM_CTL_CAM_ROW_SET(ind, row);
	odi_reg_write(ODI_SW_DSF_ALLOC_CAM_CTL_OFF, ind);
	ind = ODI_SW_DSF_ALLOC_CAM_CTL_REQ_SET(ind, 1);
	odi_reg_write(ODI_SW_DSF_ALLOC_CAM_CTL_OFF, ind);
}

/* Assign_Alloc-ID with type 255 (G.984.3 9.2.3.9, "deallocate"): the OLT
 * takes that Alloc-ID back. Its CAM row is deleted with the same MODE=3
 * leaf gpondeact uses, under the same lock, and the row is free for the
 * next assignment. The default (ONU-ID) row is never released here: it
 * belongs to the ONU-ID, not to an Assign_Alloc-ID. An Alloc-ID this
 * driver does not hold is not an error, the OLT repeats every message.
 * Returns the row released, or -1.
 */
int odi_gpon_hw_alloc_id_release(uint16_t alloc_id)
{
	unsigned long flags;
	unsigned int row;

	for (row = 0; row < ODI_GPON_HW_ALLOC_ROWS; row++) {
		if (row == ODI_GPON_HW_ALLOC_ROW_DEFAULT)
			continue;
		if ((odi_gpon_hw.alloc_used & (1UL << row)) &&
		    odi_gpon_hw.alloc_id_values[row] == alloc_id)
			break;
	}
	if (row == ODI_GPON_HW_ALLOC_ROWS)
		return -1;

	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	hw_alloc_id_delete(row);
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);
	odi_gpon_hw.alloc_used &= ~(1UL << row);
	odi_gpon_hw.alloc_id_values[row] = 0;
	return (int)row;
}

/* GEM downstream port-ID CAM row delete -- same MODE=3 story as the
 * Alloc-ID delete above (react.txt L28-29: IND 0x340/0x8340, no PORT_WR
 * write), against DSF_GEM_CAM_CTL instead.
 */
static void hw_gem_port_delete(unsigned int row)
{
	uint32_t ind = 0;

	ind = ODI_SW_DSF_GEM_CAM_CTL_OP_SET(ind, 3);
	ind = ODI_SW_DSF_GEM_CAM_CTL_CAM_ROW_SET(ind, row);
	odi_reg_write(ODI_SW_DSF_GEM_CAM_CTL_OFF, ind);
	ind = ODI_SW_DSF_GEM_CAM_CTL_REQ_SET(ind, 1);
	odi_reg_write(ODI_SW_DSF_GEM_CAM_CTL_OFF, ind);
}

void odi_gpon_verb_deactivate(struct odi_gpon_fsm *fsm)
{
	unsigned long flags;
	uint32_t reg;
	unsigned int row;

	odi_reg_write(ODI_GPON_PONMAC_IRQ_ENABLE_OFF, 0U);
	odi_reg_write(ODI_GPON_DSF_IRQ_ENABLE_OFF, 0U);
	odi_reg_write(ODI_GPON_DSF_IRQ_ENABLE_OFF, 0U);
	odi_reg_write(ODI_GPON_PONMAC_IRQ_ENABLE_OFF, 0x22U);

	/* State O5 -> a transient value (state + 1) -> O1, exactly as
	 * captured (react.txt L7-9: 0x1a05 -> 0x1a06 -> 0x1a01) -- the
	 * transient write purpose is not resolved (unexplained); reproduced
	 * as observed, not silently smoothed over into a single write.
	 */
	reg = odi_reg_read(ODI_GPON_DSF_ONU_STATE_OFF);
	odi_reg_write(ODI_GPON_DSF_ONU_STATE_OFF,
		       ODI_GPON_DSF_ONU_STATE_ACTIVATION_STATE_SET(reg, (uint32_t)fsm->state + 1U));
	reg = odi_reg_read(ODI_GPON_DSF_ONU_STATE_OFF);
	odi_reg_write(ODI_GPON_DSF_ONU_STATE_OFF,
		       ODI_GPON_DSF_ONU_STATE_ACTIVATION_STATE_SET(reg, (uint32_t)ODI_GPON_STATE_O1));

	/* The CAM deletes and the DSF_GEM_FLOW_TYPE clear are DSF state the
	 * process-context GEM flow path shares: odi_switch_dsf_lock
	 * (odi_switch.c), released before hw_set_onu_id() below, which can
	 * reach odi_switch_gpon_alloc_write() and take it itself.
	 */
	spin_lock_irqsave(&odi_switch_dsf_lock, flags);
	for (row = 0; row < ODI_GPON_HW_ALLOC_ROWS; row++) {
		if (odi_gpon_hw.alloc_used & (1UL << row))
			hw_alloc_id_delete(row);
	}
	odi_gpon_hw.alloc_used = 0U;

	if (odi_gpon_hw.gem_port_active) {
		hw_gem_port_delete(ODI_GPON_HW_GEM_ROW_PRIMARY);
		odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(ODI_GPON_HW_GEM_ROW_PRIMARY),
			       ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_SET(0, 0));
		odi_reg_write(ODI_SW_US_GEM_PORT_MAP(ODI_GPON_HW_GEM_ROW_PRIMARY),
			       ODI_SW_US_GEM_PORT_MAP_GEM_PORT_SET(0, 0));
		odi_reg_write(ODI_SW_PONQ_STREAM_VALID(ODI_GPON_HW_PON_SID_INDEX),
			       ODI_SW_PONQ_STREAM_VALID_STREAM_ON_SET(0, 0));
		odi_gpon_hw.gem_port_active = 0;
	}
	spin_unlock_irqrestore(&odi_switch_dsf_lock, flags);

	hw_set_onu_id(NULL, ODI_GPON_ONU_ID_BROADCAST);

	fsm->state = ODI_GPON_STATE_O1;
	fsm->onu_id = 0U;
}

void odi_gpon_get_eqd(uint32_t *multframe, uint32_t *inframe)
{
	*multframe = odi_gpon_hw.eqd_multframe;
	*inframe = odi_gpon_hw.eqd_inframe;
}

void odi_gpon_get_ranging_dbg(struct odi_gpon_ranging_dbg *out)
{
	*out = odi_gpon_hw.rng;
}

unsigned int odi_gpon_get_alloc_ids(uint16_t *out, unsigned int max)
{
	unsigned int row, n = 0;

	for (row = 0; row < ODI_GPON_HW_ALLOC_ROWS && n < max; row++) {
		if (row == ODI_GPON_HW_ALLOC_ROW_DEFAULT)
			continue;
		if (odi_gpon_hw.alloc_used & (1UL << row))
			out[n++] = odi_gpon_hw.alloc_id_values[row];
	}
	return n;
}

void odi_gpon_get_ploam_counts(unsigned int *rx, unsigned int *tx)
{
	*rx = odi_gpon_hw.ploam_rx_count;
	*tx = odi_gpon_hw.ploam_tx_count;
}

void odi_gpon_hw_note_ploam_rx(const struct odi_gpon_ploam *msg)
{
	odi_gpon_hw.ploam_rx_count++;
	if (odi_gpon_hw.ploam_log)
		odi_gpon_hw.ploam_log(0U, msg);
}

void odi_gpon_hw_note_ber_interval(uint32_t interval_frames)
{
	odi_gpon_hw.ber_interval_frames = interval_frames;
}

uint32_t odi_gpon_hw_get_ber_interval_frames(void)
{
	return odi_gpon_hw.ber_interval_frames;
}

void odi_gpon_hw_send_us_ploam(const struct odi_gpon_ploam *msg)
{
	hw_send_us_ploam(NULL, msg);
}

void odi_gpon_hw_set_ploam_log(odi_gpon_ploam_log_fn fn)
{
	odi_gpon_hw.ploam_log = fn;
}

/* Extended_Burst_Length (G.984.3 clause 9.2.3.20) content: the type 3
 * preamble byte counts before ranging and once ranged. Applied through
 * hw_set_ext_burst_length() (USF_BURST_HDR_SETUP.LENGTH = count + the fixed guard,
 * type 1/2 preamble and delimiter bytes).
 */
void odi_gpon_hw_ext_burst_length(uint8_t pre_ranged_preamble_bytes, uint8_t post_o5_preamble_bytes)
{
	ODI_GPON_HW_LOG("Ext_Burst_Length: pre-ranged %u, ranged %u type 3 preamble bytes\n",
			(unsigned int)pre_ranged_preamble_bytes, (unsigned int)post_o5_preamble_bytes);
	hw_set_ext_burst_length(pre_ranged_preamble_bytes, post_o5_preamble_bytes);
}
