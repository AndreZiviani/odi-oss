/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_flow_test.c -- host test for odi_gpon_flow.c, the per-flow GEM
 * counter sampler, against a register model that behaves the way the
 * hardware does: every count clears when it is read, the indirect pairs
 * answer for the index last written, ACK can be withheld, and an address
 * the sampler has no business touching aborts the test.
 *
 * Covers: a flow starts from zero (the registers are cleared by the first
 * read, which is not counted); accumulation past 2^32; only configured
 * flows are read; a flow created again, and the flow state emptied;
 * the RSEL and ETH selector bits; the 64-bit upstream byte count; the ACK
 * wait being bounded, the count staying in the register for the next
 * sample; saturation; an index the table does not hold; and the
 * /proc/odi_gpon/flows lines against test/fixtures/odi_gpon_flows.txt
 * (UPDATE=1 rewrites it; metricsd's own tests read the same lines).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the register model ---------------------------------------------- */

#define NFLOW	128U

static uint32_t ds_cnt[NFLOW][2];	/* [flow][RSEL]: GEM frames, bytes */
static uint32_t ds_rx[NFLOW], ds_fwd[NFLOW];
static uint32_t us_cnt[NFLOW][2];	/* [flow][ETH]: GEM frames, Ethernet frames */
static uint64_t us_bytes[NFLOW];
static uint32_t ds_ind, ds_rx_ind, ds_fwd_ind, us_ind;	/* the index last written */
static int ds_ind_set, ds_rx_ind_set, ds_fwd_ind_set, us_ind_set;
static int ack_withheld;	/* the index registers never report ACK */
static unsigned int ack_polls;	/* reads of an index register */
static unsigned int touched[2][NFLOW];	/* register reads per [dir][flow] */

static void model_reset(void)
{
	memset(ds_cnt, 0, sizeof(ds_cnt));
	memset(ds_rx, 0, sizeof(ds_rx));
	memset(ds_fwd, 0, sizeof(ds_fwd));
	memset(us_cnt, 0, sizeof(us_cnt));
	memset(us_bytes, 0, sizeof(us_bytes));
	ds_ind_set = ds_rx_ind_set = ds_fwd_ind_set = us_ind_set = 0;
	ack_withheld = 0;
	ack_polls = 0;
	memset(touched, 0, sizeof(touched));
}

static void bad(const char *what, uint32_t off)
{
	fprintf(stderr, "odi_gpon_flow_test: %s 0x%06x, outside the flow counters\n", what, off);
	abort();
}

static uint32_t take32(uint32_t *r)
{
	uint32_t v = *r;

	*r = 0;
	return v;
}

static uint32_t odi_reg_read(uint32_t off)
{
	uint32_t n;

	switch (off) {
	case 0x701140U:
		ack_polls++;
		return ds_ind | (ack_withheld ? 0U : 0x8000U);
	case 0x701144U:
		if (!ds_ind_set)
			bad("read of a value with no index, ", off);
		touched[0][ds_ind & 0x7fU]++;
		return take32(&ds_cnt[ds_ind & 0x7fU][(ds_ind >> 8) & 1U]);
	case 0x704040U:
		ack_polls++;
		return ds_rx_ind | (ack_withheld ? 0U : 0x8000U);
	case 0x704044U:
		if (!ds_rx_ind_set)
			bad("read of a value with no index, ", off);
		touched[0][ds_rx_ind]++;
		return take32(&ds_rx[ds_rx_ind]);
	case 0x70404cU:
		ack_polls++;
		return ds_fwd_ind | (ack_withheld ? 0U : 0x8000U);
	case 0x704050U:
		if (!ds_fwd_ind_set)
			bad("read of a value with no index, ", off);
		touched[0][ds_fwd_ind]++;
		return take32(&ds_fwd[ds_fwd_ind]);
	case 0x706048U:
		ack_polls++;
		return us_ind | (ack_withheld ? 0U : 0x8000U);
	case 0x70604cU:
		if (!us_ind_set)
			bad("read of a value with no index, ", off);
		touched[1][us_ind & 0x7fU]++;
		return take32(&us_cnt[us_ind & 0x7fU][(us_ind >> 7) & 1U]);
	}
	if (off >= 0x706800U && off < 0x706800U + 8U * NFLOW && (off & 3U) == 0) {
		n = (off - 0x706800U) / 8U;
		touched[1][n]++;
		if (off & 4U) {
			uint32_t hi = (uint32_t)(us_bytes[n] >> 32);

			us_bytes[n] &= 0xffffffffULL;
			return hi;
		} else {
			uint32_t lo = (uint32_t)us_bytes[n];

			us_bytes[n] &= ~0xffffffffULL;
			return lo;
		}
	}
	bad("read of", off);
	return 0;
}

static void odi_reg_write(uint32_t off, uint32_t val)
{
	switch (off) {
	case 0x701140U:
		if (val & ~(0x7fU | 0x100U))
			bad("a bit outside IDX and RSEL written to", off);
		ds_ind = val;
		ds_ind_set = 1;
		return;
	case 0x704040U:
		if (val >= NFLOW)
			bad("an index past the table written to", off);
		ds_rx_ind = val;
		ds_rx_ind_set = 1;
		return;
	case 0x70404cU:
		if (val >= NFLOW)
			bad("an index past the table written to", off);
		ds_fwd_ind = val;
		ds_fwd_ind_set = 1;
		return;
	case 0x706048U:
		if (val & ~(0x7fU | 0x80U))
			bad("a bit outside the index and ETH written to", off);
		us_ind = val;
		us_ind_set = 1;
		return;
	}
	bad("write to", off);
}

#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_flow.c"

/* ---- checks ----------------------------------------------------------- */

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static struct odi_gpon_flows f;

#define DS	ODI_GPON_FLOW_DIR_DS
#define US	ODI_GPON_FLOW_DIR_US

static void start(void)
{
	model_reset();
	odi_gpon_flows_reset(&f);
}

static void ds_set(uint32_t i, uint32_t gem_frames, uint32_t bytes, uint32_t rx, uint32_t fwd)
{
	ds_cnt[i][0] = gem_frames;
	ds_cnt[i][1] = bytes;
	ds_rx[i] = rx;
	ds_fwd[i] = fwd;
}

static void us_set(uint32_t i, uint32_t gem_frames, uint32_t eth_frames, uint64_t bytes)
{
	us_cnt[i][0] = gem_frames;
	us_cnt[i][1] = eth_frames;
	us_bytes[i] = bytes;
}

static void test_starts_from_zero(void)
{
	start();
	/* What the registers hold from before the flow existed. */
	ds_set(1, 500U, 60000U, 500U, 500U);
	us_set(4, 900U, 140U, 70000U);
	CHECK(odi_gpon_flows_set(&f, DS, 1, 1434U) == 0, "a downstream flow is accepted");
	CHECK(odi_gpon_flows_set(&f, US, 4, 1562U) == 0, "an upstream flow is accepted");
	CHECK(f.ds[1].active && f.ds[1].gem_port == 1434U, "the downstream flow is recorded");
	CHECK(f.us[4].active && f.us[4].gem_port == 1562U, "the upstream flow is recorded");
	CHECK(f.ds[1].gem_frames == 0 && f.ds[1].bytes == 0 && f.ds[1].eth_rx == 0 &&
	      f.ds[1].eth_fwd == 0, "the old downstream counts are not in the total");
	CHECK(f.us[4].gem_frames == 0 && f.us[4].eth_frames == 0 && f.us[4].bytes == 0,
	      "the old upstream counts are not in the total");
	CHECK(ds_cnt[1][0] == 0 && ds_cnt[1][1] == 0 && ds_rx[1] == 0 && ds_fwd[1] == 0 &&
	      us_cnt[4][0] == 0 && us_cnt[4][1] == 0 && us_bytes[4] == 0,
	      "creating the flow cleared its registers");
	CHECK(f.saturated == 0 && f.ack_timeouts == 0, "nothing was tallied");
}

static void test_accumulate(void)
{
	start();
	odi_gpon_flows_set(&f, DS, 1, 1434U);
	odi_gpon_flows_set(&f, US, 4, 1562U);

	ds_set(1, 10U, 1000U, 10U, 9U);
	us_set(4, 66U, 10U, 5000U);
	odi_gpon_flows_sample(&f);
	CHECK(f.ds[1].gem_frames == 10U, "DS GEM frames (RSEL 0)");
	CHECK(f.ds[1].bytes == 1000U, "DS bytes (RSEL 1)");
	CHECK(f.ds[1].eth_rx == 10U, "DS Ethernet frames received");
	CHECK(f.ds[1].eth_fwd == 9U, "DS Ethernet frames forwarded");
	CHECK(f.us[4].gem_frames == 66U, "US GEM frames");
	CHECK(f.us[4].eth_frames == 10U, "US Ethernet frames (ETH)");
	CHECK(f.us[4].bytes == 5000U, "US bytes");

	odi_gpon_flows_sample(&f);
	CHECK(f.ds[1].gem_frames == 10U && f.us[4].bytes == 5000U, "cleared registers add 0");

	ds_set(1, 5U, 500U, 5U, 5U);
	odi_gpon_flows_sample(&f);
	CHECK(f.ds[1].gem_frames == 15U && f.ds[1].bytes == 1500U, "the next counts add on");
}

static void test_past_32_bits(void)
{
	unsigned int i;

	start();
	odi_gpon_flows_set(&f, DS, 0, 4095U);
	odi_gpon_flows_set(&f, US, 0, 1U);
	for (i = 0; i < 3; i++) {
		ds_set(0, 0x80000000U, 0xc0000000U, 7U, 7U);
		us_set(0, 1U, 1U, 0xc0000000ULL);
		odi_gpon_flows_sample(&f);
	}
	CHECK(f.ds[0].gem_frames == 3ULL * 0x80000000ULL, "DS GEM frames pass 2^32");
	CHECK(f.ds[0].bytes == 3ULL * 0xc0000000ULL, "DS bytes pass 2^32");
	CHECK(f.us[0].bytes == 3ULL * 0xc0000000ULL, "US bytes pass 2^32");
}

static void test_us_bytes_64_bit(void)
{
	start();
	odi_gpon_flows_set(&f, US, 2, 1562U);
	us_set(2, 0, 0, 0x0000000200000010ULL);	/* the high word carries */
	odi_gpon_flows_sample(&f);
	CHECK(f.us[2].bytes == 0x0000000200000010ULL, "both words of the byte count are read");
	CHECK(us_bytes[2] == 0, "both words were cleared by the read");
}

static void test_only_active_flows_are_read(void)
{
	unsigned int d, i, n = 0;

	start();
	odi_gpon_flows_set(&f, DS, 3, 1000U);
	odi_gpon_flows_set(&f, US, 1, 1001U);
	memset(touched, 0, sizeof(touched));
	odi_gpon_flows_sample(&f);
	for (d = 0; d < 2; d++)
		for (i = 0; i < NFLOW; i++)
			if (touched[d][i])
				n++;
	CHECK(n == 2, "exactly the two configured flows were read");
	CHECK(touched[0][3] && touched[1][1], "and they are the right two");
	/* The DS flow answers four counts, the US flow two indirect ones and the bytes. */
	CHECK(touched[0][3] == 4U, "a downstream flow is four reads");
	CHECK(touched[1][1] == 4U, "an upstream flow is four reads");
}

static void test_flow_created_again(void)
{
	start();
	odi_gpon_flows_set(&f, DS, 1, 1434U);
	ds_set(1, 10U, 100U, 10U, 10U);
	odi_gpon_flows_sample(&f);
	CHECK(f.ds[1].gem_frames == 10U, "a total builds up");

	ds_set(1, 4U, 40U, 4U, 4U);	/* counts since the last sample */
	odi_gpon_flows_set(&f, DS, 1, 1500U);
	CHECK(f.ds[1].gem_port == 1500U, "the flow has its new port");
	CHECK(f.ds[1].gem_frames == 0 && f.ds[1].bytes == 0 && f.ds[1].eth_rx == 0 &&
	      f.ds[1].eth_fwd == 0, "the totals start over");
	odi_gpon_flows_sample(&f);
	CHECK(f.ds[1].gem_frames == 0, "the counts before the new flow did not come back");
}

static void test_clear(void)
{
	unsigned int i;

	start();
	odi_gpon_flows_set(&f, DS, 1, 1434U);
	odi_gpon_flows_set(&f, US, 4, 1562U);
	ds_set(1, 10U, 100U, 10U, 10U);
	odi_gpon_flows_sample(&f);
	odi_gpon_flows_clear(&f);
	memset(touched, 0, sizeof(touched));
	odi_gpon_flows_sample(&f);
	for (i = 0; i < NFLOW; i++)
		CHECK(!touched[0][i] && !touched[1][i], "no flow is read after the clear");
	CHECK(!f.ds[1].active && f.ds[1].gem_frames == 0 && !f.us[4].active,
	      "the flows and their totals are gone");
}

static void test_bad_index(void)
{
	start();
	CHECK(odi_gpon_flows_set(&f, DS, ODI_GPON_FLOWS_DS, 1U) == -1, "a DS index past the table");
	CHECK(odi_gpon_flows_set(&f, US, ODI_GPON_FLOWS_US, 1U) == -1, "a US index past the table");
	CHECK(odi_gpon_flows_set(&f, (enum odi_gpon_flow_dir)7, 0, 1U) == -1, "an unknown direction");
	CHECK(odi_gpon_flows_set(&f, DS, ODI_GPON_FLOWS_DS - 1U, 1U) == 0, "the last DS index");
	CHECK(odi_gpon_flows_set(&f, US, ODI_GPON_FLOWS_US - 1U, 1U) == 0, "the last US index");
}

static void test_ack_is_bounded(void)
{
	start();
	odi_gpon_flows_set(&f, DS, 1, 1434U);
	ds_set(1, 10U, 100U, 10U, 10U);
	ack_withheld = 1;
	ack_polls = 0;
	odi_gpon_flows_sample(&f);
	CHECK(f.ds[1].gem_frames == 0 && f.ds[1].bytes == 0, "no ACK: nothing is read or added");
	CHECK(f.ack_timeouts == 4U, "each of the four downstream counts timed out");
	CHECK(ack_polls == 4U * ODI_GPON_FLOW_ACK_TRIES, "the wait is bounded by the try count");
	CHECK(ds_cnt[1][0] == 10U && ds_rx[1] == 10U, "the count stays in the register");

	ack_withheld = 0;
	odi_gpon_flows_sample(&f);
	CHECK(f.ds[1].gem_frames == 10U && f.ds[1].eth_rx == 10U, "the next sample picks it up");
	CHECK(f.ack_timeouts == 4U, "and tallies no more timeouts");
}

static void test_saturation(void)
{
	start();
	odi_gpon_flows_set(&f, DS, 1, 1434U);
	odi_gpon_flows_set(&f, US, 1, 1434U);
	ds_set(1, 0xffffffffU, 5U, 5U, 5U);
	us_set(1, 1U, 1U, 0xffffffffffffffffULL);
	odi_gpon_flows_sample(&f);
	CHECK(f.saturated == 2U, "a full DS count and a full US byte count are tallied");
	CHECK(f.ds[1].gem_frames == 0xffffffffULL, "the count is still added");

	/* A full count read to clear a new flow is not a lost count. */
	start();
	ds_set(2, 0xffffffffU, 0, 0, 0);
	odi_gpon_flows_set(&f, DS, 2, 7U);
	CHECK(f.saturated == 0, "the first read of a flow is not tallied");
}

static void test_flow_independence(void)
{
	start();
	odi_gpon_flows_set(&f, DS, 1, 1434U);
	odi_gpon_flows_set(&f, DS, 2, 1435U);
	ds_set(1, 10U, 100U, 10U, 10U);
	ds_set(2, 20U, 200U, 20U, 19U);
	odi_gpon_flows_sample(&f);
	CHECK(f.ds[1].gem_frames == 10U && f.ds[2].gem_frames == 20U, "each flow has its own count");
	CHECK(f.ds[2].eth_fwd == 19U, "and its own forwarded count");
}

/* ---- the /proc lines -------------------------------------------------- */

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

static void compare_golden(const char *path)
{
	FILE *fp;
	char want[8192];
	size_t n;

	if (getenv("UPDATE")) {
		fp = fopen(path, "w");
		if (!fp || fwrite(out, 1, outn, fp) != outn) {
			fprintf(stderr, "FAIL: cannot write %s\n", path);
			failures++;
		}
		if (fp)
			fclose(fp);
		return;
	}
	fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "FAIL: cannot read %s\n", path);
		failures++;
		return;
	}
	n = fread(want, 1, sizeof(want) - 1, fp);
	fclose(fp);
	want[n] = 0;
	if (n != outn || memcmp(want, out, n) != 0) {
		fprintf(stderr, "FAIL: /proc lines differ from %s; got:\n%s", path, out);
		failures++;
	}
}

