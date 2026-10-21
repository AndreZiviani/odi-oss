/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_hw.h -- register offsets and field pack/unpack helpers for the
 * RTL9602C GPON MAC block (downstream and upstream framers, PLOAM FIFOs,
 * downstream key engine). Written from scratch: the addresses and bit
 * layouts are the register table the stock firmware binary carries (the
 * same table src/diag/tools/regmap-extract.py reads out of it), and every
 * register and field name is ours -- src/diag/tools/regnames.txt is the
 * one list of them, shared with diag, and test/regnames_kernel_test.py
 * fails if a name here drifts from it.
 *
 * Naming: ODI_GPON_<register>_OFF (or _BASE plus <register>(n) for an
 * array), ODI_GPON_<register>_<field>_GET/_SET for a field, and
 * ODI_GPON_<register>_<field> for a one-bit mask.
 *
 * The block sits at switch-core register-space offsets 0x700000-0x706fff,
 * the same register file odi_switch's own MMIO primitive already reaches (base
 * 0x1B000000, odi_switch_hw.h ODI_SWITCH_MMIO_BASE) -- this header only
 * names offsets and packs/unpacks fields; it issues no register access
 * itself and carries no kernel dependency, so a host test can compile and
 * run it with the host cc, unmodified, before it is ever built for the
 * target. The leaves that call odi_reg_read()/odi_reg_write() against
 * these offsets live in odi_gpon_hw.c.
 *
 * Scope: the registers the O1-O7 activation FSM, the PLOAM codec and the
 * key path touch. The GEM-port and T-CONT CAM registers are in
 * odi_switch_hw.h, with the rest of the flow provisioning.
 */
#ifndef ODI_GPON_HW_H
#define ODI_GPON_HW_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* ---- PON MAC global/control, 0x700000-0x7002ff */

/* PONMAC_RESET: 0x70000c */
#define ODI_GPON_PONMAC_RESET_OFF	0x70000cU
#define ODI_GPON_PONMAC_RESET_RESET_COMPLETE	(1U << 8)	/* set once a RESET_REQ pulse has finished */
static inline uint32_t ODI_GPON_PONMAC_RESET_RESET_COMPLETE_GET(uint32_t reg)
{
	return (reg >> 8) & 0x1U;
}
static inline uint32_t ODI_GPON_PONMAC_RESET_RESET_REQ_GET(uint32_t reg)
{
	return (reg >> 0) & 0x1U;
}
static inline uint32_t ODI_GPON_PONMAC_RESET_RESET_REQ_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 0)) | ((val & 0x1U) << 0);
}

/* PONMAC_DECRYPT_BYPASS: 0x700020 */
#define ODI_GPON_PONMAC_DECRYPT_BYPASS_OFF	0x700020U
static inline uint32_t ODI_GPON_PONMAC_DECRYPT_BYPASS_BYPASS_GET(uint32_t reg)
{
	return (reg >> 0) & 0x1U;
}
static inline uint32_t ODI_GPON_PONMAC_DECRYPT_BYPASS_BYPASS_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 0)) | ((val & 0x1U) << 0);
}

/* PONMAC_IRQ_ENABLE: 0x700040 -- top-level aggregate, one bit per sub-block */
#define ODI_GPON_PONMAC_IRQ_ENABLE_OFF	0x700040U
#define ODI_GPON_PONMAC_IRQ_ENABLE_US_GEM	(1U << 6)
#define ODI_GPON_PONMAC_IRQ_ENABLE_US_FRAMER	(1U << 5)
#define ODI_GPON_PONMAC_IRQ_ENABLE_DS_GEM	(1U << 4)
#define ODI_GPON_PONMAC_IRQ_ENABLE_DS_DECRYPT	(1U << 3)
#define ODI_GPON_PONMAC_IRQ_ENABLE_DS_CAPTURE	(1U << 2)
#define ODI_GPON_PONMAC_IRQ_ENABLE_DS_FRAMER	(1U << 1)

/* PONMAC_IRQ_PENDING: 0x700044, same bit layout as PONMAC_IRQ_ENABLE
 * minus bit 0, which has no pending counterpart
 */
#define ODI_GPON_PONMAC_IRQ_PENDING_OFF	0x700044U
#define ODI_GPON_PONMAC_IRQ_PENDING_US_FRAMER	(1U << 5)
#define ODI_GPON_PONMAC_IRQ_PENDING_DS_FRAMER	(1U << 1)

/* ---- Downstream framer, 0x701000-0x7013ff */

