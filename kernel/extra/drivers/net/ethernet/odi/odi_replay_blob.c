// SPDX-License-Identifier: GPL-2.0
/*
 * odi_replay_blob.c -- validation and record decoding for the
 * register-replay firmware blobs; odi_replay_blob.h has the format and the
 * contract. Built into the kernel (CONFIG_ODI_SWITCH) and included as
 * source by the host tests, like odi_switch_dal.c.
 */
#include "odi_replay_blob.h"
#include "odi_switch_sdkinit.h"	/* ODI_SDKINIT_VERB_ID_COUNT */

#ifdef __KERNEL__
#include <linux/crc32.h>
#include <linux/errno.h>

static uint32_t odi_replay_crc32_update(uint32_t crc, const uint8_t *p, size_t len)
{
	return crc32_le(crc, p, len);
}
#else
#include <errno.h>

#define ODI_REPLAY_CRC32_POLY	0xedb88320U	/* IEEE 802.3, bit-reflected */

/* Same register semantics as the kernel crc32_le(): no inversion inside,
 * the caller seeds with ~0 and inverts the result.
 */
static uint32_t odi_replay_crc32_update(uint32_t crc, const uint8_t *p, size_t len)
{
	size_t i;
	int bit;

	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (ODI_REPLAY_CRC32_POLY & (0U - (crc & 1U)));
	}
	return crc;
}
#endif

/* Header field offsets. */
#define ODI_REPLAY_HDR_MAGIC		0U
#define ODI_REPLAY_HDR_VERSION		4U
#define ODI_REPLAY_HDR_TABLE		6U
#define ODI_REPLAY_HDR_HEADER_SIZE	8U
#define ODI_REPLAY_HDR_RECORD_SIZE	10U
#define ODI_REPLAY_HDR_COUNT		12U
#define ODI_REPLAY_HDR_RESERVED		16U
#define ODI_REPLAY_HDR_CRC		20U

/* Record field offsets. */
#define ODI_REPLAY_REC_KIND		0U
#define ODI_REPLAY_REC_CATEGORY		1U	/* switch */
#define ODI_REPLAY_REC_SN_WORD		1U	/* GPON init */
#define ODI_REPLAY_REC_REG_GROUP	2U	/* switch */
#define ODI_REPLAY_REC_GPON_RESERVED	2U	/* GPON init, u16 */
#define ODI_REPLAY_REC_VERB		3U	/* switch */
#define ODI_REPLAY_REC_TABLE		4U
#define ODI_REPLAY_REC_N_WORDS		6U
#define ODI_REPLAY_REC_OFFSET		8U
#define ODI_REPLAY_REC_VALUE		12U
#define ODI_REPLAY_REC_WORDS		16U

static inline uint16_t odi_replay_be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t odi_replay_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t odi_replay_word(const uint8_t *rec, unsigned int w)
{
	return odi_replay_be32(rec + ODI_REPLAY_REC_WORDS + 4U * w);
}

/* The fields every record kind shares the rules for: a TABLE record has
 * 1..ODI_REPLAY_BLOB_MAX_WORDS words, zero padding past them and no value;
 * any other record has no table, no words and no padding to speak of.
 */
static int odi_replay_check_payload(const uint8_t *rec, int is_table, const char **why)
{
	uint16_t n_words = odi_replay_be16(rec + ODI_REPLAY_REC_N_WORDS);
	unsigned int w;

	if (is_table) {
		if (n_words == 0 || n_words > ODI_REPLAY_BLOB_MAX_WORDS) {
			*why = "table record word count out of range";
			return -EINVAL;
		}
		if (odi_replay_be32(rec + ODI_REPLAY_REC_VALUE) != 0) {
			*why = "table record carries a value";
			return -EINVAL;
		}
	} else {
		if (n_words != 0 || odi_replay_be16(rec + ODI_REPLAY_REC_TABLE) != 0) {
			*why = "register record carries a table or words";
			return -EINVAL;
		}
	}
	for (w = n_words; w < ODI_REPLAY_BLOB_MAX_WORDS; w++) {
		if (odi_replay_word(rec, w) != 0) {
			*why = "record has nonzero padding past its words";
			return -EINVAL;
		}
	}
	return 0;
}

