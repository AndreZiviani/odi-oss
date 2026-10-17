// SPDX-License-Identifier: GPL-2.0
/*
 * odi_gpon_ctr.c -- the GPON MAC link counters, accumulated into 64-bit
 * totals, and the alarm and ONU state accounting kept beside them
 * (odi_gpon_ctr.h has the contract).
 *
 * Three banks of clear-on-read counters, all in the stock firmware register
 * table:
 *
 *  - the downstream framer block, DSF_BIP_ERR_BLOCKS..DSF_PLEND_CORR,
 *    read directly. Several registers carry two or three counts in 16-bit
 *    or 8-bit fields, so each register is read once and split;
 *  - the downstream GEM counters behind DS_GEM_MISC_SEL, and
 *  - the upstream counters behind USF_MISC_SEL: write the index, read the
 *    count register. The upstream ones pack two 16-bit counts as well.
 *
 * Which index and field is which is the `ctr_regs` table below; the names
 * are G.984.3 terms (BIP clause 8.1.3.3, PLend 8.1.3.5 with its Table 8-a,
 * BWmap 8.1.3.6, GEM 8.3, PLOAMu and DBRu 8.2.3).
 */
#include "odi_gpon_ctr.h"
#include "odi_gpon_fsm.h"
#include "odi_gpon_hw.h"
#include "odi_switch_reg.h"

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/string.h>
#else
#include <stdio.h>
#include <string.h>
#endif

const char *const odi_gpon_ctr_names[ODI_GPON_CTR_COUNT] = {
	[ODI_GPON_CTR_DS_BIP_ERR_BLOCKS] = "ds_bip_err_blocks",
	[ODI_GPON_CTR_DS_BIP_ERR_BITS] = "ds_bip_err_bits",
	[ODI_GPON_CTR_DS_FEC_CORRECTED_BITS] = "ds_fec_corrected_bits",
	[ODI_GPON_CTR_DS_FEC_CORRECTED_BYTES] = "ds_fec_corrected_bytes",
	[ODI_GPON_CTR_DS_FEC_CORRECTED_CW] = "ds_fec_corrected_cw",
	[ODI_GPON_CTR_DS_FEC_UNCORRECTABLE_CW] = "ds_fec_uncorrectable_cw",
	[ODI_GPON_CTR_DS_SUPERFRAME_LOSS] = "ds_superframe_loss",
	[ODI_GPON_CTR_DS_PLEND_FAIL] = "ds_plend_fail",
	[ODI_GPON_CTR_DS_PLOAM_ACCEPTED] = "ds_ploam_accepted",
	[ODI_GPON_CTR_DS_PLOAM_CRC_ERR] = "ds_ploam_crc_err",
	[ODI_GPON_CTR_DS_PLOAM_OVERFLOW] = "ds_ploam_overflow",
	[ODI_GPON_CTR_DS_BWMAP_CRC_ERR] = "ds_bwmap_crc_err",
	[ODI_GPON_CTR_DS_BWMAP_OVERFLOW] = "ds_bwmap_overflow",
	[ODI_GPON_CTR_DS_BWMAP_INVALID_0] = "ds_bwmap_invalid_0",
	[ODI_GPON_CTR_DS_BWMAP_INVALID_1] = "ds_bwmap_invalid_1",
	[ODI_GPON_CTR_DS_SN_REQUESTS] = "ds_sn_requests",
	[ODI_GPON_CTR_DS_RANGING_REQUESTS] = "ds_ranging_requests",
	[ODI_GPON_CTR_DS_BWMAP_ACCEPTED] = "ds_bwmap_accepted",
	[ODI_GPON_CTR_DS_GEM_LOS] = "ds_gem_los",
	[ODI_GPON_CTR_DS_GEM_HEC_CORRECTED] = "ds_gem_hec_corrected",
	[ODI_GPON_CTR_DS_GEM_IDLE] = "ds_gem_idle",
	[ODI_GPON_CTR_DS_GEM_LEN_MISMATCH] = "ds_gem_len_mismatch",
	[ODI_GPON_CTR_DS_GEM_PORT_MISMATCH] = "ds_gem_port_mismatch",
	[ODI_GPON_CTR_DS_GEM_NON_IDLE] = "ds_gem_non_idle",
	[ODI_GPON_CTR_DS_PLEND_CORRECTED] = "ds_plend_corrected",
	[ODI_GPON_CTR_DS_PLEND_UNCORRECTABLE] = "ds_plend_uncorrectable",
	[ODI_GPON_CTR_DS_BLEN_TOO_LONG] = "ds_blen_too_long",
	[ODI_GPON_CTR_DS_ETH_MULTICAST] = "ds_eth_multicast",
	[ODI_GPON_CTR_DS_ETH_UNICAST] = "ds_eth_unicast",
	[ODI_GPON_CTR_DS_ETH_MULTICAST_FWD] = "ds_eth_multicast_fwd",
	[ODI_GPON_CTR_DS_ETH_MULTICAST_LEAK] = "ds_eth_multicast_leak",
	[ODI_GPON_CTR_DS_ETH_FCS_ERR] = "ds_eth_fcs_err",
	[ODI_GPON_CTR_DS_GEM_OVER_INTERLEAVE] = "ds_gem_over_interleave",
	[ODI_GPON_CTR_DS_OMCI] = "ds_omci",
	[ODI_GPON_CTR_US_BURSTS] = "us_bursts",
	[ODI_GPON_CTR_US_PLOAM] = "us_ploam",
	[ODI_GPON_CTR_US_DBRU] = "us_dbru",
	[ODI_GPON_CTR_US_GEM_FRAMES] = "us_gem_frames",
	[ODI_GPON_CTR_US_PLOAM_CPU_NORMAL] = "us_ploam_cpu_normal",
	[ODI_GPON_CTR_US_PLOAM_CPU_URGENT] = "us_ploam_cpu_urgent",
	[ODI_GPON_CTR_US_PLOAM_NO_MESSAGE] = "us_ploam_no_message",
	[ODI_GPON_CTR_US_PLOAM_SN] = "us_ploam_sn",
	[ODI_GPON_CTR_US_GEM_BYTES] = "us_gem_bytes",
};