/* DSF_IRQ_EVENT: 0x701000 */
#define ODI_GPON_DSF_IRQ_EVENT_OFF	0x701000U
#define ODI_GPON_DSF_IRQ_EVENT_LOS_CHG	(1U << 0)
#define ODI_GPON_DSF_IRQ_EVENT_LOF_CHG	(1U << 1)
#define ODI_GPON_DSF_IRQ_EVENT_FEC_CHG	(1U << 2)
#define ODI_GPON_DSF_IRQ_EVENT_LOM_CHG	(1U << 3)
#define ODI_GPON_DSF_IRQ_EVENT_SN_REQUEST	(1U << 8)
#define ODI_GPON_DSF_IRQ_EVENT_RANGING_REQUEST	(1U << 9)
#define ODI_GPON_DSF_IRQ_EVENT_PLOAM_RX	(1U << 10)
#define ODI_GPON_DSF_IRQ_EVENT_PPS_CHG	(1U << 11)
#define ODI_GPON_DSF_IRQ_EVENT_ANY_EVENT	(1U << 15)

/* DSF_IRQ_ENABLE: 0x701004, same bit positions as DSF_IRQ_EVENT */
#define ODI_GPON_DSF_IRQ_ENABLE_OFF	0x701004U

/* What the bus answers for a register of the GPON MAC that nothing
 * decodes: the block not clocked or not out of reset. It is not register
 * content, and as an alarm word it would read LOS|LOF|FEC|LOM. The top
 * region (0x700000-0x700280) answers at any time; from 0x701000 up this
 * is the reply until the MAC is brought up (another open implementation's
 * measurements).
 */
#define ODI_GPON_NO_RESPONSE	0xca0eca0fU

/* Chip identity, read once at init for the log: the switch chip word, the
 * GPON MAC revision and the MAC memory self-test result. All below
 * 0xf00000 and answering at any time.
 */
#define ODI_GPON_CHIP_ID_OFF		0x010000U
#define ODI_GPON_MAC_REVISION_OFF	0x700010U
#define ODI_GPON_MAC_SELFTEST_OFF	0x700280U

/* DSF_ALARM_STATE: 0x701008 -- the four live alarm bits */
#define ODI_GPON_DSF_ALARM_STATE_OFF	0x701008U
#define ODI_GPON_DSF_ALARM_STATE_LOS_NOW	(1U << 0)
#define ODI_GPON_DSF_ALARM_STATE_LOF_NOW	(1U << 1)
#define ODI_GPON_DSF_ALARM_STATE_FEC_NOW	(1U << 2)
#define ODI_GPON_DSF_ALARM_STATE_LOM_NOW	(1U << 3)

/* DSF_ONU_STATE: 0x701010 -- also the O1-O7 state mirror software
 * maintains
 */
#define ODI_GPON_DSF_ONU_STATE_OFF	0x701010U
static inline uint32_t ODI_GPON_DSF_ONU_STATE_ASSIGNED_ONU_ID_GET(uint32_t reg)
{
	return (reg >> 8) & 0xffU;
}
static inline uint32_t ODI_GPON_DSF_ONU_STATE_ASSIGNED_ONU_ID_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xffU << 8)) | ((val & 0xffU) << 8);
}
static inline uint32_t ODI_GPON_DSF_ONU_STATE_ACTIVATION_STATE_GET(uint32_t reg)
{
	return (reg >> 0) & 0xfU;
}
static inline uint32_t ODI_GPON_DSF_ONU_STATE_ACTIVATION_STATE_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xfU << 0)) | ((val & 0xfU) << 0);
}

/* DSF_SETUP: 0x701014 -- read and re-asserted unchanged during
 * Upstream_Overhead processing in every capture this driver has seen
 * (react.txt: R/W both 0x00000620); field layout not needed by this
 * driver since it only ever replays the value it read back, the same
 * posture as the USF_MIN_RESP_DELAY re-assert (odi_gpon_hw.c
 * hw_set_upstream_overhead()).
 */
#define ODI_GPON_DSF_SETUP_OFF	0x701014U

/* DSF_LOS_SETUP: 0x701040 -- which loss-of-signal sources count, their
 * polarity, and their live state
 */
