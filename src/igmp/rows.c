/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "rows.h"
#include "program.h"

static int same_mac(const uint8_t *a, const uint8_t *b)
{
	for (unsigned i = 0; i < 6; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

void igmp_rows_clear(struct igmp_rows *rows)
{
	for (unsigned i = 0; i < IGMP_ROWS_MAX; i++)
		rows->row[i].used = rows->row[i].dirty = 0;
}

int igmp_rows_write(struct igmp_rows *rows, uint16_t vid,
		    const uint8_t mac[6], uint32_t ports)
{
	struct igmp_row *r = 0, *free = 0;
	int rc;

	/* The hardware key is MAC plus FID 0, so VID must not split owners.
	 * Reserve tracking BEFORE any hardware write. A full table defers
	 * new writes rather than losing the identity of a failed deletion. */
	for (unsigned i = 0; i < IGMP_ROWS_MAX; i++) {
		if (rows->row[i].used && same_mac(rows->row[i].mac, mac)) {
			r = &rows->row[i];
			break;
		}
		if (!rows->row[i].used && !free)
			free = &rows->row[i];
	}
	if (!r)
		r = free;
	if (!r)
		return -1;
	r->used = 1;
	r->vid = vid;
	for (unsigned i = 0; i < 6; i++)
		r->mac[i] = mac[i];
	rc = ports ? igmp_hw_mac_set(vid, mac, ports) : igmp_hw_mac_del(vid, mac);
	r->dirty = rc != 0;
	if (!rc && !ports)
		r->used = 0;
	return rc;
}

int igmp_rows_retry(struct igmp_rows *rows,
		    uint32_t (*wanted)(void *, const uint8_t *), void *ctx)
{
	int failed = 0;

	for (unsigned i = 0; i < IGMP_ROWS_MAX; i++) {
		struct igmp_row *r = &rows->row[i];
		uint32_t ports;

		if (!r->used)
			continue;
		ports = wanted(ctx, r->mac);
		/* Also reconcile rows absent from an overflowing change batch. */
		if ((r->dirty || !ports) &&
		    igmp_rows_write(rows, r->vid, r->mac, ports))
			failed = 1;
	}
	return failed;
}
