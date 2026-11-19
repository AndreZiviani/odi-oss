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
static int fail_snoop;
int __wrap_igmp_hw_snoop(uint32_t flags)
{
	(void)flags;
	if (fail_snoop) { fail_snoop--; return -16; }
	return 0;
}
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
	/* A lease change that fails starts the ten-second back-off; "not
	 * snooping" inside it still releases the lease at once. */
	lease = ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_PON_DROP;
	fail_snoop = 1;
	settle_lease(&db, ODI_SW_MC_SNOOP_COPY, 200);
	CHECK(lease == (ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_PON_DROP) &&
	      lease_retry == 210, 16);
	settle_lease(&db, ODI_SW_MC_SNOOP_COPY, 205);
	CHECK(lease == (ODI_SW_MC_SNOOP_COPY | ODI_SW_MC_SNOOP_PON_DROP), 17);
	settle_lease(&db, 0, 203);
	CHECK(lease == 0, 18);
	/* The log budget: 4096 bytes a second, the rest counted, and a new
	 * second starts again after one line on what was held back (it
	 * prints "log limited: 1097 bytes not shown" here, 34 bytes, which
	 * the new second pays for). */
	log_now = log_sec = 7;
	for (int i = 0; i < 50; i++)
		(void)log_admit(100);
	CHECK(log_used == 4000 && log_held == 1000, 19);
	CHECK(!log_admit(97) && log_admit(96) && log_held == 1097, 20);
	log_now = 8;
	log_roll();
	CHECK(log_sec == 8 && log_used == 34 && log_held == 0, 21);
	CHECK(!log_admit(4063) && log_admit(4062) && log_used == 4096, 22);
	/* A second with nothing held back starts with the whole budget. */
	log_held = 0;
	log_now = 9;
	log_roll();
	CHECK(log_used == 0 && log_admit(4096), 23);
	out("ok    daemon delete retry, MAC alias rejoin, IPv6 overflow, lease acquisition and release in the back-off, log budget\n");
	return 0;
}