#define ODI_GPON_DSF_LOS_SETUP_OFF	0x701040U
static inline uint32_t ODI_GPON_DSF_LOS_SETUP_OPTICS_LOS_ON_GET(uint32_t reg)
{
	return (reg >> 0) & 0x1U;
}
static inline uint32_t ODI_GPON_DSF_LOS_SETUP_OPTICS_LOS_ON_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 0)) | ((val & 0x1U) << 0);
}
static inline uint32_t ODI_GPON_DSF_LOS_SETUP_OPTICS_LOS_INVERT_GET(uint32_t reg)
{
	return (reg >> 1) & 0x1U;
}
static inline uint32_t ODI_GPON_DSF_LOS_SETUP_CLOCK_LOS_ON_GET(uint32_t reg)
{
	return (reg >> 2) & 0x1U;
}
static inline uint32_t ODI_GPON_DSF_LOS_SETUP_CLOCK_LOS_INVERT_GET(uint32_t reg)
{
	return (reg >> 3) & 0x1U;
}
static inline uint32_t ODI_GPON_DSF_LOS_SETUP_LOS_DEBOUNCE_GET(uint32_t reg)
{
	return (reg >> 4) & 0x1U;
}
static inline uint32_t ODI_GPON_DSF_LOS_SETUP_OPTICS_LOS_NOW_GET(uint32_t reg)
{
	return (reg >> 8) & 0x1U;
}
static inline uint32_t ODI_GPON_DSF_LOS_SETUP_CLOCK_LOS_NOW_GET(uint32_t reg)
{
	return (reg >> 10) & 0x1U;
}

/* DSF_SUPERFRAME: 0x701048 -- the downstream superframe counter, as the
 * framer keeps it (G.984.3 clause 8.1.3.2: the low 30 bits of the Ident
 * field, one count per 125 us frame)
 */
#define ODI_GPON_DSF_SUPERFRAME_OFF	0x701048U
static inline uint32_t ODI_GPON_DSF_SUPERFRAME_COUNT_GET(uint32_t reg)
{
	return (reg >> 0) & 0x3fffffffU;
}

/* The per-flow downstream GEM counters, behind two indirect pairs, are
 * 32 bits each, clear on read like the link counters above, and read only
 * by the flow sampler (odi_gpon_flow.c): nothing else may read them, the
 * stock firmware command that prints a flow counter included.
 *
 * DSF_FLOW_CNTR_IND: 0x701140 -- write the downstream flow index (the
 * row of the GEM port CAM) in IDX and RSEL (0 counts GEM frames, 1 counts
 * bytes), wait for ACK, then read the count from DSF_FLOW_CNTR_VAL.
 */
#define ODI_GPON_DSF_FLOW_CNTR_IND_OFF	0x701140U
#define ODI_GPON_DSF_FLOW_CNTR_IND_RSEL	(1U << 8)
#define ODI_GPON_DSF_FLOW_CNTR_IND_ACK	(1U << 15)
#define ODI_GPON_DSF_FLOW_CNTR_VAL_OFF	0x701144U

/* The downstream link counters, 0x701180-0x7011c8: each clears when it is
 * read and stays at all ones once full, so the one reader is the counter
 * sampler (odi_gpon_ctr.c, which has the field layout of the shared
 * registers). The REI upstream PLOAM (G.984.3 clause 9.2.4.8) carries the
 * DSF_BIP_ERR_BLOCKS count, the register the react capture reads before
 * every REI send; the sampler keeps it for the BER timer.
 */
#define ODI_GPON_DSF_BIP_ERR_BLOCKS_OFF	0x701180U
#define ODI_GPON_DSF_BIP_ERR_BITS_OFF	0x701184U
#define ODI_GPON_DSF_FEC_CORR_BITS_OFF	0x701188U
#define ODI_GPON_DSF_FEC_CORR_BYTES_OFF	0x70118cU
#define ODI_GPON_DSF_FEC_CORR_CW_OFF	0x701190U
#define ODI_GPON_DSF_FEC_UNCORR_CW_OFF	0x701194U
#define ODI_GPON_DSF_SYNC_ERRS_OFF	0x701198U
#define ODI_GPON_DSF_PLOAM_OK_OFF	0x70119cU
#define ODI_GPON_DSF_PLOAM_ERRS_OFF	0x7011a0U
#define ODI_GPON_DSF_BWMAP_ERRS_OFF	0x7011a4U
#define ODI_GPON_DSF_BWMAP_INVALID_OFF	0x7011a8U
#define ODI_GPON_DSF_ACTIVATION_REQS_OFF	0x7011acU
#define ODI_GPON_DSF_BWMAP_OK_OFF	0x7011b0U
#define ODI_GPON_DSF_GEM_LOS_OFF	0x7011b4U
#define ODI_GPON_DSF_GEM_HEC_CORR_OFF	0x7011b8U
#define ODI_GPON_DSF_GEM_IDLE_OFF	0x7011bcU
#define ODI_GPON_DSF_GEM_ERRS_OFF	0x7011c0U
#define ODI_GPON_DSF_GEM_NON_IDLE_OFF	0x7011c4U
#define ODI_GPON_DSF_PLEND_CORR_OFF	0x7011c8U

