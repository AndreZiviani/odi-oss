/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* bdgconn_rebuild() against a servtab that only a snapshot filled: the
 * state of an omcid that resumed. The driver call is wrapped (--wrap) and
 * counts the deactivates; no device or netlink is opened. */
#include "omcid.h"

static int calls, fail_id = -1, bad;

int __wrap_omci_drv_call(uint32_t cmd, void *buf, uint32_t len)
{
	uint32_t id;

	if (cmd != 50 || len != 4) {
		bad = 1;
		return -1;
	}
	id = nl_get32(buf);
	if (id != 4 && id != 9)
		bad = 1;
	calls++;
	return (int)id == fail_id ? -1 : 0;
}

int main(void)
{
	/* Two restored services, nothing in the MIB to build from. */
	apply_hw = 1;
	servtab[4].in_use = servtab[9].in_use = 1;
	bdgconn_rebuild();
	if (bad || calls != 2 || servtab[4].in_use || servtab[9].in_use)
		return 1;

	/* A deactivate the driver refuses keeps its id owned as pending: not
	 * live, not freed, and no rebuild asked for (that would redo every
	 * live service). */
	servtab[4].in_use = servtab[9].in_use = 1;
	fail_id = 9;
	conn_dirty = 0;
	bdgconn_rebuild();
	if (bad || calls != 4 || servtab[4].in_use || servtab[9].in_use != SERV_PENDING)
		return 2;
	if (conn_dirty)
		return 4;

	/* The retry touches only the pending id, never a live one. */
	servtab[4].in_use = 1;
	if (bdgconn_retry_pending() != 1 || bad || calls != 5 ||
	    servtab[9].in_use != SERV_PENDING || servtab[4].in_use != 1 ||
	    conn_dirty)
		return 5;
	servtab[4].in_use = 0;

	/* A full rebuild retries it first too. */
	bdgconn_rebuild();
	if (bad || calls != 6 || servtab[9].in_use != SERV_PENDING || conn_dirty)
		return 6;

	/* Once the driver accepts it the slot is freed. */
	fail_id = -1;
	if (bdgconn_retry_pending() != 0 || bad || calls != 7 || servtab[9].in_use)
		return 7;
	if (bdgconn_retry_pending() != 0 || calls != 7)
		return 8;

	/* A dry run frees the slots and sends nothing. */
	servtab[4].in_use = 1;
	servtab[9].in_use = SERV_PENDING;
	apply_hw = 0;
	bdgconn_rebuild();
	if (servtab[4].in_use || servtab[9].in_use || calls != 7)
		return 3;
	out("ok    a rebuild withdraws services restored from a snapshot\n");
	return 0;
}
