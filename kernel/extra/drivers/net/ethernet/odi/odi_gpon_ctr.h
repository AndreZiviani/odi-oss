/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_ctr.h -- the GPON MAC link counters, accumulated, and the link
 * state accounting kept beside them, for /proc/odi_gpon/status and
 * /proc/odi_gpon/counters.
 *
 * Every hardware counter here clears when it is read and stays at all ones
 * once full. So there is one reader, odi_gpon_ctr_sample(), run by
 * odi_gpon.c under odi_gpon_lock from a 1 s timer (and from the BER timer
 * just before each REI), and it adds each read into a 64-bit total. A read
 * of all ones is counted as saturated: that sample lost counts. The first
 * sample only clears the counters (they hold whatever accumulated since the
 * block was reset), so every total counts from the first sample on.
 *
 * The alarm accounting has a second feed: odi_gpon.c's line check, which
 * reads DSF_ALARM_STATE every 100 ms while the ONU is activated and at
 * every interrupt, hands each reading to odi_gpon_ctr_note_alarms() too.
 * Both feeds go through the same edge and time accounting, so an alarm is
 * resolved to the line poll while it runs and to the second otherwise.
 *
 * Pure: register access through odi_reg_read()/odi_reg_write(), time and
 * ONU state passed in, so test/odi_gpon_ctr_test.c runs it on the host
 * against a clear-on-read register model.
 */
#ifndef ODI_GPON_CTR_H
#define ODI_GPON_CTR_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* The counters, in /proc/odi_gpon/counters order. odi_gpon_ctr.c has the register
 * and field each one comes from.
 */
enum odi_gpon_ctr_id {
	/* downstream framer: DSF_* 0x701180-0x7011c8 */
	ODI_GPON_CTR_DS_BIP_ERR_BLOCKS,
	ODI_GPON_CTR_DS_BIP_ERR_BITS,
	ODI_GPON_CTR_DS_FEC_CORRECTED_BITS,
	ODI_GPON_CTR_DS_FEC_CORRECTED_BYTES,
	ODI_GPON_CTR_DS_FEC_CORRECTED_CW,
	ODI_GPON_CTR_DS_FEC_UNCORRECTABLE_CW,
	ODI_GPON_CTR_DS_SUPERFRAME_LOSS,
	ODI_GPON_CTR_DS_PLEND_FAIL,
	ODI_GPON_CTR_DS_PLOAM_ACCEPTED,
	ODI_GPON_CTR_DS_PLOAM_CRC_ERR,
	ODI_GPON_CTR_DS_PLOAM_OVERFLOW,
	ODI_GPON_CTR_DS_BWMAP_CRC_ERR,
	ODI_GPON_CTR_DS_BWMAP_OVERFLOW,
	ODI_GPON_CTR_DS_BWMAP_INVALID_0,
	ODI_GPON_CTR_DS_BWMAP_INVALID_1,
	ODI_GPON_CTR_DS_SN_REQUESTS,
	ODI_GPON_CTR_DS_RANGING_REQUESTS,
	ODI_GPON_CTR_DS_BWMAP_ACCEPTED,
	ODI_GPON_CTR_DS_GEM_LOS,
	ODI_GPON_CTR_DS_GEM_HEC_CORRECTED,
	ODI_GPON_CTR_DS_GEM_IDLE,
	ODI_GPON_CTR_DS_GEM_LEN_MISMATCH,
	ODI_GPON_CTR_DS_GEM_PORT_MISMATCH,
	ODI_GPON_CTR_DS_GEM_NON_IDLE,
	ODI_GPON_CTR_DS_PLEND_CORRECTED,
	ODI_GPON_CTR_DS_PLEND_UNCORRECTABLE,
	ODI_GPON_CTR_DS_BLEN_TOO_LONG,
	/* downstream GEM: DS_GEM_MISC_SEL index 0-6 */
	ODI_GPON_CTR_DS_ETH_MULTICAST,
	ODI_GPON_CTR_DS_ETH_UNICAST,
	ODI_GPON_CTR_DS_ETH_MULTICAST_FWD,
	ODI_GPON_CTR_DS_ETH_MULTICAST_LEAK,
	ODI_GPON_CTR_DS_ETH_FCS_ERR,
	ODI_GPON_CTR_DS_GEM_OVER_INTERLEAVE,
	ODI_GPON_CTR_DS_OMCI,
	/* upstream framer: USF_MISC_SEL index 0-4 */
	ODI_GPON_CTR_US_BURSTS,
	ODI_GPON_CTR_US_PLOAM,
	ODI_GPON_CTR_US_DBRU,
	ODI_GPON_CTR_US_GEM_FRAMES,
	ODI_GPON_CTR_US_PLOAM_CPU_NORMAL,
	ODI_GPON_CTR_US_PLOAM_CPU_URGENT,
	ODI_GPON_CTR_US_PLOAM_NO_MESSAGE,
	ODI_GPON_CTR_US_PLOAM_SN,
	ODI_GPON_CTR_US_GEM_BYTES,
	ODI_GPON_CTR_COUNT
};