/* DSF_PLOAM_RX_CTL: 0x701080 -- downstream PLOAM FIFO handshake */
#define ODI_GPON_DSF_PLOAM_RX_CTL_OFF	0x701080U
static inline uint32_t ODI_GPON_DSF_PLOAM_RX_CTL_POP_GET(uint32_t reg)
{
	return (reg >> 0) & 0x1U;
}
static inline uint32_t ODI_GPON_DSF_PLOAM_RX_CTL_POP_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 0)) | ((val & 0x1U) << 0);
}
static inline uint32_t ODI_GPON_DSF_PLOAM_RX_CTL_FIFO_FULL_GET(uint32_t reg)
{
	return (reg >> 4) & 0x1U;
}
static inline uint32_t ODI_GPON_DSF_PLOAM_RX_CTL_FIFO_EMPTY_GET(uint32_t reg)
{
	return (reg >> 5) & 0x1U;
}

/* DSF_PLOAM_RX_WORD: 0x7010a0, array 0..7 -- only entries 0..5 are read
 * by the stock driver; entries 6-7 exist in the table but carry no
 * message content.
 */
#define ODI_GPON_DSF_PLOAM_RX_WORD_BASE	0x7010a0U
#define ODI_GPON_DSF_PLOAM_RX_WORD(n)	(ODI_GPON_DSF_PLOAM_RX_WORD_BASE + 4U * (uint32_t)(n))
#define ODI_GPON_DS_PLOAM_WORDS	6U	/* words actually carrying message content, of the 8 the array declares */
static inline uint32_t ODI_GPON_DSF_PLOAM_RX_WORD_MSG_WORD_GET(uint32_t reg)
{
	return (reg >> 0) & 0xffffU;
}

/* ---- Downstream GEM, 0x704000-0x7040ff */

/* DS_GEM_MISC_SEL: 0x704064 and DS_GEM_MISC_COUNT: 0x704068 -- one
 * counter at a time: write its index to the selector, read the count.
 * Clear on read (odi_gpon_ctr.c has the indices).
 */
#define ODI_GPON_DS_GEM_MISC_SEL_OFF	0x704064U
#define ODI_GPON_DS_GEM_MISC_COUNT_OFF	0x704068U

/* The per-flow downstream Ethernet counters: two more indirect pairs of
 * the same shape (write the flow index, wait for ACK in bit 15, read the
 * 32-bit count; clear on read, sampled only by odi_gpon_flow.c).
 * DS_GEM_RX_IND/VAL count the Ethernet frames the GEM layer received on
 * the flow, DS_GEM_FWD_IND/VAL those it forwarded on to the switch.
 */
#define ODI_GPON_DS_GEM_RX_IND_OFF	0x704040U
#define ODI_GPON_DS_GEM_RX_VAL_OFF	0x704044U
#define ODI_GPON_DS_GEM_FWD_IND_OFF	0x70404cU
#define ODI_GPON_DS_GEM_FWD_VAL_OFF	0x704050U
#define ODI_GPON_DS_GEM_RX_IND_ACK	(1U << 15)
#define ODI_GPON_DS_GEM_FWD_IND_ACK	(1U << 15)

/* DS_MC_FILTER_CTL: 0x704084 -- the downstream multicast MAC filter, 256
 * rows, one request at a time, with the layout and handshake of the DSF
 * CAM control words: the word with REQ clear, the same with REQ set, then
 * DONE. OP 2 reads a row (ROW_VALID, and DS_MC_FILTER_RDATA), OP 3 clears
 * it.
 */
#define ODI_GPON_DS_MC_FILTER_CTL_OFF	0x704084U
#define ODI_GPON_DS_MC_FILTER_CTL_REQ	(1U << 15)
#define ODI_GPON_DS_MC_FILTER_CTL_DONE	(1U << 14)
#define ODI_GPON_DS_MC_FILTER_CTL_ROW_VALID	(1U << 13)
static inline uint32_t ODI_GPON_DS_MC_FILTER_CTL_OP_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3U << 8)) | ((val & 0x3U) << 8);
}
static inline uint32_t ODI_GPON_DS_MC_FILTER_CTL_ROW_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xffU << 0)) | ((val & 0xffU) << 0);
}

/* DS_MC_FILTER_RDATA: 0x704090 -- the row a read (OP 2) returned */
#define ODI_GPON_DS_MC_FILTER_RDATA_OFF	0x704090U

