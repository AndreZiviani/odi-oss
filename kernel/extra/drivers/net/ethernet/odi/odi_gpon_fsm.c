// SPDX-License-Identifier: GPL-2.0
/*
 * odi_gpon_fsm.c -- implementation of the O1-O7 ONU activation state
 * machine declared in odi_gpon_fsm.h. Plain C, no kernel dependency.
 *
 * Every transition below is cited to ITU-T G.984.3 Table 10-1 (clause
 * 10.2.4) unless marked otherwise (see docs/REFERENCES.md for the
 * edition and how to fetch a local copy of the standard) -- the standard
 * text is the primary source, not the author's own recollection.
 *
 * Design notes not repeated at every call site:
 *
 * - ONU-ID address filtering for a directed message (checking msg->onu_id
 *   against the ID this FSM was assigned, or against
 *   ODI_GPON_ONU_ID_BROADCAST) is applied here, inline, exactly where
 *   Table 10-1 says "Match ONU-ID?".
 * - The switch-fabric link_force_up() calls are this driver's own design,
 *   a step beyond the base standard (forcing the PON port MAC link up
 *   entering O5, down leaving it) -- G.984.3 itself says nothing about a
 *   switch fabric, since GEM/T-CONT forwarding is this SoC's own
 *   architecture, not part of the GTC/PLOAM layer.
 * - The driver-level ODI_GPON_EVENT_ACTIVATE/_DEACTIVATE commands are not
 *   G.984.3 events. ACTIVATE is this driver's own full reset to O1.
 *   DEACTIVATE is implemented as exactly what a Deactivate_ONU-ID PLOAM
 *   does (clause 9.2.3.5, Table 10-1's own Deactivate row) -- valid only from
 *   O4/O5/O6, forcing O2, a no-op everywhere else.
 * - The SN-request and ranging-request "bandwidth-map grant" events
 *   (Table 10-1's own rows for those, clause 10.2.5.2) are not PLOAM
 *   messages -- this driver's own FSM event set (odi_gpon_fsm.h) has no
 *   distinct bandwidth-grant event, so it abstracts both as the synthetic
 *   ODI_GPON_SN_REQUEST_GRANT message type carried over the ordinary
 *   ODI_GPON_EVENT_PLOAM_RX event (odi_gpon_ploam.h documents the same
 *   abstraction at its definition).
 * - The Ranging_Time message's own 32-bit decoded eqd value is handed to
 *   ops->set_eqd() raw (bits, MSB first) rather than split here into
 *   USF_EQ_DELAY's own MULTFRAME/INFRAME fields -- the real split needs a
 *   USF_MIN_RESP_DELAY register READ (odi_gpon_hw.c's own derived
 *   formula, confirmed against two activation captures), which this pure
 *   FSM has no access to; splitting on an unconfirmed fixed bit boundary
 *   here (an earlier draft of this file did exactly that) would have been
 *   this author's own guess dressed up as a fact.
 * - The Serial_Number_ONU reply is NOT sent by this FSM at all, on the
 *   ODI_GPON_SN_REQUEST_GRANT synthetic event or otherwise: hardware
 *   sends it autonomously once the real serial number is pre-armed into
 *   the upstream PLOAM FIFO's own dedicated slot at driver bring-up
 *   (the `gpondev`/`gponsn` PON steps, odi_gpon_init.c), with zero
 *   software involvement per grant. ODI_GPON_SN_REQUEST_GRANT
 *   (odi_gpon_ploam.h) is kept defined for the O3/O4 Table 10-1
 *   bandwidth-map-grant citation it documents, but this FSM no longer
 *   dispatches on it.
 */
#include "odi_gpon_fsm.h"
#include "odi_gpon_hw.h"	/* ODI_GPON_DSK_SLOT_NEXT -- a slot id, no register access from here */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static int odi_gpon_serial_number_eq(const struct odi_gpon_fsm *fsm, const uint8_t sn[8])
{
	return memcmp(fsm->serial_number, sn, 8U) == 0;
}

static int odi_gpon_onu_id_matches(const struct odi_gpon_fsm *fsm, uint8_t onu_id)
{
	return onu_id == fsm->onu_id || onu_id == ODI_GPON_ONU_ID_BROADCAST;
}

/* Stops whatever timer is running for the state being left, if any. Safe
 * to call from any state.
 */
