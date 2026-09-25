/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_replay_blob.h -- the register-replay firmware blobs: the three
 * captured write sequences this driver set replays (the sdkinit verbs, the
 * module-load replay, the GPON boot init), kept out of the kernel image
 * and loaded with request_firmware() from /lib/firmware/odi/ when a
 * trigger actually needs one.
 *
 * tools/regtrace/replayblob.py writes and dumps this format and its
 * docstring is the byte-level reference; the two must agree field for
 * field. Every multi-byte field is big-endian and is read byte by byte
 * (odi_replay_be16()/odi_replay_be32() below), so neither side depends on a
 * C struct layout or on the host byte order.
 *
 *   header (ODI_REPLAY_BLOB_HEADER_SIZE bytes)
 *     0  u32 magic        ODI_REPLAY_BLOB_MAGIC
 *     4  u16 version      ODI_REPLAY_BLOB_VERSION
 *     6  u16 table        enum odi_replay_table
 *     8  u16 header_size  ODI_REPLAY_BLOB_HEADER_SIZE
 *    10  u16 record_size  ODI_REPLAY_BLOB_RECORD_SIZE
 *    12  u32 count        records that follow
 *    16  u32 reserved     0
 *    20  u32 crc32        CRC-32 (IEEE, as zlib) of bytes 0..19 then every record
 *   count records (ODI_REPLAY_BLOB_RECORD_SIZE bytes each), file size exactly
 *   header + count * record.
 *
 *   switch record (sdkinit, modload)
 *     0  u8  kind         enum odi_sw_modload_event_kind
 *     1  u8  category     modload only
 *     2  u8  reg_group    modload category 0 only
 *     3  u8  verb         sdkinit only: enum odi_sw_sdkinit_verb_id
 *     4  u16 table        TABLE records only
 *     6  u16 n_words      TABLE records only
 *     8  u32 offset
 *    12  u32 value        REG and SOC records only
 *    16  u32 words[5]     TABLE records only, zero past n_words
 *
 *   GPON init record: byte 1 is sn_word, bytes 2..3 are reserved (0), the
 *   rest as the switch record. Byte 3 carries no verb.
 *
 * odi_replay_blob_parse() checks the header, the size, the CRC and then
 * every record against the rules of its table (kind, field ranges,
 * unused fields zero, sdkinit records grouped by verb) before a caller
 * sees any of it, so the apply loops can decode records without checking
 * them again: a truncated, corrupted or mis-generated file is refused
 * whole, never half applied.
 *
 * Pure C with no kernel dependency, like odi_switch_dal.c: the host tests
 * include it as source. odi_replay_fw_load()/odi_replay_fw_release() are
 * the one environment-specific pair: odi_replay_fw.c under __KERNEL__
 * (request_firmware()), test/odi_replay_fw_host.h on the host (a plain
 * file read from rootfs/skeleton/lib/firmware/).
 */
#ifndef ODI_REPLAY_BLOB_H
#define ODI_REPLAY_BLOB_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#include "odi_switch_dal.h"	/* struct odi_sw_modload_event */
#include "odi_gpon_init.h"	/* struct odi_gpon_init_event */

#define ODI_REPLAY_BLOB_MAGIC		0x4f444952U	/* "ODIR" */
#define ODI_REPLAY_BLOB_VERSION		1U
#define ODI_REPLAY_BLOB_HEADER_SIZE	24U
#define ODI_REPLAY_BLOB_RECORD_SIZE	36U
#define ODI_REPLAY_BLOB_MAX_WORDS	5U
/* Far above any capture (the largest table, sdkinit, is under 8k records);
 * bounds count before it is multiplied into a size.
 */
#define ODI_REPLAY_BLOB_MAX_RECORDS	65536U

/* modload field ranges: categories 0..5 (ODI_SWITCH_INIT_MODLOAD_ITEM_ALL)
 * and reg_group 0..25, mask bits 6..31 -- the same bound mkmodload.py
 * refuses to generate past.
 */
#define ODI_REPLAY_BLOB_MODLOAD_MAX_CATEGORY	5U
#define ODI_REPLAY_BLOB_MODLOAD_MAX_REG_GROUP	25U

/* The GPON serial-number words odi_gpon_init_apply() substitutes. */
#define ODI_REPLAY_BLOB_GPON_SN_WORD_MIN	1U
#define ODI_REPLAY_BLOB_GPON_SN_WORD_MAX	4U

enum odi_replay_table {
	ODI_REPLAY_TABLE_SDKINIT = 1,
	ODI_REPLAY_TABLE_MODLOAD = 2,
	ODI_REPLAY_TABLE_GPON_INIT = 3,
};

/* Paths under the firmware search path (/lib/firmware). */
#define ODI_REPLAY_FW_SDKINIT		"odi/sdkinit.bin"
#define ODI_REPLAY_FW_MODLOAD		"odi/modload.bin"
#define ODI_REPLAY_FW_GPON_INIT		"odi/gpon_init.bin"

/* A validated table: count records of ODI_REPLAY_BLOB_RECORD_SIZE bytes at
 * records, borrowed from whoever owns the file contents.
 */
struct odi_replay_blob {
	const uint8_t *records;
	uint32_t count;
};

/* Validates data[0..size) as a blob of the given table. Returns 0 and
 * fills *out, or a negative errno (-EINVAL malformed, -EBADMSG CRC
 * mismatch) with *why pointing at a static description of the first
 * defect found; out is untouched on failure.
 */
int odi_replay_blob_parse(const uint8_t *data, size_t size, enum odi_replay_table table,
			  struct odi_replay_blob *out, const char **why);

/* Record i of a parsed switch-table blob (sdkinit or modload), decoded.
 * verb receives byte 3 (sdkinit: the verb id; modload: always 0).
 */
void odi_replay_blob_switch_event(const struct odi_replay_blob *blob, uint32_t i,
				  struct odi_sw_modload_event *e, uint8_t *verb);

/* Record i of a parsed GPON init blob, decoded. */
void odi_replay_blob_gpon_event(const struct odi_replay_blob *blob, uint32_t i,
				struct odi_gpon_init_event *e);

/* A loaded table and whatever owns its bytes. */
struct odi_replay_fw {
	struct odi_replay_blob blob;
	const void *priv;
};

/* Loads and validates one table from the firmware search path. Sleeps:
 * process context only, never under a spinlock. Returns 0, or a negative
 * errno after logging which file failed and why; *fw is then empty and
 * needs no release.
 */
int odi_replay_fw_load(enum odi_replay_table table, struct odi_replay_fw *fw);
void odi_replay_fw_release(struct odi_replay_fw *fw);

#endif /* ODI_REPLAY_BLOB_H */
