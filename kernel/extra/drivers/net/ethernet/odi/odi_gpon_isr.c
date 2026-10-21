// SPDX-License-Identifier: GPL-2.0
/*
 * odi_gpon_isr.c -- odi_gpon_isr_poll(), the top-level GPON MAC block
 * interrupt servicing entry point, against G.984.3 clause 5.8 (GTC
 * framing): mask the top interrupt, read the aggregate status, drain and
 * dispatch every queued downstream PLOAM message (bounded), restore the
 * mask; and
 * odi_gpon_line_sample(), the LOS/LOF level the interrupt and the timers
 * feed to the FSM. This
 * is the only place a PLOAM message type dictates register work beyond
 * what struct odi_gpon_fsm_ops covers -- odi_gpon_fsm.c's own header comment
 * lists Acknowledge/GEM-port/Alloc-ID handling as deliberately out of the
 * pure FSM scope; this file is where that scope lives instead.
 */
#include "odi_gpon_drv.h"
#include "odi_gpon_hw.h"
#include "odi_switch_reg.h"
#include "odi_switch_hw.h"

#ifdef __KERNEL__
#include <linux/printk.h>
#define ODI_GPON_ISR_WARN(fmt, ...) pr_warn_ratelimited("odi_gpon: " fmt, ##__VA_ARGS__)
#else
#include <stdio.h>
#define ODI_GPON_ISR_WARN(fmt, ...) printf("odi_gpon: " fmt, ##__VA_ARGS__)
#endif

/* The most downstream PLOAM messages one interrupt drains. The FIFO depth
 * is not decoded; the most queued at one interrupt in any capture is two
 * (the stock driver popping two in one pass, twice, early in an
 * activation). The bound only keeps a FIFO_EMPTY that never sets from
 * looping with interrupts off; what it leaves is drained at the next
 * interrupt.
 */
#define ODI_GPON_DS_PLOAM_DRAIN_MAX	16U

static odi_gpon_isr_msg_fn odi_gpon_isr_msg_hook;