enum ctr_bank {
	CTR_BANK_DSF,		/* `sel` is the register offset */
	CTR_BANK_DS_GEM,	/* `sel` is the DS_GEM_MISC_SEL index */
	CTR_BANK_USF,		/* `sel` is the USF_MISC_SEL index */
};

struct ctr_field {
	uint8_t id;	/* enum odi_gpon_ctr_id */
	uint8_t lsb;
	uint8_t width;
};

struct ctr_reg {
	uint8_t bank;	/* enum ctr_bank */
	uint8_t nfields;
	uint32_t sel;
	struct ctr_field f[3];
};

#define CTR1(bank, sel, id) \
	{ bank, 1, sel, { { id, 0, 32 } } }
#define CTR2(bank, sel, lo, hi) \
	{ bank, 2, sel, { { lo, 0, 16 }, { hi, 16, 16 } } }

/* Every counter exactly once (test/odi_gpon_ctr_test.c checks it). The
 * field split is the stock firmware register table; the meaning of each
 * index and field is what the stock firmware's own counter commands print
 * for it, matched against G.984.3.
 */
static const struct ctr_reg ctr_regs[] = {
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_BIP_ERR_BLOCKS_OFF, ODI_GPON_CTR_DS_BIP_ERR_BLOCKS),
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_BIP_ERR_BITS_OFF, ODI_GPON_CTR_DS_BIP_ERR_BITS),
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_FEC_CORR_BITS_OFF, ODI_GPON_CTR_DS_FEC_CORRECTED_BITS),
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_FEC_CORR_BYTES_OFF, ODI_GPON_CTR_DS_FEC_CORRECTED_BYTES),
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_FEC_CORR_CW_OFF, ODI_GPON_CTR_DS_FEC_CORRECTED_CW),
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_FEC_UNCORR_CW_OFF, ODI_GPON_CTR_DS_FEC_UNCORRECTABLE_CW),
	CTR2(CTR_BANK_DSF, ODI_GPON_DSF_SYNC_ERRS_OFF,
	     ODI_GPON_CTR_DS_SUPERFRAME_LOSS, ODI_GPON_CTR_DS_PLEND_FAIL),
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_PLOAM_OK_OFF, ODI_GPON_CTR_DS_PLOAM_ACCEPTED),
	CTR2(CTR_BANK_DSF, ODI_GPON_DSF_PLOAM_ERRS_OFF,
	     ODI_GPON_CTR_DS_PLOAM_CRC_ERR, ODI_GPON_CTR_DS_PLOAM_OVERFLOW),
	CTR2(CTR_BANK_DSF, ODI_GPON_DSF_BWMAP_ERRS_OFF,
	     ODI_GPON_CTR_DS_BWMAP_CRC_ERR, ODI_GPON_CTR_DS_BWMAP_OVERFLOW),
	CTR2(CTR_BANK_DSF, ODI_GPON_DSF_BWMAP_INVALID_OFF,
	     ODI_GPON_CTR_DS_BWMAP_INVALID_0, ODI_GPON_CTR_DS_BWMAP_INVALID_1),
	CTR2(CTR_BANK_DSF, ODI_GPON_DSF_ACTIVATION_REQS_OFF,
	     ODI_GPON_CTR_DS_SN_REQUESTS, ODI_GPON_CTR_DS_RANGING_REQUESTS),
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_BWMAP_OK_OFF, ODI_GPON_CTR_DS_BWMAP_ACCEPTED),
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_GEM_LOS_OFF, ODI_GPON_CTR_DS_GEM_LOS),
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_GEM_HEC_CORR_OFF, ODI_GPON_CTR_DS_GEM_HEC_CORRECTED),
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_GEM_IDLE_OFF, ODI_GPON_CTR_DS_GEM_IDLE),
	CTR2(CTR_BANK_DSF, ODI_GPON_DSF_GEM_ERRS_OFF,
	     ODI_GPON_CTR_DS_GEM_LEN_MISMATCH, ODI_GPON_CTR_DS_GEM_PORT_MISMATCH),
	CTR1(CTR_BANK_DSF, ODI_GPON_DSF_GEM_NON_IDLE_OFF, ODI_GPON_CTR_DS_GEM_NON_IDLE),
	{ CTR_BANK_DSF, 3, ODI_GPON_DSF_PLEND_CORR_OFF,
	  { { ODI_GPON_CTR_DS_PLEND_CORRECTED, 0, 16 },
	    { ODI_GPON_CTR_DS_PLEND_UNCORRECTABLE, 16, 8 },
	    { ODI_GPON_CTR_DS_BLEN_TOO_LONG, 24, 8 } } },

	CTR1(CTR_BANK_DS_GEM, 0, ODI_GPON_CTR_DS_ETH_MULTICAST),
	CTR1(CTR_BANK_DS_GEM, 1, ODI_GPON_CTR_DS_ETH_UNICAST),
	CTR1(CTR_BANK_DS_GEM, 2, ODI_GPON_CTR_DS_ETH_MULTICAST_FWD),
	CTR1(CTR_BANK_DS_GEM, 3, ODI_GPON_CTR_DS_ETH_MULTICAST_LEAK),
	CTR1(CTR_BANK_DS_GEM, 4, ODI_GPON_CTR_DS_ETH_FCS_ERR),
	CTR1(CTR_BANK_DS_GEM, 5, ODI_GPON_CTR_DS_GEM_OVER_INTERLEAVE),
	CTR1(CTR_BANK_DS_GEM, 6, ODI_GPON_CTR_DS_OMCI),

	CTR2(CTR_BANK_USF, 0, ODI_GPON_CTR_US_BURSTS, ODI_GPON_CTR_US_PLOAM),
	CTR2(CTR_BANK_USF, 1, ODI_GPON_CTR_US_DBRU, ODI_GPON_CTR_US_GEM_FRAMES),
	CTR2(CTR_BANK_USF, 2, ODI_GPON_CTR_US_PLOAM_CPU_NORMAL, ODI_GPON_CTR_US_PLOAM_CPU_URGENT),
	CTR2(CTR_BANK_USF, 3, ODI_GPON_CTR_US_PLOAM_NO_MESSAGE, ODI_GPON_CTR_US_PLOAM_SN),
	CTR1(CTR_BANK_USF, 4, ODI_GPON_CTR_US_GEM_BYTES),
};

