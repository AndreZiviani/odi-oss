/* SPDX-FileCopyrightText: 2026 Andre Ziviani */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* Host-side tests for the partial slot check (slot.h).
 *
 * Kernel and rootfs are built in memory in the layouts slot.h documents, the
 * CRC is held to the published CRC-32 check value first, and each way a
 * slot can be broken is made once.
 *
 *     cc -o t test_slot.c ../slot.c ../env.c && ./t
 */
#include <stdio.h>
#include <string.h>
#include "../env.h"
#include "../slot.h"

static int fails;

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

/* A partition in memory; stop_at ends the reads early, the way a read
 * error would. */
struct mem {
	const uint8_t *p;
	uint32_t len, pos, stop_at, chunk;
};

static long mem_read(void *ctx, uint8_t *buf, uint32_t n)
{
	struct mem *m = ctx;
	uint32_t end = m->stop_at ? m->stop_at : m->len;

	if (m->pos >= end)
		return 0;
	if (n > end - m->pos)
		n = end - m->pos;
	if (m->chunk && n > m->chunk)
		n = m->chunk;
	memcpy(buf, m->p + m->pos, n);
	m->pos += n;
	return (long)n;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#define KPART 20000u
#define RPART 65536u
static uint8_t kpart[KPART], rpart[RPART];

/* A uImage of len payload bytes at the start of an erased partition. */
static void build_kernel(uint32_t len)
{
	memset(kpart, 0xff, sizeof kpart);
	memset(kpart, 0, UIMAGE_HDR_LEN);
	for (uint32_t i = 0; i < len; i++)
		kpart[UIMAGE_HDR_LEN + i] = (uint8_t)(i * 7 + 3);
	put_be32(kpart, UIMAGE_MAGIC);
	put_be32(kpart + 12, len);
	put_be32(kpart + 24, env_crc32(kpart + UIMAGE_HDR_LEN, len));
	kpart[28] = 5; kpart[29] = 5; kpart[30] = 2; kpart[31] = 3;
	put_be32(kpart + 4, env_crc32(kpart, UIMAGE_HDR_LEN));
}

static void rehash_header(void)
{
	put_be32(kpart + 4, 0);
	put_be32(kpart + 4, env_crc32(kpart, UIMAGE_HDR_LEN));
}

/* A version 4 superblock, 128 KiB blocks, used bytes as given. */
static void build_rootfs(uint32_t used)
{
	memset(rpart, 0xff, sizeof rpart);
	memset(rpart, 0, SQFS_SB_LEN);
	memcpy(rpart, "hsqs", 4);
	put_le32(rpart + 12, 1u << 17);
	rpart[20] = 4;                        /* xz */
	rpart[22] = 17;
	rpart[28] = 4;
	put_le32(rpart + 40, used);
}

static int kcheck(uint32_t part, uint32_t stop_at, uint32_t chunk, uint32_t buflen,
		  struct slot_report *r)
{
	static uint8_t buf[4096];
	struct mem m = { kpart, part, 0, stop_at, chunk };
	struct slot_src s = { &m, mem_read, part };

	memset(r, 0, sizeof *r);
	return slot_check_kernel(&s, buf, buflen, r);
}

static int rcheck(uint32_t part, uint32_t stop_at, struct slot_report *r)
{
	struct mem m = { rpart, part, 0, stop_at, 0 };
	struct slot_src s = { &m, mem_read, part };

	memset(r, 0, sizeof *r);
	return slot_check_rootfs(&s, r);
}

int main(void)
{
	struct slot_report r;
	int rc;

	/* The CRC: the standard check value, and continuing equals whole. */
	ck(env_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u,
	   "env_crc32(\"123456789\") is the CRC-32 check value cbf43926");
	ck(env_crc32_update(env_crc32((const uint8_t *)"1234", 4),
			    (const uint8_t *)"56789", 5) == 0xcbf43926u,
	   "env_crc32_update continues a finished CRC");
	ck(env_crc32_update(0, (const uint8_t *)"", 0) == 0,
	   "the CRC of nothing is 0, as zlib starts it");

	/* Kernel. */
	build_kernel(12345);
	rc = kcheck(KPART, 0, 0, 4096, &r);
	ck(rc == SLOT_OK && r.kernel_size == 12345 + UIMAGE_HDR_LEN,
	   "a whole uImage passes and reports header + payload size");
	rc = kcheck(KPART, 0, 13, 7, &r);
	ck(rc == SLOT_OK, "short reads and a 7-byte buffer give the same answer");
	build_kernel(KPART - UIMAGE_HDR_LEN);
	ck(kcheck(KPART, 0, 0, 4096, &r) == SLOT_OK,
	   "a uImage that fills the partition exactly passes");

	memset(kpart, 0xff, sizeof kpart);
	ck(kcheck(KPART, 0, 0, 4096, &r) == SLOT_K_MAGIC,
	   "an erased kernel partition has no magic");
	build_kernel(12345);
	kpart[40] ^= 0x01;                    /* the name field */
	ck(kcheck(KPART, 0, 0, 4096, &r) == SLOT_K_HCRC,
	   "a changed header byte fails the header CRC");
	build_kernel(12345);
	put_be32(kpart + 12, KPART - UIMAGE_HDR_LEN + 1);
	rehash_header();
	ck(kcheck(KPART, 0, 0, 4096, &r) == SLOT_K_SIZE,
	   "a data size one past the partition is refused before reading");
	build_kernel(12345);
	put_be32(kpart + 12, 0xffffffffu);
	rehash_header();
	ck(kcheck(KPART, 0, 0, 4096, &r) == SLOT_K_SIZE,
	   "a data size of 0xffffffff does not wrap past the size check");
	build_kernel(12345);
	kpart[UIMAGE_HDR_LEN + 12344] ^= 0x80;
	ck(kcheck(KPART, 0, 0, 4096, &r) == SLOT_K_DCRC,
	   "a changed last payload byte fails the data CRC");
	build_kernel(12345);
	memset(kpart + UIMAGE_HDR_LEN + 6000, 0xff, KPART - UIMAGE_HDR_LEN - 6000);
	ck(kcheck(KPART, 0, 0, 4096, &r) == SLOT_K_DCRC,
	   "a payload erased from the middle on (an interrupted write) fails the data CRC");
	build_kernel(12345);
	ck(kcheck(KPART, 5000, 0, 4096, &r) == SLOT_K_READ,
	   "a read that ends early is a read failure, not a bad slot");
	ck(kcheck(KPART, 10, 0, 4096, &r) == SLOT_K_READ,
	   "a read that ends inside the header is a read failure");
	ck(kcheck(UIMAGE_HDR_LEN - 1, 0, 0, 4096, &r) == SLOT_K_SHORT,
	   "a partition smaller than a header is refused");

	/* Rootfs. */
	build_rootfs(40000);
	rc = rcheck(RPART, 0, &r);
	ck(rc == SLOT_OK && r.rootfs_size == 40000,
	   "a version 4 superblock passes and reports bytes_used");
	build_rootfs(RPART);
	ck(rcheck(RPART, 0, &r) == SLOT_OK, "bytes_used equal to the partition passes");
	build_rootfs(RPART + 1);
	ck(rcheck(RPART, 0, &r) == SLOT_R_SIZE, "bytes_used past the partition fails");
	build_rootfs(40000);
	rpart[44] = 1;
	ck(rcheck(RPART, 0, &r) == SLOT_R_SIZE, "a bytes_used above 4 GiB fails");
	build_rootfs(10);
	ck(rcheck(RPART, 0, &r) == SLOT_R_SIZE, "a bytes_used smaller than the superblock fails");
	memset(rpart, 0xff, sizeof rpart);
	ck(rcheck(RPART, 0, &r) == SLOT_R_MAGIC, "an erased rootfs partition has no magic");
	build_rootfs(40000);
	rpart[28] = 3;
	ck(rcheck(RPART, 0, &r) == SLOT_R_VERSION, "a little-endian version 3 superblock fails");
	build_rootfs(40000);
	rpart[30] = 1;
	ck(rcheck(RPART, 0, &r) == SLOT_R_VERSION, "a version 4.1 superblock fails (Linux refuses it)");
	build_rootfs(40000);
	rpart[22] = 16;
	ck(rcheck(RPART, 0, &r) == SLOT_R_BLOCK, "a block_log that disagrees with block_size fails");
	build_rootfs(40000);
	put_le32(rpart + 12, 1u << 21);
	rpart[22] = 21;
	ck(rcheck(RPART, 0, &r) == SLOT_R_BLOCK, "a 2 MiB block size fails");
	memset(rpart, 0, SQFS_SB_LEN);
	memcpy(rpart, "sqsh", 4);
	ck(rcheck(RPART, 0, &r) == SLOT_R_MAGIC,
	   "the big-endian magic of older squashfs versions is not accepted");
	build_rootfs(40000);
	put_le32(rpart + 12, 1u << 20);
	rpart[22] = 20;
	rpart[20] = 2;                        /* lzma, as the stock rootfs */
	ck(rcheck(RPART, 0, &r) == SLOT_OK, "1 MiB blocks and another compressor pass");
	build_rootfs(40000);
	ck(rcheck(RPART, 50, &r) == SLOT_R_READ, "a read that ends inside the superblock is a read failure");
	ck(rcheck(SQFS_SB_LEN - 1, 0, &r) == SLOT_R_SHORT,
	   "a partition smaller than a superblock is refused");

	/* What counts as broken. */
	ck(!slot_bad(SLOT_OK) && !slot_bad(SLOT_K_READ) && !slot_bad(SLOT_R_READ),
	   "ok and read failures are not a broken slot");
	ck(slot_bad(SLOT_K_DCRC) && slot_bad(SLOT_R_MAGIC) && slot_bad(SLOT_K_SHORT),
	   "check failures are a broken slot");
	for (rc = SLOT_OK; rc <= SLOT_R_SIZE; rc++)
		if (!strcmp(slot_check_msg(rc), "unknown result"))
			ck(0, "every result has a message");

	/* The state file nv commit reads its warning from. */
	{
		static const char st[] =
			"running=1\nuncommitted=0\nother_slot=0\n"
			"other_slot_check=fail\n"
			"other_slot_detail=kernel: uImage data CRC does not match (incomplete or changed)\n";
		char v[128], tiny[4];
		uint32_t n = (uint32_t)strlen(st);

		ck(slot_state_get(st, n, "other_slot", v, sizeof v) && !strcmp(v, "0"),
		   "other_slot is read, not other_slot_check");
		ck(slot_state_get(st, n, "other_slot_check", v, sizeof v) && !strcmp(v, "fail"),
		   "other_slot_check is read");
		ck(slot_state_get(st, n, "other_slot_detail", v, sizeof v) &&
		   !strcmp(v, "kernel: uImage data CRC does not match (incomplete or changed)"),
		   "a value with spaces and colons is read whole");
		ck(slot_state_get(st, n, "running", v, sizeof v) && !strcmp(v, "1"),
		   "the first key is read");
		ck(!slot_state_get(st, n, "slot", v, sizeof v),
		   "a key is matched from the start of a line only");
		ck(!slot_state_get(st, n, "other_slot_detail", tiny, sizeof tiny),
		   "a value that does not fit is not returned cut");
		ck(!slot_state_get(st, 20, "other_slot", v, sizeof v),
		   "nothing past n is read");
		ck(slot_state_get("k=\n", 3, "k", v, sizeof v) && v[0] == 0,
		   "an empty value is an empty string");
		ck(slot_state_get("k=v", 3, "k", v, sizeof v) && !strcmp(v, "v"),
		   "a last line with no newline is read");
	}

	printf("\n%s\n", fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
