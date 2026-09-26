/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_drv.h -- what odi_gpon.c calls in the host-testable GPON core:
 * the FSM ops table bound to the register leaves (odi_gpon_hw.c), the
 * gponact/gpondeact verbs (local state changes, not G.984.3 messages), the
 * ISR poll and switch interrupt line (odi_gpon_isr.c), and status getters.
 *
 * The caller owns the struct odi_gpon_fsm. odi_gpon_hw.c keeps only
 * hardware-shadow bookkeeping (CAM rows, burst overhead, counters).
 */
#ifndef ODI_GPON_DRV_H
#define ODI_GPON_DRV_H

#include "odi_gpon_fsm.h"

/* The ops table for odi_gpon_fsm_handle_event(), bound to the
 * odi_gpon_hw.c register leaves.
 */
const struct odi_gpon_fsm_ops *odi_gpon_hw_ops(void);

/* gponact after the first one: re-activation after a gpondeact.
 * ODI_GPON_EVENT_ACTIVATE (-> O1) and ODI_GPON_EVENT_LOS_CLEAR (-> O2; LOS
 * is already clear when this verb runs), then AES current-slot zero,
 * key-switch-time clear and the DS/US interrupt-mask ramps, in the order of
 * the re-activation capture (test/fixtures/gpon-react-260922-excerpt.txt).
 * The first gponact replays gpon_init.bin instead (odi_gpon_init.h).
 */
void odi_gpon_verb_activate(struct odi_gpon_fsm *fsm);

/* gpondeact verb: administrative force-deactivate straight to O1, not the
 * O2 a Deactivate_ONU-ID PLOAM produces (ODI_GPON_EVENT_DEACTIVATE).
 * Interrupt-mask bracket, state -> state + 1 -> O1 as captured, every
 * allocated Alloc-ID CAM row and GEM port row 64 deleted, the stream-valid
 * gate cleared, ONU-ID reset to broadcast in both directions.
 */
void odi_gpon_verb_deactivate(struct odi_gpon_fsm *fsm);

/* The status words odi_gpon_isr_poll() read, for the /proc/odi_gpon
 * interrupt counters. ds_dlt/us_sts are 0 when top_sts did not flag that
 * sub-block, which is then not read.
 */
struct odi_gpon_isr_poll_status {
	uint32_t top_sts;	/* PONMAC_IRQ_PENDING (0x700044) */
	uint32_t ds_dlt;	/* DSF_IRQ_EVENT (0x701000), or 0 */
	uint32_t us_sts;	/* USF_IRQ_EVENT (0x705000), or 0 */
};

/* GPON block ISR body (G.984.3 clause 5.8): masks PONMAC_IRQ_ENABLE, reads
 * PONMAC_IRQ_PENDING, drains at most one downstream PLOAM message and
 * dispatches it (FSM, then Acknowledge/GEM-port/Alloc-ID handling), acks
 * the upstream sub-block, restores the mask. Returns the number of
 * messages drained, so a replay test can loop until it returns 0. status
 * may be NULL; otherwise it is filled on every call.
 */
unsigned int odi_gpon_isr_poll(struct odi_gpon_fsm *fsm, struct odi_gpon_isr_poll_status *status);

/* The switch interrupt line (IRQ 8, "apl_sw"), whose one consumer is the
 * GPON block: CHIP_IRQ_* in odi_switch_hw.h.
 *
 * odi_gpon_chip_irq_reset(): polarity high, every source masked, every
 * latched status cleared, as the stock intr and irq steps do; at boot,
 * before the line is requested.
 *
 * odi_gpon_chip_irq_enable(): unmasks the GPON source, a read-modify-write
 * of CHIP_IRQ_ENABLE: the acl sdkinit replay has written the whole
 * register (0x80, the ACL source) before gpondrv, and it must stay.
 *
 * odi_gpon_chip_irq_demux(): the handler body. For every source pending
 * and enabled, in bit order: the GPON one calls gpon(), and each is acked
 * (write 1 to clear), serviced or not. The ACL source
 * is enabled by the acl replay with nothing to service it, and a source
 * left pending holds the level-triggered line up: an interrupt storm.
 * Returns the bits it acked; 0 is a spurious interrupt.
 */
void odi_gpon_chip_irq_reset(void);
void odi_gpon_chip_irq_enable(void);
uint32_t odi_gpon_chip_irq_demux(void (*gpon)(void));

/* The USF_EQ_DELAY fields last written (hw_apply_eqd() in odi_gpon_hw.c);
 * 0/0 before the first Upstream_Overhead.
 */
void odi_gpon_get_eqd(uint32_t *multframe, uint32_t *inframe);

/* PLOAM totals: rx drained by odi_gpon_isr_poll(), tx sent upstream. */
void odi_gpon_get_ploam_counts(unsigned int *rx, unsigned int *tx);

/* ---- odi_gpon_hw.c leaves the PLOAM dispatch in odi_gpon_isr.c calls
 * directly, for the message types outside the FSM (Acknowledge, GEM port,
 * Alloc-ID). odi_gpon_hw_test_seed_active() is host-build only.
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

/* ---- Seams for odi_gpon.c, which owns the kernel clock and timers while
 * the core stays host-testable: the BER interval, the REI send, the PLOAM
 * log.
 */

/* BER_Interval (G.984.3 clause 9.2.3.18), in downstream frames, as last
 * received; 0 = none yet, and the REI timer stays disarmed.
 */
void odi_gpon_hw_note_ber_interval(uint32_t interval_frames);
uint32_t odi_gpon_hw_get_ber_interval_frames(void);

/* Sends a complete upstream PLOAM message through the FIFO path every
 * reply uses; the REI timer in odi_gpon.c sends through it.
 */
void odi_gpon_hw_send_us_ploam(const struct odi_gpon_ploam *msg);

/* Extended_Burst_Length (G.984.3 clause 9.2.3.20): the type 3 preamble
 * byte counts before ranging and once ranged. Rewrites the burst overhead
 * with the pattern/delimiter bytes Upstream_Overhead cached.
 */
void odi_gpon_hw_ext_burst_length(uint8_t pre_ranged_preamble_bytes,
				   uint8_t post_o5_preamble_bytes);

/* Observer for every PLOAM message received (dir 0) or sent (dir 1),
 * called with the caller's lock held; feeds the /proc/odi_gpon ring. NULL
 * after odi_gpon_hw_reset().
 */
typedef void (*odi_gpon_ploam_log_fn)(unsigned int dir, const struct odi_gpon_ploam *msg);
void odi_gpon_hw_set_ploam_log(odi_gpon_ploam_log_fn fn);

#endif /* ODI_GPON_DRV_H */