static void odi_gpon_fsm_stop_timers_for_state(const struct odi_gpon_fsm *fsm,
						const struct odi_gpon_fsm_ops *ops, void *ctx)
{
	if (fsm->state == ODI_GPON_STATE_O3 || fsm->state == ODI_GPON_STATE_O4)
		ops->stop_to1(ctx);
	else if (fsm->state == ODI_GPON_STATE_O6)
		ops->stop_to2(ctx);
}

static void odi_gpon_fsm_send_password(const struct odi_gpon_fsm *fsm,
					const struct odi_gpon_fsm_ops *ops, void *ctx)
{
	struct odi_gpon_ploam reply;

	odi_gpon_encode_password(fsm->onu_id, fsm->password, &reply);
	ops->send_us_ploam(ctx, &reply);
}

void odi_gpon_fsm_init(struct odi_gpon_fsm *fsm)
{
	memset(fsm, 0, sizeof(*fsm));
	fsm->state = ODI_GPON_STATE_O1;
	fsm->key_index = ODI_GPON_KEY_INDEX_DEFAULT;
}

void odi_gpon_fsm_set_serial_number(struct odi_gpon_fsm *fsm, const uint8_t serial_number[8])
{
	memcpy(fsm->serial_number, serial_number, 8U);
}

void odi_gpon_fsm_set_password(struct odi_gpon_fsm *fsm, const uint8_t password[10])
{
	memcpy(fsm->password, password, ODI_GPON_PLOAM_CONTENT_LEN);
}

void odi_gpon_fsm_set_aes_key(struct odi_gpon_fsm *fsm, const uint8_t key[16])
{
	memcpy(fsm->aes_key, key, ODI_GPON_AES_KEY_BYTES);
}

/* ---- Event handlers ---- */

/* Driver-level full reset (not itself a G.984.3 event). */
static void odi_gpon_fsm_do_activate(struct odi_gpon_fsm *fsm, const struct odi_gpon_fsm_ops *ops,
				      void *ctx)
{
	if (fsm->state == ODI_GPON_STATE_O5)
		ops->link_force_up(ctx, 0);
	odi_gpon_fsm_stop_timers_for_state(fsm, ops, ctx);

	ops->laser_enable(ctx, 0);
	fsm->onu_id = 0;
	/* The wire/register value for "no ONU-ID assigned" is the G.984.3
	 * broadcast ID (0xff, ODI_GPON_ONU_ID_BROADCAST), confirmed by every
	 * capture this driver has seen, including boot's own O1 entry -- not
	 * the field's own zero value, which is this struct's own separate
	 * "nothing assigned yet" software sentinel, kept as 0 for fsm->onu_id
	 * itself, not written to hardware.
	 */
	ops->set_onu_id(ctx, ODI_GPON_ONU_ID_BROADCAST);
	fsm->state = ODI_GPON_STATE_O1;
	ops->set_state(ctx, ODI_GPON_STATE_O1);
}

/* Deactivate_ONU-ID's own effect (clause 9.2.3.5's own "Effect of receipt":
 * laser off, ONU-ID/OMCI-Port-ID/Alloc-IDs discarded, => Standby) and
 * Table 10-1's own Deactivate row (defined only from Ranging/Operation/POPUP
 * -- O2/O3/O1/O7 are dashes, a no-op). Shared by the PLOAM handler and
 * the ODI_GPON_EVENT_DEACTIVATE driver command.
 */
static void odi_gpon_fsm_apply_deactivate(struct odi_gpon_fsm *fsm,
					   const struct odi_gpon_fsm_ops *ops, void *ctx)
{
	if (fsm->state != ODI_GPON_STATE_O4 && fsm->state != ODI_GPON_STATE_O5 &&
	    fsm->state != ODI_GPON_STATE_O6)
		return;

	if (fsm->state == ODI_GPON_STATE_O4)
		ops->stop_to1(ctx);
	else if (fsm->state == ODI_GPON_STATE_O5)
		ops->link_force_up(ctx, 0);
	else
		ops->stop_to2(ctx);

	ops->laser_enable(ctx, 0);
	fsm->onu_id = 0;
	ops->set_onu_id(ctx, ODI_GPON_ONU_ID_BROADCAST);	/* see odi_gpon_fsm_do_activate()'s own comment */
	fsm->state = ODI_GPON_STATE_O2;
	ops->set_state(ctx, ODI_GPON_STATE_O2);
}

/* Table 10-1, "Timer TO1 expires" row: Serial_Number(O3) and Ranging(O4)
 * both => O2.
 */
