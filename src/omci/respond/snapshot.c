/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* omcid's own MIB state, snapshotted to tmpfs so a respawned process can
 * resume answering the OLT without re-registration. See omcid.h and
 * docs/BOOT.md, "Resume without re-registration".
 *
 * What is saved is exactly what mib_reset_all() throws away on a MIB reset:
 * the rows, the table-attribute pool, the flow and T-CONT bookkeeping, and
 * the service table -- the state that maps a managed entity to switch
 * programming already sitting in hardware. Restoring it is a plain memory
 * copy: nothing here calls apply_entity() or any driver function, so a
 * resume issues no switch-programming driver call at all -- the datapath is
 * untouched because this never touches it. The one thing it can start is a
 * rebuild that was pending when the process was killed (SNAP_PENDING_*,
 * below): the flags come back with the MIB, and the main loop runs it.
 *
 * Written atomically (temp file + rename, cfgstore.c's own pattern) after
 * every MIB-changing message (Create, Set, Delete); mib_reset_all() deletes
 * it instead, so a MIB reset is never followed by a resume into a MIB the
 * OLT just discarded. Compact: the two pools sized for the worst case (320
 * rows, 256 services) are written only as the entries actually in use, not
 * the fixed arrays -- ISP1's few dozen rows and half a dozen services are a
 * few KB, not the tens of KB the fixed pools would cost.
 */
#include "omcid.h"

#define SNAP_MAGIC0 'O'
#define SNAP_MAGIC1 'M'
#define SNAP_MAGIC2 'S'
#define SNAP_MAGIC3 '1'
#define SNAP_VERSION 7   /* 2: the upstream state the driver holds (qos_prog_state()) and table attribute storage; 3: both; 4: rows of 96 bytes; 5: a wide row also carries the bytes past 96 (side pool); 6: a table pool of 255 rows; 7: the rebuilds pending (SNAP_PENDING_*) */

/* The rebuilds a Create, Set or Delete marked and the main loop has not run
 * yet (conn_dirty, qos_dirty, qos_full): the MIB in the snapshot is ahead of
 * the switch by these. A resume sets the flags again, so the pending
 * rebuild runs after the first quiet second; with none pending it still
 * touches nothing. */
#define SNAP_PENDING_CONN     0x01
#define SNAP_PENDING_QOS      0x02
#define SNAP_PENDING_QOS_FULL 0x04

/* Header (27 bytes: magic 4, version 2, devid 8, serial 9, sync 1, alarm 2,
 * rebuilds pending 1)
 * + one row's worst case (2+2+2+1+1+1+1+96 = 106) times every row (320:
 * 33920), + the bytes past 96 of every wide row (32 * 288: 9216), + the
 * fixed pools (tblpool 255 * 35, flow_us, flow_ds, bc_flow, tcont_map, the
 * upstream state) + the service table worst case (256 * (2 + 160)) + the
 * trailing crc32 (4): close to 100 KB. Every row and 256 services in use at
 * once never happens on this MIB (ISP1 holds 185 rows and a handful of
 * services), but the buffer is sized for it so a pathological session
 * degrades to "truncated, next save catches up" rather than overrunning.
 *
 * One buffer serves both the save and the resume. They cannot overlap:
 * the resume runs once, from main() before the main loop, and nothing it
 * calls saves; every save comes later, from the loop (a Create, Set or
 * Delete, a rebuild, an alarm, a reload). And the resume keeps nothing that
 * points into the buffer -- it copies every row, pool and service out --
 * so the first save may overwrite it. Two buffers cost 128 KB of RAM for
 * nothing.
 */
#define SNAP_BUF_MAX 131072

static uint8_t snapbuf[SNAP_BUF_MAX];

static void put_raw(uint8_t **p, const void *src, uint32_t n)
{
	const uint8_t *s = src;

	for (uint32_t i = 0; i < n; i++)
		(*p)[i] = s[i];
	*p += n;
}

static void get_raw(const uint8_t **p, void *dst, uint32_t n)
{
	uint8_t *d = dst;

	for (uint32_t i = 0; i < n; i++)
		d[i] = (*p)[i];
	*p += n;
}

static void put_u8(uint8_t **p, uint8_t v)
{
	**p = v;
	*p += 1;
}

static void put_u16(uint8_t **p, uint16_t v)
{
	nl_put16(*p, v);
	*p += 2;
}

static void put_u32(uint8_t **p, uint32_t v)
{
	nl_put32(*p, v);
	*p += 4;
}

static uint8_t get_u8(const uint8_t **p)
{
	uint8_t v = **p;

	*p += 1;
	return v;
}

static uint16_t get_u16(const uint8_t **p)
{
	uint16_t v = nl_get16(*p);

	*p += 2;
	return v;
}

static uint32_t get_u32(const uint8_t **p)
{
	uint32_t v = nl_get32(*p);

	*p += 4;
	return v;
}

/* Bitwise CRC32 (poly 0xEDB88320, no table): this runs once per MIB-changing
 * message over a few KB, not per frame, and a 256-entry table is not worth
 * the bss in a freestanding image that has no libc one to borrow. */
static uint32_t crc32_calc(const uint8_t *p, uint32_t n)
{
	uint32_t crc = 0xfffffffful;

	for (uint32_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++) {
			uint32_t mask = (uint32_t)(-(int32_t)(crc & 1));

			crc = (crc >> 1) ^ (0xEDB88320ul & mask);
		}
	}
	return ~crc;
}