/* Code words in one downstream frame with FEC on: 152 of 255 bytes and one
 * shortened last code word (G.984.3 clause 13.2.1: 152 x 255 + 120 = 38880
 * bytes, the 125 us frame at 2.48832 Gbit/s).
 */
#define ODI_GPON_FEC_CW_PER_FRAME	153U

/* The three downstream alarms DSF_ALARM_STATE reports, in this order. */
#define ODI_GPON_CTR_ALARMS	3U
/* Signal fail and signal degrade, in this order. */
#define ODI_GPON_SIGNAL_SF	0U
#define ODI_GPON_SIGNAL_SD	1U
#define ODI_GPON_SIGNALS	2U
#define ODI_GPON_SIGNAL_SF_DEFAULT	5U	/* G.988 ANI-G SF threshold default */
#define ODI_GPON_SIGNAL_SD_DEFAULT	9U	/* and SD */
/* One evaluation per 10 s of O5: the interval the stock driver evaluates
 * at on ISP1 (its BER_Interval, 80000 frames), long enough to count a
 * 10^-9 BER (25 bit errors at 2.48832 Gbit/s).
 */
#define ODI_GPON_SIGNAL_WINDOW_MS	10000U

/* Indexed by enum odi_gpon_state (1-7); 0 is "not started". */
#define ODI_GPON_CTR_STATES	8U

struct odi_gpon_ctr {
	uint64_t total[ODI_GPON_CTR_COUNT];
	/* ds_fec_total_cw: downstream code words received with FEC on and the
	 * line locked, derived (no register counts it): ODI_GPON_FEC_CW_PER_FRAME
	 * for each 125 us frame the superframe counter advanced between two
	 * samples that both saw FEC on and LOS/LOF clear. Shown beside the
	 * totals, but not one of them (it has no register).
	 */
	uint64_t ds_fec_total_cw;
	int fec_locked;		/* the last sample saw FEC on, LOS and LOF clear */
	uint32_t saturated[ODI_GPON_CTR_COUNT];	/* samples that read all ones */
	uint32_t samples;	/* runs of odi_gpon_ctr_sample(), the first included */
	uint32_t last_ms;	/* time of the last sample, wrapping */
	unsigned int last_state;	/* ONU state at the last sample */
	uint32_t alarm_state;	/* DSF_ALARM_STATE, last reading of either feed */
	uint32_t alarm_last_ms;	/* time of that reading */
	int alarm_seen;		/* a reading has been noted */
	uint32_t us_state;	/* USF_STATE at the last sample */
	uint32_t superframe;	/* DSF_SUPERFRAME count at the last sample */
	/* DSF_BIP_ERR_BLOCKS counted in O5 and not yet sent in an REI */
	uint64_t rei_pending;

