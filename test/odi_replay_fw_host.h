/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_replay_fw_host.h -- the host half of odi_replay_fw_load()/
 * odi_replay_fw_release() (odi_replay_blob.h): reads the table from the
 * same file the image ships, rootfs/skeleton/lib/firmware/odi/<name>, and
 * validates it with the same odi_replay_blob_parse() the kernel runs, so a
 * host test replays exactly the bytes a stick would load.
 *
 * Include after odi_replay_blob.c. The directory is ODI_REPLAY_FW_DIR from
 * the environment when set, otherwise rootfs/skeleton/lib/firmware next to
 * this header (derived from __FILE__, so it works from any working
 * directory). odi_replay_fw_host_dir, when non-NULL, overrides both: a test
 * points it at a scratch directory to feed a missing or corrupted file.
 * The load/release counters let a test check that every load was released.
 */
#ifndef ODI_REPLAY_FW_HOST_H
#define ODI_REPLAY_FW_HOST_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ODI_REPLAY_FW_HOST_PATH_MAX	1024

static const char *odi_replay_fw_host_dir;
static unsigned int odi_replay_fw_host_loads;
static unsigned int odi_replay_fw_host_releases;

static const char *odi_replay_fw_host_name(enum odi_replay_table table)
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

static void odi_replay_fw_host_path(char *path, size_t len, const char *name)
{
	const char *dir = odi_replay_fw_host_dir ? odi_replay_fw_host_dir : getenv("ODI_REPLAY_FW_DIR");
	const char *here = __FILE__;
	const char *slash = strrchr(here, '/');

	if (dir) {
		snprintf(path, len, "%s/%s", dir, name);
		return;
	}
	if (slash)
		snprintf(path, len, "%.*s/../rootfs/skeleton/lib/firmware/%s",
			 (int)(slash - here), here, name);
	else
		snprintf(path, len, "../rootfs/skeleton/lib/firmware/%s", name);
}

int odi_replay_fw_load(enum odi_replay_table table, struct odi_replay_fw *fw)
{
	const char *name = odi_replay_fw_host_name(table);
	char path[ODI_REPLAY_FW_HOST_PATH_MAX];
	const char *why = "";
	uint8_t *data;
	long size;
	FILE *f;
	int rc;

	fw->blob.records = NULL;
	fw->blob.count = 0;
	fw->priv = NULL;
	if (!name)
		return -EINVAL;

	odi_replay_fw_host_path(path, sizeof(path), name);
	f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "odi_replay: cannot load %s\n", path);
		return -ENOENT;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return -EIO;
	}
	data = malloc(size ? (size_t)size : 1U);
	if (!data) {
		fclose(f);
		return -ENOMEM;
	}
	if (fread(data, 1, (size_t)size, f) != (size_t)size) {
		free(data);
		fclose(f);
		return -EIO;
	}
	fclose(f);

	rc = odi_replay_blob_parse(data, (size_t)size, table, &fw->blob, &why);
	if (rc) {
		fprintf(stderr, "odi_replay: %s (%ld bytes) rejected: %s\n", path, size, why);
		free(data);
		return rc;
	}
	fw->priv = data;
	odi_replay_fw_host_loads++;
	return 0;
}

void odi_replay_fw_release(struct odi_replay_fw *fw)
{
	if (fw->priv)
		odi_replay_fw_host_releases++;
	free((void *)fw->priv);
	fw->priv = NULL;
	fw->blob.records = NULL;
	fw->blob.count = 0;
}

#endif /* ODI_REPLAY_FW_HOST_H */