static void odi_gpon_fsm_do_to1_expire(struct odi_gpon_fsm *fsm, const struct odi_gpon_fsm_ops *ops,
					void *ctx)
{
	if (fsm->state != ODI_GPON_STATE_O3 && fsm->state != ODI_GPON_STATE_O4)
		return;	/* stale/unrelated expiry, nothing to do */

	ops->stop_to1(ctx);
	fsm->state = ODI_GPON_STATE_O2;
	ops->set_state(ctx, ODI_GPON_STATE_O2);
}

/* Table 10-1, "Timer TO2 expires" row: POPUP(O6) => O1 (not O5 -- the ONU
 * could not reacquire sync in time, so it falls all the way back to
 * Initial, clause 10.2.2 f)).
 */
static void odi_gpon_fsm_do_to2_expire(struct odi_gpon_fsm *fsm, const struct odi_gpon_fsm_ops *ops,
					void *ctx)
{
	if (fsm->state != ODI_GPON_STATE_O6)
		return;

	ops->stop_to2(ctx);
	fsm->state = ODI_GPON_STATE_O1;
	ops->set_state(ctx, ODI_GPON_STATE_O1);
}

/* Table 10-1, "ONU detects LOS or LOF" row: Standby(O2) => O1;
 * Serial_Number(O3) stops TO1 => O1; Ranging(O4) stops TO1 => O1;
 * Operation(O5) ceases upstream tx, starts TO2, => O6. O1/O6/O7 are
 * dashes (O6's own LOS handling is the PSync-recovery row instead, see
 * odi_gpon_fsm_do_los_clear()).
 */
static void odi_gpon_fsm_do_los(struct odi_gpon_fsm *fsm, const struct odi_gpon_fsm_ops *ops,
				 void *ctx)
{
	switch (fsm->state) {
	case ODI_GPON_STATE_O2:
		fsm->state = ODI_GPON_STATE_O1;
		ops->set_state(ctx, ODI_GPON_STATE_O1);
		break;
	case ODI_GPON_STATE_O3:
	case ODI_GPON_STATE_O4:
		ops->stop_to1(ctx);
		fsm->state = ODI_GPON_STATE_O1;
		ops->set_state(ctx, ODI_GPON_STATE_O1);
		break;
	case ODI_GPON_STATE_O5:
		ops->link_force_up(ctx, 0);
		ops->start_to2(ctx);
		fsm->state = ODI_GPON_STATE_O6;
		ops->set_state(ctx, ODI_GPON_STATE_O6);
		break;
	case ODI_GPON_STATE_O1:
	case ODI_GPON_STATE_O6:
	case ODI_GPON_STATE_O7:
		break;	/* dashes in Table 10-1 */
	}
}

/* Table 10-1, "ONU achieves PSync synchronization" row: Init(O1) clears
 * LOS/LOF => O2; POPUP(O6) remains in O6 (resumes PCBd processing, per
 * clause 10.2.2 f) a no-op transition rather than a distinct target
 * state -- O6 has its own separate LOS-clear handling, distinct from O1
 * own handling above). All other states are dashes.
 */
static void odi_gpon_fsm_do_los_clear(struct odi_gpon_fsm *fsm, const struct odi_gpon_fsm_ops *ops,
				       void *ctx)
{
	if (fsm->state != ODI_GPON_STATE_O1)
		return;	/* O6: no-op by design (see comment above); everything else: dash */

	ops->laser_enable(ctx, 1);
	fsm->state = ODI_GPON_STATE_O2;
	ops->set_state(ctx, ODI_GPON_STATE_O2);
}

/* Table 10-1, Disable_Serial_Number rows (clause 9.2.3.6's own content
 * codes: 0xFF disable, 0x00 enable, 0x0F enable-all). "disable" applies
 * from Standby/Serial_Number/Ranging/Operation/POPUP (O2-O6), matching
 * serial number, and stops whatever timer that state was running before
 * moving to Emergency Stop (O7) and shutting the laser off. "enable"
 * applies only from Emergency Stop (O7), moving to Standby (O2, "All
 * parameters ... are re-examined", clause 10.2.2 g)) and turning the
 * laser back on. ODI_GPON_DISABLE_SN_ENABLE_ALL (broadcast re-enable,
 * ignoring the serial number bytes) is decoded but not acted on by this
 * pass -- out of scope for now.
 * Returns 1 if this message was recognized (caller should not fall
 * through to per-state dispatch), 0 otherwise.
 */
