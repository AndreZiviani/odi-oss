/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_fsm.h -- O1-O7 ONU activation state machine for the RTL9602C
 * GPON MAC block, against ITU-T G.984.3 clause 10 (activation method; see
 * docs/REFERENCES.md for the edition and how to fetch a local copy).
 * Table 10-1 (clause 10.2.4) is this file's own primary source for every
 * state transition below, cited by clause where it matters. Two things
 * are this driver's own design, not G.984.3 itself: the switch-fabric
 * link-force-up/down calls (this SoC's own switch fabric is outside the
 * GTC/PLOAM layer the standard covers) and the driver-level ACTIVATE/
 * DEACTIVATE commands (administrative actions, not G.984.3 protocol
 * events -- DEACTIVATE is implemented as exactly what a Deactivate_ONU-ID
 * PLOAM would do, restricted to the same O4/O5/O6 states the standard
 * own table restricts that message to).
 *
 * The FSM is a pure function of (state, event, message) -> actions:
 * odi_gpon_fsm_handle_event() reads and writes only the struct
 * odi_gpon_fsm passed to it, and every side effect is a call through the
 * struct odi_gpon_fsm_ops function-pointer table -- no register access, no
 * global state, no kernel dependency. This lets a host test record the
 * action sequence a given (state, event, message) input produces and
 * compare it against this driver's own documented sequences; the target
 * build later binds odi_gpon_fsm_ops's own function pointers to real
 * odi_gpon_hw.h register leaves (a later cut).
 *
 * States O1-O7 (G.984.3 clause 10.2.1 ONU activation states): O1 Initial,
 * O2 Standby, O3 Serial_Number state, O4 Ranging state, O5 Operation
 * state, O6 POPUP state, O7 Emergency Stop.
 */
#ifndef ODI_GPON_FSM_H
#define ODI_GPON_FSM_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#include "odi_gpon_ploam.h"

enum odi_gpon_state {
	ODI_GPON_STATE_O1 = 1,	/* Initial */
	ODI_GPON_STATE_O2,	/* Standby */
	ODI_GPON_STATE_O3,	/* Serial number state */
	ODI_GPON_STATE_O4,	/* Ranging state */
	ODI_GPON_STATE_O5,	/* Operation state */
	ODI_GPON_STATE_O6,	/* Popup state */
	ODI_GPON_STATE_O7,	/* Emergency Stop */
};

enum odi_gpon_event {
	ODI_GPON_EVENT_ACTIVATE,	/* driver command: (re)start the FSM at O1 */
	ODI_GPON_EVENT_DEACTIVATE,	/* driver command: same effect as a Deactivate_ONU-ID
					 * PLOAM (G.984.3 clause 9.2.3.5, Table 10-1) -- O4/
					 * O5/O6 only, forces O2 (Standby), a no-op elsewhere
					 */
	ODI_GPON_EVENT_PLOAM_RX,	/* a downstream PLOAM message arrived; msg is non-NULL */
	ODI_GPON_EVENT_TO1_EXPIRE,	/* the O3/O4 ranging-wait timer expired (G.984.3 clause 10.2.1) */
	ODI_GPON_EVENT_TO2_EXPIRE,	/* the O6 popup-window timer expired (G.984.3 clause 10.2.1) */
	ODI_GPON_EVENT_LOS,		/* loss of optical signal detected */
	ODI_GPON_EVENT_LOS_CLEAR,	/* LOS condition cleared */
};

/* Every side effect the FSM can produce, as calls into this ops table.
 * ctx is an opaque pointer the caller supplies to odi_gpon_fsm_handle_event()
 * and gets back unchanged on every callback -- a host test binds it to a
 * recording struct; a kernel build binds it to whatever odi_gpon_hw.c
 * later needs to reach the real registers.
 */
struct odi_gpon_fsm_ops {
	/* Write the O1-O7 state into the hardware-mirrored state register
	 * (DSF_ONU_STATE.ACTIVATION_STATE).
	 */
	void (*set_state)(void *ctx, enum odi_gpon_state state);

	/* Write the assigned ONU-ID into both direction registers
	 * (DSF_ONU_STATE.ASSIGNED_ONU_ID, USF_ONU_ID) -- called
	 * before set_state() on the O3->O4 transition, matching this
	 * driver's own documented register-write ordering for that step.
	 */
	void (*set_onu_id)(void *ctx, uint8_t onu_id);

	/* Apply the equalization delay the OLT granted, as the raw
	 * Ranging_Time delay value (clause 9.2.3.4, bits, MSB first) -- not
	 * pre-split into USF_EQ_DELAY's own MULTFRAME/INFRAME fields here,
	 * because the real split needs USF_MIN_RESP_DELAY (odi_gpon_hw.h),
	 * a register READ this pure FSM has no access to; the leaf
	 * (odi_gpon_hw.c) does the read and the split, see its own comment
	 * for the derived formula and its evidence.
	 */
	void (*set_eqd)(void *ctx, uint32_t eqd_bits);

	/* Apply Upstream_Overhead burst timing/preamble config (G.984.3
	 * clause 9.2.3.1) -- not itself a state transition (the O2->O3
	 * transition and the TO1 start that go with receiving this message
	 * are the FSM's own doing, not this leaf's own).
	 *
	 * preassigned_delay is the pre-assigned delay in 32-byte units, and
	 * is 0 whenever the message's e flag is clear: the flag is folded
	 * into the value here so the leaf never applies a delay the OLT did
	 * not enable.
	 */
	void (*set_upstream_overhead)(void *ctx, uint8_t guard_bits, uint8_t type1_preamble_bits,
				       uint8_t type2_preamble_bits, uint8_t type3_pattern,
				       const uint8_t delimiter[3], uint16_t preassigned_delay,
				       uint8_t power_level_mode);

