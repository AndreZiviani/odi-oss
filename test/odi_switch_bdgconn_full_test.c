/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * odi_switch_bdgconn_full_test.c -- a cmd 51 the classifier table refuses
 * must leave the chip rows as the software table has them.
 *
 * A two-way rule inserts two legs. cf_insert() moves the rows above the
 * insertion point in the software table at once and only queues the chip
 * writes; they go out in odi_sw_cf_add(). When the first leg went in with
 * such a shift and the second leg finds no row, the refusal path used to
 * release the slot without writing the queue: the release then deleted the
 * chip row at the shifted position, which on the chip still held an older,
 * active service, and from then on every cmd 50 deleted the row next to the
 * one it meant.
 *
 * The test fills the ordered region with 95 two-way VID services, sends a
 * 96th that sorts before all of them (VID and priority), and checks that
 * the refusal leaves software and chip in step, that a later cmd 50 takes
 * only its own rows, and that a neighbour keeps its rule.
 *
 * Usage: odi_switch_bdgconn_full_test. Returns non-zero on any failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"

#include "bdgconn_rules.h"

static int failures;

static void ok(int cond, const char *label)
{
	printf("  %-66s %s\n", label, cond ? "ok" : "FAIL");
	if (!cond)
		failures++;
}

/* The classifier rule words as the chip holds them, replayed from the
 * table-write log. */
static uint32_t hw_rule[256][2];

static void hw_sync(void)
{
	unsigned int i;

	odi_mock_table_flush();
	if (odi_mock.log_n >= ODI_MOCK_LOG_MAX) {
		printf("  write log overflow\n");
		failures++;
	}
	for (i = 0; i < odi_mock.log_n; i++) {
		const struct odi_mock_write *e = &odi_mock.log[i];

		if (e->kind == 'T' && (e->addr >> 16) == ODI_SW_TBL_CLS_RULE_B) {
			uint32_t row = e->addr & 0xffffU;

			if (row < 256 && i + 2 < odi_mock.log_n) {
				hw_rule[row][0] = odi_mock.log[i + 1].val;
				hw_rule[row][1] = odi_mock.log[i + 2].val;
			}
		}
	}
	odi_mock.log_n = 0;
}

static int activate(int id, unsigned vid, int pbit)
{
	struct omci_vlan_oper vr;
	struct omci_bdgconn b;
	int rc;

	bdg_gen_vid_filter(&vr, vid, pbit);
	bdg_conn(&b, id, OMCI_DIR_BI, 1, 1, 1, &vr);
	rc = odi_switch_cmd(51, &b, sizeof b);
	hw_sync();
	return rc;
}

/* Writes in the log, since the last hw_sync(), to one table. */
static unsigned int table_writes(uint32_t tbl)
{
	unsigned int i, n = 0;

	odi_mock_table_flush();
	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'T' && (odi_mock.log[i].addr >> 16) == tbl)
			n++;
	return n;
}

/* Rows of the ordered region where the software table and the chip differ. */
static unsigned int diverged(void)
{
	unsigned int r, n = 0;

	for (r = ODI_SW_CF_SORTED_BASE; r < ODI_SW_CF_BOTTOM + 1U; r++) {
		uint32_t sw0 = odi_sw_cf[r].used ? (ODI_SW_CF_W0_VALID | odi_sw_cf[r].rule_w0) : 0;
		uint32_t sw1 = odi_sw_cf[r].used ? odi_sw_cf[r].rule_w1 : 0;

		if (sw0 != hw_rule[r][0] || sw1 != hw_rule[r][1])
			n++;
	}
	return n;
}

static struct odi_sw_bdgconn_slot *slot_of(int32_t id)
{
	unsigned int s;

	for (s = 0; s < ODI_SW_CMD_BDGCONN_MAX; s++)
		if (odi_sw_bdgconn[s].used && odi_sw_bdgconn[s].serv_id == id)
			return &odi_sw_bdgconn[s];
	return NULL;
}

/* How many valid chip rows hold the rule word of the given software row. */
static unsigned int chip_rows_with(int row)
{
	unsigned int r, n = 0;

	if (row < 0)
		return 0;
	for (r = ODI_SW_CF_SORTED_BASE; r < ODI_SW_CF_BOTTOM + 1U; r++)
		if ((hw_rule[r][0] & ODI_SW_CF_W0_VALID) &&
		    hw_rule[r][0] == (ODI_SW_CF_W0_VALID | odi_sw_cf[row].rule_w0) &&
		    hw_rule[r][1] == odi_sw_cf[row].rule_w1)
			n++;
	return n;
}

int main(void)
{
	struct odi_sw_bdgconn_slot *sl;
	int i, rc, fill_ok = 1;
	int32_t id;

	printf("odi_switch_bdgconn_full_test: a refused cmd 51 and the chip rows\n");
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	hw_sync();

	for (i = 0; i < 95; i++)
		if (activate(100 + i, 100 + (unsigned)i, -1) != 0)
			fill_ok = 0;
	ok(fill_ok, "95 two-way VID services accepted");
	ok(diverged() == 0, "software and chip rows agree after the fill");

	{
		struct omci_vlan_oper vr;
		struct omci_bdgconn b;

		bdg_gen_vid_filter(&vr, 999, 3);
		bdg_conn(&b, 999, OMCI_DIR_BI, 1, 1, 1, &vr);
		rc = odi_switch_cmd(51, &b, sizeof b);
		/* The refusal writes the queued rows and nothing of the
		 * VLAN table: no service changed, and the full sequence would
		 * set all 4096 rows to the sentinel and back. */
		ok(table_writes(ODI_SW_TBL_VLAN_MEMBERS) == 0,
		   "the refusal leaves the VLAN table alone");
		hw_sync();
	}
	ok(rc == ODI_SW_ENOSPC, "a 96th that sorts first is refused (table full)");
	ok(slot_of(999) == NULL, "the refused service holds no slot");
	ok(diverged() == 0, "software and chip rows still agree after the refusal");

	id = 100;
	rc = odi_switch_cmd(50, &id, sizeof id);
	hw_sync();
	ok(rc == 0, "cmd 50 of the first service");
	ok(diverged() == 0, "software and chip rows agree after the cmd 50");

	sl = slot_of(101);
	ok(sl != NULL, "its neighbour is still active");
	ok(sl && chip_rows_with(sl->us_row) == 1, "the neighbour's upstream rule is on the chip once");
	ok(sl && chip_rows_with(sl->ds_row) == 1, "the neighbour's downstream rule is on the chip once");

	if (failures) {
		printf("odi_switch_bdgconn_full_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_switch_bdgconn_full_test: ok\n");
	return 0;
}
