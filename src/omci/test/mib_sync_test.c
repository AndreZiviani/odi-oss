/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* The MIB data sync counter across its wrap: 255 is followed by 1, never by
 * 0, which is reserved for a MIB just instantiated or reset. No device or
 * netlink is opened. */
#include "omcid.h"

int __wrap_omci_drv_call(uint32_t cmd, void *buf, uint32_t len)
{
	(void)cmd; (void)buf; (void)len;
	return 0;
}

int main(void)
{
	/* From a reset, the counter counts 1, 2, ... */
	mib_data_sync = 0;
	mib_data_sync_advance();
	if (mib_data_sync != 1)
		return 1;

	/* Through the wrap. */
	mib_data_sync = 254;
	mib_data_sync_advance();
	if (mib_data_sync != 255)
		return 2;
	mib_data_sync_advance();
	if (mib_data_sync != 1)
		return 3;
	mib_data_sync_advance();
	if (mib_data_sync != 2)
		return 4;

	/* Several laps after a reset never land on 0. */
	mib_data_sync = 0;
	for (int i = 0; i < 1000; i++) {
		mib_data_sync_advance();
		if (mib_data_sync == 0)
			return 5;
	}
	out("ok    MIB data sync goes from 255 to 1, never to 0\n");
	return 0;
}