/* DS_MC_FILTER_BIST: 0x70428c, eight words -- the built-in self-test
 * status of the filter RAM, not filter content: ALL_BIST_DONE when the
 * self-test of the downstream GEM RAMs has run, a FAIL bit when it found
 * a fault. Never written: the other fields start a test.
 */
#define ODI_GPON_DS_MC_FILTER_BIST_BASE	0x70428cU
#define ODI_GPON_DS_MC_FILTER_BIST(n)	(ODI_GPON_DS_MC_FILTER_BIST_BASE + 4U * (uint32_t)(n))
#define ODI_GPON_DS_MC_FILTER_BIST_ALL_BIST_DONE	(1U << 15)
#define ODI_GPON_DS_MC_FILTER_BIST_DRF_BIST_FAIL	(1U << 9)
#define ODI_GPON_DS_MC_FILTER_BIST_BIST_FAIL	(1U << 7)

/* ---- Upstream framer, 0x705000-0x7052ff */

/* USF_IRQ_EVENT: 0x705000 */
#define ODI_GPON_USF_IRQ_EVENT_OFF	0x705000U
#define ODI_GPON_USF_IRQ_EVENT_DYING_GASP_SENT	(1U << 0)
#define ODI_GPON_USF_IRQ_EVENT_FEC_CHG	(1U << 2)
#define ODI_GPON_USF_IRQ_EVENT_URGENT_Q_EMPTY	(1U << 5)
#define ODI_GPON_USF_IRQ_EVENT_NORMAL_Q_EMPTY	(1U << 7)
#define ODI_GPON_USF_IRQ_EVENT_SD_TOO_LONG	(1U << 8)
#define ODI_GPON_USF_IRQ_EVENT_SD_MISMATCH	(1U << 9)
#define ODI_GPON_USF_IRQ_EVENT_ALL_Q_EMPTY	(1U << 10)
#define ODI_GPON_USF_IRQ_EVENT_ANY_EVENT	(1U << 15)

/* USF_IRQ_ENABLE: 0x705004, same bit positions as USF_IRQ_EVENT */
#define ODI_GPON_USF_IRQ_ENABLE_OFF	0x705004U

/* USF_STATE: 0x705008 -- the three live status bits */
#define ODI_GPON_USF_STATE_OFF	0x705008U
#define ODI_GPON_USF_STATE_NORMAL_Q_EMPTY	(1U << 7)
#define ODI_GPON_USF_STATE_URGENT_Q_EMPTY	(1U << 5)
#define ODI_GPON_USF_STATE_FEC_ON	(1U << 2)

/* USF_ONU_ID: 0x705010 */
#define ODI_GPON_USF_ONU_ID_OFF	0x705010U
static inline uint32_t ODI_GPON_USF_ONU_ID_TX_ONU_ID_GET(uint32_t reg)
{
	return (reg >> 8) & 0xffU;
}
static inline uint32_t ODI_GPON_USF_ONU_ID_TX_ONU_ID_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xffU << 8)) | ((val & 0xffU) << 8);
}

/* USF_EQ_DELAY: 0x705044 -- equalization delay, applied once ranging
 * completes: FRAMES whole frames plus EQD bits within the frame
 */
#define ODI_GPON_USF_EQ_DELAY_OFF	0x705044U
static inline uint32_t ODI_GPON_USF_EQ_DELAY_FRAMES_GET(uint32_t reg)
{
	return (reg >> 24) & 0x7U;
}
static inline uint32_t ODI_GPON_USF_EQ_DELAY_FRAMES_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x7U << 24)) | ((val & 0x7U) << 24);
}
static inline uint32_t ODI_GPON_USF_EQ_DELAY_EQD_GET(uint32_t reg)
{
	return (reg >> 0) & 0x3ffffU;
}
static inline uint32_t ODI_GPON_USF_EQ_DELAY_EQD_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3ffffU << 0)) | ((val & 0x3ffffU) << 0);
}

/* USF_MIN_RESP_DELAY: 0x705040 -- minimum-response-delay guard values.
 * DELAY_HI (bits 15:7, 9 bits) is the field this driver's own EqD formula
 * reads before every USF_EQ_DELAY write (odi_gpon_hw.c); DELAY_LO (bits
 * 6:0, 7 bits) is read back unchanged by this driver, not otherwise used.
 */
