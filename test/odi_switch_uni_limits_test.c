/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_uni_limits_test.c -- the driver commands that used to be
 * refused and are not any more, against the register mock:
 *
 *   cmd 53  the learning limit of a UNI port (a field of a shared word)
 *   cmd 67  the UNI rates: ingress meter, egress shaper, and the second
 *           shaper register of port 0
 *   cmd 0   the MIB reset puts back only what was changed
 *   cmd 63, 40  the two that succeed or are refused without a write
 *   cmd 61 and OMCI_BRIDGE_TBL_CMD  the bridge table of ME 50, read from
 *           a model of the L2 table
 *
 * and, for each, the arguments that must change nothing. The starting
 * words are the ones read on a stick (learn limit 0x00220440, meter
 * 0x001ffffe, shaper 0x000ffffe, second shaper 0x0003ffff).
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

#define LEARN	0x17008U
#define POLICE0	0x20010U
#define POLICE1	0x20410U
#define SHAPE0	0x2d804U
#define SHAPE1	0x2d808U
#define SHAPE0B	0x2d8b8U
#define SETUP	0x17000U

static uint32_t reg(uint32_t addr)
{
	return odi_mock.regs[odi_mock_slot(addr)];
}

static void set_reg(uint32_t addr, uint32_t v)
{
	odi_mock.regs[odi_mock_slot(addr)] = v;
}

/* ---- A read-only model of the L2 table -------------------------------- */

#define MODEL_ROWS	1088U
static uint32_t model_rows[MODEL_ROWS][3];
static int model_used[MODEL_ROWS];

static void lut_hook(uint32_t off, uint32_t val)
{
	uint32_t row, i, sts;
	uint32_t out[3] = { 0, 0, 0 };

	if (off != ODI_SW_TABLE_CMD_OFF || !(val & (1U << 31)))
		return;
	odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_CMD_OFF)] = val & ~(1U << 31);
	if (((val >> 3) & 1U) || ((val >> 4) & 7U) != ODI_SW_L2_METHOD_ROW) {
		fprintf(stderr, "model: only reads by row are modelled\n");
		failures++;
		return;
	}
	row = (val >> 9) & 0xfffU;
	sts = row & ODI_SW_TABLE_STATUS_ROW_MASK;
	if (row >= ODI_SW_L2_HASH_ROWS)
		sts |= ODI_SW_TABLE_STATUS_IN_CAM;
	if (row < MODEL_ROWS && model_used[row]) {
		memcpy(out, model_rows[row], sizeof(out));
		sts |= ODI_SW_TABLE_STATUS_HIT;
	}
	out[2] |= 1U << 13;
	for (i = 0; i < 3; i++)
		set_reg(ODI_SW_TABLE_READ_WORD(i), out[i]);
	set_reg(ODI_SW_TABLE_STATUS_OFF, sts);
}

/* A unicast row: MAC in bits 0..47, the static bit 62, the source port at
 * 65, the age at 67 and the destination-block bit at 72. */
static void put_row(uint32_t idx, const uint8_t mac[6], unsigned int port,
		    unsigned int age, int is_static, int da_block)
{
	model_rows[idx][0] = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
			     ((uint32_t)mac[4] << 8) | mac[5];
	model_rows[idx][1] = ((uint32_t)mac[0] << 8) | mac[1] | (is_static ? 1U << 30 : 0U);
	model_rows[idx][2] = ((uint32_t)port << 1) | ((uint32_t)age << 3) |
			     (da_block ? 1U << 8 : 0U) | (1U << 13);
	model_used[idx] = 1;
}

