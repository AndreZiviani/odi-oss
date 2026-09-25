/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_drv.h -- the clean entry points odi_gpon.c (kernel-facing
 * odi_gpon_init/odi_gpon_verb/proc file, defined elsewhere) calls into: the
 * FSM ops table bound to the real register leaves (odi_gpon_hw.c), the two
 * administrative verbs a real OLT never sends as a PLOAM message
 * (gponact/gpondeact -- forcing a state transition locally, not in
 * response to a G.984.3 message), the top-level ISR (odi_gpon_isr.c), and
 * small getters for status surfaces (proc file / RTK_OPT_GPON_STATUS).
 *
 * Every function here takes or is bound to a struct odi_gpon_fsm the
 * caller owns (module-static in a real build, a local in a host test) --
 * nothing in this file or its .c files keeps FSM state of its own beyond
 * the small hardware-shadow bookkeeping odi_gpon_hw.c needs for its own
 * CAM row allocation (documented at its own definition, not part of the
 * FSM proper).
 */
#ifndef ODI_GPON_DRV_H
#define ODI_GPON_DRV_H

#include "odi_gpon_fsm.h"

/* The ops table odi_gpon_fsm_handle_event() needs, bound to the real
 * odi_gpon_hw.c leaves (register access through odi_reg_read()/
 * odi_reg_write(), the odi_switch MMIO primitive -- odi_switch_mock.h on
 * the host build, real MMIO under __KERNEL__, exactly like odi_switch_tbl.c).
 */
const struct odi_gpon_fsm_ops *odi_gpon_hw_ops(void);

/* gponact verb: the boot-capture-derived "gponact" table (odi_gpon_init.h)
 * (re)armed -- AES current-slot zero, key-switch-time clear, the DS/US
 * interrupt-mask ramp, then ODI_GPON_EVENT_ACTIVATE (-> O1) and
 * ODI_GPON_EVENT_LOS_CLEAR (-> O2, since LOS is already clear by the time
 * this verb runs on this box -- rxsd already ran before it).
 * This is the RE-activation sequence (react.txt's own "# react act"); the
 * one-time boot sequence ahead of it (gpondrv/gpondev/gponsn/gponpw) is
 * odi_gpon_init_apply() (odi_gpon_init.h), called once at module load, not
 * by this verb.
 */
void odi_gpon_verb_activate(struct odi_gpon_fsm *fsm);

/* gpondeact verb: administrative force-deactivate -- goes straight to O1,
 * not the O2 Standby state a real Deactivate_ONU-ID PLOAM would produce
 * (odi_gpon_fsm.h's own ODI_GPON_EVENT_DEACTIVATE handles that
 * protocol-driven path already): interrupt-mask bracket, state O5 -> a
 * transient value -> O1 (the transient write is reproduced as captured;
 * its purpose is not resolved, see odi_gpon_hw.c's own comment at the call
 * site), every Alloc-ID CAM row this driver had allocated cleared, the one
 * GEM port row implemented here (odi_gpon_hw.c's own fixed row 64)
 * cleared, the switch-core stream-valid gate cleared, ONU-ID reset to
 * broadcast in both directions.
 */
void odi_gpon_verb_deactivate(struct odi_gpon_fsm *fsm);

/* Optional observability out-parameter for odi_gpon_isr_poll() below -- the
 * top-level and per-sub-block status words it read this pass, so a caller
 * that needs them for /proc/odi_gpon (odi_gpon.c's own spurious-interrupt and
 * last-nonzero-STS counters) does not have to re-read the same registers a
 * second time. ds_dlt/us_sts are 0 when this pass's own top_sts did not have
 * the matching GTC_DS/GTC_US bit set (odi_gpon_isr_poll() then never reads
 * that sub-block at all, same as before this struct existed).
 */
struct odi_gpon_isr_poll_status {
	uint32_t top_sts;	/* PONMAC_IRQ_PENDING (0x700044) this pass */
	uint32_t ds_dlt;	/* DSF_IRQ_EVENT (0x701000), or 0 */
	uint32_t us_sts;	/* USF_STATE (0x705008), or 0 */
};

/* Top-level ISR (G.984.3 clause 5.8 GTC framing, downstream PLOAM
 * handshake): masks the top interrupt, reads PONMAC_IRQ_PENDING, and for GTC_DS
 * drains and dispatches every downstream PLOAM message currently queued
 * (odi_gpon_ploam.c decode + odi_gpon_fsm_handle_event() for the
 * state-machine-relevant message types, plus this driver's own
 * Acknowledge/GEM-port/Alloc-ID/key handling for the types the FSM itself
 * does not touch, odi_gpon_fsm.h's own documented scope), then restores the
 * mask. Returns the number of PLOAM messages it drained and dispatched (0
 * if nothing was pending) -- a caller (or a host replay test) can loop
 * "while odi_gpon_isr_poll() keeps returning nonzero" to drain a burst one
 * message at a time. status may be NULL (every existing caller before this
 * struct existed passes NULL and sees no change in behaviour); when
 * non-NULL it is filled in exactly once, unconditionally, on every call.
 */