#define CTR_NREGS	(sizeof(ctr_regs) / sizeof(ctr_regs[0]))

/* DSF_ALARM_STATE bits, in the order of alarm_events[]/alarm_ms[]. */
static const uint32_t ctr_alarm_bit[ODI_GPON_CTR_ALARMS] = {
	ODI_GPON_DSF_ALARM_STATE_LOS_NOW,
	ODI_GPON_DSF_ALARM_STATE_LOF_NOW,
	ODI_GPON_DSF_ALARM_STATE_LOM_NOW,
};
static const char *const ctr_alarm_name[ODI_GPON_CTR_ALARMS] = { "los", "lof", "lom" };
static const char *const ctr_state_name[ODI_GPON_CTR_STATES] = {
	"O0", "O1", "O2", "O3", "O4", "O5", "O6", "O7",
};

static uint32_t ctr_read(const struct ctr_reg *r)
{
	switch (r->bank) {
	case CTR_BANK_DS_GEM:
		odi_reg_write(ODI_GPON_DS_GEM_MISC_SEL_OFF, r->sel);
		return odi_reg_read(ODI_GPON_DS_GEM_MISC_COUNT_OFF);
	case CTR_BANK_USF:
		odi_reg_write(ODI_GPON_USF_MISC_SEL_OFF, r->sel);
		return odi_reg_read(ODI_GPON_USF_MISC_COUNT_OFF);
	default:
		return odi_reg_read(r->sel);
	}
}