static void setup(void)
{
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	memset(model_rows, 0, sizeof(model_rows));
	memset(model_used, 0, sizeof(model_used));
	odi_mock_write_hook = lut_hook;
	/* What a stick reads: CAM on, ageing time 3000 tenths of a second. */
	set_reg(SETUP, 0x00400bb8U);
	set_reg(LEARN, 0x00220440U);
	set_reg(LEARN + 4U, 0x00220440U);
	set_reg(POLICE0, 0x001ffffeU);
	set_reg(POLICE1, 0x001ffffeU);
	set_reg(SHAPE0, 0x000ffffeU);
	set_reg(SHAPE1, 0x000ffffeU);
	set_reg(SHAPE0B, 0x0003ffffU);
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

/* ---- cmd 53 ----------------------------------------------------------- */

static int learn(uint32_t port, uint32_t limit)
{
	struct omci_learn_limit a = { .port = port, .limit = limit };

	return odi_switch_cmd(OMCI_LEARN_LIMIT_CMD, &a, sizeof a);
}

static void test_learn_limit(void)
{
	setup();
	CHECK(learn(0, 8) == 0, "a limit of 8 on the UNI is taken");
	CHECK(reg(LEARN) == 0x00220008U, "port 0's field holds 8, port 1's is untouched");
	CHECK(odi_sw_l2_learn_limit_get(0) == 8U && odi_sw_l2_learn_limit_get(1) == 0x440U,
	      "the leaf reads each port's own field back");
	CHECK(learn(1, 300) == 0, "port 1 takes its own");
	CHECK(reg(LEARN) == 0x00096008U, "port 1 lives in bits 21..11, port 0's 8 is kept");
	CHECK(reg(LEARN + 4U) == 0x00220440U, "the word of ports 2 and 3 is never touched");
	CHECK(writes_to(LEARN + 4U) == 0, "no write to the second word");

	/* A limit at or past the table is the boot value, not an overflow. */
	CHECK(learn(0, 1088) == 0 && odi_sw_l2_learn_limit_get(0) == 0x440U,
	      "1088 is the table size: no limit");
	CHECK(learn(0, 100000) == 0 && odi_sw_l2_learn_limit_get(0) == 0x440U,
	      "a value past the 11-bit field is the same, it does not wrap to a small one");
	CHECK(reg(LEARN) >> 22 == 0U, "bits above the two fields are kept");

	setup();
	CHECK(learn(0, 0) == -22 && writes() == 0, "0 is refused: it would stop learning");
	CHECK(learn(2, 8) == ODI_SW_EOPNOTSUPP && writes() == 0, "the PON port is refused");
	CHECK(learn(3, 8) == ODI_SW_EOPNOTSUPP && writes() == 0, "the CPU port is refused");
	CHECK(learn(4, 8) == ODI_SW_EOPNOTSUPP && writes() == 0, "a port past the switch is refused");
	CHECK(odi_switch_cmd(OMCI_LEARN_LIMIT_CMD, &(uint32_t){ 0 }, 4) == -1 && writes() == 0,
	      "a short argument is refused");
	CHECK(reg(0x1c00cU) == 0U && writes_to(0x1c00cU) == 0,
	      "the over-limit action is never written");
}

/* ---- cmd 67 ----------------------------------------------------------- */

static int rate(uint32_t port, uint32_t dir, uint32_t kbps)
{
	struct omci_unirate a = { .port = port, .dir = dir, .kbps = kbps };

	return odi_switch_cmd(OMCI_UNIRATE_CMD, &a, sizeof a);
}

static void test_uni_rate(void)
{
	setup();
	/* 100 Mbit/s = 12500 units of 8 kbit/s. */
	CHECK(rate(0, OMCI_UNIRATE_DIR_IN, 100000) == 0, "an inbound rate is taken");
	CHECK(reg(POLICE0) == ((12500U << 2) | 0x2U),
	      "the ingress meter holds 12500 units and keeps its flow-control bit");
	CHECK(reg(POLICE1) == 0x001ffffeU, "port 1's meter is untouched");
	CHECK(reg(SHAPE0) == 0x000ffffeU && reg(SHAPE0B) == 0x0003ffffU,
	      "an inbound rate leaves the shaper alone");

	CHECK(rate(0, OMCI_UNIRATE_DIR_OUT, 100000) == 0, "an outbound rate is taken");
	CHECK(reg(SHAPE0) == (12500U << 1), "the egress shaper holds 12500 units");
	CHECK(reg(SHAPE0B) == 6250U, "port 0's second register holds half, in 16 kbit/s");
	CHECK(reg(SHAPE1) == 0x000ffffeU, "port 1's shaper is untouched");

	/* Bit 19 of the second register (the 2.5G override) is not ours. */
	set_reg(SHAPE0B, 0x000bffffU);
	CHECK(rate(0, OMCI_UNIRATE_DIR_OUT, 1032) == 0, "1032 kbit/s: 129 units");
	CHECK(reg(SHAPE0B) == (0x80000U | 65U), "the override bit stays, the half rounds up");

	/* No limit is exactly the reset words, however it is asked for. */
	CHECK(rate(0, OMCI_UNIRATE_DIR_IN, OMCI_UNIRATE_NOLIMIT) == 0, "no limit, inbound");
	CHECK(reg(POLICE0) == 0x001ffffeU, "the meter is back to its reset word");
	set_reg(SHAPE0B, 0x0003ffffU);
	CHECK(rate(0, OMCI_UNIRATE_DIR_OUT, OMCI_UNIRATE_NOLIMIT) == 0, "no limit, outbound");
	CHECK(reg(SHAPE0) == 0x000ffffeU && reg(SHAPE0B) == 0x0003ffffU,
	      "the shaper and its second register are back to their reset words");

	setup();
	CHECK(rate(0, OMCI_UNIRATE_DIR_IN, 1023) == -22 && writes() == 0,
	      "below 1 Mbit/s is refused and nothing is written");
	CHECK(rate(0, OMCI_UNIRATE_DIR_IN, 0) == -22 && writes() == 0, "0 is refused too");
	CHECK(rate(0, 3, 100000) == ODI_SW_EOPNOTSUPP && writes() == 0, "an unknown direction is refused");
	CHECK(rate(2, OMCI_UNIRATE_DIR_OUT, 100000) == ODI_SW_EOPNOTSUPP && writes() == 0,
	      "the PON port is refused");
	CHECK(rate(3, OMCI_UNIRATE_DIR_IN, 100000) == ODI_SW_EOPNOTSUPP && writes() == 0,
	      "the CPU port is refused");
	CHECK(odi_switch_cmd(OMCI_UNIRATE_CMD, &(uint32_t){ 0 }, 4) == -1 && writes() == 0,
	      "a short argument is refused");
	/* The bound of the largest rate that is a limit: one unit under the cap. */
	CHECK(rate(0, OMCI_UNIRATE_DIR_IN, 0xffff7U) == 0 &&
	      odi_sw_uni_police_get(0) == (0xffff7U / 8U),
	      "the largest rate below the vendor's off value is a real limit");
}

/* ---- the MIB reset ------------------------------------------------------ */

static void test_mib_reset(void)
{
	uint8_t none[4] = { 0 };

	setup();
	CHECK(odi_switch_cmd(OMCI_MIB_RESET_CMD, none, sizeof none) == 0, "a MIB reset with nothing set");
	CHECK(writes_to(LEARN) == 0 && writes_to(POLICE0) == 0 && writes_to(SHAPE0) == 0 &&
	      writes_to(SHAPE0B) == 0, "writes none of the UNI limit registers");

	setup();
	CHECK(learn(0, 8) == 0 && rate(0, OMCI_UNIRATE_DIR_IN, 100000) == 0 &&
	      rate(0, OMCI_UNIRATE_DIR_OUT, 100000) == 0, "limits set");
	CHECK(odi_switch_cmd(OMCI_MIB_RESET_CMD, none, sizeof none) == 0, "a MIB reset");
	CHECK(reg(LEARN) == 0x00220440U && reg(POLICE0) == 0x001ffffeU &&
	      reg(SHAPE0) == 0x000ffffeU && reg(SHAPE0B) == 0x0003ffffU,
	      "puts the boot words back");
	CHECK(writes_to(POLICE1) == 0 && writes_to(SHAPE1) == 0 && writes_to(LEARN + 4U) == 0,
	      "and writes nothing for a port it never changed");
	{
		unsigned int before = writes_to(LEARN);

		CHECK(odi_switch_cmd(OMCI_MIB_RESET_CMD, none, sizeof none) == 0 &&
		      writes_to(LEARN) == before, "a second reset has nothing left to put back");
	}
}

/* ---- cmd 63 and 40 ------------------------------------------------------ */

static void test_bridging_and_pause(void)
{
	uint32_t one = 1, zero = 0, two = 2;
	uint32_t p0[2] = { 0, 0 }, p1[2] = { 0, 5 };

	setup();
	CHECK(odi_switch_cmd(OMCI_PORT_BRIDGING_CMD, &one, 4) == 0, "bridging 1 succeeds");
	CHECK(odi_switch_cmd(OMCI_PORT_BRIDGING_CMD, &zero, 4) == ODI_SW_EOPNOTSUPP, "bridging 0 is refused");
	CHECK(odi_switch_cmd(OMCI_PORT_BRIDGING_CMD, &two, 4) == ODI_SW_EOPNOTSUPP, "so is anything else");
	CHECK(writes() == 0, "neither writes a register: the isolation masks stay as they are");
	CHECK(odi_switch_cmd(OMCI_PAUSE_CONTROL_CMD, p0, 8) == 0, "pause time 0 succeeds");
	CHECK(odi_switch_cmd(OMCI_PAUSE_CONTROL_CMD, p1, 8) == ODI_SW_EOPNOTSUPP, "pause time 5 is refused");
	CHECK(writes() == 0, "neither writes a register");
}

/* ---- ME 50 ---------------------------------------------------------- */

static const uint8_t mac_a[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

static int tbl(uint32_t port, uint32_t first, struct omci_bridge_tbl *t)
{
	memset(t, 0xee, sizeof(*t));
	t->port = port;
	t->first = first;
	return odi_switch_cmd(OMCI_BRIDGE_TBL_CMD, t, sizeof(*t));
}

static void test_bridge_table(void)
{
	struct omci_bridge_tbl t;
	struct omci_bridge_tbl_per_port c = { .portId = 2, .cnt = 0xeeee };
	uint8_t m[6];
	unsigned int i;

	setup();
	/* Port 0: two learned, one of them blocking as a destination. Port 2:
	 * 35 learned. Port 3: the stick's own static pin. A multicast row with
	 * port bits of its own, which belongs to no port. Rows spread over the
	 * table, the CAM among them. */
	memcpy(m, mac_a, 6);
	m[5] = 0x10;
	put_row(5, m, 0, 7, 0, 0);
	m[5] = 0x11;
	put_row(900, m, 0, 3, 0, 1);
	for (i = 0; i < 35U; i++) {
		m[5] = (uint8_t)(0x40 + i);
		put_row(10U + i * 3U, m, 2, 7, 0, 0);
	}
	m[5] = 0x99;
	put_row(1080, m, 3, 0, 1, 0);
	model_rows[20][0] = 0x5e010203U;
	model_rows[20][1] = 0x0100U;
	model_rows[20][2] = (1U << 2) | (1U << 13);
	model_used[20] = 1;

	CHECK(odi_switch_cmd(OMCI_BRIDGE_TBL_PORT_CMD, &c, sizeof c) == 0 && c.cnt == 35U,
	      "cmd 61 counts the PON port's 35");
	c.portId = 0;
	CHECK(odi_switch_cmd(OMCI_BRIDGE_TBL_PORT_CMD, &c, sizeof c) == 0 && c.cnt == 2U,
	      "and the UNI's 2: the multicast row is nobody's");
	c.portId = 1;
	CHECK(odi_switch_cmd(OMCI_BRIDGE_TBL_PORT_CMD, &c, sizeof c) == 0 && c.cnt == 0U,
	      "an empty port is 0");
	c.portId = 9;
	CHECK(odi_switch_cmd(OMCI_BRIDGE_TBL_PORT_CMD, &c, sizeof c) == -22, "a port past the switch is refused");

	CHECK(tbl(0, 0, &t) == 0 && t.total == 2U && t.count == 2U, "the UNI's table: 2 of 2");
	/* Row 5: age 7 of 7 at a 3000-tick ageing time is 7 * 42 s = 294 s,
	 * learned (bit 2), forwarding (bit 0 clear): 0x1260 | 4. */
	CHECK(t.entry[0] == 0x12 && t.entry[1] == 0x64, "entry 0: age 294 s, learned, forward");
	CHECK(memcmp(t.entry + 2, "\x02\x00\x00\x00\x00\x10", 6) == 0, "entry 0: the MAC, octet 0 first");
	/* Row 900: age 3 = 126 s, learned, blocking: 0x07e0 | 4 | 1. */
	CHECK(t.entry[8] == 0x07 && t.entry[9] == 0xe5, "entry 1: age 126 s, learned, blocks");
	CHECK(memcmp(t.entry + 10, "\x02\x00\x00\x00\x00\x11", 6) == 0, "entry 1: the MAC");
	CHECK(t.entry[16] == 0 && t.entry[239] == 0, "the rest of the page is zero, not stale bytes");

	CHECK(tbl(2, 0, &t) == 0 && t.total == 35U && t.count == OMCI_BRIDGE_TBL_PAGE,
	      "the PON port: 35 in all, a full first page of 30");
	CHECK(t.entry[2 + 5] == 0x40 && t.entry[29 * 8 + 2 + 5] == 0x40 + 29, "in table order");
	CHECK(tbl(2, OMCI_BRIDGE_TBL_PAGE, &t) == 0 && t.total == 35U && t.count == 5U,
	      "the second page holds the last 5");
	CHECK(t.entry[2 + 5] == 0x40 + 30 && t.entry[4 * 8 + 2 + 5] == 0x40 + 34, "from where the first stopped");
	CHECK(tbl(2, 35, &t) == 0 && t.total == 35U && t.count == 0U, "past the end: nothing, total still 35");

	CHECK(tbl(3, 0, &t) == 0 && t.total == 1U && t.count == 1U, "the CPU port: the pin");
	CHECK(t.entry[0] == 0x00 && t.entry[1] == 0x00, "a static row, age 0: information word 0");
	CHECK(tbl(7, 0, &t) == -22, "a port past the switch is refused");
	CHECK(odi_switch_cmd(OMCI_BRIDGE_TBL_CMD, &t, 8) == -1, "a short buffer is refused");

	/* The ageing time scales the age: 600 s (6000 ticks), 7 -> 7 * 85 s. */
	set_reg(SETUP, (reg(SETUP) & ~0x1fffffU) | 6000U);
	CHECK(tbl(0, 0, &t) == 0 && t.entry[0] == 0x25 && t.entry[1] == 0x34,
	      "the age follows the configured ageing time (595 s)");

	/* Reading the table writes no register of the switch except the
	 * engine's own command and data words. */
	{
		unsigned int i2, other = 0;

		for (i2 = 0; i2 < odi_mock.log_n; i2++)
			if (odi_mock.log[i2].kind == 'W' &&
			    odi_mock.log[i2].addr != ODI_SW_TABLE_CMD_OFF)
				other++;
		CHECK(other == 0, "a table read writes only TABLE_CMD");
	}
}

static void test_refusals(void)
{
	static const uint32_t cmds[] = { 34, 52, 54, 55, 56, 59, 60, 71 };
	uint8_t buf[80] = { 0 };
	unsigned int i;

	setup();
	for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
		CHECK(odi_switch_cmd(cmds[i], buf, sizeof buf) == ODI_SW_EOPNOTSUPP,
		      "the commands that are still not built are refused");
	CHECK(writes() == 0, "and write nothing");
}

int main(void)
{
	test_learn_limit();
	test_uni_rate();
	test_mib_reset();
	test_bridging_and_pause();
	test_bridge_table();
	test_refusals();
	if (failures) {
		fprintf(stderr, "odi_switch_uni_limits_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_switch_uni_limits_test: ok\n");
	return 0;
}
