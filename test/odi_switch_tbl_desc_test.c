/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_tbl_desc_test.c -- asserts the exact TABLE_CMD and
 * TABLE_WRITE_WORD register sequence odi_switch_table_write() produces
 * for one row of each of the five tables boot5's cmd 51 brackets use
 * ("TBL_ACCESS encoding").
 *
 * No capture shows this handshake's register content (boot3's skip list
 * and boot5's table_write()-level hook both drop it -- odi_switch_tbl.h's
 * odi_switch_table_write() comment explains why), so the expected values
 * here are derived directly from odi_switch_hw.h's odi_sw_table_desc[]
 * and the TABLE_CMD FIELD bit positions, not from a trace. Each
 * test's comment shows the arithmetic.
 *
 * The odi_reg_write() calls TABLE_CMD/WR_DATA land in are normally
 * swallowed by odi_switch_mock.h's skip range (ODI_MOCK_TBL_ACCESS_LO/HI,
 * mirroring the real regtrace skip list) -- this file reads
 * odi_mock.regs[] directly afterward instead, bypassing the write log
 * entirely, since the log is deliberately empty for this address range.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* VLAN: odi_sw_table_desc[ODI_SW_TBL_VLAN_MEMBERS] = { type=1, size=4096,
 * datareg_num=1, addr_offset=0 }. Row index 1, one data word.
 *   WR_DATA[0] = data[0] (a single-word row has nothing to reverse).
 *   ADDR = 1 + 0 = 1. CTRL = 0x80000000 | (1<<9) | (1<<4) | (1<<3) | 1
 *        = 0x80000219.
 */
static void test_vlan(void)
{
	static const uint32_t data[1] = { 0x000000f0U };
	int rc;

	odi_mock_reset();
	rc = odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, 1, data, 1);
	CHECK(rc == 0, "VLAN table_write reports success");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(0))] == 0x000000f0U,
	      "VLAN WR_DATA[0] == data[0]");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_CMD_OFF)] == 0x80000219U,
	      "VLAN TABLE_CMD == 0x80000219 (ADDR=1, TABLE_KIND=1)");
}

/* CLS_RULE_B: { type=4, size=256, datareg_num=2, addr_offset=256
 * (the captured CF_RULE_48_* row addresses are offset by the table
 * size) }. Row index 1, two data words [a, b].
 *   WR_DATA[0] = b (data[1]), WR_DATA[1] = a (data[0]) -- reversed.
 *   ADDR = 1 + 256 = 257. CTRL = 0x80000000 | (257<<9) | (1<<4) | (1<<3) | 4
 *        = 0x80000000 | 0x20200 | 0x1c = 0x8002021c.
 */
static void test_cf_rule(void)
{
	static const uint32_t data[2] = { 0x00010000U, 0x80005808U };
	int rc;

	odi_mock_reset();
	rc = odi_switch_table_write(ODI_SW_TBL_CLS_RULE_B, 1, data, 2);
	CHECK(rc == 0, "CLS_RULE_B table_write reports success");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(0))] == data[1],
	      "CLS_RULE_B WR_DATA[0] == data[1] (reversed)");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(1))] == data[0],
	      "CLS_RULE_B WR_DATA[1] == data[0] (reversed)");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_CMD_OFF)] == 0x8002021cU,
	      "CLS_RULE_B TABLE_CMD == 0x8002021c (ADDR=257, TABLE_KIND=4)");
}

/* CLS_MASK_B: { type=4, size=256, datareg_num=2, addr_offset=0 --
 * the += pTable->size special case names only CF_RULE_48_*, not
 * CF_MASK_48_* }. Row index 1, two data words [a, b].
 *   WR_DATA[0] = b, WR_DATA[1] = a -- reversed, same as CLS_RULE_B.
 *   ADDR = 1 + 0 = 1. CTRL = 0x80000000 | (1<<9) | (1<<4) | (1<<3) | 4
 *        = 0x8000021c.
 */
static void test_cf_mask(void)
{
	static const uint32_t data[2] = { 0x00000000U, 0x8000001fU };
	int rc;

	odi_mock_reset();
	rc = odi_switch_table_write(ODI_SW_TBL_CLS_MASK_B, 1, data, 2);
	CHECK(rc == 0, "CLS_MASK_B table_write reports success");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(0))] == data[1],
	      "CLS_MASK_B WR_DATA[0] == data[1] (reversed)");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(1))] == data[0],
	      "CLS_MASK_B WR_DATA[1] == data[0] (reversed)");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_CMD_OFF)] == 0x8000021cU,
	      "CLS_MASK_B TABLE_CMD == 0x8000021c (ADDR=1, TABLE_KIND=4, no offset)");
}