	/* Signal fail and signal degrade (G.984.3 11.1.2, ONU side): the
	 * downstream BER over windows of ODI_GPON_SIGNAL_WINDOW_MS of O5,
	 * from DSF_BIP_ERR_BITS. Set when the BER is >= 10^-exp, cleared when
	 * it is < 10^-(exp+1), kept in between; both cleared outside O5,
	 * where nothing is measured. exp is the ANI-G threshold the OLT set
	 * (cmd 11), defaults 5 (SF) and 9 (SD).
	 */
	uint64_t signal_bits;		/* BIP bit errors in O5, this window */
	uint32_t signal_ms;		/* O5 time in this window */
	uint64_t signal_last_bits;	/* the last whole window */
	uint32_t signal_last_ms;
	uint8_t signal_exp[ODI_GPON_SIGNALS];
	uint8_t signal_on[ODI_GPON_SIGNALS];
	uint32_t signal_events[ODI_GPON_SIGNALS];	/* clear -> set */
	uint32_t alarm_events[ODI_GPON_CTR_ALARMS];	/* clear -> set, as read */
	uint64_t alarm_ms[ODI_GPON_CTR_ALARMS];	/* time seen set */
	uint64_t state_ms[ODI_GPON_CTR_STATES];	/* time in each ONU state */
	uint32_t transitions[ODI_GPON_CTR_STATES][ODI_GPON_CTR_STATES];	/* [from][to] */
};

/* The /proc/odi_gpon/counters name of each counter, "ds_bip_err_blocks" and so on. */
extern const char *const odi_gpon_ctr_names[ODI_GPON_CTR_COUNT];

void odi_gpon_ctr_reset(struct odi_gpon_ctr *c);

/* Read every counter once and add it in; then the alarm and state
 * accounting for the time since the last sample. now_ms is any
 * millisecond clock (only differences are used, modulo 2^32); state is the
 * ONU state (enum odi_gpon_state) now. Called with odi_gpon_lock held.
 */
void odi_gpon_ctr_sample(struct odi_gpon_ctr *c, uint32_t now_ms, unsigned int state);

/* One DSF_ALARM_STATE reading: a set bit that was clear at the previous
 * reading is an event, and the time since the previous reading is added to
 * each alarm that was set then. odi_gpon_ctr_sample() calls it with its own
 * read; the line check calls it with each of its. Lock held.
 */
void odi_gpon_ctr_note_alarms(struct odi_gpon_ctr *c, uint32_t alarms, uint32_t now_ms);

/* The DSF_BIP_ERR_BLOCKS count gathered in O5 since the last call, for the
 * REI error count (clamped to 32 bits), and starts the next interval.
 */
uint32_t odi_gpon_ctr_rei_take(struct odi_gpon_ctr *c);

/* The threshold exponent of signal (ODI_GPON_SIGNAL_SF or _SD): the BER
 * 10^-exp at which it is set. The ranges G.988 gives the ANI-G attributes:
 * SF 3-8, SD 4-10. -1 outside them, nothing changed. Lock held.
 */
int odi_gpon_ctr_signal_threshold_set(struct odi_gpon_ctr *c, unsigned int signal,
				      unsigned int exp);

/* One ONU state transition, from the driver transition note. */
void odi_gpon_ctr_note_transition(struct odi_gpon_ctr *c, unsigned int from, unsigned int to);

/* The lines of /proc/odi_gpon/status and /proc/odi_gpon/counters that come
 * from the sampler, each passed to put() whole, newline included. The status
 * ones are the live alarm and FEC bits, the alarm and ONU state times and the
 * state transitions; the counters ones are the sample count and the 64-bit
 * totals. docs/TOOLS.md, "/proc/odi_gpon/", has the format.
 */
typedef void (*odi_gpon_ctr_put_fn)(void *ctx, const char *line);
void odi_gpon_ctr_show_status(const struct odi_gpon_ctr *c, odi_gpon_ctr_put_fn put, void *ctx);
void odi_gpon_ctr_show_counters(const struct odi_gpon_ctr *c, odi_gpon_ctr_put_fn put, void *ctx);

#endif /* ODI_GPON_CTR_H */