static void test_show_empty(void)
{
	start();
	outn = 0;
	out[0] = 0;
	odi_gpon_flows_show(&f, put, NULL);
	CHECK(strcmp(out, "flow_ack_timeouts 0\nflow_saturated 0\n") == 0,
	      "no flows: the two tallies and no flow line");
}

/* The ISP1 line of the stick: the OMCI channel and three data flows
 * downstream, three flows upstream, a few seconds of traffic each.
 */
static void test_show_golden(const char *dir)
{
	unsigned int i;
	char path[512];

	start();
	odi_gpon_flows_set(&f, DS, 0, 4095U);
	odi_gpon_flows_set(&f, DS, 1, 1434U);
	odi_gpon_flows_set(&f, DS, 2, 1435U);
	odi_gpon_flows_set(&f, DS, 5, 1440U);
	odi_gpon_flows_set(&f, US, 0, 1562U);
	odi_gpon_flows_set(&f, US, 4, 1566U);
	for (i = 0; i < 5; i++) {
		ds_set(0, 2U, 160U, 2U, 2U);
		ds_set(1, 4000U, 5600000U, 4000U, 4000U);
		ds_set(2, i == 3 ? 12U : 0U, i == 3 ? 900U : 0U, i == 3 ? 12U : 0U, i == 3 ? 11U : 0U);
		ds_set(5, 0xffffffffU * (i == 1), 300000000U, 0, 0);
		us_set(0, 2600U, 400U, 7400000U);
		us_set(4, 66U, 10U, 0x100000000ULL * (i == 2) + 5500U);
		odi_gpon_flows_sample(&f);
	}
	outn = 0;
	out[0] = 0;
	odi_gpon_flows_show(&f, put, NULL);
	snprintf(path, sizeof(path), "%s/odi_gpon_flows.txt", dir);
	compare_golden(path);
}

int main(int argc, char **argv)
{
	test_starts_from_zero();
	test_accumulate();
	test_past_32_bits();
	test_us_bytes_64_bit();
	test_only_active_flows_are_read();
	test_flow_created_again();
	test_clear();
	test_bad_index();
	test_ack_is_bounded();
	test_saturation();
	test_flow_independence();
	test_show_empty();
	test_show_golden(argc > 1 ? argv[1] : "test/fixtures");

	if (failures) {
		fprintf(stderr, "odi_gpon_flow_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_gpon_flow_test: ok\n");
	return 0;
}
