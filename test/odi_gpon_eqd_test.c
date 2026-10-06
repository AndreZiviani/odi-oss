/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_eqd_test.c -- the USF_EQ_DELAY value odi_gpon_hw.c writes, against
 * test/odi_switch_mock.h: the split of the equalization delay into FRAMES and
 * INFRAME (the DELAY_HI term goes in before the division), the pre-assigned
 * delay of Upstream_Overhead (G.984.3 clause 9.2.3.1), and a Ranging_Time
 * with the protection path bit set (clause 9.2.3.4), which writes nothing.
 *
 * The register values are the ones the captured activation shows (one
 * upstream frame is 155520 bits, USF_MIN_RESP_DELAY 0x9132, so DELAY_HI is
 * 290 and DELAY_HI << 7 is 37120), plus cases the two test lines never hit.
 */
#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_isr.c"

#include <stdio.h>

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

#define FRAME_BITS	155520U
#define DELAY_HI_BITS	37120U	/* 290 << 7, from USF_MIN_RESP_DELAY 0x9132 */
#define NO_CARRY_LIMIT	(FRAME_BITS - DELAY_HI_BITS)	/* 118400 */

static uint32_t eq_delay_reg(void)
{
	return odi_mock.regs[odi_mock_slot(ODI_GPON_USF_EQ_DELAY_OFF)];
}

static void expect_eqd(uint32_t eqd_bits, uint32_t frames, uint32_t inframe, const char *what)
{
	char msg[160];

	odi_gpon_hw_eqd_rewrite(eqd_bits);
	snprintf(msg, sizeof msg, "%s: FRAMES", what);
	CHECK(ODI_GPON_USF_EQ_DELAY_FRAMES_GET(eq_delay_reg()) == frames, msg);
	snprintf(msg, sizeof msg, "%s: INFRAME", what);
	CHECK(ODI_GPON_USF_EQ_DELAY_EQD_GET(eq_delay_reg()) == inframe, msg);
}

/* What the code did before the total was formed first: divide the delay,
 * then add the DELAY_HI term to the remainder.
 */
static void old_split(uint32_t eqd_bits, uint32_t *frames, uint32_t *inframe)
{
	*frames = eqd_bits / FRAME_BITS;
	*inframe = (eqd_bits % FRAME_BITS) + DELAY_HI_BITS;
}

static void setup(void)
{
	odi_mock_reset();
	odi_gpon_hw_reset();
	odi_mock.regs[odi_mock_slot(ODI_GPON_USF_MIN_RESP_DELAY_OFF)] = 0x9132U;
}

static void test_split(void)
{
	uint32_t f, i;

	setup();

	/* The captured delay, both test lines: 227172 bits -> 1 frame + 108772. */
	expect_eqd(227172U, 1U, 108772U, "captured delay");

	/* No carry: the remainder is below 118400, so the old order gives the
	 * same registers, checked against it.
	 */
	old_split(FRAME_BITS + 10000U, &f, &i);
	CHECK(f == 1U && i == 47120U, "the old order, for the no-carry case, is as expected");
	expect_eqd(FRAME_BITS + 10000U, f, i, "no carry, one frame");
	old_split(1000U, &f, &i);
	expect_eqd(1000U, f, i, "no carry, no whole frame");
	old_split(2U * FRAME_BITS + NO_CARRY_LIMIT - 1U, &f, &i);
	expect_eqd(2U * FRAME_BITS + NO_CARRY_LIMIT - 1U, f, i, "no carry, last remainder below the limit");

	/* Carry: the remainder is 118400 or more, so remainder + DELAY_HI term
	 * reaches a whole frame. Old: FRAMES 0, INFRAME 157120 (above a frame).
	 * Now: FRAMES 1, INFRAME 1600.
	 */
	old_split(120000U, &f, &i);
	CHECK(f == 0U && i == 157120U, "the old order, for the carry case, left INFRAME above a frame");
	expect_eqd(120000U, 1U, 1600U, "carry from zero frames");
	expect_eqd(NO_CARRY_LIMIT, 1U, 0U, "carry, remainder exactly at the limit");
	expect_eqd(FRAME_BITS + 150000U, 2U, 31600U, "carry with one whole frame already");
	expect_eqd(2U * FRAME_BITS + 118500U, 3U, 100U, "carry with two whole frames");

	/* INFRAME is always below one frame now. */
	CHECK(ODI_GPON_USF_EQ_DELAY_EQD_GET(eq_delay_reg()) < FRAME_BITS, "INFRAME stays below one frame");
}