void odi_gpon_ctr_reset(struct odi_gpon_ctr *c)
{
	memset(c, 0, sizeof(*c));
	c->signal_exp[ODI_GPON_SIGNAL_SF] = ODI_GPON_SIGNAL_SF_DEFAULT;
	c->signal_exp[ODI_GPON_SIGNAL_SD] = ODI_GPON_SIGNAL_SD_DEFAULT;
}

/* The downstream line rate, bits per millisecond (2.48832 Gbit/s). */
#define ODI_GPON_DS_BITS_PER_MS	2488320ULL

static const char *const ctr_signal_name[ODI_GPON_SIGNALS] = { "sf", "sd" };

/* bits / 10^exp. A 64-bit divide is a library call on a 32-bit kernel:
 * div_u64() there, by ten at a time.
 */
static uint64_t ctr_div_pow10(uint64_t bits, unsigned int exp)
{
	while (exp--)
#ifdef __KERNEL__
		bits = div_u64(bits, 10U);
#else
		bits /= 10U;
#endif
	return bits;
}

/* One whole window: the BIP bit errors against the bits received in it. */
static void ctr_signal_evaluate(struct odi_gpon_ctr *c)
{
	uint64_t bits = ODI_GPON_DS_BITS_PER_MS * c->signal_ms;
	unsigned int k;

	for (k = 0; k < ODI_GPON_SIGNALS; k++) {
		/* BER >= 10^-x: errors >= bits / 10^x, at least one error. */
		uint64_t set = ctr_div_pow10(bits, c->signal_exp[k]);
		uint64_t clear = ctr_div_pow10(bits, c->signal_exp[k] + 1U);

		if (!set)
			set = 1U;
		if (c->signal_bits >= set) {
			if (!c->signal_on[k])
				c->signal_events[k]++;
			c->signal_on[k] = 1U;
		} else if (c->signal_bits < clear || !clear) {
			c->signal_on[k] = 0U;
		}
	}
	c->signal_last_bits = c->signal_bits;
	c->signal_last_ms = c->signal_ms;
	c->signal_bits = 0U;
	c->signal_ms = 0U;
}

static void ctr_signal_account(struct odi_gpon_ctr *c, unsigned int state, uint32_t dt)
{
	unsigned int k;

	if (state != ODI_GPON_STATE_O5 || c->last_state != ODI_GPON_STATE_O5) {
		/* Not measured outside O5: no alarm, and the window restarts
		 * at the next O5. Errors counted on the way in are dropped.
		 */
		for (k = 0; k < ODI_GPON_SIGNALS; k++)
			c->signal_on[k] = 0U;
		c->signal_bits = 0U;
		c->signal_ms = 0U;
		return;
	}
	c->signal_ms += dt;
	if (c->signal_ms >= ODI_GPON_SIGNAL_WINDOW_MS)
		ctr_signal_evaluate(c);
}