/* Same shape as cfgstore.c's own write: a temp file in the same directory,
 * written in full or not at all, then renamed over the target -- a reader
 * (the next boot's resume attempt) never sees a half-written snapshot. */
int atomic_write(const char *path, const char *tmp, const uint8_t *buf,
			uint32_t n)
{
	long fd = sys_create(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0)
		return -1;
	for (uint32_t done = 0; done < n; ) {
		long w = sys_write((int)fd, buf + done, n - done);

		if (w <= 0) {
			sys_close((int)fd);
			sys_unlink(tmp);
			return -1;
		}
		done += (uint32_t)w;
	}
	sys_close((int)fd);
	if (sys_rename(tmp, path) < 0) {
		sys_unlink(tmp);
		return -1;
	}
	return 0;
}

void snapshot_invalidate(void)
{
	sys_unlink(SNAPSHOT_PATH);
	sys_unlink(SNAPSHOT_TMP_PATH);
}

/* 0 when the snapshot is on disk, -1 when it could not be written. The old
 * snapshot is removed then, and event=snapshot_fail says so: it holds a MIB
 * older than the one the OLT was just answered for, and a respawn that
 * resumed from it would answer for that older MIB. With no snapshot the
 * respawn registers afresh, and the OLT provisions the MIB again. */