/* CLS_DS_ACTION: { type=5, size=256, datareg_num=3, addr_offset=0 }.
 * Row index 1, three data words [a, b, c].
 *   WR_DATA[0] = c (data[2]), WR_DATA[1] = b (data[1]), WR_DATA[2] = a
 *   (data[0]) -- reversed.
 *   ADDR = 1. CTRL = 0x80000000 | (1<<9) | (1<<4) | (1<<3) | 5
 *        = 0x8000021d.
 */
static void test_cf_action_ds(void)
{
	static const uint32_t data[3] = { 0x00000000U, 0x00240005U, 0x88240003U };
	int rc;

	odi_mock_reset();
	rc = odi_switch_table_write(ODI_SW_TBL_CLS_DS_ACTION, 1, data, 3);
	CHECK(rc == 0, "CLS_DS_ACTION table_write reports success");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(0))] == data[2],
	      "CLS_DS_ACTION WR_DATA[0] == data[2] (reversed)");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(1))] == data[1],
	      "CLS_DS_ACTION WR_DATA[1] == data[1]");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(2))] == data[0],
	      "CLS_DS_ACTION WR_DATA[2] == data[0] (reversed)");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_CMD_OFF)] == 0x8000021dU,
	      "CLS_DS_ACTION TABLE_CMD == 0x8000021d (ADDR=1, TABLE_KIND=5)");
}

/* CLS_US_ACTION: { type=5, size=256, datareg_num=3, addr_offset=0 } --
 * same descriptor as CLS_DS_ACTION (both type 5), so the same row index
 * produces the identical CTRL word; the two tables are told apart by
 * software only (which enum value the caller passes), not by any
 * register content -- worth stating plainly since it looks, from the
 * register side alone, like a single table.
 */
static void test_cf_action_us(void)
{
	static const uint32_t data[3] = { 0x00000000U, 0x0024005aU, 0x04a40004U };
	int rc;

	odi_mock_reset();
	rc = odi_switch_table_write(ODI_SW_TBL_CLS_US_ACTION, 1, data, 3);
	CHECK(rc == 0, "CLS_US_ACTION table_write reports success");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(0))] == data[2],
	      "CLS_US_ACTION WR_DATA[0] == data[2] (reversed)");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(1))] == data[1],
	      "CLS_US_ACTION WR_DATA[1] == data[1]");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_WRITE_WORD(2))] == data[0],
	      "CLS_US_ACTION WR_DATA[2] == data[0] (reversed)");
	CHECK(odi_mock.regs[odi_mock_slot(ODI_SW_TABLE_CMD_OFF)] == 0x8000021dU,
	      "CLS_US_ACTION TABLE_CMD == 0x8000021d (ADDR=1, TABLE_KIND=5, same as CLS_DS_ACTION)");
}

/* A table id with no descriptor entry (datareg_num == 0, e.g. every
 * LUT/L34/HSx-funnel id) is refused rather than given a guessed TABLE_KIND.
 */
static void test_unresolved_table_refused(void)
{
	static const uint32_t data[1] = { 0 };
	int rc;

	odi_mock_reset();
	rc = odi_switch_table_write(ODI_SW_TBL_L2_UNICAST, 0, data, 1);
	CHECK(rc == -95, "an L2_UNICAST (LUT-funnel, no descriptor) write returns -EOPNOTSUPP");
}

/* A wrong word count for a known table is refused too, rather than
 * silently reading/writing past the caller's array.
 */
static void test_wrong_word_count_refused(void)
{
	static const uint32_t data[1] = { 0 };
	int rc;

	odi_mock_reset();
	rc = odi_switch_table_write(ODI_SW_TBL_VLAN_MEMBERS, 0, data, 2);
	CHECK(rc == -95, "VLAN write with the wrong word count returns -EOPNOTSUPP");
}

int main(void)
{
	test_vlan();
	test_cf_rule();
	test_cf_mask();
	test_cf_action_ds();
	test_cf_action_us();
	test_unresolved_table_refused();
	test_wrong_word_count_refused();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_tbl_desc_test: ok\n");
	return 0;
}