#define ODI_GPON_USF_MIN_RESP_DELAY_OFF	0x705040U
static inline uint32_t ODI_GPON_USF_MIN_RESP_DELAY_DELAY_HI_GET(uint32_t reg)
{
	return (reg >> 7) & 0x1ffU;
}
static inline uint32_t ODI_GPON_USF_MIN_RESP_DELAY_DELAY_LO_GET(uint32_t reg)
{
	return (reg >> 0) & 0x7fU;
}

/* USF_LASER_MARGIN: 0x70504c -- laser on/off timing margins */
#define ODI_GPON_USF_LASER_MARGIN_OFF	0x70504cU
static inline uint32_t ODI_GPON_USF_LASER_MARGIN_ON_MARGIN_GET(uint32_t reg)
{
	return (reg >> 8) & 0x3fU;
}
static inline uint32_t ODI_GPON_USF_LASER_MARGIN_ON_MARGIN_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3fU << 8)) | ((val & 0x3fU) << 8);
}
static inline uint32_t ODI_GPON_USF_LASER_MARGIN_OFF_MARGIN_GET(uint32_t reg)
{
	return (reg >> 0) & 0x3fU;
}
static inline uint32_t ODI_GPON_USF_LASER_MARGIN_OFF_MARGIN_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3fU << 0)) | ((val & 0x3fU) << 0);
}

/* USF_BURST_HDR_SETUP: 0x705054 -- burst overhead (G.984.3 BOH) length
 * and repeat count
 */
#define ODI_GPON_USF_BURST_HDR_SETUP_OFF	0x705054U
static inline uint32_t ODI_GPON_USF_BURST_HDR_SETUP_REPEAT_GET(uint32_t reg)
{
	return (reg >> 8) & 0xfU;
}
static inline uint32_t ODI_GPON_USF_BURST_HDR_SETUP_REPEAT_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xfU << 8)) | ((val & 0xfU) << 8);
}
static inline uint32_t ODI_GPON_USF_BURST_HDR_SETUP_LENGTH_GET(uint32_t reg)
{
	return (reg >> 0) & 0xffU;
}
static inline uint32_t ODI_GPON_USF_BURST_HDR_SETUP_LENGTH_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xffU << 0)) | ((val & 0xffU) << 0);
}

/* USF_BURST_HDR_BYTE: 0x705080, array 0..11 -- the burst-overhead
 * pattern bytes, one per array entry
 */
#define ODI_GPON_USF_BURST_HDR_BYTE_BASE	0x705080U
#define ODI_GPON_USF_BURST_HDR_BYTE(n)	(ODI_GPON_USF_BURST_HDR_BYTE_BASE + 4U * (uint32_t)(n))
static inline uint32_t ODI_GPON_USF_BURST_HDR_BYTE_VALUE_GET(uint32_t reg)
{
	return (reg >> 0) & 0xffU;
}
static inline uint32_t ODI_GPON_USF_BURST_HDR_BYTE_VALUE_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xffU << 0)) | ((val & 0xffU) << 0);
}

/* USF_PLOAM_TX_CTL: 0x7050c0 -- upstream PLOAM enqueue handshake, two
 * priority queues
 */
#define ODI_GPON_USF_PLOAM_TX_CTL_OFF	0x7050c0U
#define ODI_GPON_USF_PLOAM_TX_CTL_URGENT_FULL	(1U << 4)
static inline uint32_t ODI_GPON_USF_PLOAM_TX_CTL_PUSH_GET(uint32_t reg)
{
	return (reg >> 0) & 0x1U;
}
static inline uint32_t ODI_GPON_USF_PLOAM_TX_CTL_PUSH_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 0)) | ((val & 0x1U) << 0);
}
static inline uint32_t ODI_GPON_USF_PLOAM_TX_CTL_URGENT_FULL_GET(uint32_t reg)
{
	return (reg >> 4) & 0x1U;
}
static inline uint32_t ODI_GPON_USF_PLOAM_TX_CTL_URGENT_EMPTY_GET(uint32_t reg)
{
	return (reg >> 5) & 0x1U;
}
static inline uint32_t ODI_GPON_USF_PLOAM_TX_CTL_NORMAL_FULL_GET(uint32_t reg)
{
	return (reg >> 6) & 0x1U;
}
static inline uint32_t ODI_GPON_USF_PLOAM_TX_CTL_NORMAL_EMPTY_GET(uint32_t reg)
{
	return (reg >> 7) & 0x1U;
}
static inline uint32_t ODI_GPON_USF_PLOAM_TX_CTL_MSG_TYPE_GET(uint32_t reg)
{
	return (reg >> 8) & 0x7U;
}
static inline uint32_t ODI_GPON_USF_PLOAM_TX_CTL_MSG_TYPE_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x7U << 8)) | ((val & 0x7U) << 8);
}
/* MSG_TYPE values (observed on the wire, not a G.984.3 fact): NOT a
 * simple urgent/normal binary split (an earlier draft guess, flagged
 * wrong by direct capture evidence: two real activation captures show
 * values 0x01 (every software-driven reply this driver sends at run time
 * -- Acknowledge, Encryption_Key, and by extension Password, all observed
 * at USF_PLOAM_TX_CTL 0x100/0x101), 0x05, 0x06, 0x07 (the three messages
 * gpondev/gponsn pre-arm once into their own fixed FIFO slots for
 * AUTONOMOUS hardware transmission -- Dying_Gasp, Serial_Number_ONU,
 * No_Message respectively -- never re-sent by software after that one
 * init write, odi_gpon_init.c). ODI_GPON_PLOAM_TYPE_REPLY is the one
 * value this driver's own run-time send path (odi_gpon_hw.c send_us_ploam)
 * ever uses; the three pre-armed slot values are baked into the generated
 * init table's own literal writes, not named here.
 */
