/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * odi_switch_bdgconn_full_test.c -- a cmd 51 the classifier table refuses
 * must leave the switch as it found it.
 *
 * A two-way rule inserts two legs. cf_insert() moves the rows above the
 * insertion point in the software table at once and only queues the chip
 * writes; they go out in odi_sw_cf_add(). When one leg went in with such a
 * move and the other found no row, the refusal path used to release the
 * slot without writing the queue: the release then deleted the chip row at
 * the moved position, which on the chip still held an older, active
 * service, and from then on every cmd 50 deleted the row next to the one it
 * meant. The refusal now takes the leg back out of the software table and
 * writes nothing.
 *
 * Two refusals, each on a table with one way left in: the upstream leg goes
 * in first and moves every row (a VID and priority rule that sorts first),
 * and the downstream leg goes in and moves the rows below a hole after the
 * upstream leg found the bottom region full (the manual add-tag rule). Each
 * checks that nothing reaches the switch, that the software table is what
 * it was, that it matches the chip, that a later cmd 50 takes only its own
 * rows, and that a neighbour keeps its rule.
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

static int send(int id, uint32_t dir, const struct omci_vlan_oper *vr)
{
	struct omci_bdgconn b;

	bdg_conn(&b, id, dir, 1, 1, 1, vr);
	return odi_switch_cmd(51, &b, sizeof b);
}

static int activate(int id, uint32_t dir, unsigned vid, int pbit)
{
	struct omci_vlan_oper vr;
	int rc;

	bdg_gen_vid_filter(&vr, vid, pbit);
	rc = send(id, dir, &vr);
	hw_sync();
	return rc;
}

static int deactivate(int32_t id)
{
	int rc = odi_switch_cmd(50, &id, sizeof id);

	hw_sync();
	return rc;
}

/* Register and table writes in the log since the last hw_sync(). */
static unsigned int writes(void)
{
	unsigned int i, n = 0;

	odi_mock_table_flush();
	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' || odi_mock.log[i].kind == 'T')
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

/* The software table and the slots, as a refusal must leave them. */
static struct odi_sw_cf_row saved_cf[ODI_SW_CF_ROWS];
static struct odi_sw_bdgconn_slot saved_slots[ODI_SW_CMD_BDGCONN_MAX];

static void save(void)
{
	memcpy(saved_cf, odi_sw_cf, sizeof saved_cf);
	memcpy(saved_slots, odi_sw_bdgconn, sizeof saved_slots);
}

static int unchanged(void)
{
	unsigned int r;

	for (r = 0; r < ODI_SW_CF_ROWS; r++) {
		if (saved_cf[r].used != odi_sw_cf[r].used)
			return 0;
		if (odi_sw_cf[r].used && memcmp(&saved_cf[r], &odi_sw_cf[r], sizeof saved_cf[r]))
			return 0;
	}
	for (r = 0; r < ODI_SW_CMD_BDGCONN_MAX; r++) {
		if (saved_slots[r].used != odi_sw_bdgconn[r].used)
			return 0;
		if (odi_sw_bdgconn[r].used &&
		    memcmp(&saved_slots[r], &odi_sw_bdgconn[r], sizeof saved_slots[r]))
			return 0;
	}
	return 1;
}

/* The checks after a refused cmd 51 of service id: its rc, then the
 * switch, the software table and the chip. */
static void refused(int rc, int32_t id)
{
	ok(writes() == 0, "the refusal writes nothing to the switch");
	hw_sync();
	ok(rc == ODI_SW_ENOSPC, "the rule is refused (table full)");
	ok(slot_of(id) == NULL, "the refused service holds no slot");
	ok(unchanged(), "the software table is what it was before");
	ok(diverged() == 0, "software and chip rows agree");
}

/* A later cmd 50 of `gone` takes its own rows only, and `kept` keeps its. */
static void after(int32_t gone, int32_t kept)
{
	struct odi_sw_bdgconn_slot *sl;

	ok(deactivate(gone) == 0, "cmd 50 of another service");
	ok(diverged() == 0, "software and chip rows agree after the cmd 50");
	sl = slot_of(kept);
	ok(sl != NULL, "its neighbour is still active");
	ok(sl && chip_rows_with(sl->us_row) == 1, "the neighbour's upstream rule is on the chip once");
	ok(sl && chip_rows_with(sl->ds_row) == 1, "the neighbour's downstream rule is on the chip once");
}

/* 95 two-way VID services fill rows 64 to 253 of the ordered region. */
static void fill(void)
{
	int i, fill_ok = 1;

	odi_mock_reset();
	odi_switch_cmd_reset_state();
	memset(hw_rule, 0, sizeof hw_rule);
	hw_sync();
	for (i = 0; i < 95; i++)
		if (activate(100 + i, OMCI_DIR_BI, 100 + (unsigned)i, -1) != 0)
			fill_ok = 0;
	ok(fill_ok, "95 two-way VID services accepted");
	ok(diverged() == 0, "software and chip rows agree after the fill");
}

int main(void)
{
	struct omci_vlan_oper vr;
	int rc;

	printf("odi_switch_bdgconn_full_test: a refused cmd 51 and the chip rows\n");

	puts("the upstream leg goes in and moves every row, the downstream leg finds none:");
	fill();
	save();
	bdg_gen_vid_filter(&vr, 999, 3);
	rc = send(999, OMCI_DIR_BI, &vr);
	refused(rc, 999);
	after(100, 101);

	puts("the upstream leg finds the bottom region full, the downstream leg moves rows:");
	fill();
	/* Row 254 to a one-way service, so no row is left for an upstream
	 * rule that compares no VID; then a hole in the middle. */
	ok(activate(300, OMCI_DIR_DS, 300, -1) == 0, "a downstream-only VID service takes row 254");
	ok(odi_sw_cf[ODI_SW_CF_BOTTOM].used, "row 254 is in use");
	ok(deactivate(150) == 0, "cmd 50 of a service in the middle leaves a hole");
	save();
	bdg_gen_manual(&vr, 999, 0, 0);
	rc = send(999, OMCI_DIR_BI, &vr);
	refused(rc, 999);
	after(100, 101);

	if (failures) {
		printf("odi_switch_bdgconn_full_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("odi_switch_bdgconn_full_test: ok\n");
	return 0;
}