int snapshot_save(void)
{
	uint8_t *p = snapbuf;
	uint8_t *rows_at, *servs_at, *crc_at;
	uint16_t rows = 0, servs = 0;
	uint32_t qlen;
	void *qstate = qos_prog_state(&qlen);
	/* Leave room for the fixed pools and the crc after the last row or
	 * service entry is appended. A save that would not fit fails as a
	 * whole rather than writing a snapshot without the rows or services
	 * past the end: SNAP_BUF_MAX holds the worst case, so this is a
	 * backstop, not a path a session takes. */
	const uint32_t fixed_tail = (uint32_t)sizeof(tblpool) +
		(uint32_t)sizeof(flow_us) + (uint32_t)sizeof(flow_ds) +
		4 /* bc_flow */ + (uint32_t)sizeof(tcont_map) + qlen +
		2 /* serv_count */ + 4 /* crc */;

	put_u8(&p, SNAP_MAGIC0);
	put_u8(&p, SNAP_MAGIC1);
	put_u8(&p, SNAP_MAGIC2);
	put_u8(&p, SNAP_MAGIC3);
	put_u16(&p, SNAP_VERSION);
	put_raw(&p, devid, 8);
	put_raw(&p, serial, 9);
	put_u8(&p, mib_data_sync);
	put_u16(&p, alarm_snapshot);
	put_u8(&p, (uint8_t)((conn_dirty ? SNAP_PENDING_CONN : 0) |
			     (qos_dirty ? SNAP_PENDING_QOS : 0) |
			     (qos_full ? SNAP_PENDING_QOS_FULL : 0)));

	rows_at = p;
	put_u16(&p, 0);                          /* row_count, patched below */
	for (int i = 0; i < MIB_ROWS; i++) {
		uint32_t row_bytes = 2 + 2 + 2 + 1 + 1 + 1 + 1 + MIB_ROW_MAX +
			(mib[i].tail ? MIB_WIDE_MAX - MIB_ROW_MAX : 0);

		if (!mib[i].used)
			continue;
		if ((uint32_t)(p - snapbuf) + row_bytes + fixed_tail > SNAP_BUF_MAX)
			goto fail;
		put_u16(&p, mib[i].classId);
		put_u16(&p, mib[i].inst);
		put_u16(&p, mib[i].written);
		put_u8(&p, mib[i].truncated);
		put_u8(&p, mib[i].tbl_head);
		put_u8(&p, mib[i].tbl_count);
		put_u8(&p, mib[i].tail != 0);    /* the bytes past 96 follow */
		put_raw(&p, mib[i].data, MIB_ROW_MAX);
		if (mib[i].tail)
			put_raw(&p, mib[i].tail, MIB_WIDE_MAX - MIB_ROW_MAX);
		rows++;
	}
	put_u16(&rows_at, rows);

	put_raw(&p, tblpool, sizeof tblpool);
	put_raw(&p, flow_us, sizeof flow_us);
	put_raw(&p, flow_ds, sizeof flow_ds);
	put_u32(&p, (uint32_t)bc_flow);
	put_raw(&p, tcont_map, sizeof tcont_map);
	put_raw(&p, qstate, qlen);

	servs_at = p;
	put_u16(&p, 0);
	for (int i = 0; i < SERV_MAX; i++) {
		uint32_t ent_bytes = 2 + (uint32_t)sizeof(struct omci_bdgconn);

		if (!servtab[i].in_use)
			continue;
		if ((uint32_t)(p - snapbuf) + ent_bytes + 4 > SNAP_BUF_MAX)
			goto fail;
		put_u16(&p, (uint16_t)i);
		put_raw(&p, &servtab[i], sizeof(struct omci_bdgconn));
		servs++;
	}
	put_u16(&servs_at, servs);

	crc_at = p;
	put_u32(&p, 0);                          /* placeholder, patched below */
	{
		uint32_t total = (uint32_t)(p - snapbuf);
		uint32_t crc = crc32_calc(snapbuf, total - 4);

		put_u32(&crc_at, crc);
	}

	if (atomic_write(SNAPSHOT_PATH, SNAPSHOT_TMP_PATH, snapbuf,
			 (uint32_t)(p - snapbuf)) == 0)
		return 0;
fail:
	snapshot_invalidate();
	ev_snapshot_fail((unsigned)mib_count());
	return -1;
}

/* Why a resume was refused, for omcid's event=start line (events.c). */
static int refuse(const char **why, const char *reason)
{
	*why = reason;
	return 0;
}

/* The rest of a snapshot, walked before any of it is loaded: every count
 * within its table, every part inside the bytes that were read and the file
 * exactly as long as its parts, every row and every service once, and the
 * table-attribute lists whole. The CRC says the file is what some omcid
 * wrote, not that its memory was in order when it did. A count taken on
 * trust becomes an index later (the queue count, in us_qos_rebuild()), and
 * a list that runs into itself is walked for ever by the first thing that
 * walks it (tbl_free() at a MIB reset, the Get of a table attribute that
 * primes its Get Next): every walker stops at MIB_TBL_NONE and at nothing
 * else. A node in use that no list reaches is never freed, since tbl_free()
 * only walks lists, and a pool full of them refuses every table row until
 * omcid starts afresh. Checked here and not where each part is loaded,
 * because a refusal halfway through the load leaves half a MIB behind for
 * the registration that follows. `p` is behind the device identity, `end`
 * at the CRC. */