static int odi_gpon_fsm_handle_disable_sn(struct odi_gpon_fsm *fsm,
					   const struct odi_gpon_fsm_ops *ops, void *ctx,
					   const struct odi_gpon_ploam *msg)
{
	struct odi_gpon_ds_disable_serial_number dsn;

	if (msg->type != ODI_GPON_DS_DISABLE_SERIAL_NUMBER &&
	    msg->type != ODI_GPON_DS_DISABLE_SERIAL_NUMBER_ZTE)
		return 0;

	odi_gpon_decode_disable_serial_number(msg, &dsn);

	if (dsn.code == ODI_GPON_DISABLE_SN_DISABLE) {
		if (fsm->state == ODI_GPON_STATE_O1 || fsm->state == ODI_GPON_STATE_O7)
			return 1;	/* dashes in Table 10-1 */
		if (!odi_gpon_serial_number_eq(fsm, dsn.serial_number))
			return 1;	/* addressed to a different ONU */

		if (fsm->state == ODI_GPON_STATE_O5)
			ops->link_force_up(ctx, 0);
		odi_gpon_fsm_stop_timers_for_state(fsm, ops, ctx);
		ops->laser_enable(ctx, 0);
		fsm->state = ODI_GPON_STATE_O7;
		ops->set_state(ctx, ODI_GPON_STATE_O7);
	} else if (dsn.code == ODI_GPON_DISABLE_SN_ENABLE) {
		if (fsm->state != ODI_GPON_STATE_O7)
			return 1;	/* "enable" is only defined from Emergency Stop */
		if (!odi_gpon_serial_number_eq(fsm, dsn.serial_number))
			return 1;

		ops->laser_enable(ctx, 1);
		fsm->state = ODI_GPON_STATE_O2;
		ops->set_state(ctx, ODI_GPON_STATE_O2);
	}
	/* ODI_GPON_DISABLE_SN_ENABLE_ALL: decoded, not acted on. */
	return 1;
}

