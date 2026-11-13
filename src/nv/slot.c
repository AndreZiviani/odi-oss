/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* See slot.h: what is checked, and what is not. */
#include "slot.h"
#include "env.h"

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
	     | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t le32(const uint8_t *p)
{
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16)
	     | ((uint32_t)p[1] << 8)  |  (uint32_t)p[0];
}

static uint32_t le16(const uint8_t *p)
{
	return ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

/* Exactly n bytes, or 0. */
static int read_full(const struct slot_src *s, uint8_t *buf, uint32_t n)
{
	uint32_t got = 0;

	while (got < n) {
		long r = s->read(s->ctx, buf + got, n - got);

		if (r <= 0)
			return 0;
		got += (uint32_t)r;
	}
	return 1;
}

int slot_bad(int rc)
{
	return rc != SLOT_OK && rc != SLOT_K_READ && rc != SLOT_R_READ;
}

int slot_check_kernel(const struct slot_src *k, uint8_t *buf, uint32_t buflen,
		      struct slot_report *r)
{
	uint8_t h[UIMAGE_HDR_LEN];
	uint32_t size, left, crc = 0;

	if (k->size < UIMAGE_HDR_LEN)
		return SLOT_K_SHORT;
	if (!read_full(k, h, UIMAGE_HDR_LEN))
		return SLOT_K_READ;
	if (be32(h) != UIMAGE_MAGIC)
		return SLOT_K_MAGIC;
	{
		uint32_t want = be32(h + 4);

		h[4] = h[5] = h[6] = h[7] = 0;
		if (env_crc32(h, UIMAGE_HDR_LEN) != want)
			return SLOT_K_HCRC;
	}
	size = be32(h + 12);
	/* No overflow: size is checked against what is left of the partition,
	 * never added to the header length first. */
	if (size > k->size - UIMAGE_HDR_LEN)
		return SLOT_K_SIZE;
	r->kernel_size = UIMAGE_HDR_LEN + size;
	for (left = size; left; ) {
		uint32_t n = left < buflen ? left : buflen;

		if (!read_full(k, buf, n))
			return SLOT_K_READ;
		crc = env_crc32_update(crc, buf, n);
		left -= n;
	}
	if (crc != be32(h + 24))
		return SLOT_K_DCRC;
	return SLOT_OK;
}

int slot_check_rootfs(const struct slot_src *rf, struct slot_report *r)
{
	uint8_t sb[SQFS_SB_LEN];
	uint32_t block, log, lo, hi;

	if (rf->size < SQFS_SB_LEN)
		return SLOT_R_SHORT;
	if (!read_full(rf, sb, SQFS_SB_LEN))
		return SLOT_R_READ;
	if (!(sb[0] == 'h' && sb[1] == 's' && sb[2] == 'q' && sb[3] == 's'))
		return SLOT_R_MAGIC;
	if (le16(sb + 28) != 4 || le16(sb + 30) != 0)
		return SLOT_R_VERSION;
	/* The kernel refuses a block size over 1 MiB or one that does not
	 * match its log; 4 KiB is the smallest mksquashfs writes. */
	block = le32(sb + 12);
	log = le16(sb + 22);
	if (log < 12 || log > 20 || block != (1u << log))
		return SLOT_R_BLOCK;
	lo = le32(sb + 40);
	hi = le32(sb + 44);
	if (hi || lo < SQFS_SB_LEN || lo > rf->size)
		return SLOT_R_SIZE;
	r->rootfs_size = lo;
	return SLOT_OK;
}

const char *slot_check_msg(int rc)
{
	switch (rc) {
	case SLOT_OK:        return "ok";
	case SLOT_K_READ:    return "kernel: could not read the partition";
	case SLOT_K_SHORT:   return "kernel: partition is smaller than a uImage header";
	case SLOT_K_MAGIC:   return "kernel: no uImage magic (erased or not a kernel)";
	case SLOT_K_HCRC:    return "kernel: uImage header CRC does not match";
	case SLOT_K_SIZE:    return "kernel: uImage data size runs past the partition";
	case SLOT_K_DCRC:    return "kernel: uImage data CRC does not match (incomplete or changed)";
	case SLOT_R_READ:    return "rootfs: could not read the partition";
	case SLOT_R_SHORT:   return "rootfs: partition is smaller than a squashfs superblock";
	case SLOT_R_MAGIC:   return "rootfs: no squashfs magic (erased or not a squashfs)";
	case SLOT_R_VERSION: return "rootfs: squashfs superblock is not version 4.0";
	case SLOT_R_BLOCK:   return "rootfs: squashfs block size does not match its block_log";
	case SLOT_R_SIZE:    return "rootfs: squashfs bytes_used does not fit the partition";
	}
	return "unknown result";
}

int slot_state_get(const char *buf, uint32_t n, const char *key,
		   char *out, uint32_t cap)
{
	uint32_t kl = 0;

	while (key[kl])
		kl++;
	for (uint32_t i = 0; i + kl < n; i++) {
		uint32_t j = 0, k = 0;

		if (i && buf[i - 1] != '\n')
			continue;
		while (j < kl && buf[i + j] == key[j])
			j++;
		if (j < kl || buf[i + kl] != '=')
			continue;
		for (i += kl + 1; i < n && buf[i] != '\n'; i++) {
			if (k + 1 >= cap)
				return 0;
			out[k++] = buf[i];
		}
		out[k] = 0;
		return 1;
	}
	return 0;
}