#define ODI_GPON_PLOAM_TYPE_REPLY	1U

/* USF_PLOAM_TX_WORD: 0x7050e0, array 0..7 -- only entries 0..5 are
 * written by the stock driver, mirroring the downstream read
 */
#define ODI_GPON_USF_PLOAM_TX_WORD_BASE	0x7050e0U
#define ODI_GPON_USF_PLOAM_TX_WORD(n)	(ODI_GPON_USF_PLOAM_TX_WORD_BASE + 4U * (uint32_t)(n))
#define ODI_GPON_US_PLOAM_WORDS	6U
static inline uint32_t ODI_GPON_USF_PLOAM_TX_WORD_MSG_WORD_GET(uint32_t reg)
{
	return (reg >> 0) & 0xffffU;
}
static inline uint32_t ODI_GPON_USF_PLOAM_TX_WORD_MSG_WORD_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xffffU << 0)) | ((val & 0xffffU) << 0);
}

/* USF_PLOAM_TX_SETUP: 0x705100 */
#define ODI_GPON_USF_PLOAM_TX_SETUP_OFF	0x705100U
static inline uint32_t ODI_GPON_USF_PLOAM_TX_SETUP_ONU_ID_OVERRIDE_GET(uint32_t reg)
{
	return (reg >> 0) & 0x1U;
}
static inline uint32_t ODI_GPON_USF_PLOAM_TX_SETUP_CRC_INSERT_GET(uint32_t reg)
{
	return (reg >> 1) & 0x1U;
}
static inline uint32_t ODI_GPON_USF_PLOAM_TX_SETUP_FLUSH_GET(uint32_t reg)
{
	return (reg >> 4) & 0x1U;
}
static inline uint32_t ODI_GPON_USF_PLOAM_TX_SETUP_FLUSH_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 4)) | ((val & 0x1U) << 4);
}

/* USF_DYING_GASP: 0x705184 -- dying-gasp status */
#define ODI_GPON_USF_DYING_GASP_OFF	0x705184U
static inline uint32_t ODI_GPON_USF_DYING_GASP_ACTIVE_GET(uint32_t reg)
{
	return (reg >> 8) & 0x1U;
}
static inline uint32_t ODI_GPON_USF_DYING_GASP_SENT_COUNT_GET(uint32_t reg)
{
	return (reg >> 4) & 0xfU;
}
static inline uint32_t ODI_GPON_USF_DYING_GASP_SEND_LIMIT_GET(uint32_t reg)
{
	return (reg >> 0) & 0xfU;
}

/* USF_MISC_SEL: 0x705140 and USF_MISC_COUNT: 0x705148 -- the upstream
 * counters, selected the same way as DS_GEM_MISC_SEL. Clear on read.
 */
#define ODI_GPON_USF_MISC_SEL_OFF	0x705140U
#define ODI_GPON_USF_MISC_COUNT_OFF	0x705148U

/* ---- Downstream key engine, 0x703000-0x70329f */

/* DSK_SWITCH_ARM: 0x703010 -- arms the hitless key switchover at the
 * superframe count in DSK_SWITCH_FRAME
 */
#define ODI_GPON_DSK_SWITCH_ARM_OFF	0x703010U
static inline uint32_t ODI_GPON_DSK_SWITCH_ARM_ARM_GET(uint32_t reg)
{
	return (reg >> 15) & 0x1U;
}
static inline uint32_t ODI_GPON_DSK_SWITCH_ARM_ARM_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 15)) | ((val & 0x1U) << 15);
}
static inline uint32_t ODI_GPON_DSK_SWITCH_ARM_ACTIVE_SLOT_GET(uint32_t reg)
{
	return (reg >> 14) & 0x1U;
}

