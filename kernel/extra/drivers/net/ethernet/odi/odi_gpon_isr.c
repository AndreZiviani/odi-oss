// SPDX-License-Identifier: GPL-2.0
/*
 * odi_gpon_isr.c -- odi_gpon_isr_poll(), the top-level GPON MAC block
 * interrupt servicing entry point, against G.984.3 clause 5.8 (GTC
 * framing): mask the top interrupt, read the aggregate status, drain and
 * dispatch every queued downstream PLOAM message, restore the mask. This
 * is the only place a PLOAM message type dictates register work beyond
 * what struct odi_gpon_fsm_ops covers -- odi_gpon_fsm.c's own header comment
 * lists Acknowledge/GEM-port/Alloc-ID handling as deliberately out of the
 * pure FSM scope; this file is where that scope lives instead.
 */
#include "odi_gpon_drv.h"
#include "odi_gpon_hw.h"
#include "odi_switch_reg.h"
#include "odi_switch_hw.h"

/* Every downstream PLOAM message is fed to the FSM first (it owns the
 * state-machine-relevant types and silently ignores the rest, by design,
 * odi_gpon_fsm.c's own header comment) -- then this driver's own
 * Acknowledge/GEM-port/Alloc-ID handling runs for the message types that
 * draw register work or an upstream Acknowledge the FSM itself does not
 * produce. Order matches the capture: the FSM's own action (e.g.
 * switch_key() for Key_Switching_Time) happens before the Acknowledge this
 * driver sends for it (react.txt shows the AES_KEY_SWITCH_TIME write, then
 * the Acknowledge send, in that order).
 */
static void odi_gpon_isr_dispatch(struct odi_gpon_fsm *fsm, const struct odi_gpon_ploam *msg)
{
	struct odi_gpon_ds_configure_port_id cfg;
	struct odi_gpon_ds_encrypted_port_id enc;
	struct odi_gpon_ds_assign_alloc_id alloc;
	struct odi_gpon_ds_ranging_time ranging;
	struct odi_gpon_ds_ber_interval ber;
	enum odi_gpon_state prev_state = fsm->state;

	odi_gpon_hw_note_ploam_rx(msg);
	odi_gpon_fsm_handle_event(fsm, odi_gpon_hw_ops(), NULL, ODI_GPON_EVENT_PLOAM_RX, msg);

	switch (msg->type) {
	case ODI_GPON_DS_RANGING_TIME:
		/* The FSM's own ops->set_eqd() already ran, once, on the
		 * O4->O5 transition -- only when this call found the FSM
		 * ALREADY in O5 (a genuine second/third read of the same
		 * message, not the transition this same call just made) is
		 * this the plain EQD-only rewrite (odi_gpon_hw_eqd_rewrite()
		 * own comment has the evidence for why this is not folded
		 * into the FSM path itself).
		 */
		if (prev_state == ODI_GPON_STATE_O5 && fsm->state == ODI_GPON_STATE_O5) {
			odi_gpon_decode_ranging_time(msg, &ranging);
			odi_gpon_hw_eqd_rewrite(ranging.eqd);
		}
		break;

	case ODI_GPON_DS_CONFIGURE_PORT_ID:
		odi_gpon_decode_configure_port_id(msg, &cfg);
		odi_gpon_hw_gem_port_configure(cfg.port_id, cfg.activate);
		odi_gpon_hw_ack_send(fsm->onu_id, msg);
		break;

	case ODI_GPON_DS_ENCRYPTED_PORT_ID:
		odi_gpon_decode_encrypted_port_id(msg, &enc);
		/* This DOES get an Acknowledge -- confirmed by a
		 * direct read of react.txt (an Ack-shaped send, DM_ID 0x08,
		 * follows every one of the three CAM touches). Register work
		 * only for the one GEM port row implemented here (see
		 * odi_gpon_hw_gem_port_encrypted()'s own comment for the
		 * documented gap on any other Port-ID).
		 */
		odi_gpon_hw_gem_port_encrypted(enc.port_id, enc.encrypted);
		odi_gpon_hw_ack_send(fsm->onu_id, msg);
		break;

	case ODI_GPON_DS_ASSIGN_ALLOC_ID:
		odi_gpon_decode_assign_alloc_id(msg, &alloc);
		odi_gpon_hw_alloc_id_assign(alloc.alloc_id);
		odi_gpon_hw_ack_send(fsm->onu_id, msg);
		break;

	case ODI_GPON_DS_BER_INTERVAL:
		/* No GPON MAC register write (G.984.3 clause 9.2.3.18 is
		 * software-only bookkeeping: a timer restart) -- decode the
		 * interval and hand it to odi_gpon_hw_note_ber_interval() so
		 * odi_gpon.c's own kernel timer can (re)arm the REI send at the
		 * right period, then Acknowledge.
		 */
		odi_gpon_decode_ber_interval(msg, &ber);
		odi_gpon_hw_note_ber_interval(ber.interval_frames);
		odi_gpon_hw_ack_send(fsm->onu_id, msg);
		break;

	case ODI_GPON_DS_KEY_SWITCHING_TIME:
		/* The FSM's own switch_key() call already ran above; this is
		 * only the Acknowledge, sent after it (the observed order).
		 */
		odi_gpon_hw_ack_send(fsm->onu_id, msg);
		break;

	case ODI_GPON_DS_EXT_BURST_LENGTH:
		/* Broadcast, no Acknowledge (same as Upstream_Overhead). Sets
		 * the burst preamble length; some OLTs never detect a burst
		 * that ignores it.
		 */
		odi_gpon_hw_ext_burst_length(msg->content[0], msg->content[1]);
		break;

	default:
		break;	/* every other type: the FSM's own dispatch was enough */
	}
}

