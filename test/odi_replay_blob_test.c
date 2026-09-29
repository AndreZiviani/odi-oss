/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_replay_blob_test.c -- the register-replay firmware blobs
 * (odi_replay_blob.h): the three files the image ships under
 * rootfs/skeleton/lib/firmware/odi/ parse with the record counts the
 * replay tests pin, odi_replay_blob_parse() refuses every defect it is
 * meant to (header fields, size, CRC, per-record rules, each checked with
 * a CRC that is otherwise valid so the rule itself is what refuses it), a
 * missing file fails the load, and odi_gpon_init_apply() replays
 * gpon_init.bin with the serial-number words substituted. The other two
 * tables are replayed against their captures by
 * odi_switch_sdkinit_replay_test.sh and odi_switch_modload_replay_test.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "odi_replay_fw_host.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_init.c"

/* The record counts of the shipped tables. */
#define SDKINIT_RECORDS		7429U
#define MODLOAD_RECORDS		3619U
#define GPON_INIT_RECORDS	2528U

/* gponsn: the four serial-number words, USF_PLOAM_TX_WORD[1..4]. */
#define GPON_SN_WORD1_OFF	0x007050e4U
#define GPON_SN_WORDS		4U

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* Recomputes the header CRC after a test edit, so the edit is refused by
 * the rule it breaks and not by the CRC.
 */
static void reseal(uint8_t *data, size_t size)
{
	uint32_t crc = odi_replay_crc32_update(~0U, data, ODI_REPLAY_HDR_CRC);

	crc = odi_replay_crc32_update(crc, data + ODI_REPLAY_BLOB_HEADER_SIZE,
				      size - ODI_REPLAY_BLOB_HEADER_SIZE);
	put_be32(data + ODI_REPLAY_HDR_CRC, crc ^ ~0U);
}

/* A private, writable copy of one shipped table. */
static uint8_t *copy_of(enum odi_replay_table table, size_t *size)
{
	struct odi_replay_fw fw;
	uint8_t *copy;

	if (odi_replay_fw_load(table, &fw))
		return NULL;
	*size = ODI_REPLAY_BLOB_HEADER_SIZE + (size_t)fw.blob.count * ODI_REPLAY_BLOB_RECORD_SIZE;
	copy = malloc(*size);
	if (copy)
		memcpy(copy, fw.blob.records - ODI_REPLAY_BLOB_HEADER_SIZE, *size);
	odi_replay_fw_release(&fw);
	return copy;
}

static int parse(const uint8_t *data, size_t size, enum odi_replay_table table, const char **why)
{
	struct odi_replay_blob blob;

	*why = "";
	return odi_replay_blob_parse(data, size, table, &blob, why);
}

static uint8_t *record(uint8_t *data, uint32_t i)
{
	return data + ODI_REPLAY_BLOB_HEADER_SIZE + (size_t)i * ODI_REPLAY_BLOB_RECORD_SIZE;
}

static void test_shipped_tables_parse(void)
{
	struct odi_replay_fw fw;

	CHECK(odi_replay_fw_load(ODI_REPLAY_TABLE_SDKINIT, &fw) == 0 &&
	      fw.blob.count == SDKINIT_RECORDS, "sdkinit.bin parses, 7429 records");
	odi_replay_fw_release(&fw);
	CHECK(odi_replay_fw_load(ODI_REPLAY_TABLE_MODLOAD, &fw) == 0 &&
	      fw.blob.count == MODLOAD_RECORDS, "modload.bin parses, 3619 records");
	odi_replay_fw_release(&fw);
	CHECK(odi_replay_fw_load(ODI_REPLAY_TABLE_GPON_INIT, &fw) == 0 &&
	      fw.blob.count == GPON_INIT_RECORDS, "gpon_init.bin parses, 2528 records");
	odi_replay_fw_release(&fw);
	CHECK(odi_replay_fw_host_loads == odi_replay_fw_host_releases, "every load was released");
}

static void test_missing_file(void)
{
	struct odi_replay_fw fw;

	odi_replay_fw_host_dir = "/nonexistent-odi-replay-fw-dir";
	CHECK(odi_replay_fw_load(ODI_REPLAY_TABLE_MODLOAD, &fw) == -ENOENT,
	      "a missing modload.bin fails the load with -ENOENT");
	CHECK(fw.priv == NULL && fw.blob.count == 0, "a failed load leaves nothing to release");
	odi_replay_fw_host_dir = NULL;
}