static int snap_whole(const uint8_t *p, const uint8_t *end, uint32_t qlen)
{
	long fixed = (long)(sizeof(tblpool) + sizeof(flow_us) + sizeof(flow_ds) +
			    4 + sizeof(tcont_map) + qlen + 2);
	const struct mib_tblent *pool;           /* bytes only: no alignment */
	static const uint8_t *row[MIB_ROWS];     /* static: 1280 bytes */
	uint8_t seen[(MIB_TBL_NODES + 7) / 8];
	uint8_t slot[SERV_MAX / 8];
	uint16_t rows, servs;
	unsigned wide = 0;

	if (end - p < 6)
		return 0;
	p += 4;                                  /* sync, alarm, pending */
	rows = get_u16(&p);
	if (rows > MIB_ROWS)
		return 0;
	for (uint16_t i = 0; i < rows; i++) {
		long data = MIB_ROW_MAX;

		if (end - p < 10)
			return 0;
		row[i] = p;
		/* One row per class and instance: mib_add() hands a second
		 * one the row of the first, which it overwrites, and the
		 * list of the first is in use with no row to reach it. */
		for (uint16_t j = 0; j < i; j++)
			if (row[j][0] == p[0] && row[j][1] == p[1] &&
			    row[j][2] == p[2] && row[j][3] == p[3])
				return 0;
		p += 9;
		if (get_u8(&p)) {                /* has_tail */
			data = MIB_WIDE_MAX;
			/* No more rows with a slot than the pool has:
			 * the resume gives one to each that says so. */
			if (++wide > MIB_WIDE_ROWS)
				return 0;
		}
		if (end - p < data)
			return 0;
		p += data;
	}
	if (end - p < fixed)
		return 0;
	pool = (const struct mib_tblent *)p;
	p += fixed - (long)qlen - 2;
	if (!qos_prog_state_ok(p))
		return 0;
	p += qlen;
	servs = get_u16(&p);
	if (servs > SERV_MAX)
		return 0;
	if (end - p != (long)servs * (long)(2 + sizeof(struct omci_bdgconn)))
		return 0;
	/* Each service at a slot of its own within the table: the loader
	 * skips one outside it and lets a second one overwrite the first, so
	 * a service of the file would be lost without a word. */
	for (unsigned i = 0; i < sizeof slot; i++)
		slot[i] = 0;
	for (uint16_t i = 0; i < servs; i++) {
		uint16_t idx = get_u16(&p);

		if (idx >= SERV_MAX || (slot[idx >> 3] & (1u << (idx & 7))))
			return 0;
		slot[idx >> 3] |= (uint8_t)(1u << (idx & 7));
		p += sizeof(struct omci_bdgconn);
	}

	/* The list of each row: nodes in use, none of them twice, in this
	 * list or in the list of another row, and as many as the row counts.
	 * A node is marked when it is first met, so a walk ends after at most
	 * MIB_TBL_NODES steps whatever the links say; an index is a byte and
	 * 0xff is the end, so none is outside the pool. */
	for (unsigned i = 0; i < sizeof seen; i++)
		seen[i] = 0;
	for (uint16_t i = 0; i < rows; i++) {
		const uint8_t *q = row[i];
		unsigned len = 0;

		for (uint8_t t = q[7]; t != MIB_TBL_NONE; t = pool[t].next) {
			if (!pool[t].used || (seen[t >> 3] & (1u << (t & 7))))
				return 0;
			seen[t >> 3] |= (uint8_t)(1u << (t & 7));
			len++;
		}
		if (len != q[8])                 /* tbl_count */
			return 0;
	}
	/* And every node in use on a list. */
	for (unsigned t = 0; t < MIB_TBL_NODES; t++)
		if (pool[t].used && !(seen[t >> 3] & (1u << (t & 7))))
			return 0;
	return 1;
}

