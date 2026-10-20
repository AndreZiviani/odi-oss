/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_dsq_test.c -- the schedule of the UNI's downstream queues,
 * which cmd 23 (direction DS) gives on top of the stock write of
 * PORT_QUEUE_MAP: strict or weighted per queue, the weight, the priority to
 * queue map, and the MIB reset that puts the boot state back.
 *
 * The registers start as a stick reads them (every queue strict, every
 * weight 1, the UNI's priority map the identity the stock init writes), and
 * the queues are the class 277 rows an OLT provisioned on claro: entities
 * 0 and 1 weight 0, then 30, 24, 19, 14, 9 and 4, priority 7 down to 0.
 * Host-side, against the register mock. Part of `make test-host`.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

#define MAP0	0x1c0b0U
#define MAPQ	0x1c0c0U
#define WFQ0	0x2d85cU
#define W0	0x2d824U
#define W1_3	0x2d82cU
#define W4_6	0x2d830U
#define W7	0x2d834U
#define SHAPE0	0x2d804U
#define SHAPE0B	0x2d8b8U
#define CTRL	0x2d800U

static uint32_t reg(uint32_t addr)
{
	return odi_mock.regs[odi_mock_slot(addr)];
}

static void set_reg(uint32_t addr, uint32_t v)
{
	odi_mock.regs[odi_mock_slot(addr)] = v;
}

static void setup(void)
{
	odi_mock_reset();
	odi_mock_dsq_reset_state();
	odi_switch_cmd_reset_state();
	odi_sw_ds_sched = true;
	set_reg(MAP0, 0x00fac688U);
	set_reg(SHAPE0, 0x000ffffeU);
	set_reg(SHAPE0B, 0x0003ffffU);
	set_reg(CTRL, 0x00003fffU);
	odi_mock.log_n = 0;
}

static unsigned int writes_to(uint32_t addr)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == addr)
			n++;
	return n;
}

static unsigned int writes(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W')
			n++;
	return n;
}

/* omcid's cmd 23 for a downstream queue: priority, port, weight, and
 * WRR when the weight is 2 or more. */
static int dsq(uint16_t prio, uint16_t port, uint16_t weight)
{
	struct omci_priq q;

	memset(&q, 0, sizeof q);
	q.index = prio;
	q.owner = port;
	q.weight = weight < 1 ? 1 : weight;
	q.wrr = weight >= 2;
	q.dir = OMCI_GEMFLOW_DS;
	return odi_switch_cmd(OMCI_PRIQ_CMD, &q, sizeof q);
}

/* The eight rows of claro's OLT: entity e is priority 7 - e. */
static void provision(void)
{
	static const uint16_t w[8] = { 0, 0, 30, 24, 19, 14, 9, 4 };
	unsigned int e;

	for (e = 0; e < 8; e++)
		CHECK(dsq((uint16_t)(7 - e), 0, w[e]) == 0, "a downstream queue is taken");
}

static void test_olt_template(void)
{
	setup();
	provision();
	/* Queues 7 and 6 strict, 5..0 weighted: bits 5..0. */
	CHECK(reg(WFQ0) == 0x3fU, "queues 0..5 weighted, 6 and 7 strict");
	/* Weight 4 * w: queue 5 is 120, 4 is 96, 3 is 76, 2 is 56, 1 is 36,
	 * queue 0 is 16. Strict queues keep 1. */
	CHECK(reg(W0) == (0x00010000U | 16U), "queue 0 weight 16 in the low half, port 1's half kept");
	CHECK(reg(W1_3) == ((36U) | (56U << 10) | (76U << 20)), "queues 1..3 weights 36, 56, 76");
	CHECK(reg(W4_6) == ((96U) | (120U << 10) | (1U << 20)), "queues 4, 5 weighted 96, 120; 6 strict at 1");
	CHECK(reg(W7) == 1U, "queue 7 strict at 1");
	CHECK(reg(MAP0) == 0x00fac688U, "the identity map is not written again");
	CHECK(writes_to(MAP0) == 0, "no write to the priority map when it already is identity");
	CHECK(writes_to(MAPQ) == 8, "PORT_QUEUE_MAP still written once per queue, as the stock driver does");
	CHECK(reg(MAPQ) == 0xd4U, "with the stock word");
	CHECK(reg(SHAPE0) == 0x000ffffeU && reg(SHAPE0B) == 0x0003ffffU && reg(CTRL) == 0x3fffU,
	      "the shaper and the scheduler control (cmd 67, weights in bytes) are untouched");
	CHECK(writes_to(SHAPE0) == 0 && writes_to(SHAPE0B) == 0 && writes_to(CTRL) == 0,
	      "and not written");
	CHECK(reg(WFQ0 + 4U) == 0U && reg(W1_3 + 12U) == 0x00100401U,
	      "port 1 is not touched");
}

static void test_defaults_write_nothing(void)
{
	unsigned int e;

	setup();
	/* An OLT that leaves every weight at 1: all strict, as the reset
	 * state, so the scheduler registers see no write at all. */
	for (e = 0; e < 8; e++)
		CHECK(dsq((uint16_t)(7 - e), 0, 1) == 0, "weight 1 is taken");
	CHECK(writes() == 8 && writes_to(MAPQ) == 8,
	      "eight queues at the default write PORT_QUEUE_MAP and nothing else");
	CHECK(reg(WFQ0) == 0U && reg(W0) == 0x00010001U, "the scheduler is as the reset left it");

	setup();
	CHECK(dsq(3, 0, 0) == 0, "weight 0 is taken");
	CHECK(reg(WFQ0) == 0U && writes() == 1, "weight 0 is strict, and writes only the stock word");
}