	/* Enqueue one upstream PLOAM message for transmission -- msg is
	 * already a complete wire message (onu_id/type/content).
	 */
	void (*send_us_ploam)(void *ctx, const struct odi_gpon_ploam *msg);

	/* TO1 (recommended initial value 10 s, G.984.3 clause 10.2.1): bounds
	 * O3 (waiting for Assign_ONU-ID) and O4 (waiting for Ranging_Time).
	 */
	void (*start_to1)(void *ctx);
	void (*stop_to1)(void *ctx);

	/* TO2 (recommended initial value 100 ms, G.984.3 clause 10.2.1):
	 * bounds the O6 POPUP-state window.
	 */
	void (*start_to2)(void *ctx);
	void (*stop_to2)(void *ctx);

	/* Enable/disable the upstream laser (USF_LASER_MARGIN timing is a
	 * later leaf concern; this is the on/off gate) -- on entering O2
	 * (ready to range) and O7 (emergency stop), and back on leaving O7.
	 */
	void (*laser_enable)(void *ctx, int enable);

	/* Load one 128-bit AES key into the given hitless-switch slot
	 * (ODI_GPON_DSK_SLOT_CURRENT/_NEXT).
	 */
	void (*load_key)(void *ctx, unsigned int slot, const uint8_t key[16]);

	/* Arm the hitless key switchover at the given downstream superframe
	 * count (DSK_SWITCH_FRAME/_REQ), driven by the OLT's own
	 * Key_switching_time PLOAM message (G.984.3 clause 9.2.3.19).
	 */
	void (*switch_key)(void *ctx, uint32_t switch_superframe);

	/* Force the switch-core PON port MAC link state: entering O5 forces
	 * link up independent of GTC LOS/LOF; this FSM also calls it with 0
	 * on leaving O5. This SoC's own switch fabric is outside what G.984.3
	 * itself specifies.
	 */
	void (*link_force_up)(void *ctx, int up);

	/* Discard anything queued in the upstream PLOAM software buffer from
	 * before ranging completed (this driver's own O4->O5 sequence).
	 */
	void (*flush_us_ploam_buf)(void *ctx);
};

/* FSM context: everything the pure function needs across calls, owned by
 * the caller (a host test, or later odi_gpon.c). No pointers into hardware
 * or the ops table are stored here -- both are passed fresh to every
 * odi_gpon_fsm_handle_event() call.
 */
struct odi_gpon_fsm {
	enum odi_gpon_state state;
	uint8_t onu_id;			/* 0 until Assign_ONU-ID (O3->O4) */
	uint8_t serial_number[8];	/* set once, before activation starts */
	uint8_t password[10];		/* set once, before activation starts */
	uint8_t aes_key[16];		/* the key this ONU reports on Request_Key (G.984.3 clause 9.2.4.5) */
	/* Key_Index (content[0] of every Encryption_Key fragment, clause
	 * 9.2.4.5) -- 0x02 in every capture this driver has seen; whether
	 * it changes across more than the one Request_Key cycle captured so
	 * far is unverified, so this stays a fixed field this driver does
	 * not itself increment.
	 */
	uint8_t key_index;
};

#define ODI_GPON_KEY_INDEX_DEFAULT	0x02U

void odi_gpon_fsm_init(struct odi_gpon_fsm *fsm);
void odi_gpon_fsm_set_serial_number(struct odi_gpon_fsm *fsm, const uint8_t serial_number[8]);
void odi_gpon_fsm_set_password(struct odi_gpon_fsm *fsm, const uint8_t password[10]);
void odi_gpon_fsm_set_aes_key(struct odi_gpon_fsm *fsm, const uint8_t key[16]);

/* The pure transition function. msg is only read for
 * ODI_GPON_EVENT_PLOAM_RX (must be non-NULL then); ignored (may be NULL)
 * for every other event.
 */
void odi_gpon_fsm_handle_event(struct odi_gpon_fsm *fsm, const struct odi_gpon_fsm_ops *ops,
				void *ctx, enum odi_gpon_event event,
				const struct odi_gpon_ploam *msg);

/* Why the FSM moved, as the driver logs it (odi_gpon.c, the event=onu_state
 * line; docs/TOOLS.md, "Link and provisioning events", has the table).
 * `name` is the event or the downstream PLOAM message that drove the
 * transition; `side` is who started it:
 *
 *   olt    a downstream PLOAM message (Deactivate_ONU-ID, Disable_Serial_
 *          Number, Upstream_Overhead, ...)
 *   timer  TO1 or TO2 expired: the OLT stopped ranging this ONU, or the
 *          POPUP window closed without a POPUP message
 *   line   loss of signal, or its clearing
 *   local  a driver command from this side (gponact, gpondeact)
 *
 * Both strings are static and never NULL. msg is only read for
 * ODI_GPON_EVENT_PLOAM_RX and may be NULL otherwise; NULL there reads as an
 * unknown message.
 */
struct odi_gpon_fsm_cause {
	const char *name;
	const char *side;
};

void odi_gpon_fsm_cause(enum odi_gpon_event event, const struct odi_gpon_ploam *msg,
			struct odi_gpon_fsm_cause *out);

#endif /* ODI_GPON_FSM_H */
