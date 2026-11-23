/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* snapshot_try_resume() against snapshots that carry a valid CRC and state
 * no omcid in order would have saved: a count of upstream queues past the
 * table, table-attribute lists that loop, share a node, run through a free
 * node or are not as long as the row says, a node in use on no list, a row
 * saved twice, and a file a byte short or long. Each is made by putting the
 * memory in that state and letting snapshot_save() write it, so the file is
 * a real one and nothing here knows an offset in it; only the length cases
 * change the saved file, at its end, and make its CRC right. Every one has
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

/* The bitwise CRC32 of snapshot.c, for a file changed here by hand. */
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	uint32_t crc = 0xfffffffful;

	for (uint32_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320ul & (uint32_t)(-(int32_t)(crc & 1)));
	}
	return ~crc;
}

static uint8_t file[131072 + 1];

/* Save, then rewrite the file `delta` bytes shorter or longer before its
 * CRC, the CRC made right again, and ask for it back as back() does. */
static int back_resized(int delta)
{
	const char *why = "";
	long fd, n;
	int rc;

	if (snapshot_save() != 0)
		sys_exit(91);
	fd = sys_open(SNAPSHOT_PATH, O_RDONLY);
	if (fd < 0)
		sys_exit(92);
	n = sys_read((int)fd, file, sizeof file - 1);
	sys_close((int)fd);
	if (n < 8)
		sys_exit(93);
	n -= 4;                                  /* the old CRC */
	if (delta > 0)
		file[n] = 0;
	n += delta;
	nl_put32(file + n, crc32(file, (uint32_t)n));
	if (atomic_write(SNAPSHOT_PATH, SNAPSHOT_TMP_PATH, file, (uint32_t)n + 4) != 0)
		sys_exit(94);
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

	/* A node in use that no list reaches. */
	empty();
	row_with_list(0, 3, 7);
	tblpool[20].used = 1;
	if (back() != 0)
		return 11;

	/* The same class and instance twice: the second row of the file
	 * would overwrite the first and leave its list unreachable. */
	empty();
	row_with_list(0, 3, MIB_TBL_NONE);
	for (int i = 0; i < MIB_ROWS; i++)
		if (!mib[i].used) {
			mib[i] = *mib_find(256, 0);
			mib[i].tbl_head = MIB_TBL_NONE;
			mib[i].tbl_count = 0;
			break;
		}
	if (back() != 0)
		return 12;

	/* One byte short of, and one byte past, the sum of its parts. */
	empty();
	row_with_list(0, 3, 7);
	if (back_resized(-1) != 0)
		return 13;
	empty();
	row_with_list(0, 3, 7);
	if (back_resized(1) != 0)
		return 14;
	/* And unchanged in size, so the rewrite itself is not the refusal. */
	empty();
	row_with_list(0, 3, 7);
	if (back_resized(0) != 1)
		return 15;

	snapshot_invalidate();
	out("ok    a snapshot with a count, a list, a row or a length out of order is refused whole\n");
	return 0;
}
