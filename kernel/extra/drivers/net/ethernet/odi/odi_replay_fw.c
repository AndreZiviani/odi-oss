// SPDX-License-Identifier: GPL-2.0
/*
 * odi_replay_fw.c -- odi_replay_fw_load()/odi_replay_fw_release(), the
 * kernel half of odi_replay_blob.h: one request_firmware() per trigger, the
 * contents validated whole by odi_replay_blob_parse() before any caller
 * applies a record, and released as soon as that caller is done. Nothing
 * stays resident between triggers.
 *
 * No struct device: these tables belong to the switch core, not to one
 * probed device, and the loader accepts NULL. firmware_request_nowarn()
 * because this file logs the failure itself, naming the table, rather
 * than leaving a "(NULL device *)" line from the core. Without
 * CONFIG_FW_LOADER_USER_HELPER a missing file fails at once instead of
 * waiting on a user-space helper.
 */
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/kernel.h>
#include <linux/printk.h>

#include "odi_replay_blob.h"

static const char *odi_replay_fw_name(enum odi_replay_table table)
{
	switch (table) {
	case ODI_REPLAY_TABLE_SDKINIT:
		return ODI_REPLAY_FW_SDKINIT;
	case ODI_REPLAY_TABLE_MODLOAD:
		return ODI_REPLAY_FW_MODLOAD;
	case ODI_REPLAY_TABLE_GPON_INIT:
		return ODI_REPLAY_FW_GPON_INIT;
	}
	return NULL;
}

int odi_replay_fw_load(enum odi_replay_table table, struct odi_replay_fw *fw)
{
	const char *name = odi_replay_fw_name(table);
	const struct firmware *raw;
	const char *why = "";
	int rc;

	fw->blob.records = NULL;
	fw->blob.count = 0;
	fw->priv = NULL;
	if (!name)
		return -EINVAL;

	might_sleep();
	rc = firmware_request_nowarn(&raw, name, NULL);
	if (rc) {
		pr_err("odi_replay: cannot load /lib/firmware/%s: %d\n", name, rc);
		return rc;
	}

	rc = odi_replay_blob_parse(raw->data, raw->size, table, &fw->blob, &why);
	if (rc) {
		pr_err("odi_replay: /lib/firmware/%s (%zu bytes) rejected: %s\n",
		       name, raw->size, why);
		release_firmware(raw);
		return rc;
	}

	fw->priv = raw;
	return 0;
}

void odi_replay_fw_release(struct odi_replay_fw *fw)
{
	release_firmware(fw->priv);
	fw->priv = NULL;
	fw->blob.records = NULL;
	fw->blob.count = 0;
}
