/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real daemon apply/resync and lease paths. Only the three
 * hardware writes are wrapped by the test link; no device is opened. */
#define main igmpd_unused_main
#include "../main.c"
#undef main
#define CHECK(c, n) do { if (!(c)) return n; } while (0)
static int delete_calls, fail_delete;
static uint32_t programmed;
int __wrap_igmp_hw_mac_set(uint16_t vid, const uint8_t *mac, uint32_t ports)
{
	(void)vid; (void)mac;
	programmed = ports;
	return 0;
}
int __wrap_igmp_hw_mac_del(uint16_t vid, const uint8_t *mac)
{
	(void)vid; (void)mac;
	delete_calls++;
	if (fail_delete) { fail_delete--; return -16; }
	programmed = 0;
	return 0;
}
int __wrap_igmp_hw_snoop(uint32_t flags) { (void)flags; return 0; }
int main(void)
{
	static struct igmp_db db;
	igmp_hw_set_enabled(1);
	db.g[0].in_use = 1; db.g[0].group = 0xe0010101;
	db.g[0].vid = 100; db.g[0].ports = 1;
	CHECK(!row_set(&db, 100, db.g[0].group), 1);
	db.g[0].in_use = 0; fail_delete = 1;
	CHECK(row_set(&db, 100, 0xe0010101), 2);
	rows_dirty = 0; resync(&db);
	CHECK(!programmed && delete_calls == 2 && !rows_dirty, 3);
	/* A group on another VID aliases the same SVL MAC before retry. */
	db.g[0].in_use = 1;
	CHECK(!row_set(&db, 100, 0xe0010101), 4);
	db.g[0].in_use = 0; fail_delete = 1;
	CHECK(row_set(&db, 100, 0xe0010101), 5);
	db.g[1].in_use = 1; db.g[1].group = 0xe1010101;
	db.g[1].vid = 200; db.g[1].ports = 2;
	int before = delete_calls;
	resync(&db);
	CHECK(programmed == (2 | PON_PORTBIT) && delete_calls == before, 6);
	mld_on = 1; db6.g[0].in_use = 1;
	db6.g[0].ports = 1; db6.g[0].vid = 100;
	db6.g[0].group[0] = 0xff; db6.g[0].group[15] = 1;
	CHECK(!row_set6(100, db6.g[0].group), 7);
	db6.g[0].in_use = 0; fail_delete = 1;
	CHECK(row_set6(100, db6.g[0].group), 8);
	before = delete_calls; resync(&db);
	CHECK(delete_calls == before + 1, 9);
	/* Overflow must schedule a full retry, including vanished IPv6 MACs. */
	db6.g[0].in_use = 1;
	CHECK(!row_set6(100, db6.g[0].group), 10);
	db6.g[0].in_use = 0; rows_dirty = 0; before = delete_calls;
	apply6(0, 2, 1);
	CHECK(rows_dirty, 11);
	resync(&db);
	CHECK(delete_calls > before, 12);
	/* Acquiring a lease must not forget a pending failed deletion. */
	db.g[1].in_use = 0; db.g[0].in_use = 1;
	CHECK(!row_set(&db, 100, 0xe0010101), 13);
	db.g[0].in_use = 0; fail_delete = 1;
	CHECK(row_set(&db, 100, 0xe0010101), 14);
	lease = 0; lease_retry = 0; before = delete_calls;
	settle_lease(&db, ODI_SW_MC_SNOOP_COPY, 100);
	CHECK(delete_calls == before + 1 && lease == ODI_SW_MC_SNOOP_COPY, 15);
	out("ok    daemon delete retry, MAC alias rejoin, IPv6 overflow and lease acquisition\n");
	return 0;
}