int snapshot_try_resume(uint32_t onu_state, const char **why)
{
	uint8_t *buf = snapbuf;                  /* see SNAP_BUF_MAX */
	const uint8_t *p;
	long fd, n;
	uint16_t rows, servs;
	uint32_t qlen;
	void *qstate = qos_prog_state(&qlen);

	if (onu_state != 5)
		return refuse(why, "not_o5");
	fd = sys_open(SNAPSHOT_PATH, O_RDONLY);
	if (fd < 0)
		return refuse(why, "no_snapshot");
	n = sys_read((int)fd, buf, SNAP_BUF_MAX);
	sys_close((int)fd);
	/* Header plus at least the fixed tail and its own crc; anything
	 * shorter is not a snapshot this code wrote. */
	if (n < 27 + 2 + (long)sizeof(tblpool) + (long)sizeof(flow_us) +
		(long)sizeof(flow_ds) + 4 + (long)sizeof(tcont_map) + (long)qlen + 2 + 4)
		return refuse(why, "bad_snapshot");

	p = buf;
	if (p[0] != SNAP_MAGIC0 || p[1] != SNAP_MAGIC1 ||
	    p[2] != SNAP_MAGIC2 || p[3] != SNAP_MAGIC3)
		return refuse(why, "bad_snapshot");
	p += 4;
	if (get_u16(&p) != SNAP_VERSION)
		return refuse(why, "bad_snapshot");

	{
		uint32_t stored = nl_get32(buf + n - 4);
		uint32_t calc = crc32_calc(buf, (uint32_t)n - 4);

		if (stored != calc)
			return refuse(why, "bad_snapshot");
	}

	{
		uint8_t snap_devid[8], snap_serial[9];

		get_raw(&p, snap_devid, 8);
		get_raw(&p, snap_serial, 9);
		for (int i = 0; i < 8; i++)
			if (snap_devid[i] != devid[i])
				return refuse(why, "other_device");
		for (int i = 0; i < 9; i++)
			if (snap_serial[i] != serial[i])
				return refuse(why, "other_device");
	}

	if (!snap_whole(p, buf + n - 4, qlen))
		return refuse(why, "bad_snapshot");

	{
		uint8_t sync = get_u8(&p);
		uint16_t alarm = get_u16(&p);
		uint8_t pending = get_u8(&p);

		/* snap_whole() has refused a count past its table; the two
		 * checks in this block are what it replaced and cost nothing. */
		rows = get_u16(&p);
		if (rows > MIB_ROWS)
			return refuse(why, "bad_snapshot");
		for (int i = 0; i < MIB_ROWS; i++)
			mib[i].used = 0;
		for (uint16_t i = 0; i < rows; i++) {
			uint16_t cls = get_u16(&p);
			uint16_t inst = get_u16(&p);
			uint16_t written = get_u16(&p);
			uint8_t truncated = get_u8(&p);
			uint8_t tbl_head = get_u8(&p);
			uint8_t tbl_count = get_u8(&p);
			uint8_t has_tail = get_u8(&p);
			struct mib_row *r = mib_add(cls, inst);

			if (!r) {                /* cannot happen with a
						   * snapshot this process
						   * itself wrote */
				p += MIB_ROW_MAX;
				if (has_tail)
					p += MIB_WIDE_MAX - MIB_ROW_MAX;
				continue;
			}
			/* The slot goes back to the rows the file says had one.
			 * mib_add() gave this row a slot if one was free,
			 * whether or not it had one; a row that had none gives
			 * it back at once, so it is free for a later row that
			 * had one. Left in place, the first 32 wide rows in
			 * index order took the slots, and a row created behind
			 * one that never had a slot lost its own: its Sets
			 * past byte 96 failed after the respawn. */
			if (!has_tail)
				r->tail = 0;
			r->written = written;
			r->truncated = truncated;
			r->tbl_head = tbl_head;
			r->tbl_count = tbl_count;
			get_raw(&p, r->data, MIB_ROW_MAX);
			for (int j = 0; j < MIB_ROW_MAX; j++)
				r->prev[j] = r->data[j];
			/* A row the file says had a slot gets one: at most 32
			 * rows say so, and every row without one gave its slot
			 * back above. Should the pool still be full, the bytes
			 * are dropped, as a Set of them would have been. */
			if (has_tail) {
				if (r->tail)
					get_raw(&p, r->tail,
						MIB_WIDE_MAX - MIB_ROW_MAX);
				else
					p += MIB_WIDE_MAX - MIB_ROW_MAX;
			}
		}

		get_raw(&p, tblpool, sizeof tblpool);
		get_raw(&p, flow_us, sizeof flow_us);
		get_raw(&p, flow_ds, sizeof flow_ds);
		bc_flow = (int)get_u32(&p);
		get_raw(&p, tcont_map, sizeof tcont_map);
		get_raw(&p, qstate, qlen);

		servs = get_u16(&p);
		if (servs > SERV_MAX)
			return refuse(why, "bad_snapshot");
		for (int i = 0; i < SERV_MAX; i++)
			servtab[i].in_use = 0;
		for (uint16_t i = 0; i < servs; i++) {
			uint16_t idx = get_u16(&p);

			if (idx < SERV_MAX)
				get_raw(&p, &servtab[idx],
					sizeof(struct omci_bdgconn));
			else
				p += sizeof(struct omci_bdgconn);
		}

		mib_data_sync = sync;
		alarm_snapshot = alarm;
		conn_dirty = (pending & SNAP_PENDING_CONN) != 0;
		qos_dirty = (pending & SNAP_PENDING_QOS) != 0;
		qos_full = (pending & SNAP_PENDING_QOS_FULL) != 0;
	}
	*why = "resumed";
	return 1;
}

void snapshot_write_decision(int resumed)
{
	static const char R[] = "resumed\n";
	static const char F[] = "reprovision\n";
	const char *tmp = RESUME_DECISION_PATH ".tmp";

	if (resumed)
		atomic_write(RESUME_DECISION_PATH, tmp,
			     (const uint8_t *)R, sizeof R - 1);
	else
		atomic_write(RESUME_DECISION_PATH, tmp,
			     (const uint8_t *)F, sizeof F - 1);
}