static void odi_gpon_fsm_do_ploam_rx(struct odi_gpon_fsm *fsm, const struct odi_gpon_fsm_ops *ops,
				      void *ctx, const struct odi_gpon_ploam *msg)
{
	struct odi_gpon_ds_upstream_overhead boh;
	struct odi_gpon_ds_assign_onu_id assign;
	struct odi_gpon_ds_ranging_time ranging;
	struct odi_gpon_ds_key_switching_time kst;
	struct odi_gpon_ploam reply;
	unsigned int i, rep;

	if (odi_gpon_fsm_handle_disable_sn(fsm, ops, ctx, msg))
		return;

	if (msg->type == ODI_GPON_DS_DEACTIVATE_ONU_ID) {
		if (odi_gpon_onu_id_matches(fsm, msg->onu_id))
			odi_gpon_fsm_apply_deactivate(fsm, ops, ctx);
		return;
	}

	switch (fsm->state) {
	case ODI_GPON_STATE_O1:
	case ODI_GPON_STATE_O7:
		break;	/* no PLOAM expected/acted on in these states beyond disable/enable above */

	case ODI_GPON_STATE_O2:
		if (msg->type == ODI_GPON_DS_UPSTREAM_OVERHEAD) {
			odi_gpon_decode_upstream_overhead(msg, &boh);
			ops->set_upstream_overhead(ctx, boh.guard_bits, boh.type1_preamble_bits,
						    boh.type2_preamble_bits, boh.type3_pattern,
						    boh.delimiter, boh.preassigned_delay,
						    boh.power_level_mode);
			ops->start_to1(ctx);
			fsm->state = ODI_GPON_STATE_O3;
			ops->set_state(ctx, ODI_GPON_STATE_O3);
		}
		break;

	case ODI_GPON_STATE_O3:
		/* No case for ODI_GPON_SN_REQUEST_GRANT here (see this file's own
		 * header comment): hardware answers the SN-request grant on
		 * its own, from the FIFO slot armed at bring-up.
		 */
		if (msg->type == ODI_GPON_DS_ASSIGN_ONU_ID) {
			odi_gpon_decode_assign_onu_id(msg, &assign);
			if (!odi_gpon_serial_number_eq(fsm, assign.serial_number))
				break;	/* addressed to a different ONU */
			fsm->onu_id = assign.onu_id;
			ops->set_onu_id(ctx, assign.onu_id);
			fsm->state = ODI_GPON_STATE_O4;
			ops->set_state(ctx, ODI_GPON_STATE_O4);
		}
		break;

	case ODI_GPON_STATE_O4:
		/* No case for ODI_GPON_SN_REQUEST_GRANT here either, same
		 * reason as O3 above.
		 */
		if (msg->type == ODI_GPON_DS_RANGING_TIME) {
			odi_gpon_decode_ranging_time(msg, &ranging);
			/* This driver's own O4->O5 sequence: stop TO1, apply
			 * EqD, flush the pre-ranging US PLOAM buffer, set
			 * state, then force the port link up.
			 */
			ops->stop_to1(ctx);
			ops->set_eqd(ctx, ranging.eqd);
			ops->flush_us_ploam_buf(ctx);
			fsm->state = ODI_GPON_STATE_O5;
			ops->set_state(ctx, ODI_GPON_STATE_O5);
			ops->link_force_up(ctx, 1);
		} else if (msg->type == ODI_GPON_DS_REQUEST_PASSWORD) {
			odi_gpon_fsm_send_password(fsm, ops, ctx);
		}
		break;

	case ODI_GPON_STATE_O5:
		if (msg->type == ODI_GPON_DS_REQUEST_KEY) {
			/* Both fragments, each sent 3x, alternating (0,1,0,1,0,1),
			 * not 3 of fragment 0 followed by 3 of fragment 1.
			 * Loaded into hardware's own inactive key slot only AFTER
			 * all six sends -- ops->load_key(), not part of this
			 * loop.
			 */
			for (rep = 0; rep < 3U; rep++) {
				for (i = 0; i < ODI_GPON_KEY_FRAGMENTS; i++) {
					odi_gpon_encode_encryption_key_fragment(fsm->onu_id,
										 fsm->key_index,
										 fsm->aes_key, i,
										 &reply);
					ops->send_us_ploam(ctx, &reply);
				}
			}
			ops->load_key(ctx, ODI_GPON_DSK_SLOT_NEXT, fsm->aes_key);
		} else if (msg->type == ODI_GPON_DS_KEY_SWITCHING_TIME) {
			odi_gpon_decode_key_switching_time(msg, &kst);
			ops->switch_key(ctx, kst.switch_superframe);
		} else if (msg->type == ODI_GPON_DS_REQUEST_PASSWORD) {
			odi_gpon_fsm_send_password(fsm, ops, ctx);
		}
		/* Encrypted_Port-ID, Configure_Port-ID, Assign_Alloc-ID,
		 * BER_interval, Change_Power_Level and the rest are GEM/
		 * T-CONT/alarm/power-level programming, not an FSM state
		 * concern -- not handled here by design. POPUP has no
		 * defined effect from Operation per Table 10-1 (LOS/LOF, not
		 * a PLOAM, is what enters O6).
		 */
		break;

	case ODI_GPON_STATE_O6:
		if (msg->type == ODI_GPON_DS_POPUP) {
			if (msg->onu_id == ODI_GPON_ONU_ID_BROADCAST) {
				ops->stop_to2(ctx);
				ops->start_to1(ctx);
				fsm->state = ODI_GPON_STATE_O4;
				ops->set_state(ctx, ODI_GPON_STATE_O4);
			} else if (msg->onu_id == fsm->onu_id) {
				ops->stop_to2(ctx);
				fsm->state = ODI_GPON_STATE_O5;
				ops->set_state(ctx, ODI_GPON_STATE_O5);
			}
		}
		break;
	}
}

void odi_gpon_fsm_handle_event(struct odi_gpon_fsm *fsm, const struct odi_gpon_fsm_ops *ops,
				void *ctx, enum odi_gpon_event event,
				const struct odi_gpon_ploam *msg)
{
	switch (event) {
	case ODI_GPON_EVENT_ACTIVATE:
		odi_gpon_fsm_do_activate(fsm, ops, ctx);
		break;
	case ODI_GPON_EVENT_DEACTIVATE:
		odi_gpon_fsm_apply_deactivate(fsm, ops, ctx);
		break;
	case ODI_GPON_EVENT_TO1_EXPIRE:
		odi_gpon_fsm_do_to1_expire(fsm, ops, ctx);
		break;
	case ODI_GPON_EVENT_TO2_EXPIRE:
		odi_gpon_fsm_do_to2_expire(fsm, ops, ctx);
		break;
	case ODI_GPON_EVENT_LOS:
		odi_gpon_fsm_do_los(fsm, ops, ctx);
		break;
	case ODI_GPON_EVENT_LOS_CLEAR:
		odi_gpon_fsm_do_los_clear(fsm, ops, ctx);
		break;
	case ODI_GPON_EVENT_PLOAM_RX:
		odi_gpon_fsm_do_ploam_rx(fsm, ops, ctx, msg);
		break;
	}
}