/* DSK_SWITCH_FRAME: 0x703014 -- the downstream superframe count to switch
 * keys at, sourced from the OLT's own Key_switching_time PLOAM message
 */
#define ODI_GPON_DSK_SWITCH_FRAME_OFF	0x703014U
static inline uint32_t ODI_GPON_DSK_SWITCH_FRAME_SUPERFRAME_GET(uint32_t reg)
{
	return (reg >> 0) & 0x3fffffffU;
}
static inline uint32_t ODI_GPON_DSK_SWITCH_FRAME_SUPERFRAME_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x3fffffffU << 0)) | ((val & 0x3fffffffU) << 0);
}

/* DSK_KEY_LOAD: 0x703020 -- one 16-bit word of a 128-bit (8-word) key per
 * write, targeting one of the two hitless-switch key slots
 */
#define ODI_GPON_DSK_KEY_LOAD_OFF	0x703020U
#define ODI_GPON_DSK_KEY_LOAD_WRITE_DONE	(1U << 14)	/* set once the word written is taken */
static inline uint32_t ODI_GPON_DSK_KEY_LOAD_WRITE_REQ_GET(uint32_t reg)
{
	return (reg >> 15) & 0x1U;
}
static inline uint32_t ODI_GPON_DSK_KEY_LOAD_WRITE_REQ_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 15)) | ((val & 0x1U) << 15);
}
static inline uint32_t ODI_GPON_DSK_KEY_LOAD_WRITE_DONE_GET(uint32_t reg)
{
	return (reg >> 14) & 0x1U;
}
static inline uint32_t ODI_GPON_DSK_KEY_LOAD_SLOT_GET(uint32_t reg)
{
	return (reg >> 7) & 0x1U;
}
static inline uint32_t ODI_GPON_DSK_KEY_LOAD_SLOT_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x1U << 7)) | ((val & 0x1U) << 7);
}
static inline uint32_t ODI_GPON_DSK_KEY_LOAD_WORD_INDEX_GET(uint32_t reg)
{
	return (reg >> 0) & 0x7U;
}
static inline uint32_t ODI_GPON_DSK_KEY_LOAD_WORD_INDEX_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0x7U << 0)) | ((val & 0x7U) << 0);
}
#define ODI_GPON_DSK_KEY_WORDS	8U	/* 8 x 16-bit words = 128-bit AES key */
#define ODI_GPON_DSK_SLOT_CURRENT	0U
#define ODI_GPON_DSK_SLOT_NEXT	1U

/* DSK_KEY_WORD: 0x703024 */
#define ODI_GPON_DSK_KEY_WORD_OFF	0x703024U
static inline uint32_t ODI_GPON_DSK_KEY_WORD_KEY_BITS_GET(uint32_t reg)
{
	return (reg >> 0) & 0xffffU;
}
static inline uint32_t ODI_GPON_DSK_KEY_WORD_KEY_BITS_SET(uint32_t reg, uint32_t val)
{
	return (reg & ~(0xffffU << 0)) | ((val & 0xffffU) << 0);
}

/* ---- Upstream GEM counters, 0x706000-0x706fff */

/* US_FLOW_CNTR_IND: 0x706048 -- the per-flow upstream counters, 32 bits
 * each and clear on read (sampled only by odi_gpon_flow.c): write the
 * upstream flow index, with bit 7 (ETH) set to count Ethernet frames
 * instead of GEM frames, wait for ACK, and read the count from
 * US_FLOW_CNTR_VAL.
 */
#define ODI_GPON_US_FLOW_CNTR_IND_OFF	0x706048U
#define ODI_GPON_US_FLOW_CNTR_IND_IDX_ETH	(1U << 7)
#define ODI_GPON_US_FLOW_CNTR_IND_ACK	(1U << 15)
#define ODI_GPON_US_FLOW_CNTR_VAL_OFF	0x70604cU

/* US_FLOW_BYTES: 0x706800, array 0..127 of 64 bits, two words each --
 * the bytes sent on upstream flow n, clear on read. The low word is at
 * +0 of the entry and the high word at +4; the sampler reads the low one
 * first.
 */
#define ODI_GPON_US_FLOW_BYTES_BASE	0x706800U
#define ODI_GPON_US_FLOW_BYTES(n)	(ODI_GPON_US_FLOW_BYTES_BASE + 8U * (uint32_t)(n))

#endif /* ODI_GPON_HW_H */