static void test_header_defects(void)
{
	size_t size;
	uint8_t *d = copy_of(ODI_REPLAY_TABLE_MODLOAD, &size);
	const char *why;

	CHECK(d != NULL, "test setup: a copy of modload.bin");
	if (!d)
		return;
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == 0, "the untouched copy parses");

	CHECK(parse(d, size, ODI_REPLAY_TABLE_SDKINIT, &why) == -EINVAL,
	      "modload.bin is refused as the sdkinit table");
	CHECK(parse(d, size - 1U, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a truncated file is refused");
	CHECK(parse(d, ODI_REPLAY_BLOB_HEADER_SIZE - 1U, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a file shorter than the header is refused");

	d[ODI_REPLAY_BLOB_HEADER_SIZE + 9U] ^= 0x01U;
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EBADMSG,
	      "one flipped record bit fails the crc32");
	d[ODI_REPLAY_BLOB_HEADER_SIZE + 9U] ^= 0x01U;

	d[ODI_REPLAY_HDR_MAGIC] ^= 0x01U;
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL, "a bad magic is refused");
	d[ODI_REPLAY_HDR_MAGIC] ^= 0x01U;

	put_be16(d + ODI_REPLAY_HDR_VERSION, ODI_REPLAY_BLOB_VERSION + 1U);
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a newer version is refused");
	put_be16(d + ODI_REPLAY_HDR_VERSION, ODI_REPLAY_BLOB_VERSION);

	put_be16(d + ODI_REPLAY_HDR_RECORD_SIZE, ODI_REPLAY_BLOB_RECORD_SIZE + 4U);
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a different record size is refused");
	put_be16(d + ODI_REPLAY_HDR_RECORD_SIZE, ODI_REPLAY_BLOB_RECORD_SIZE);

	put_be32(d + ODI_REPLAY_HDR_COUNT, MODLOAD_RECORDS - 1U);
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a count that disagrees with the size is refused");
	put_be32(d + ODI_REPLAY_HDR_COUNT, 0xffffffffU);
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a huge count is refused before it is multiplied");
	put_be32(d + ODI_REPLAY_HDR_COUNT, MODLOAD_RECORDS);

	put_be32(d + ODI_REPLAY_HDR_RESERVED, 1U);
	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == -EINVAL,
	      "a nonzero reserved header field is refused");
	put_be32(d + ODI_REPLAY_HDR_RESERVED, 0U);

	reseal(d, size);
	CHECK(parse(d, size, ODI_REPLAY_TABLE_MODLOAD, &why) == 0, "every edit above was undone");
	free(d);
}

/* Breaks one field of record i, checks the table refuses it, restores it. */
static void expect_record_refused(uint8_t *d, size_t size, enum odi_replay_table table,
				  uint32_t i, unsigned int byte, uint8_t value, const char *msg)
{
	uint8_t *rec = record(d, i);
	uint8_t old = rec[byte];
	const char *why;

	rec[byte] = value;
	reseal(d, size);
	CHECK(parse(d, size, table, &why) == -EINVAL, msg);
	rec[byte] = old;
	reseal(d, size);
}

/* The index of the first record of a kind, or count when there is none. */
static uint32_t first_of_kind(uint8_t *d, uint32_t count, uint8_t kind)
{
	uint32_t i;

	for (i = 0; i < count; i++)
		if (record(d, i)[ODI_REPLAY_REC_KIND] == kind)
			break;
	return i;
}

static void test_record_rules(void)
{
	size_t size;
	uint8_t *d;
	uint32_t reg, tbl;

	d = copy_of(ODI_REPLAY_TABLE_MODLOAD, &size);
	CHECK(d != NULL, "test setup: a copy of modload.bin");
	if (d) {
		reg = first_of_kind(d, MODLOAD_RECORDS, ODI_REPLAY_REG);
		tbl = first_of_kind(d, MODLOAD_RECORDS, ODI_REPLAY_TABLE);
		CHECK(reg < MODLOAD_RECORDS && tbl < MODLOAD_RECORDS,
		      "test setup: modload.bin has register and table records");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, reg, ODI_REPLAY_REC_KIND,
				      ODI_REPLAY_SOC, "modload: a SoC-window record is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, reg, ODI_REPLAY_REC_REG_GROUP,
				      ODI_REPLAY_BLOB_MODLOAD_MAX_REG_GROUP + 1U,
				      "modload: a reg_group past bit 31 is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, reg, ODI_REPLAY_REC_VERB, 1U,
				      "modload: a record with a verb is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, reg, ODI_REPLAY_REC_WORDS + 3U, 1U,
				      "modload: a register record with words is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, tbl, ODI_REPLAY_REC_CATEGORY, 0U,
				      "modload: a table record in category 0 is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, tbl, ODI_REPLAY_REC_N_WORDS + 1U,
				      ODI_REPLAY_BLOB_MAX_WORDS + 1U,
				      "modload: a table row wider than five words is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_MODLOAD, tbl, ODI_REPLAY_REC_N_WORDS + 1U, 0U,
				      "modload: a table row with no words is refused");
		free(d);
	}

	d = copy_of(ODI_REPLAY_TABLE_SDKINIT, &size);
	CHECK(d != NULL, "test setup: a copy of sdkinit.bin");
	if (d) {
		expect_record_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, SDKINIT_RECORDS - 1U,
				      ODI_REPLAY_REC_VERB, ODI_SDKINIT_VERB_ID_COUNT,
				      "sdkinit: a verb past the enum is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, SDKINIT_RECORDS - 1U,
				      ODI_REPLAY_REC_VERB, 0U,
				      "sdkinit: a record out of verb order is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, 0U, ODI_REPLAY_REC_CATEGORY, 1U,
				      "sdkinit: a record with a category is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_SDKINIT, 0U, ODI_REPLAY_REC_KIND, 3U,
				      "sdkinit: an unknown kind is refused");
		free(d);
	}

	d = copy_of(ODI_REPLAY_TABLE_GPON_INIT, &size);
	CHECK(d != NULL, "test setup: a copy of gpon_init.bin");
	if (d) {
		expect_record_refused(d, size, ODI_REPLAY_TABLE_GPON_INIT, 0U, ODI_REPLAY_REC_SN_WORD,
				      ODI_REPLAY_BLOB_GPON_SN_WORD_MAX + 1U,
				      "gpon init: an sn_word past word 4 is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_GPON_INIT, 0U, ODI_REPLAY_REC_GPON_RESERVED, 1U,
				      "gpon init: a nonzero reserved field is refused");
		expect_record_refused(d, size, ODI_REPLAY_TABLE_GPON_INIT, 0U, ODI_REPLAY_REC_KIND,
				      ODI_REPLAY_SOC, "gpon init: a SoC-window record is refused");
		free(d);
	}
}

