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

	/* A deactivate the driver refuses is logged and the slot still
	 * freed, as it was for a service this process built itself. */
	servtab[4].in_use = servtab[9].in_use = 1;
	fail_id = 9;
	bdgconn_rebuild();
	if (bad || calls != 4 || servtab[4].in_use || servtab[9].in_use)
		return 2;

	/* A dry run frees the slots and sends nothing. */
	servtab[4].in_use = 1;
	apply_hw = 0;
	bdgconn_rebuild();
	if (servtab[4].in_use || calls != 4)
		return 3;
	out("ok    a rebuild withdraws services restored from a snapshot\n");
	return 0;
}