static int odi_replay_check_modload(const uint8_t *rec, const char **why)
{
	uint8_t kind = rec[ODI_REPLAY_REC_KIND];
	uint8_t category = rec[ODI_REPLAY_REC_CATEGORY];
	uint8_t reg_group = rec[ODI_REPLAY_REC_REG_GROUP];

	if (rec[ODI_REPLAY_REC_VERB] != 0) {
		*why = "modload record carries a verb";
		return -EINVAL;
	}
	if (kind == ODI_SW_MODLOAD_REG) {
		if (category != 0 || reg_group > ODI_REPLAY_BLOB_MODLOAD_MAX_REG_GROUP) {
			*why = "modload register record category or reg_group out of range";
			return -EINVAL;
		}
		return odi_replay_check_payload(rec, 0, why);
	}
	if (kind == ODI_SW_MODLOAD_TABLE) {
		if (category == 0 || category > ODI_REPLAY_BLOB_MODLOAD_MAX_CATEGORY ||
		    reg_group != 0) {
			*why = "modload table record category or reg_group out of range";
			return -EINVAL;
		}
		return odi_replay_check_payload(rec, 1, why);
	}
	/* No SoC-window kind here: odi_switch_init_modload() has no
	 * allowlisted path for one.
	 */
	*why = "modload record kind not register or table";
	return -EINVAL;
}

static int odi_replay_check_sdkinit(const uint8_t *rec, uint8_t prev_verb, const char **why)
{
	uint8_t kind = rec[ODI_REPLAY_REC_KIND];
	uint8_t verb = rec[ODI_REPLAY_REC_VERB];

	if (rec[ODI_REPLAY_REC_CATEGORY] != 0 || rec[ODI_REPLAY_REC_REG_GROUP] != 0) {
		*why = "sdkinit record carries a category or reg_group";
		return -EINVAL;
	}
	if (verb >= ODI_SDKINIT_VERB_ID_COUNT) {
		*why = "sdkinit record verb out of range";
		return -EINVAL;
	}
	if (verb < prev_verb) {
		*why = "sdkinit records not grouped by verb";
		return -EINVAL;
	}
	if (kind != ODI_SW_MODLOAD_REG && kind != ODI_SW_MODLOAD_TABLE &&
	    kind != ODI_SW_MODLOAD_SOC) {
		*why = "sdkinit record kind out of range";
		return -EINVAL;
	}
	return odi_replay_check_payload(rec, kind == ODI_SW_MODLOAD_TABLE, why);
}

static int odi_replay_check_gpon(const uint8_t *rec, const char **why)
{
	uint8_t kind = rec[ODI_REPLAY_REC_KIND];
	uint8_t sn_word = rec[ODI_REPLAY_REC_SN_WORD];

	if (odi_replay_be16(rec + ODI_REPLAY_REC_GPON_RESERVED) != 0) {
		*why = "gpon init record reserved field nonzero";
		return -EINVAL;
	}
	if (kind == ODI_GPON_INIT_REG) {
		if (sn_word != ODI_GPON_INIT_SN_WORD_NONE &&
		    (sn_word < ODI_REPLAY_BLOB_GPON_SN_WORD_MIN ||
		     sn_word > ODI_REPLAY_BLOB_GPON_SN_WORD_MAX)) {
			*why = "gpon init record sn_word out of range";
			return -EINVAL;
		}
		return odi_replay_check_payload(rec, 0, why);
	}
	if (kind == ODI_GPON_INIT_TABLE) {
		if (sn_word != ODI_GPON_INIT_SN_WORD_NONE) {
			*why = "gpon init table record carries an sn_word";
			return -EINVAL;
		}
		return odi_replay_check_payload(rec, 1, why);
	}
	*why = "gpon init record kind not register or table";
	return -EINVAL;
}

int odi_replay_blob_parse(const uint8_t *data, size_t size, enum odi_replay_table table,
			  struct odi_replay_blob *out, const char **why)
{
	const char *unused;
	const uint8_t *records;
	uint32_t count, crc, i;
	uint8_t prev_verb = 0;
	int rc;

