/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_replay.h -- the register replay engine: one record type and one loop
 * for every captured write sequence this driver set replays.
 *
 *   sequence    source                   REG record          also
 *   board init  odi_board_data.c         write               SOC
 *   sdkinit     sdkinit.bin, one verb    read, then write    SOC, TABLE
 *   modload     modload.bin              read, then write    TABLE
 *   gpon init   gpon_init.bin            write, value hook   TABLE
 *
 * The board table is compiled in because it runs at device_initcall,
 * before the rootfs exists; the other three are firmware files
 * (odi_replay_blob.h). The read before a REG write is the full-mask
 * read-modify-write of the capture, kept as a real read: without it the
 * bus traffic differs, and a clear-on-read register would see one read
 * fewer.
 *
 * Pure C, built on the host by the unity tests like the switch-core
 * leaves. No allocation, no sleeping and no logging per record: odi_gpon.c
 * runs a replay under its spinlock with interrupts off.
 */
#ifndef ODI_REPLAY_H
#define ODI_REPLAY_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

enum odi_replay_kind {
	ODI_REPLAY_REG = 0,	/* switch-core register: offset, value */
	ODI_REPLAY_TABLE = 1,	/* switch-core table row: table, offset (the row), words */
	ODI_REPLAY_SOC = 2,	/* SoC-window register: offset (its KSEG1 address), value */
};

/* The widest row any capture writes (ACL_PATTERN, odi_sw_table_desc[]). */
#define ODI_REPLAY_MAX_WORDS	5U

struct odi_replay_event {
	uint8_t kind;		/* enum odi_replay_kind */
	uint8_t verb;		/* sdkinit: the verb id (odi_switch_sdkinit.h); else 0 */
	uint8_t sn_word;	/* gpon init REG: serial-number word 1..4 to write
				 * instead of value; else 0
				 */
	uint16_t table;		/* TABLE: enum odi_sw_table id */
	uint16_t n_words;	/* TABLE: words used */
	uint32_t offset;
	uint32_t value;		/* REG, SOC */
	uint32_t words[ODI_REPLAY_MAX_WORDS];	/* TABLE, zero past n_words */
};

#define ODI_REPLAY_ALL_VERBS	(-1)

struct odi_replay_opts {
	int rmw;	/* REG: read the register before writing it */
	int verb;	/* replay only the records of this sdkinit verb, or
			 * ODI_REPLAY_ALL_VERBS
			 */
	/* REG: the value to write, when set; ctx is passed through. */
	uint32_t (*value)(const struct odi_replay_event *e, const void *ctx);
	const void *ctx;
};

struct odi_replay_blob;	/* odi_replay_blob.h */

/* Replays the records of blob in order, as opts says, and returns how
 * many it applied. A REG record goes through odi_reg_write(), a TABLE
 * record through odi_switch_table_write(), a SOC record through
 * odi_replay_soc_write().
 */
unsigned int odi_replay_run(const struct odi_replay_blob *blob,
			    const struct odi_replay_opts *opts);

/* A SOC record: addr is the KSEG1 address the table carries, written
 * through odi_soc_write() and its allowlist (odi_soc.c). A refused one is
 * logged and returns -EPERM.
 */
int odi_replay_soc_write(uint32_t addr, uint32_t val);

#endif /* ODI_REPLAY_H */
