/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/igmp/rows.h"

static struct igmp_rows rows;
static int fail_set, fail_del, sets, dels;
static uint32_t desired, programmed;
static uint8_t key[6] = {1, 0, 0x5e, 1, 1, 1};
int igmp_hw_mac_set(uint16_t vid, const uint8_t *mac, uint32_t ports)
{
	(void)vid; (void)mac;
	sets++;
	if (fail_set) { fail_set--; return -16; }
	programmed = ports;
	return 0;
}
int igmp_hw_mac_del(uint16_t vid, const uint8_t *mac)
{
	(void)vid; (void)mac;
	dels++;
	if (fail_del) { fail_del--; return -16; }
	programmed = 0;
	return 0;
}
static uint32_t wanted(void *ctx, const uint8_t *mac)
{
	(void)ctx;
	return !memcmp(mac, key, 6) ? desired : 0;
}
static int used(void)
{
	int n = 0;
	for (unsigned i = 0; i < IGMP_ROWS_MAX; i++) n += rows.row[i].used;
	return n;
}
int main(void)
{
	assert(!igmp_rows_write(&rows, 100, key, 9));
	fail_del = 1;
	assert(igmp_rows_write(&rows, 100, key, 0));
	assert(used() == 1 && programmed == 9);
	assert(!igmp_rows_retry(&rows, wanted, 0));
	assert(dels == 2 && used() == 0 && !programmed);
	/* A rejoin or aliased live group owns the MAC by retry time. VID
	 * is deliberately different, because SVL uses MAC plus FID 0. */
	assert(!igmp_rows_write(&rows, 100, key, 9));
	fail_del = 1;
	assert(igmp_rows_write(&rows, 100, key, 0));
	desired = 10;
	int before = dels;
	assert(!igmp_rows_retry(&rows, wanted, 0));
	assert(programmed == 10 && dels == before && used() == 1);
	assert(!igmp_rows_write(&rows, 200, key, 12));
	assert(used() == 1);
	/* IPv6 uses the same retry path with a distinct 33:33 MAC. */
	igmp_rows_clear(&rows);
	key[0] = key[1] = 0x33;
	assert(!igmp_rows_write(&rows, 200, key, 9));
	fail_del = 2; desired = 0;
	assert(igmp_rows_write(&rows, 200, key, 0));
	assert(igmp_rows_retry(&rows, wanted, 0));
	assert(used() == 1);
	assert(!igmp_rows_retry(&rows, wanted, 0));
	assert(used() == 0);
	/* A failed add is tracked too and follows the latest membership. */
	fail_set = 1;
	assert(igmp_rows_write(&rows, 200, key, 9));
	desired = 11;
	assert(!igmp_rows_retry(&rows, wanted, 0));
	assert(programmed == 11);
	igmp_rows_clear(&rows);
	/* Bound exhaustion must never send an untracked hardware write. */
	for (unsigned i = 0; i < IGMP_ROWS_MAX; i++) {
		uint8_t mac[6] = {1, 0, 0x5e, 0, (uint8_t)(i >> 8), (uint8_t)i};
		assert(!igmp_rows_write(&rows, 100, mac, 9));
	}
	before = sets;
	assert(igmp_rows_write(&rows, 100, key, 9));
	assert(sets == before && used() == IGMP_ROWS_MAX);
	/* Reconciliation deletes tracked rows even when their change was
	 * omitted from a full change batch. Capacity becomes usable again. */
	desired = 0;
	assert(!igmp_rows_retry(&rows, wanted, 0));
	assert(used() == 0);
	assert(!igmp_rows_write(&rows, 100, key, 9));
	igmp_rows_clear(&rows);
	before = dels;
	assert(!igmp_rows_retry(&rows, wanted, 0) && dels == before);
	puts("ok    multicast MAC delete retry, rejoin, SVL aliasing, IPv6, failed add, capacity, lease clear");
	return 0;
}