unsigned int odi_gpon_isr_poll(struct odi_gpon_fsm *fsm, struct odi_gpon_isr_poll_status *status);

/* The equalization delay this driver last applied (odi_gpon_hw.c's own
 * EqD-formula leaf, see its definition for the formula and its evidence),
 * as the raw register fields -- 0/0 if Ranging_Time has not landed yet
 * this activation.
 */
void odi_gpon_get_eqd(uint32_t *multframe, uint32_t *inframe);

/* PLOAM counters (rx = messages drained by odi_gpon_isr_poll(), tx = messages
 * this driver sent upstream, including Acknowledge/Encryption_Key) -- a
 * gponstat-style surface.
 */
void odi_gpon_get_ploam_counts(unsigned int *rx, unsigned int *tx);

/* ---- odi_gpon_hw.c internals odi_gpon_isr.c's own PLOAM dispatch calls
 * directly (not through struct odi_gpon_fsm_ops -- odi_gpon_fsm.c's own
 * header comment documents these message types as out of the pure FSM
 * scope: Acknowledge, GEM-port table, Alloc-ID table). Not part of the
 * odi_gpon.c-facing surface above this comment; kept in this same header
 * only so odi_gpon_isr.c has one include to reach both halves.
 */
void odi_gpon_hw_reset(void);
void odi_gpon_hw_test_seed_active(uint16_t gem_port_id, const unsigned int *alloc_rows,
				   unsigned int n_alloc_rows);
void odi_gpon_hw_note_ploam_rx(const struct odi_gpon_ploam *msg);
void odi_gpon_hw_ack_send(uint8_t onu_id, const struct odi_gpon_ploam *acked);
void odi_gpon_hw_gem_port_configure(uint16_t gem_port_id, int activate);
int odi_gpon_hw_gem_port_encrypted(uint16_t gem_port_id, int encrypted);
int odi_gpon_hw_alloc_id_assign(uint16_t alloc_id);
void odi_gpon_hw_eqd_rewrite(uint32_t eqd_bits);

/* ---- Kernel-integration seams: odi_gpon_hw.c stays host-testable and
 * kernel/time-independent (this file's own header contract), so the three
 * things below that DO need a kernel clock/timer -- the PLOAM
 * observability ring, the BER-interval timer period, and the REI send
 * itself -- are split so odi_gpon.c (the kernel-facing glue, __KERNEL__
 * only) owns the clock and this file keeps owning the registers.
 */

/* BER_Interval bookkeeping: odi_gpon_isr.c decodes the message content
 * (G.984.3 clause 9.2.3.18, downstream frame count) and calls this so
 * odi_gpon.c's own kernel timer can (re)arm itself at the right period
 * without decoding PLOAM content a second time. 0 means "no BER_Interval
 * received yet this activation" -- odi_gpon.c leaves its own REI timer
 * disarmed then.
 */
void odi_gpon_hw_note_ber_interval(uint32_t interval_frames);
uint32_t odi_gpon_hw_get_ber_interval_frames(void);

/* Sends a fully-built upstream PLOAM message through the same FIFO path
 * hw_send_us_ploam() (odi_gpon_hw.c, static) uses for every other reply --
 * exposed so odi_gpon.c's own BER-interval kernel timer can send the REI
 * message odi_gpon_encode_rei() (odi_gpon_ploam.h) builds, without
 * duplicating the FIFO write sequence.
 */
void odi_gpon_hw_send_us_ploam(const struct odi_gpon_ploam *msg);

/* Ext_Burst_Length (message type 0x14, G.984.3 clause 9.2.3.20 -- a
 * vendor/extension message, no G.984.3 content definition): reconfigures
 * USF_BURST_HDR_SETUP/_DATA for the one content pair this driver
 * recognizes (pre-ranged preamble byte count, post-O5 preamble byte
 * count), replaying the same pattern/delimiter bytes
 * set_upstream_overhead() cached -- see odi_gpon_hw.c's own definition for
 * the evidence and the documented gap for any other content pair.
 */
void odi_gpon_hw_ext_burst_length(uint8_t pre_ranged_preamble_bytes,
				   uint8_t post_o5_preamble_bytes);

/* Optional observer for every downstream/upstream PLOAM message this
 * driver processes -- odi_gpon.c's own /proc/odi_gpon ring buffer uses
 * this instead of duplicating the FIFO/dispatch logic; NULL (the default,
 * and odi_gpon_hw_reset()'s own state) disables it at the cost of one NULL
 * check per message. dir 0 = downstream (received), 1 = upstream (sent by
 * this driver, Acknowledge/Encryption_Key/REI/Serial_Number/Password
 * alike).
 */
typedef void (*odi_gpon_ploam_log_fn)(unsigned int dir, const struct odi_gpon_ploam *msg);
void odi_gpon_hw_set_ploam_log(odi_gpon_ploam_log_fn fn);

#endif /* ODI_GPON_DRV_H */
