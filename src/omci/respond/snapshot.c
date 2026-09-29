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
 * untouched because this never touches it.
 *
 * Written atomically (temp file + rename, cfgstore.c's own pattern) after
 * every MIB-changing message (Create, Set, Delete); mib_reset_all() deletes
 * it instead, so a MIB reset is never followed by a resume into a MIB the
 * OLT just discarded. Compact: the two pools sized for the worst case (192
 * rows, 256 services) are written only as the entries actually in use, not
 * the fixed arrays -- ISP1's few dozen rows and half a dozen services are a
 * few KB, not the tens of KB the fixed pools would cost.
 */
#include "omcid.h"

#define SNAP_MAGIC0 'O'
#define SNAP_MAGIC1 'M'
#define SNAP_MAGIC2 'S'
#define SNAP_MAGIC3 '1'
#define SNAP_VERSION 1

/* Header (26 bytes: magic 4, version 2, devid 8, serial 9, sync 1, alarm 2)
 * + one row's worst case (2+2+2+1+1+1+64 = 73) times every row, + the fixed
 * pools (tblpool, flow_us, flow_ds, bc_flow, tcont_map) + the service table
 * worst case (256 * (2 + 160)) + the trailing crc32 (4). 192 rows and 256
 * services both in use at once never happens on this MIB (ISP1 holds a few
 * dozen rows and a handful of services), but the buffer is sized for it so a
 * pathological session degrades to "truncated, next save catches up" rather
 * than overrunning.
 */
#define SNAP_BUF_MAX 65536

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
static int atomic_write(const char *path, const char *tmp, const uint8_t *buf,
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

void snapshot_save(void)
{
	uint8_t *p = snapbuf;
	uint8_t *rows_at, *servs_at, *crc_at;
	uint16_t rows = 0, servs = 0;
	/* Leave room for the fixed pools and the crc after the last row or
	 * service entry is appended; a save that would not fit is skipped
	 * rather than truncated silently -- the next MIB-changing message
	 * tries again, and until then the last good snapshot on disk stands. */
	const uint32_t fixed_tail = (uint32_t)sizeof(tblpool) +
		(uint32_t)sizeof(flow_us) + (uint32_t)sizeof(flow_ds) +
		4 /* bc_flow */ + (uint32_t)sizeof(tcont_map) +
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

	rows_at = p;
	put_u16(&p, 0);                          /* row_count, patched below */
	for (int i = 0; i < MIB_ROWS; i++) {
		uint32_t row_bytes = 2 + 2 + 2 + 1 + 1 + 1 + MIB_ROW_MAX;

		if (!mib[i].used)
			continue;
		if ((uint32_t)(p - snapbuf) + row_bytes + fixed_tail > SNAP_BUF_MAX)
			break;
		put_u16(&p, mib[i].classId);
		put_u16(&p, mib[i].inst);
		put_u16(&p, mib[i].written);
		put_u8(&p, mib[i].truncated);
		put_u8(&p, mib[i].tbl_head);
		put_u8(&p, mib[i].tbl_count);
		put_raw(&p, mib[i].data, MIB_ROW_MAX);
		rows++;
	}
	put_u16(&rows_at, rows);

	put_raw(&p, tblpool, sizeof tblpool);
	put_raw(&p, flow_us, sizeof flow_us);
	put_raw(&p, flow_ds, sizeof flow_ds);
	put_u32(&p, (uint32_t)bc_flow);
	put_raw(&p, tcont_map, sizeof tcont_map);

	servs_at = p;
	put_u16(&p, 0);
	for (int i = 0; i < SERV_MAX; i++) {
		uint32_t ent_bytes = 2 + (uint32_t)sizeof(struct omci_bdgconn);

		if (!servtab[i].in_use)
			continue;
		if ((uint32_t)(p - snapbuf) + ent_bytes + 4 > SNAP_BUF_MAX)
			break;
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

	atomic_write(SNAPSHOT_PATH, SNAPSHOT_TMP_PATH, snapbuf,
		     (uint32_t)(p - snapbuf));
}

/* Why a resume was refused, for omcid's event=start line (events.c). */
static int refuse(const char **why, const char *reason)
{
	*why = reason;
	return 0;
}

int snapshot_try_resume(uint32_t onu_state, const char **why)
{
	static uint8_t buf[SNAP_BUF_MAX];
	const uint8_t *p;
	long fd, n;
	uint16_t rows, servs;

	if (onu_state != 5)
		return refuse(why, "not_o5");
	fd = sys_open(SNAPSHOT_PATH, O_RDONLY);
	if (fd < 0)
		return refuse(why, "no_snapshot");
	n = sys_read((int)fd, buf, sizeof buf);
	sys_close((int)fd);
	/* Header plus at least the fixed tail and its own crc; anything
	 * shorter is not a snapshot this code wrote. */
	if (n < 26 + 2 + (long)sizeof(tblpool) + (long)sizeof(flow_us) +
		(long)sizeof(flow_ds) + 4 + (long)sizeof(tcont_map) + 2 + 4)
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

	{
		uint8_t sync = get_u8(&p);
		uint16_t alarm = get_u16(&p);

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
			struct mib_row *r = mib_add(cls, inst);

			if (!r) {                /* cannot happen with a
						   * snapshot this process
						   * itself wrote */
				p += MIB_ROW_MAX;
				continue;
			}
			r->written = written;
			r->truncated = truncated;
			r->tbl_head = tbl_head;
			r->tbl_count = tbl_count;
			get_raw(&p, r->data, MIB_ROW_MAX);
			for (int j = 0; j < MIB_ROW_MAX; j++)
				r->prev[j] = r->data[j];
		}

		get_raw(&p, tblpool, sizeof tblpool);
		get_raw(&p, flow_us, sizeof flow_us);
		get_raw(&p, flow_ds, sizeof flow_ds);
		bc_flow = (int)get_u32(&p);
		get_raw(&p, tcont_map, sizeof tcont_map);

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