unsigned int odi_gpon_isr_poll(struct odi_gpon_fsm *fsm, struct odi_gpon_isr_poll_status *status)
{
	uint32_t top_sts, ds_dlt = 0U, us_sts = 0U;
	uint32_t ploam_ind;
	unsigned int n = 0;

	/* G.984.3 clause 5.8: disable, read+zero the top mask, read the top
	 * status.
	 */
	odi_reg_write(ODI_GPON_PONMAC_IRQ_ENABLE_OFF, 0U);
	top_sts = odi_reg_read(ODI_GPON_PONMAC_IRQ_PENDING_OFF);

	if (top_sts & ODI_GPON_PONMAC_IRQ_PENDING_DS_FRAMER) {
		ds_dlt = odi_reg_read(ODI_GPON_DSF_IRQ_EVENT_OFF);

		if (ds_dlt & ODI_GPON_DSF_IRQ_EVENT_PLOAM_RX) {
			/* One drain per bracket, not a while-not-empty loop:
			 * every PLOAM_RX-triggered pass this driver has
			 * captured drains exactly one message -- no batched
			 * multi-message drain was ever observed, and the
			 * G.984.3 clause 5.4 handshake is a single
			 * check-then-drain sequence, not a loop. A caller sees
			 * more queued messages the same way the real
			 * shared-IRQ line does: another interrupt, i.e.
			 * another odi_gpon_isr() call (this function's own
			 * return value is exactly "was there one to drain
			 * on this call").
			 */
			ploam_ind = odi_reg_read(ODI_GPON_DSF_PLOAM_RX_CTL_OFF);

			if (!ODI_GPON_DSF_PLOAM_RX_CTL_FIFO_EMPTY_GET(ploam_ind)) {
				uint16_t words[6];
				struct odi_gpon_ploam msg;
				unsigned int i;

				for (i = 0; i < ODI_GPON_DS_PLOAM_WORDS; i++)
					words[i] = (uint16_t)odi_reg_read(ODI_GPON_DSF_PLOAM_RX_WORD(i));
				odi_gpon_ploam_unpack_words(words, &msg);

				/* Dequeue handshake: IND=0 then IND=1 (two
				 * writes, not one -- G.984.3 clause 5.4).
				 */
				odi_reg_write(ODI_GPON_DSF_PLOAM_RX_CTL_OFF, 0U);
				odi_reg_write(ODI_GPON_DSF_PLOAM_RX_CTL_OFF,
					      ODI_GPON_DSF_PLOAM_RX_CTL_POP_SET(0, 1));

				odi_gpon_isr_dispatch(fsm, &msg);
				n++;
			}
		}
	}

	if (top_sts & ODI_GPON_PONMAC_IRQ_PENDING_US_FRAMER) {
		/* Read the latched delta register USF_IRQ_EVENT, which
		 * clears on read; the level register USF_STATE does
		 * not clear, so reading only it left the upstream sub-block
		 * pending and IRQ 8 firing about 16000 times a second, observed
		 * on ISP1. No upstream event needs handling beyond the read.
		 */
		us_sts = odi_reg_read(ODI_GPON_USF_IRQ_EVENT_OFF);
	}

	odi_reg_write(ODI_GPON_PONMAC_IRQ_ENABLE_OFF, 0x22U);

	if (status) {
		status->top_sts = top_sts;
		status->ds_dlt = ds_dlt;
		status->us_sts = us_sts;
	}

	return n;
}

/* ---- The switch interrupt line (odi_gpon_drv.h) ----------------------- */

void odi_gpon_chip_irq_reset(void)
{
	odi_reg_write(ODI_SW_CHIP_IRQ_SETUP_OFF, ODI_SW_CHIP_IRQ_SETUP_POLARITY_SEL_SET(0, 0));
	odi_reg_write(ODI_SW_CHIP_IRQ_ENABLE_OFF, 0);
	odi_reg_write(ODI_SW_CHIP_IRQ_PENDING_OFF, (1U << ODI_SW_CHIP_IRQ_SOURCES) - 1U);
}

void odi_gpon_chip_irq_enable(void)
{
	odi_reg_write(ODI_SW_CHIP_IRQ_ENABLE_OFF,
		      odi_reg_read(ODI_SW_CHIP_IRQ_ENABLE_OFF) | ODI_SW_CHIP_IRQ_ENABLE_GPON);
}

uint32_t odi_gpon_chip_irq_demux(void (*gpon)(void))
{
	uint32_t pending = odi_reg_read(ODI_SW_CHIP_IRQ_ENABLE_OFF);
	unsigned int bit;

	pending &= odi_reg_read(ODI_SW_CHIP_IRQ_PENDING_OFF);
	for (bit = 0; bit < ODI_SW_CHIP_IRQ_SOURCES; bit++) {
		if (!(pending & (1U << bit)))
			continue;
		if ((1U << bit) == ODI_SW_CHIP_IRQ_ENABLE_GPON)
			gpon();
		odi_reg_write(ODI_SW_CHIP_IRQ_PENDING_OFF, 1U << bit);
	}
	return pending;
}