/* gpon_init.bin through odi_gpon_init_apply(): one register write per
 * record (no table rows in this capture), in order, with the four
 * serial-number words carrying this box serial number, never the zeros
 * the public table holds.
 */
static void test_gpon_init_apply(void)
{
	static const uint8_t sn[8] = { 'O', 'D', 'I', 'X', 0x12, 0x34, 0x56, 0x78 };
	static const uint8_t pw[10];
	struct odi_replay_fw fw;
	unsigned int i, w, sn_seen = 0;

	if (odi_replay_fw_load(ODI_REPLAY_TABLE_GPON_INIT, &fw)) {
		CHECK(0, "test setup: gpon_init.bin loads");
		return;
	}
	odi_mock_reset();
	odi_gpon_init_apply(&fw.blob, sn, pw);
	odi_mock_table_flush();

	CHECK(odi_mock.log_n == GPON_INIT_RECORDS, "gpon init: one register write per record");
	for (i = 0; i < odi_mock.log_n; i++) {
		const struct odi_mock_write *e = &odi_mock.log[i];
		struct odi_replay_event ev;

		odi_replay_blob_event(&fw.blob, i, &ev);
		CHECK(e->kind == 'W' && e->addr == ev.offset, "gpon init: writes follow the table order");
		if (ev.sn_word == 0) {
			CHECK(e->val == ev.value, "gpon init: a literal record writes its own value");
			continue;
		}
		w = ev.sn_word - 1U;
		CHECK(e->addr == GPON_SN_WORD1_OFF + 4U * w, "gpon init: sn words sit in USF_PLOAM_TX_WORD[1..4]");
		CHECK(e->val == (((uint32_t)sn[2U * w] << 8) | sn[2U * w + 1U]),
		      "gpon init: an sn word carries this box serial number");
		sn_seen++;
	}
	CHECK(sn_seen == GPON_SN_WORDS, "gpon init: all four serial-number words substituted");
	odi_replay_fw_release(&fw);
}

/* A serial number set after boot rewrites the armed slot and nothing else:
 * the six message words between the two USF_PLOAM_TX_CTL writes, the four
 * serial words carrying the new number. */
static void test_gpon_init_apply_serial(void)
{
	static const uint8_t sn[8] = { 'O', 'D', 'I', 'Y', 0xa1, 0xb2, 0xc3, 0xd4 };
	struct odi_replay_fw fw;
	unsigned int i;

	if (odi_replay_fw_load(ODI_REPLAY_TABLE_GPON_INIT, &fw)) {
		CHECK(0, "test setup: gpon_init.bin loads");
		return;
	}
	odi_mock_reset();
	CHECK(odi_gpon_init_apply_serial(&fw.blob, sn) == 8U,
	      "serial rearm: eight records, the CTL open, six words, the CTL arm");
	odi_mock_table_flush();
	CHECK(odi_mock.log_n == 8U, "serial rearm: eight register writes");
	if (odi_mock.log_n == 8U) {
		CHECK(odi_mock.log[0].addr == 0x7050c0U && odi_mock.log[0].val == 0x600U,
		      "serial rearm: opens the slot");
		CHECK(odi_mock.log[7].addr == 0x7050c0U && odi_mock.log[7].val == 0x601U,
		      "serial rearm: arms it");
		for (i = 0; i < 4U; i++)
			CHECK(odi_mock.log[2U + i].addr == GPON_SN_WORD1_OFF + 4U * i &&
			      odi_mock.log[2U + i].val ==
				      (((uint32_t)sn[2U * i] << 8) | sn[2U * i + 1U]),
			      "serial rearm: a serial word carries the new number");
	}
	odi_replay_fw_release(&fw);
}

int main(void)
{
	test_gpon_init_apply_serial();
	test_shipped_tables_parse();
	test_missing_file();
	test_header_defects();
	test_record_rules();
	test_gpon_init_apply();
	CHECK(odi_replay_fw_host_loads == odi_replay_fw_host_releases, "every load was released");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_replay_blob_test: ok\n");
	return 0;
}
