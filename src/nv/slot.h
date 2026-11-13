/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* A PARTIAL check of a slot: does kN still hold a kernel that U-Boot's bootm
 * would accept, and does rN start with a plausible squashfs superblock?
 *
 * It is not proof the slot boots. It does not decompress the kernel, does
 * not read the root filesystem past its superblock, and knows nothing about
 * what the image does once it runs. What it catches is the slot that cannot
 * boot at all: an erased or half-written kernel (a flash interrupted between
 * fwu.sh's erase and its last write), a kernel whose bytes have changed, a
 * rootfs partition with no filesystem on it.
 *
 * The kernel part is the uImage checks that bootm makes before it jumps,
 * on the header (big-endian, 64 bytes):
 *
 *     0   magic       0x27051956
 *     4   header CRC  CRC32 of the 64 bytes with this field zero
 *     12  data size   bytes of payload after the header
 *     24  data CRC    CRC32 of those bytes
 *
 * plus one bootm cannot see: header and payload fit the partition (a size
 * past it means the CRC runs into the next partition, which is not a kernel
 * anyone flashed). The other header fields bootm looks at (OS,
 * architecture, image type, compression) are not checked.
 *
 * The rootfs part reads the first 96 bytes of rN: a version 4.0 squashfs
 * superblock ("hsqs", little-endian; Linux refuses a newer minor), a block size that matches its
 * block_log and is at most 1 MiB, and a bytes_used that fits the
 * partition. The compressor is not looked at: this image writes xz, the
 * stock firmware LZMA (a stock rootfs taken from a vendor download passes,
 * with its 1 MiB blocks), and only the kernel of that slot has to read it.
 *
 * No syscalls: the partitions are reached through slot_src, so the logic is
 * tested on the host against images built in memory (test/test_slot.c). */
#ifndef NV_SLOT_H
#define NV_SLOT_H

#include <stdint.h>

#define UIMAGE_MAGIC    0x27051956u
#define UIMAGE_HDR_LEN  64u
#define SQFS_SB_LEN     96u

/* One partition, read front to back. read returns bytes read, 0 at the end,
 * negative on an error. */
struct slot_src {
	void *ctx;
	long (*read)(void *ctx, uint8_t *buf, uint32_t n);
	uint32_t size;                  /* the partition, from /proc/mtd */
};

enum {
	SLOT_OK = 0,
	/* kernel */
	SLOT_K_READ,                    /* could not read it -- unknown, not bad */
	SLOT_K_SHORT,                   /* partition smaller than a header */
	SLOT_K_MAGIC,
	SLOT_K_HCRC,
	SLOT_K_SIZE,
	SLOT_K_DCRC,
	/* rootfs */
	SLOT_R_READ,
	SLOT_R_SHORT,
	SLOT_R_MAGIC,
	SLOT_R_VERSION,
	SLOT_R_BLOCK,
	SLOT_R_SIZE,
};

struct slot_report {
	uint32_t kernel_size;           /* header + payload, when the header was read */
	uint32_t rootfs_size;           /* bytes_used, when the superblock passed */
};

/* 1 for a result that says the slot is broken, 0 for SLOT_OK and for a read
 * that failed (which says nothing about the slot). */
int slot_bad(int rc);

/* buf is scratch for the payload CRC: any size above zero, bigger is fewer
 * reads. */
int slot_check_kernel(const struct slot_src *k, uint8_t *buf, uint32_t buflen,
		      struct slot_report *r);
int slot_check_rootfs(const struct slot_src *rf, struct slot_report *r);

const char *slot_check_msg(int rc);

/* The value of key in a KEY=value file (slot-state.sh's /var/run/odi-slot),
 * NUL-terminated into out: 1 when the key is there, 0 when it is not or
 * its value does not fit. */
int slot_state_get(const char *buf, uint32_t n, const char *key,
		   char *out, uint32_t cap);

#endif