int odi_gpon_ctr_signal_threshold_set(struct odi_gpon_ctr *c, unsigned int signal,
				      unsigned int exp)
{
	if (signal == ODI_GPON_SIGNAL_SF && (exp < 3U || exp > 8U))
		return -1;
	if (signal == ODI_GPON_SIGNAL_SD && (exp < 4U || exp > 10U))
		return -1;
	if (signal >= ODI_GPON_SIGNALS)
		return -1;
	c->signal_exp[signal] = (uint8_t)exp;
	return 0;
}

void odi_gpon_ctr_sample(struct odi_gpon_ctr *c, uint32_t now_ms, unsigned int state)
{
	int started = c->samples != 0;
	uint32_t dt = started ? now_ms - c->last_ms : 0U;
	uint32_t alarms = odi_reg_read(ODI_GPON_DSF_ALARM_STATE_OFF);
	unsigned int i, k;

	for (i = 0; i < CTR_NREGS; i++) {
		const struct ctr_reg *r = &ctr_regs[i];
		uint32_t v = ctr_read(r);

		if (!started)
			continue;	/* the baseline read: clears only */
		for (k = 0; k < r->nfields; k++) {
			const struct ctr_field *f = &r->f[k];
			uint32_t max = f->width >= 32U ? 0xffffffffU : (1U << f->width) - 1U;
			uint32_t n = (v >> f->lsb) & max;

			c->total[f->id] += n;
			if (n == max)
				c->saturated[f->id]++;
			if (f->id == ODI_GPON_CTR_DS_BIP_ERR_BLOCKS && state == ODI_GPON_STATE_O5)
				c->rei_pending += n;
			if (f->id == ODI_GPON_CTR_DS_BIP_ERR_BITS && state == ODI_GPON_STATE_O5)
				c->signal_bits += n;
		}
	}

	odi_gpon_ctr_note_alarms(c, alarms, now_ms);
	if (c->last_state < ODI_GPON_CTR_STATES)
		c->state_ms[c->last_state] += dt;
	if (started)
		ctr_signal_account(c, state, dt);

	c->superframe = ODI_GPON_DSF_SUPERFRAME_COUNT_GET(odi_reg_read(ODI_GPON_DSF_SUPERFRAME_OFF));
	c->us_state = odi_reg_read(ODI_GPON_USF_STATE_OFF);
	c->last_state = state;
	c->last_ms = now_ms;
	c->samples++;
}

void odi_gpon_ctr_note_alarms(struct odi_gpon_ctr *c, uint32_t alarms, uint32_t now_ms)
{
	uint32_t dt = c->alarm_seen ? now_ms - c->alarm_last_ms : 0U;
	unsigned int k;

	/* An alarm already set at the first reading counts as one event: the
	 * line went down at some point before it.
	 */
	for (k = 0; k < ODI_GPON_CTR_ALARMS; k++) {
		uint32_t bit = ctr_alarm_bit[k];

		if ((alarms & bit) && !(c->alarm_state & bit))
			c->alarm_events[k]++;
		if (c->alarm_state & bit)
			c->alarm_ms[k] += dt;
	}
	c->alarm_state = alarms;
	c->alarm_last_ms = now_ms;
	c->alarm_seen = 1;
}

uint32_t odi_gpon_ctr_rei_take(struct odi_gpon_ctr *c)
{
	uint64_t n = c->rei_pending;

	c->rei_pending = 0;
	return n > 0xffffffffULL ? 0xffffffffU : (uint32_t)n;
}

void odi_gpon_ctr_note_transition(struct odi_gpon_ctr *c, unsigned int from, unsigned int to)
{
	if (from < ODI_GPON_CTR_STATES && to < ODI_GPON_CTR_STATES)
		c->transitions[from][to]++;
}