void odi_gpon_isr_set_msg_hook(odi_gpon_isr_msg_fn fn)
{
	odi_gpon_isr_msg_hook = fn;
}

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

	/* The directed messages below act, and are acknowledged, only when
	 * they carry our ONU-ID (odi_gpon_fsm_directed_to_us()): a broadcast
	 * copy is not ours to apply. BER_Interval and Key_Switching_Time may
	 * be broadcast (G.984.3 9.2.3.18, 9.2.3.19) and are taken from a
	 * broadcast or our own copy, never from one addressed to another ONU
	 * (odi_gpon_fsm_addressed_to_us()).
	 */
	switch (msg->type) {
	case ODI_GPON_DS_BER_INTERVAL:
	case ODI_GPON_DS_KEY_SWITCHING_TIME:
		if (!odi_gpon_fsm_addressed_to_us(fsm, msg))
			return;
		break;
	default:
		break;
	}

	switch (msg->type) {
	case ODI_GPON_DS_RANGING_TIME:
	case ODI_GPON_DS_CONFIGURE_PORT_ID:
	case ODI_GPON_DS_ENCRYPTED_PORT_ID:
	case ODI_GPON_DS_ASSIGN_ALLOC_ID:
		if (!odi_gpon_fsm_directed_to_us(fsm, msg))
			return;
		break;
	default:
		break;
	}

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
			/* A protection path EqD (G.984.3 9.2.3.4) is not ours:
			 * the FSM ignores it in O4 and so does this rewrite.
			 */
			if (!ranging.protection_path)
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
		/* G.984.3 9.2.3.10: type 1 (GEM) assigns the Alloc-ID and
		 * type 255 takes it back; only these touch the CAM, which
		 * decides the grants this ONU answers. Type 2 (DBA, deprecated)
		 * is not acknowledged and the Alloc-ID is treated as not this
		 * ONU's, as G.984.3 9.2.3 asks; the other types are reserved,
		 * acknowledged and ignored, as the stock module does. omcid
		 * binds the T-CONTs to what is assigned, from
		 * /proc/odi_gpon/status.
		 */
		odi_gpon_decode_assign_alloc_id(msg, &alloc);
		if (alloc.alloc_id_type == ODI_GPON_ALLOC_ID_TYPE_DBA)
			break;
		if (alloc.alloc_id_type == ODI_GPON_ALLOC_ID_TYPE_DEALLOCATE)
			odi_gpon_hw_alloc_id_release(alloc.alloc_id);
		else if (alloc.alloc_id_type == ODI_GPON_ALLOC_ID_TYPE_GEM)
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
		 * that ignores it. G.984.3 Table 10-1 and 10.2.2 b): taken in
		 * Serial_Number (O3) only. The message does not move the FSM,
		 * so prev_state is the state it arrived in. An OLT that
		 * repeats it for as long as the ONU is up (some do, every few
		 * seconds, as it does Upstream_Overhead) would otherwise
		 * rewrite the burst header of a running ONU each time. What
		 * the first one set holds until the ONU is deactivated
		 * (clause 9.2.3.20 note 2). Whether a serial number request
		 * already came in O3 is not visible here: hardware answers it
		 * on its own.
		 */
		if (prev_state != ODI_GPON_STATE_O3)
			break;
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
			/* Every message queued, until FIFO_EMPTY, at most
			 * ODI_GPON_DS_PLOAM_DRAIN_MAX. PLOAM_RX is one event
			 * bit for however many messages arrived since the last
			 * read: two queued during one interrupt raise it once,
			 * and draining one per interrupt would leave the
			 * second waiting for the next message from the OLT,
			 * the lag never shrinking. The stock driver drains in
			 * a loop too; the react capture has it popping two
			 * messages in one pass. With one message queued this
			 * is the old single drain plus one more read of the
			 * control word.
			 */
			while (n < ODI_GPON_DS_PLOAM_DRAIN_MAX) {
				uint16_t words[6];
				struct odi_gpon_ploam msg;
				enum odi_gpon_state prev;
				unsigned int i;

				/* After a pop, FIFO_EMPTY is read twice and the
				 * FIFO counts as empty if either read says so: a
				 * status that trails the pop by an access would
				 * otherwise show the message just taken as still
				 * there, and it would be read and dispatched a
				 * second time, with a pop of an empty FIFO. Not
				 * needed on the first read, which no pop of ours
				 * precedes, so a lone message costs no more reads
				 * than before.
				 */
				ploam_ind = odi_reg_read(ODI_GPON_DSF_PLOAM_RX_CTL_OFF);
				if (n && !ODI_GPON_DSF_PLOAM_RX_CTL_FIFO_EMPTY_GET(ploam_ind))
					ploam_ind = odi_reg_read(ODI_GPON_DSF_PLOAM_RX_CTL_OFF);
				if (ODI_GPON_DSF_PLOAM_RX_CTL_FIFO_EMPTY_GET(ploam_ind))
					break;

				for (i = 0; i < ODI_GPON_DS_PLOAM_WORDS; i++)
					words[i] = (uint16_t)odi_reg_read(ODI_GPON_DSF_PLOAM_RX_WORD(i));
				odi_gpon_ploam_unpack_words(words, &msg);

				/* Dequeue handshake: IND=0 then IND=1 (two
				 * writes, not one -- G.984.3 clause 5.4).
				 */
				odi_reg_write(ODI_GPON_DSF_PLOAM_RX_CTL_OFF, 0U);
				odi_reg_write(ODI_GPON_DSF_PLOAM_RX_CTL_OFF,
					      ODI_GPON_DSF_PLOAM_RX_CTL_POP_SET(0, 1));

				prev = fsm->state;
				odi_gpon_isr_dispatch(fsm, &msg);
				n++;
				if (odi_gpon_isr_msg_hook)
					odi_gpon_isr_msg_hook(prev, &msg);
			}
			if (n == ODI_GPON_DS_PLOAM_DRAIN_MAX)
				ODI_GPON_ISR_WARN("downstream PLOAM drain stopped at %u messages\n", n);
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

/* ---- The line alarm (odi_gpon_drv.h) ----------------------------------- */

/* DSF_ALARM_STATE holds the framer own live alarm levels; on a working
 * line both ISPs read LOS and LOF clear in O5, and a fibre pull sets LOS
 * for as long as the light is gone. A sample is acted on as read, with no
 * software debounce on top: G.984.3 gives an intermittent loss its own
 * window, O6 and TO2, and a filter in front of it would only delay
 * O5 -> O6. LOM is not fed: Table 10-1 acts on LOS and LOF alone. A plain
 * register read, side effect free, unlike DSF_IRQ_EVENT, whose read would
 * take pending bits from the interrupt.
 */
void odi_gpon_line_sample(struct odi_gpon_fsm *fsm, int watch, struct odi_gpon_line_sample *out)
{
	uint32_t alarms = odi_reg_read(ODI_GPON_DSF_ALARM_STATE_OFF);
	int down = (alarms & (ODI_GPON_DSF_ALARM_STATE_LOS_NOW |
			      ODI_GPON_DSF_ALARM_STATE_LOF_NOW)) != 0U;
	enum odi_gpon_event event;

	out->alarms = alarms;
	out->dark = alarms == ODI_GPON_NO_RESPONSE;
	out->fed = 0;
	out->event = ODI_GPON_EVENT_LOS;
	out->cause = "";
	if (out->dark || !watch || !odi_gpon_fsm_line_event(fsm, down, &event))
		return;

	odi_gpon_fsm_handle_event(fsm, odi_gpon_hw_ops(), NULL, event, NULL);
	out->fed = 1;
	out->event = event;
	if (event == ODI_GPON_EVENT_LOS_CLEAR)
		out->cause = "los_cleared";
	else if (alarms & ODI_GPON_DSF_ALARM_STATE_LOS_NOW)
		out->cause = "los";
	else
		out->cause = "lof";
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
