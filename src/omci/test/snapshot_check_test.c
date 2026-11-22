/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* snapshot_try_resume() against snapshots that carry a valid CRC and state
 * no omcid in order would have saved: a count of upstream queues past the
 * table, and table-attribute lists that loop, share a node, run through a
 * free node or are not as long as the row says. Each is made by putting
 * the memory in that state and letting snapshot_save() write it, so the
 * file is a real one and nothing here knows an offset in it. Every one has
 * to be refused as bad_snapshot before any of it is loaded; the same row
 * with its list in order has to resume.
 *
 * The snapshot is written to SNAPSHOT_PATH, which the Makefile points at
 * /tmp for this test so it runs as an ordinary user, and removed at the
 * end. No device or netlink is opened. */
#include "omcid.h"

int __wrap_omci_drv_call(uint32_t cmd, void *buf, uint32_t len)
{
	(void)cmd; (void)buf; (void)len;
	return 0;
}

static int same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

/* An empty MIB, an empty pool, no queue programmed. */
static void empty(void)
{
	uint32_t qlen;
	uint8_t *q = qos_prog_state(&qlen);

	for (int i = 0; i < MIB_ROWS; i++)
		mib[i].used = 0;
	for (int i = 0; i < MIB_TBL_NODES; i++) {
		tblpool[i].used = 0;
		tblpool[i].next = 0;
	}
	for (uint32_t i = 0; i < qlen; i++)
		q[i] = 0;
}

/* A row with the list head -> next -> end, both nodes in use. */
static struct mib_row *row_with_list(uint16_t inst, uint8_t head, uint8_t next)
{
	struct mib_row *r = mib_add(256, inst);

	if (!r)
		sys_exit(90);
	tblpool[head].used = 1;
	tblpool[head].next = next;
	r->tbl_head = head;
	r->tbl_count = 1;
	if (next != MIB_TBL_NONE) {
		tblpool[next].used = 1;
		tblpool[next].next = MIB_TBL_NONE;
		r->tbl_count = 2;
	}
	return r;
}

/* Save what is in memory, forget it, and ask for it back. 1: resumed,
 * 0: refused as bad_snapshot, -1: anything else. */
static int back(void)
{
	const char *why = "";
	int rc;

	if (snapshot_save() != 0)
		sys_exit(91);
	empty();
	rc = snapshot_try_resume(5, &why);
	if (rc)
		return same(why, "resumed") ? 1 : -1;
	return same(why, "bad_snapshot") ? 0 : -1;
}

int main(void)
{
	uint32_t qlen;
	uint8_t *q = qos_prog_state(&qlen);
	struct mib_row *r;

	/* In order: resumed, and the list is there again. */
	empty();
	row_with_list(0, 3, 7);
	if (back() != 1)
		return 1;
	r = mib_find(256, 0);
	if (!r || r->tbl_head != 3 || r->tbl_count != 2 ||
	    tblpool[3].next != 7 || tblpool[7].next != MIB_TBL_NONE)
		return 2;

	/* Every byte of the upstream QoS state 0xff: the queue count with
	 * them, wherever in the state it lies. */
	empty();
	row_with_list(0, 3, 7);
	for (uint32_t i = 0; i < qlen; i++)
		q[i] = 0xff;
	if (back() != 0)
		return 3;
	/* Refused before anything was loaded. */
	if (mib_find(256, 0))
		return 4;

	/* A list that ends in itself. */
	empty();
	row_with_list(0, 3, 3);
	tblpool[3].next = 3;
	if (back() != 0)
		return 5;

	/* A loop further in: 3 -> 7 -> 3. */
	empty();
	row_with_list(0, 3, 7);
	tblpool[7].next = 3;
	if (back() != 0)
		return 6;

	/* A node on the lists of two rows. */
	empty();
	row_with_list(0, 3, 7);
	row_with_list(1, 9, 7);
	if (back() != 0)
		return 7;

	/* A free node on a list: its next is whatever was left there. */
	empty();
	row_with_list(0, 3, 7);
	tblpool[7].used = 0;
	if (back() != 0)
		return 8;

	/* A list shorter, and longer, than the row counts. */
	empty();
	r = row_with_list(0, 3, 7);
	r->tbl_count = 3;
	if (back() != 0)
		return 9;
	empty();
	r = row_with_list(0, 3, 7);
	r->tbl_count = 1;
	if (back() != 0)
		return 10;

	snapshot_invalidate();
	out("ok    a snapshot with a queue count or a table list out of order is refused whole\n");
	return 0;
}