	if (!why)
		why = &unused;
	if (!data || size < ODI_REPLAY_BLOB_HEADER_SIZE) {
		*why = "shorter than the header";
		return -EINVAL;
	}
	if (odi_replay_be32(data + ODI_REPLAY_HDR_MAGIC) != ODI_REPLAY_BLOB_MAGIC) {
		*why = "bad magic";
		return -EINVAL;
	}
	if (odi_replay_be16(data + ODI_REPLAY_HDR_VERSION) != ODI_REPLAY_BLOB_VERSION) {
		*why = "unsupported version";
		return -EINVAL;
	}
	if (odi_replay_be16(data + ODI_REPLAY_HDR_TABLE) != (uint16_t)table) {
		*why = "blob is for a different table";
		return -EINVAL;
	}
	if (odi_replay_be16(data + ODI_REPLAY_HDR_HEADER_SIZE) != ODI_REPLAY_BLOB_HEADER_SIZE ||
	    odi_replay_be16(data + ODI_REPLAY_HDR_RECORD_SIZE) != ODI_REPLAY_BLOB_RECORD_SIZE ||
	    odi_replay_be32(data + ODI_REPLAY_HDR_RESERVED) != 0) {
		*why = "bad header or record size";
		return -EINVAL;
	}
	count = odi_replay_be32(data + ODI_REPLAY_HDR_COUNT);
	if (count > ODI_REPLAY_BLOB_MAX_RECORDS ||
	    size != ODI_REPLAY_BLOB_HEADER_SIZE + (size_t)count * ODI_REPLAY_BLOB_RECORD_SIZE) {
		*why = "file size does not match the record count";
		return -EINVAL;
	}
	records = data + ODI_REPLAY_BLOB_HEADER_SIZE;

	crc = odi_replay_crc32_update(~0U, data, ODI_REPLAY_HDR_CRC);
	crc = odi_replay_crc32_update(crc, records, (size_t)count * ODI_REPLAY_BLOB_RECORD_SIZE);
	if ((crc ^ ~0U) != odi_replay_be32(data + ODI_REPLAY_HDR_CRC)) {
		*why = "crc32 mismatch";
		return -EBADMSG;
	}

	for (i = 0; i < count; i++) {
		const uint8_t *rec = records + (size_t)i * ODI_REPLAY_BLOB_RECORD_SIZE;

		switch (table) {
		case ODI_REPLAY_TABLE_SDKINIT:
			rc = odi_replay_check_sdkinit(rec, prev_verb, why);
			prev_verb = rec[ODI_REPLAY_REC_VERB];
			break;
		case ODI_REPLAY_TABLE_MODLOAD:
			rc = odi_replay_check_modload(rec, why);
			break;
		case ODI_REPLAY_TABLE_GPON_INIT:
			rc = odi_replay_check_gpon(rec, why);
			break;
		default:
			*why = "unknown table";
			rc = -EINVAL;
			break;
		}
		if (rc)
			return rc;
	}

	out->records = records;
	out->count = count;
	return 0;
}

static void odi_replay_decode_words(const uint8_t *rec, uint32_t *words)
{
	unsigned int w;

	for (w = 0; w < ODI_REPLAY_BLOB_MAX_WORDS; w++)
		words[w] = odi_replay_word(rec, w);
}

void odi_replay_blob_switch_event(const struct odi_replay_blob *blob, uint32_t i,
				  struct odi_sw_modload_event *e, uint8_t *verb)
{
	const uint8_t *rec = blob->records + (size_t)i * ODI_REPLAY_BLOB_RECORD_SIZE;

	e->kind = rec[ODI_REPLAY_REC_KIND];
	e->category = rec[ODI_REPLAY_REC_CATEGORY];
	e->reg_group = rec[ODI_REPLAY_REC_REG_GROUP];
	e->table = odi_replay_be16(rec + ODI_REPLAY_REC_TABLE);
	e->n_words = odi_replay_be16(rec + ODI_REPLAY_REC_N_WORDS);
	e->offset = odi_replay_be32(rec + ODI_REPLAY_REC_OFFSET);
	e->value = odi_replay_be32(rec + ODI_REPLAY_REC_VALUE);
	odi_replay_decode_words(rec, e->words);
	*verb = rec[ODI_REPLAY_REC_VERB];
}

void odi_replay_blob_gpon_event(const struct odi_replay_blob *blob, uint32_t i,
				struct odi_gpon_init_event *e)
{
	const uint8_t *rec = blob->records + (size_t)i * ODI_REPLAY_BLOB_RECORD_SIZE;

	e->kind = rec[ODI_REPLAY_REC_KIND];
	e->sn_word = rec[ODI_REPLAY_REC_SN_WORD];
	e->table = odi_replay_be16(rec + ODI_REPLAY_REC_TABLE);
	e->n_words = odi_replay_be16(rec + ODI_REPLAY_REC_N_WORDS);
	e->offset = odi_replay_be32(rec + ODI_REPLAY_REC_OFFSET);
	e->value = odi_replay_be32(rec + ODI_REPLAY_REC_VALUE);
	odi_replay_decode_words(rec, e->words);
}