/* /proc/odi_gpon/status: the live bits, the alarm and state times. */
void odi_gpon_ctr_show_status(const struct odi_gpon_ctr *c, odi_gpon_ctr_put_fn put, void *ctx)
{
	char line[96];
	unsigned int i, k;

	/* The live bits only once there is a reading: 0 before it would claim
	 * a clear line nobody looked at.
	 */
	if (c->samples) {
		for (k = 0; k < ODI_GPON_CTR_ALARMS; k++) {
			snprintf(line, sizeof(line), "alarm_now %s %u\n", ctr_alarm_name[k],
				 (c->alarm_state & ctr_alarm_bit[k]) ? 1U : 0U);
			put(ctx, line);
		}
		snprintf(line, sizeof(line), "fec_now ds %u\n",
			 (c->alarm_state & ODI_GPON_DSF_ALARM_STATE_FEC_NOW) ? 1U : 0U);
		put(ctx, line);
		snprintf(line, sizeof(line), "fec_now us %u\n",
			 (c->us_state & ODI_GPON_USF_STATE_FEC_ON) ? 1U : 0U);
		put(ctx, line);
		snprintf(line, sizeof(line), "ds_superframe %u\n", c->superframe);
		put(ctx, line);
	}
	for (k = 0; k < ODI_GPON_SIGNALS; k++) {
		snprintf(line, sizeof(line), "signal %s %u threshold %u\n", ctr_signal_name[k],
			 (unsigned int)c->signal_on[k], (unsigned int)c->signal_exp[k]);
		put(ctx, line);
	}
	for (k = 0; k < ODI_GPON_SIGNALS; k++) {
		snprintf(line, sizeof(line), "signal_events %s %u\n", ctr_signal_name[k],
			 c->signal_events[k]);
		put(ctx, line);
	}
	snprintf(line, sizeof(line), "signal_window ms %u bip_err_bits %llu\n",
		 c->signal_last_ms, (unsigned long long)c->signal_last_bits);
	put(ctx, line);
	for (k = 0; k < ODI_GPON_CTR_ALARMS; k++) {
		snprintf(line, sizeof(line), "alarm_events %s %u\n", ctr_alarm_name[k],
			 c->alarm_events[k]);
		put(ctx, line);
	}
	for (k = 0; k < ODI_GPON_CTR_ALARMS; k++) {
		snprintf(line, sizeof(line), "alarm_ms %s %llu\n", ctr_alarm_name[k],
			 (unsigned long long)c->alarm_ms[k]);
		put(ctx, line);
	}
	for (i = 1; i < ODI_GPON_CTR_STATES; i++) {
		snprintf(line, sizeof(line), "onu_state_ms %s %llu\n", ctr_state_name[i],
			 (unsigned long long)c->state_ms[i]);
		put(ctx, line);
	}
	for (i = 0; i < ODI_GPON_CTR_STATES; i++) {
		for (k = 0; k < ODI_GPON_CTR_STATES; k++) {
			if (!c->transitions[i][k])
				continue;
			snprintf(line, sizeof(line), "onu_transitions %s %s %u\n",
				 ctr_state_name[i], ctr_state_name[k], c->transitions[i][k]);
			put(ctx, line);
		}
	}
}

/* /proc/odi_gpon/counters: the sample count and the 64-bit totals. */
void odi_gpon_ctr_show_counters(const struct odi_gpon_ctr *c, odi_gpon_ctr_put_fn put, void *ctx)
{
	char line[96];
	unsigned int i;

	snprintf(line, sizeof(line), "ctr_samples %u\n", c->samples);
	put(ctx, line);
	for (i = 0; i < ODI_GPON_CTR_COUNT; i++) {
		snprintf(line, sizeof(line), "ctr %s %llu\n", odi_gpon_ctr_names[i],
			 (unsigned long long)c->total[i]);
		put(ctx, line);
	}
	for (i = 0; i < ODI_GPON_CTR_COUNT; i++) {
		if (!c->saturated[i])
			continue;
		snprintf(line, sizeof(line), "ctr_saturated %s %u\n", odi_gpon_ctr_names[i],
			 c->saturated[i]);
		put(ctx, line);
	}
}