static void set_upstream_overhead(uint16_t preassigned_delay)
{
	static const uint8_t delim[3] = { 0xb5, 0x98, 0x30 };

	odi_gpon_hw_ops()->set_upstream_overhead(NULL, 8U, 4U, 4U, 0x55U, delim, preassigned_delay, 0U);
}

static void test_preassigned_delay(void)
{
	setup();

	/* No pre-assigned delay: only the DELAY_HI term, as the capture shows. */
	set_upstream_overhead(0U);
	CHECK(eq_delay_reg() == 0x9100U, "no pre-assigned delay: USF_EQ_DELAY is the DELAY_HI term alone");

	/* 16 units of 32 bytes = 4096 bits. */
	set_upstream_overhead(16U);
	CHECK(ODI_GPON_USF_EQ_DELAY_FRAMES_GET(eq_delay_reg()) == 0U, "pre-assigned 16: FRAMES");
	CHECK(ODI_GPON_USF_EQ_DELAY_EQD_GET(eq_delay_reg()) == 4096U + DELAY_HI_BITS, "pre-assigned 16: INFRAME");

	/* 600 units = 153600 bits: goes through the same split, with a carry. */
	set_upstream_overhead(600U);
	CHECK(ODI_GPON_USF_EQ_DELAY_FRAMES_GET(eq_delay_reg()) == 1U, "pre-assigned 600: FRAMES carries");
	CHECK(ODI_GPON_USF_EQ_DELAY_EQD_GET(eq_delay_reg()) == 153600U + DELAY_HI_BITS - FRAME_BITS,
	      "pre-assigned 600: INFRAME");
}

/* A Ranging_Time reaches odi_gpon_isr_poll() through the PLOAM RX FIFO. */
static void feed_ranging(struct odi_gpon_fsm *fsm, uint8_t onu_id, uint8_t protection, uint32_t eqd)
{
	uint16_t words[6];
	struct odi_gpon_ploam msg;
	unsigned int i;

	memset(&msg, 0, sizeof msg);
	msg.onu_id = onu_id;
	msg.type = ODI_GPON_DS_RANGING_TIME;
	msg.content[0] = protection;
	msg.content[1] = (uint8_t)(eqd >> 24);
	msg.content[2] = (uint8_t)(eqd >> 16);
	msg.content[3] = (uint8_t)(eqd >> 8);
	msg.content[4] = (uint8_t)eqd;
	odi_gpon_ploam_pack_words(&msg, words);

	odi_mock.regs[odi_mock_slot(ODI_GPON_PONMAC_IRQ_PENDING_OFF)] = ODI_GPON_PONMAC_IRQ_PENDING_DS_FRAMER;
	odi_mock.regs[odi_mock_slot(ODI_GPON_DSF_IRQ_EVENT_OFF)] = ODI_GPON_DSF_IRQ_EVENT_PLOAM_RX;
	odi_mock.regs[odi_mock_slot(ODI_GPON_DSF_PLOAM_RX_CTL_OFF)] = 0U;
	for (i = 0; i < 6U; i++)
		odi_mock.regs[odi_mock_slot(ODI_GPON_DSF_PLOAM_RX_WORD(i))] = words[i];
	(void)odi_gpon_isr_poll(fsm, NULL);
}

static void test_protection_path_ignored(void)
{
	struct odi_gpon_fsm fsm;
	unsigned int n_before, i;
	int eqd_writes = 0;

	setup();
	odi_gpon_fsm_init(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = 0x1aU;

	/* O5, the second and third read of a main path message: rewritten. */
	feed_ranging(&fsm, 0x1aU, 0U, 227172U);
	CHECK(eq_delay_reg() == ((1U << 24) | 108772U), "O5: a main path Ranging_Time rewrites USF_EQ_DELAY");

	/* The protection path message must not touch it. */
	n_before = odi_mock.log_n;
	feed_ranging(&fsm, 0x1aU, 1U, 120000U);
	CHECK(eq_delay_reg() == ((1U << 24) | 108772U), "O5: a protection path Ranging_Time leaves USF_EQ_DELAY alone");
	for (i = n_before; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].addr == ODI_GPON_USF_EQ_DELAY_OFF)
			eqd_writes++;
	CHECK(eqd_writes == 0, "O5: a protection path Ranging_Time writes no USF_EQ_DELAY at all");
}

int main(void)
{
	test_split();
	test_preassigned_delay();
	test_protection_path_ignored();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_eqd_test: all checks passed\n");
	return 0;
}