static void test_weights_and_limits(void)
{
	setup();
	CHECK(dsq(0, 0, 2) == 0 && reg(WFQ0) == 1U, "weight 2 is the smallest weighted");
	CHECK((reg(W0) & 0xffffU) == 8U, "queue 0 field 8");
	CHECK(dsq(0, 0, 255) == 0 && (reg(W0) & 0xffffU) == 1020U, "255 is 1020 in queue 0's 16 bits");
	CHECK(dsq(7, 0, 255) == 0 && reg(W7) == 1020U, "255 is 1020 in a 10-bit field");
	CHECK(reg(W7) >> 10 == 0U, "and does not spill into the next field");
	CHECK(dsq(7, 0, 1) == 0 && reg(W7) == 1U && (reg(WFQ0) & 0x80U) == 0, "back to strict: type and weight");
	CHECK((reg(WFQ0) & 1U) == 1U, "the other queue is not disturbed");
	CHECK(dsq(0, 0, 65535) == 0 && (reg(W0) & 0xffffU) == 0xffffU, "an absurd weight is clamped to the field");
	CHECK(dsq(5, 0, 65535) == 0 && ((reg(W4_6) >> 10) & 0x3ffU) == 0x3ffU, "and in a 10-bit field");
}

static void test_not_scheduled(void)
{
	setup();
	/* A priority past the eight queues, and a queue of the PON port (a
	 * VEIP's) or the CPU port: the call succeeds, as before, and only
	 * the stock word is written. */
	CHECK(dsq(8, 0, 30) == 0, "priority 8 is accepted");
	CHECK(dsq(0xffff, 0, 30) == 0, "so is 0xffff");
	CHECK(dsq(3, 2, 30) == 0, "a queue of the PON port");
	CHECK(dsq(3, 3, 30) == 0, "a queue of the CPU port");
	CHECK(dsq(3, 9, 30) == 0, "a queue of a port that does not exist");
	CHECK(writes() == writes_to(MAPQ) && writes_to(MAPQ) == 5,
	      "none of them writes a scheduler register");
	CHECK(reg(WFQ0) == 0U && reg(WFQ0 + 8U) == 0U && reg(WFQ0 + 12U) == 0U, "the type words are as they were");
}

static void test_map_identity(void)
{
	setup();
	/* A chip that did not go through the stock init: every priority to
	 * queue 0, the reset word. The first scheduled queue puts the map
	 * right, and only once. */
	set_reg(MAP0, 0U);
	CHECK(dsq(5, 0, 30) == 0 && reg(MAP0) == 0x00fac688U, "the priority map is made identity");
	CHECK(writes_to(MAP0) == 1, "with one write");
	CHECK(dsq(4, 0, 30) == 0 && writes_to(MAP0) == 1, "and not again");
}

static void test_switch_off(void)
{
	setup();
	odi_sw_ds_sched = false;
	provision();
	CHECK(writes() == 8 && writes_to(MAPQ) == 8, "ds_sched off: the stock word only, as the stock driver");
	CHECK(reg(WFQ0) == 0U && reg(W0) == 0x00010001U, "scheduler untouched");
}

static void test_mib_reset(void)
{
	setup();
	CHECK(odi_switch_cmd(0, NULL, 0) == 0, "a MIB reset with nothing scheduled");
	CHECK(writes() == 0, "writes nothing");

	provision();
	odi_mock.log_n = 0;
	CHECK(odi_switch_cmd(0, NULL, 0) == 0, "a MIB reset");
	CHECK(reg(WFQ0) == 0U, "every queue strict again");
	CHECK(reg(W0) == 0x00010001U && reg(W1_3) == 0x00100401U && reg(W4_6) == 0x00100401U &&
	      reg(W7) == 1U, "every weight 1 again");
	CHECK(reg(MAP0) == 0x00fac688U, "the map is left identity");
	CHECK(writes_to(W1_3 + 12U) == 0 && writes_to(WFQ0 + 4U) == 0, "port 1 is never written");
	odi_mock.log_n = 0;
	CHECK(odi_switch_cmd(0, NULL, 0) == 0 && writes() == 0, "and a second reset has nothing left to put back");
}

static void test_reprovision_same(void)
{
	uint32_t w1, w4, t;

	setup();
	provision();
	w1 = reg(W1_3); w4 = reg(W4_6); t = reg(WFQ0);
	odi_mock.log_n = 0;
	provision();
	CHECK(reg(W1_3) == w1 && reg(W4_6) == w4 && reg(WFQ0) == t, "the same MIB again gives the same words");
	CHECK(writes() == writes_to(MAPQ), "and writes no scheduler word: nothing changed");
}

int main(void)
{
	test_olt_template();
	test_defaults_write_nothing();
	test_weights_and_limits();
	test_not_scheduled();
	test_map_identity();
	test_switch_off();
	test_mib_reset();
	test_reprovision_same();
	if (failures) {
		fprintf(stderr, "odi_switch_dsq_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_switch_dsq_test: ok\n");
	return 0;
}
