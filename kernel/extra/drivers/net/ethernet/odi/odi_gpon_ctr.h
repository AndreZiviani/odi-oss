/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_ctr.h -- the GPON MAC link counters, accumulated, and the link
 * state accounting kept beside them, for /proc/odi_gpon.
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

/* The counters, in /proc/odi_gpon order. odi_gpon_ctr.c has the register
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

/* The three downstream alarms DSF_ALARM_STATE reports, in this order. */
#define ODI_GPON_CTR_ALARMS	3U
/* Indexed by enum odi_gpon_state (1-7); 0 is "not started". */
#define ODI_GPON_CTR_STATES	8U

struct odi_gpon_ctr {
	uint64_t total[ODI_GPON_CTR_COUNT];
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
	uint32_t alarm_events[ODI_GPON_CTR_ALARMS];	/* clear -> set, as read */
	uint64_t alarm_ms[ODI_GPON_CTR_ALARMS];	/* time seen set */
	uint64_t state_ms[ODI_GPON_CTR_STATES];	/* time in each ONU state */
	uint32_t transitions[ODI_GPON_CTR_STATES][ODI_GPON_CTR_STATES];	/* [from][to] */
};

/* The /proc/odi_gpon name of each counter, "ds_bip_err_blocks" and so on. */
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

/* One ONU state transition, from the driver transition note. */
void odi_gpon_ctr_note_transition(struct odi_gpon_ctr *c, unsigned int from, unsigned int to);

/* The /proc/odi_gpon lines, each passed to put() whole, newline included.
 * docs/TOOLS.md, "/proc/odi_gpon", has the format.
 */
typedef void (*odi_gpon_ctr_put_fn)(void *ctx, const char *line);
void odi_gpon_ctr_show(const struct odi_gpon_ctr *c, odi_gpon_ctr_put_fn put, void *ctx);

#endif /* ODI_GPON_CTR_H */
