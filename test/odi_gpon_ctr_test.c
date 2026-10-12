/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_ctr_test.c -- host test for odi_gpon_ctr.c, the GPON link
 * counter sampler, against a register model that behaves the way the
 * hardware does: every counter register clears when it is read, the two
 * selector banks answer for the index last written, and an address the
 * sampler has no business reading aborts the test.
 *
 * Covers: every counter in the table exactly once; the baseline sample
 * that only clears; accumulation past 2^32 (the 64-bit totals); one read
 * per register per sample, so no packed field loses its neighbour's count;
 * the 16-bit and 8-bit field split; saturation; the REI count, gathered in
 * O5 only and clamped; the alarm events and times, including a wrapping
 * millisecond clock; the time per ONU state and the transition counts; and
 * the /proc/odi_gpon lines, against test/fixtures/odi_gpon_ctr.txt
 * (UPDATE=1 rewrites it; metricsd's own tests read the same lines).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the register model ---------------------------------------------- */

#define DSF_LO	0x701180U
#define DSF_HI	0x7011c8U
#define NDSF	((DSF_HI - DSF_LO) / 4U + 1U)

static uint32_t dsf[NDSF];		/* the counter registers */
static uint32_t ds_gem[16], usf[8];	/* behind the two selectors */
static uint32_t ds_gem_sel = 0xffU, usf_sel = 0xffU;
static uint32_t alarm_state, us_state, superframe;
static unsigned int dsf_reads[NDSF], ds_gem_reads[16], usf_reads[8];

static void model_reset(void)
{
	memset(dsf, 0, sizeof(dsf));
	memset(ds_gem, 0, sizeof(ds_gem));
	memset(usf, 0, sizeof(usf));
	ds_gem_sel = usf_sel = 0xffU;
	alarm_state = us_state = superframe = 0;
}

static void reads_reset(void)
{
	memset(dsf_reads, 0, sizeof(dsf_reads));
	memset(ds_gem_reads, 0, sizeof(ds_gem_reads));
	memset(usf_reads, 0, sizeof(usf_reads));
}

static uint32_t take(uint32_t *r)
{
	uint32_t v = *r;

	*r = 0;
	return v;
}

static uint32_t odi_reg_read(uint32_t off)
{
	if (off >= DSF_LO && off <= DSF_HI && (off & 3U) == 0) {
		dsf_reads[(off - DSF_LO) / 4U]++;
		return take(&dsf[(off - DSF_LO) / 4U]);
	}
	switch (off) {
	case 0x701008U:
		return alarm_state;
	case 0x701048U:
		return superframe;
	case 0x705008U:
		return us_state;
	case 0x704068U:
		if (ds_gem_sel > 6U)
			break;
		ds_gem_reads[ds_gem_sel]++;
		return take(&ds_gem[ds_gem_sel]);
	case 0x705148U:
		if (usf_sel > 4U)
			break;
		usf_reads[usf_sel]++;
		return take(&usf[usf_sel]);
	}
	fprintf(stderr, "odi_gpon_ctr_test: read of 0x%06x, outside the counter set\n", off);
	abort();
}

static void odi_reg_write(uint32_t off, uint32_t val)
{
	if (off == 0x704064U && val <= 6U) {
		ds_gem_sel = val;
		return;
	}
	if (off == 0x705140U && val <= 4U) {
		usf_sel = val;
		return;
	}
	fprintf(stderr, "odi_gpon_ctr_test: write 0x%x to 0x%06x\n", val, off);
	abort();
}

#define DSF(off)	dsf[((off) - DSF_LO) / 4U]

#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ctr.c"

/* ---- checks ----------------------------------------------------------- */

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

#define O1	ODI_GPON_STATE_O1
#define O2	ODI_GPON_STATE_O2
#define O3	ODI_GPON_STATE_O3
#define O4	ODI_GPON_STATE_O4
#define O5	ODI_GPON_STATE_O5

static struct odi_gpon_ctr c;

static void start(void)
{
	model_reset();
	odi_gpon_ctr_reset(&c);
	odi_gpon_ctr_sample(&c, 1000U, O5);	/* the baseline */
}

static void test_table(void)
{
	unsigned int seen[ODI_GPON_CTR_COUNT] = { 0 };
	unsigned int i, k;

	for (i = 0; i < CTR_NREGS; i++) {
		unsigned int bits = 0;

		for (k = 0; k < ctr_regs[i].nfields; k++) {
			const struct ctr_field *f = &ctr_regs[i].f[k];

			seen[f->id]++;
			CHECK(f->lsb + f->width <= 32U, "a field fits its register");
			bits += f->width;
		}
		CHECK(bits == 32U, "the fields of a register cover all 32 bits");
	}
	for (i = 0; i < ODI_GPON_CTR_COUNT; i++) {
		CHECK(seen[i] == 1, "every counter comes from exactly one field");
		CHECK(odi_gpon_ctr_names[i] && odi_gpon_ctr_names[i][0], "every counter has a name");
	}
}

static void test_baseline_clears_only(void)
{
	unsigned int i;

	model_reset();
	odi_gpon_ctr_reset(&c);
	/* What the block holds after boot: a saturated idle count, and some
	 * errors from before the driver looked. */
	DSF(ODI_GPON_DSF_GEM_IDLE_OFF) = 0xffffffffU;
	DSF(ODI_GPON_DSF_BIP_ERR_BLOCKS_OFF) = 1234U;
	ds_gem[1] = 99U;
	usf[4] = 1000U;
	odi_gpon_ctr_sample(&c, 5U, O1);
	for (i = 0; i < ODI_GPON_CTR_COUNT; i++) {
		CHECK(c.total[i] == 0, "the baseline adds nothing");
		CHECK(c.saturated[i] == 0, "the baseline marks nothing saturated");
	}
	CHECK(c.samples == 1, "the baseline counts as a sample");
	CHECK(DSF(ODI_GPON_DSF_GEM_IDLE_OFF) == 0 && ds_gem[1] == 0 && usf[4] == 0,
	      "the baseline cleared the hardware counters");
	CHECK(odi_gpon_ctr_rei_take(&c) == 0, "the baseline gives the REI nothing");
}

static void test_accumulate_and_clear_on_read(void)
{
	start();
	DSF(ODI_GPON_DSF_FEC_CORR_CW_OFF) = 253000U;
	DSF(ODI_GPON_DSF_BWMAP_OK_OFF) = 20700U;
	ds_gem[1] = 500U;
	usf[4] = 64000U;
	odi_gpon_ctr_sample(&c, 2000U, O5);
	CHECK(c.total[ODI_GPON_CTR_DS_FEC_CORRECTED_CW] == 253000U, "a direct counter adds");
	CHECK(c.total[ODI_GPON_CTR_DS_BWMAP_ACCEPTED] == 20700U, "BWmap allocations add");
	CHECK(c.total[ODI_GPON_CTR_DS_ETH_UNICAST] == 500U, "a DS GEM selector counter adds");
	CHECK(c.total[ODI_GPON_CTR_US_GEM_BYTES] == 64000U, "an upstream selector counter adds");

	/* Nothing new in hardware: the registers read 0, the totals hold. */
	odi_gpon_ctr_sample(&c, 3000U, O5);
	CHECK(c.total[ODI_GPON_CTR_DS_FEC_CORRECTED_CW] == 253000U, "a cleared register adds 0");
	CHECK(c.total[ODI_GPON_CTR_US_GEM_BYTES] == 64000U, "a cleared upstream register adds 0");

	DSF(ODI_GPON_DSF_FEC_CORR_CW_OFF) = 1000U;
	odi_gpon_ctr_sample(&c, 4000U, O5);
	CHECK(c.total[ODI_GPON_CTR_DS_FEC_CORRECTED_CW] == 254000U, "the next count adds on");
}

static void test_past_32_bits(void)
{
	unsigned int i;

	start();
	for (i = 0; i < 5; i++) {
		DSF(ODI_GPON_DSF_GEM_NON_IDLE_OFF) = 0xf0000000U;
		usf[4] = 0xf0000000U;
		odi_gpon_ctr_sample(&c, 2000U + i * 1000U, O5);
	}
	CHECK(c.total[ODI_GPON_CTR_DS_GEM_NON_IDLE] == 5ULL * 0xf0000000ULL,
	      "a direct total passes 2^32");
	CHECK(c.total[ODI_GPON_CTR_US_GEM_BYTES] == 5ULL * 0xf0000000ULL,
	      "an upstream total passes 2^32");
	CHECK(c.saturated[ODI_GPON_CTR_DS_GEM_NON_IDLE] == 0, "below all ones is not saturated");
}

static void test_one_read_per_register(void)
{
	unsigned int i;

	start();
	reads_reset();
	odi_gpon_ctr_sample(&c, 2000U, O5);
	for (i = 0; i < NDSF; i++)
		CHECK(dsf_reads[i] == 1, "each framer counter register is read once per sample");
	for (i = 0; i <= 6U; i++)
		CHECK(ds_gem_reads[i] == 1, "each DS GEM index is read once per sample");
	for (i = 0; i <= 4U; i++)
		CHECK(usf_reads[i] == 1, "each upstream index is read once per sample");
}

static void test_packed_fields(void)
{
	start();
	DSF(ODI_GPON_DSF_SYNC_ERRS_OFF) = (7U << 16) | 3U;
	DSF(ODI_GPON_DSF_PLOAM_ERRS_OFF) = (2U << 16) | 5U;
	DSF(ODI_GPON_DSF_BWMAP_ERRS_OFF) = (11U << 16) | 13U;
	DSF(ODI_GPON_DSF_BWMAP_INVALID_OFF) = (17U << 16) | 19U;
	DSF(ODI_GPON_DSF_ACTIVATION_REQS_OFF) = (23U << 16) | 29U;
	DSF(ODI_GPON_DSF_GEM_ERRS_OFF) = (31U << 16) | 37U;
	DSF(ODI_GPON_DSF_PLEND_CORR_OFF) = (0x12U << 24) | (0x34U << 16) | 0x5678U;
	usf[0] = (40U << 16) | 8000U;
	usf[1] = (2000U << 16) | 300U;
	usf[2] = (6U << 16) | 1U;
	usf[3] = (9U << 16) | 7900U;
	odi_gpon_ctr_sample(&c, 2000U, O5);
	CHECK(c.total[ODI_GPON_CTR_DS_SUPERFRAME_LOSS] == 3U, "superframe loss: low half");
	CHECK(c.total[ODI_GPON_CTR_DS_PLEND_FAIL] == 7U, "PLend fail: high half");
	CHECK(c.total[ODI_GPON_CTR_DS_PLOAM_CRC_ERR] == 5U, "PLOAM CRC: low half");
	CHECK(c.total[ODI_GPON_CTR_DS_PLOAM_OVERFLOW] == 2U, "PLOAM overflow: high half");
	CHECK(c.total[ODI_GPON_CTR_DS_BWMAP_CRC_ERR] == 13U, "BWmap CRC: low half");
	CHECK(c.total[ODI_GPON_CTR_DS_BWMAP_OVERFLOW] == 11U, "BWmap overflow: high half");
	CHECK(c.total[ODI_GPON_CTR_DS_BWMAP_INVALID_0] == 19U, "BWmap invalid 0: low half");
	CHECK(c.total[ODI_GPON_CTR_DS_BWMAP_INVALID_1] == 17U, "BWmap invalid 1: high half");
	CHECK(c.total[ODI_GPON_CTR_DS_SN_REQUESTS] == 29U, "SN requests: low half");
	CHECK(c.total[ODI_GPON_CTR_DS_RANGING_REQUESTS] == 23U, "ranging requests: high half");
	CHECK(c.total[ODI_GPON_CTR_DS_GEM_LEN_MISMATCH] == 37U, "GEM length mismatch: low half");
	CHECK(c.total[ODI_GPON_CTR_DS_GEM_PORT_MISMATCH] == 31U, "GEM port mismatch: high half");
	CHECK(c.total[ODI_GPON_CTR_DS_PLEND_CORRECTED] == 0x5678U, "PLend corrected: bits 15:0");
	CHECK(c.total[ODI_GPON_CTR_DS_PLEND_UNCORRECTABLE] == 0x34U, "PLend uncorrectable: 23:16");
	CHECK(c.total[ODI_GPON_CTR_DS_BLEN_TOO_LONG] == 0x12U, "Blen too long: 31:24");
	CHECK(c.total[ODI_GPON_CTR_US_BURSTS] == 8000U, "bursts: low half");
	CHECK(c.total[ODI_GPON_CTR_US_PLOAM] == 40U, "PLOAMu: high half");
	CHECK(c.total[ODI_GPON_CTR_US_DBRU] == 300U, "DBRu: low half");
	CHECK(c.total[ODI_GPON_CTR_US_GEM_FRAMES] == 2000U, "GEM frames: high half");
	CHECK(c.total[ODI_GPON_CTR_US_PLOAM_CPU_NORMAL] == 1U, "CPU normal PLOAMu: low half");
	CHECK(c.total[ODI_GPON_CTR_US_PLOAM_CPU_URGENT] == 6U, "CPU urgent PLOAMu: high half");
	CHECK(c.total[ODI_GPON_CTR_US_PLOAM_NO_MESSAGE] == 7900U, "no-message PLOAMu: low half");
	CHECK(c.total[ODI_GPON_CTR_US_PLOAM_SN] == 9U, "serial-number PLOAMu: high half");
}

static void test_saturation(void)
{
	start();
	DSF(ODI_GPON_DSF_GEM_IDLE_OFF) = 0xffffffffU;
	usf[1] = 0xffffU << 16;			/* GEM frames full, DBRu 0 */
	DSF(ODI_GPON_DSF_PLEND_CORR_OFF) = 0xffU << 24;	/* the 8-bit field full */
	odi_gpon_ctr_sample(&c, 2000U, O5);
	CHECK(c.saturated[ODI_GPON_CTR_DS_GEM_IDLE] == 1, "a full 32-bit counter is saturated");
	CHECK(c.total[ODI_GPON_CTR_DS_GEM_IDLE] == 0xffffffffULL, "and still adds what it read");
	CHECK(c.saturated[ODI_GPON_CTR_US_GEM_FRAMES] == 1, "a full 16-bit field is saturated");
	CHECK(c.saturated[ODI_GPON_CTR_US_DBRU] == 0, "its neighbour is not");
	CHECK(c.saturated[ODI_GPON_CTR_DS_BLEN_TOO_LONG] == 1, "a full 8-bit field is saturated");
	CHECK(c.saturated[ODI_GPON_CTR_DS_PLEND_UNCORRECTABLE] == 0, "its neighbour is not");
}

static void test_rei(void)
{
	start();
	DSF(ODI_GPON_DSF_BIP_ERR_BLOCKS_OFF) = 40U;
	odi_gpon_ctr_sample(&c, 2000U, O4);
	CHECK(odi_gpon_ctr_rei_take(&c) == 0, "BIP errors outside O5 do not go into an REI");
	CHECK(c.total[ODI_GPON_CTR_DS_BIP_ERR_BLOCKS] == 40U, "but they are counted");

	DSF(ODI_GPON_DSF_BIP_ERR_BLOCKS_OFF) = 5U;
	odi_gpon_ctr_sample(&c, 3000U, O5);
	DSF(ODI_GPON_DSF_BIP_ERR_BLOCKS_OFF) = 6U;
	odi_gpon_ctr_sample(&c, 4000U, O5);
	CHECK(odi_gpon_ctr_rei_take(&c) == 11U, "the REI carries every O5 sample since the last one");
	CHECK(odi_gpon_ctr_rei_take(&c) == 0, "and the next interval starts empty");
	CHECK(c.total[ODI_GPON_CTR_DS_BIP_ERR_BLOCKS] == 51U, "the total keeps everything");

	c.rei_pending = 0x100000000ULL;
	CHECK(odi_gpon_ctr_rei_take(&c) == 0xffffffffU, "the REI count is clamped to 32 bits");
}

static void test_alarms(void)
{
	start();					/* t=1000, line clear */
	odi_gpon_ctr_sample(&c, 2000U, O5);
	alarm_state = ODI_GPON_DSF_ALARM_STATE_LOS_NOW | ODI_GPON_DSF_ALARM_STATE_LOF_NOW;
	odi_gpon_ctr_sample(&c, 3000U, O5);		/* seen set */
	CHECK(c.alarm_events[0] == 1 && c.alarm_events[1] == 1, "LOS and LOF rise once");
	CHECK(c.alarm_ms[0] == 0, "no time until the next sample");
	odi_gpon_ctr_sample(&c, 4500U, O5);
	odi_gpon_ctr_sample(&c, 5500U, O5);
	CHECK(c.alarm_events[0] == 1, "a held alarm is one event");
	CHECK(c.alarm_ms[0] == 2500U, "the time since it was first seen");
	alarm_state = ODI_GPON_DSF_ALARM_STATE_LOF_NOW;
	odi_gpon_ctr_sample(&c, 6500U, O5);		/* LOS cleared */
	CHECK(c.alarm_ms[0] == 3500U, "the interval it cleared in counts as set");
	odi_gpon_ctr_sample(&c, 7500U, O5);
	CHECK(c.alarm_ms[0] == 3500U, "a clear alarm stops adding");
	CHECK(c.alarm_ms[1] == 4500U, "LOF keeps its own time");
	alarm_state = ODI_GPON_DSF_ALARM_STATE_LOS_NOW;
	odi_gpon_ctr_sample(&c, 8500U, O5);
	CHECK(c.alarm_events[0] == 2, "LOS rising again is a second event");
	CHECK(c.alarm_events[2] == 0 && c.alarm_ms[2] == 0, "LOM never set");

	/* The millisecond clock wraps: only the difference counts. */
	model_reset();
	odi_gpon_ctr_reset(&c);
	alarm_state = ODI_GPON_DSF_ALARM_STATE_LOS_NOW;
	odi_gpon_ctr_sample(&c, 0xfffffc00U, O1);
	CHECK(c.alarm_events[0] == 1, "an alarm set at the first sample is an event");
	odi_gpon_ctr_sample(&c, 0x00000400U, O1);
	CHECK(c.alarm_ms[0] == 0x800U, "a wrapped clock gives the right interval");
}

/* The line check reads DSF_ALARM_STATE every 100 ms and hands each reading
 * to odi_gpon_ctr_note_alarms(); the 1 s sampler reads it too. */
static void test_alarms_from_the_line_poll(void)
{
	unsigned int t;

	start();					/* t=1000, a sampler reading */
	odi_gpon_ctr_note_alarms(&c, 0, 1100U);
	odi_gpon_ctr_note_alarms(&c, ODI_GPON_DSF_ALARM_STATE_LOS_NOW, 1200U);
	odi_gpon_ctr_note_alarms(&c, ODI_GPON_DSF_ALARM_STATE_LOS_NOW, 1300U);
	odi_gpon_ctr_note_alarms(&c, 0, 1400U);
	odi_gpon_ctr_sample(&c, 2000U, O5);		/* the sampler sees it clear */
	CHECK(c.alarm_events[0] == 1, "a 200 ms loss between two samples is one event");
	CHECK(c.alarm_ms[0] == 200U, "and lasts its 200 ms");

	/* Both feeds interleaved over a held loss, reading the same register:
	 * one event, and the time counted once. */
	alarm_state = ODI_GPON_DSF_ALARM_STATE_LOF_NOW;
	odi_gpon_ctr_note_alarms(&c, alarm_state, 2050U);
	for (t = 2150U; t <= 4050U; t += 100U) {
		odi_gpon_ctr_note_alarms(&c, alarm_state, t);
		if (t == 3050U || t == 4050U)
			odi_gpon_ctr_sample(&c, t + 10U, O5);
	}
	CHECK(c.alarm_events[1] == 1, "a held LOF is one event across both feeds");
	CHECK(c.alarm_ms[1] == 2010U, "and its time is counted once (2050 to 4060)");
	alarm_state = 0;

	/* Readings only from the sampler again (line poll stopped): to the
	 * second. */
	start();
	alarm_state = ODI_GPON_DSF_ALARM_STATE_LOM_NOW;
	odi_gpon_ctr_sample(&c, 2000U, O1);
	odi_gpon_ctr_sample(&c, 3000U, O1);
	CHECK(c.alarm_events[2] == 1 && c.alarm_ms[2] == 1000U, "the sampler alone still counts");
}

static void test_states(void)
{
	start();					/* t=1000 in O5 */
	odi_gpon_ctr_sample(&c, 3000U, O5);		/* 2 s of O5 */
	odi_gpon_ctr_note_transition(&c, O5, O1);
	odi_gpon_ctr_note_transition(&c, O1, O2);
	odi_gpon_ctr_sample(&c, 4000U, O2);		/* 1 s more, counted as O5 */
	odi_gpon_ctr_sample(&c, 9000U, O2);		/* 5 s of O2 */
	odi_gpon_ctr_note_transition(&c, O2, O3);
	odi_gpon_ctr_note_transition(&c, O3, O2);
	odi_gpon_ctr_note_transition(&c, O2, O3);
	CHECK(c.state_ms[O5] == 3000U, "time in O5, to the sample that saw it leave");
	CHECK(c.state_ms[O2] == 5000U, "time in O2");
	CHECK(c.transitions[O5][O1] == 1 && c.transitions[O1][O2] == 1, "one of each");
	CHECK(c.transitions[O2][O3] == 2 && c.transitions[O3][O2] == 1, "a flap counts each way");
	odi_gpon_ctr_note_transition(&c, 9, 1);
	odi_gpon_ctr_note_transition(&c, 1, ODI_GPON_CTR_STATES);
	CHECK(c.transitions[O2][O3] == 2 && c.transitions[O1][O2] == 1,
	      "an out-of-range state changes nothing");
}

/* ---- the /proc lines --------------------------------------------------- */

static char out[8192];
static size_t outn;

static void put(void *ctx, const char *line)
{
	size_t n = strlen(line);

	(void)ctx;
	if (outn + n >= sizeof(out)) {
		fprintf(stderr, "FAIL: /proc output overflow\n");
		failures++;
		return;
	}
	memcpy(out + outn, line, n);
	outn += n;
	out[outn] = 0;
}

static void test_show_before_first_sample(void)
{
	odi_gpon_ctr_reset(&c);
	outn = 0;
	out[0] = 0;
	odi_gpon_ctr_show(&c, put, NULL);
	CHECK(strncmp(out, "ctr_samples 0\n", 14) == 0, "the sample count leads");
	CHECK(!strstr(out, "\nalarm_now ") && !strstr(out, "\nfec_now ") &&
	      !strstr(out, "\nds_superframe "),
	      "no live bits before the first sample");
	CHECK(strstr(out, "ctr ds_bip_err_blocks 0\n") != NULL, "the totals are there, at 0");
}

/* A scenario with a value in every kind of line, compared with the golden
 * file metricsd's own test takes as its input.
 */
static void test_show_golden(const char *path)
{
	unsigned int i;
	FILE *f;
	char want[8192];
	size_t n;

	start();
	for (i = 0; i < 10; i++) {
		DSF(ODI_GPON_DSF_FEC_CORR_BITS_OFF) = 256000U;
		DSF(ODI_GPON_DSF_FEC_CORR_BYTES_OFF) = 250000U;
		DSF(ODI_GPON_DSF_FEC_CORR_CW_OFF) = 253000U;
		DSF(ODI_GPON_DSF_FEC_UNCORR_CW_OFF) = i == 3 ? 2U : 0U;
		DSF(ODI_GPON_DSF_BIP_ERR_BLOCKS_OFF) = i == 3 ? 4U : 0U;
		DSF(ODI_GPON_DSF_BIP_ERR_BITS_OFF) = i == 3 ? 9U : 0U;
		DSF(ODI_GPON_DSF_PLOAM_OK_OFF) = 8000U;
		DSF(ODI_GPON_DSF_BWMAP_OK_OFF) = 2070U;
		DSF(ODI_GPON_DSF_GEM_IDLE_OFF) = i == 0 ? 0xffffffffU : 61000000U;
		DSF(ODI_GPON_DSF_GEM_NON_IDLE_OFF) = 12000U;
		DSF(ODI_GPON_DSF_GEM_HEC_CORR_OFF) = i == 3 ? 1U : 0U;
		DSF(ODI_GPON_DSF_PLEND_CORR_OFF) = i == 3 ? 3U : 0U;
		ds_gem[0] = 10U;
		ds_gem[1] = 11000U;
		ds_gem[2] = 10U;
		ds_gem[6] = 2U;
		usf[0] = (2U << 16) | 7998U;
		usf[1] = (900U << 16) | 0U;
		usf[2] = (1U << 16) | 0U;
		usf[3] = (0U << 16) | 1U;
		usf[4] = 1100000U;
		alarm_state = ODI_GPON_DSF_ALARM_STATE_FEC_NOW |
			      (i == 4 || i == 5 ? ODI_GPON_DSF_ALARM_STATE_LOS_NOW : 0U);
		us_state = ODI_GPON_USF_STATE_FEC_ON;
		superframe = 0x12345678U + i * 8000U;
		odi_gpon_ctr_sample(&c, 2000U + i * 1000U, i < 2 ? O4 : O5);
		if (i == 1)
			odi_gpon_ctr_note_transition(&c, O4, O5);
	}
	odi_gpon_ctr_note_transition(&c, O1, O2);
	odi_gpon_ctr_note_transition(&c, O2, O3);
	odi_gpon_ctr_note_transition(&c, O3, O4);

	outn = 0;
	out[0] = 0;
	odi_gpon_ctr_show(&c, put, NULL);

	if (getenv("UPDATE")) {
		f = fopen(path, "w");
		if (!f || fwrite(out, 1, outn, f) != outn) {
			fprintf(stderr, "FAIL: cannot write %s\n", path);
			failures++;
		}
		if (f)
			fclose(f);
		return;
	}
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "FAIL: cannot read %s\n", path);
		failures++;
		return;
	}
	n = fread(want, 1, sizeof(want) - 1, f);
	fclose(f);
	want[n] = 0;
	if (n != outn || memcmp(want, out, n) != 0) {
		fprintf(stderr, "FAIL: /proc lines differ from %s; got:\n%s", path, out);
		failures++;
	}
}

int main(int argc, char **argv)
{
	test_table();
	test_baseline_clears_only();
	test_accumulate_and_clear_on_read();
	test_past_32_bits();
	test_one_read_per_register();
	test_packed_fields();
	test_saturation();
	test_rei();
	test_alarms();
	test_alarms_from_the_line_poll();
	test_states();
	test_show_before_first_sample();
	test_show_golden(argc > 1 ? argv[1] : "test/fixtures/odi_gpon_ctr.txt");

	if (failures) {
		fprintf(stderr, "odi_gpon_ctr_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_gpon_ctr_test: ok\n");
	return 0;
}
