// SPDX-License-Identifier: GPL-2.0
/*
 * odi_gpon_ploam.c -- codec implementation for odi_gpon_ploam.h. No kernel
 * dependency: plain C, so test/odi_gpon_test.c compiles and runs it with
 * the host cc before it is ever built for the target.
 */
#include "odi_gpon_ploam.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

void odi_gpon_ploam_pack_words(const struct odi_gpon_ploam *msg, uint16_t words[6])
{
	uint8_t wire[ODI_GPON_PLOAM_WIRE_LEN];
	unsigned int i;

	wire[0] = msg->onu_id;
	wire[1] = msg->type;
	memcpy(&wire[2], msg->content, ODI_GPON_PLOAM_CONTENT_LEN);

	/* 2 bytes per word, big-endian: the word packing the downstream FIFO read and the
	 * upstream FIFO write both use in the register captures.
	 */
	for (i = 0; i < 6U; i++)
		words[i] = ((uint16_t)wire[2U * i] << 8) | (uint16_t)wire[2U * i + 1U];
}

void odi_gpon_ploam_unpack_words(const uint16_t words[6], struct odi_gpon_ploam *msg)
{
	uint8_t wire[ODI_GPON_PLOAM_WIRE_LEN];
	unsigned int i;

	for (i = 0; i < 6U; i++) {
		wire[2U * i] = (uint8_t)(words[i] >> 8);
		wire[2U * i + 1U] = (uint8_t)(words[i] & 0xffU);
	}

	msg->onu_id = wire[0];
	msg->type = wire[1];
	memcpy(msg->content, &wire[2], ODI_GPON_PLOAM_CONTENT_LEN);
}

/* ---- Downstream decoders (G.984.3 clause 9.2.3.x) ---- */

void odi_gpon_decode_upstream_overhead(const struct odi_gpon_ploam *msg,
					struct odi_gpon_ds_upstream_overhead *out)
{
	const uint8_t *c = msg->content;

	out->guard_bits = c[0];
	out->type1_preamble_bits = c[1];
	out->type2_preamble_bits = c[2];
	out->type3_pattern = c[3];
	out->delimiter[0] = c[4];
	out->delimiter[1] = c[5];
	out->delimiter[2] = c[6];
	out->preassigned_delay_en = (c[7] >> 5) & 0x1U;
	out->sn_mask_enabled = (c[7] >> 4) & 0x1U;
	out->extra_sn_tx = (c[7] >> 2) & 0x3U;
	out->power_level_mode = c[7] & 0x3U;
	out->preassigned_delay = ((uint16_t)c[8] << 8) | (uint16_t)c[9];
}

void odi_gpon_decode_assign_onu_id(const struct odi_gpon_ploam *msg,
				    struct odi_gpon_ds_assign_onu_id *out)
{
	out->onu_id = msg->content[0];
	memcpy(out->serial_number, &msg->content[1], sizeof(out->serial_number));
}

void odi_gpon_decode_ranging_time(const struct odi_gpon_ploam *msg,
				   struct odi_gpon_ds_ranging_time *out)
{
	const uint8_t *c = msg->content;

	out->protection_path = c[0] & 0x1U;
	out->eqd = ((uint32_t)c[1] << 24) | ((uint32_t)c[2] << 16) |
		   ((uint32_t)c[3] << 8) | (uint32_t)c[4];
}

void odi_gpon_decode_disable_serial_number(const struct odi_gpon_ploam *msg,
					    struct odi_gpon_ds_disable_serial_number *out)
{
	out->code = msg->content[0];
	memcpy(out->serial_number, &msg->content[1], sizeof(out->serial_number));
}

void odi_gpon_decode_encrypted_port_id(const struct odi_gpon_ploam *msg,
					struct odi_gpon_ds_encrypted_port_id *out)
{
	const uint8_t *c = msg->content;

	out->encrypted = c[0] & 0x1U;
	out->valid = (c[0] >> 1) & 0x1U;
	out->port_id = ((uint16_t)c[1] << 4) | ((uint16_t)c[2] >> 4);
}

void odi_gpon_decode_assign_alloc_id(const struct odi_gpon_ploam *msg,
				      struct odi_gpon_ds_assign_alloc_id *out)
{
	const uint8_t *c = msg->content;

	out->alloc_id = ((uint16_t)c[0] << 4) | ((uint16_t)c[1] >> 4);
	out->alloc_id_type = c[2];
}

void odi_gpon_decode_configure_port_id(const struct odi_gpon_ploam *msg,
					struct odi_gpon_ds_configure_port_id *out)
{
	const uint8_t *c = msg->content;

	out->activate = c[0] & 0x1U;
	out->port_id = ((uint16_t)c[1] << 4) | ((uint16_t)c[2] >> 4);
}

void odi_gpon_decode_ber_interval(const struct odi_gpon_ploam *msg,
				   struct odi_gpon_ds_ber_interval *out)
{
	const uint8_t *c = msg->content;

	out->interval_frames = ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) |
				((uint32_t)c[2] << 8) | (uint32_t)c[3];
}

void odi_gpon_decode_key_switching_time(const struct odi_gpon_ploam *msg,
					 struct odi_gpon_ds_key_switching_time *out)
{
	const uint8_t *c = msg->content;

	out->switch_superframe = ((uint32_t)(c[0] & 0x3fU) << 24) | ((uint32_t)c[1] << 16) |
				  ((uint32_t)c[2] << 8) | (uint32_t)c[3];
}

/* ---- Upstream encoders (G.984.3 clause 9.2.4.x) ---- */

static void odi_gpon_ploam_clear(struct odi_gpon_ploam *out, uint8_t onu_id, uint8_t type)
{
	memset(out, 0, sizeof(*out));
	out->onu_id = onu_id;
	out->type = type;
}

void odi_gpon_encode_password(uint8_t onu_id, const uint8_t password[10],
			       struct odi_gpon_ploam *out)
{
	odi_gpon_ploam_clear(out, onu_id, ODI_GPON_US_PASSWORD);
	memcpy(out->content, password, ODI_GPON_PLOAM_CONTENT_LEN);
}

void odi_gpon_encode_encryption_key_fragment(uint8_t onu_id, uint8_t key_index,
					      const uint8_t key[16], unsigned int fragment_index,
					      struct odi_gpon_ploam *out)
{
	unsigned int off = fragment_index * ODI_GPON_KEY_FRAGMENT_BYTES;
	unsigned int n = ODI_GPON_KEY_FRAGMENT_BYTES;

	odi_gpon_ploam_clear(out, onu_id, ODI_GPON_US_ENCRYPTION_KEY);
	out->content[0] = key_index;
	out->content[1] = (uint8_t)fragment_index;

	if (off >= ODI_GPON_AES_KEY_BYTES)
		return;	/* out-of-range fragment index: index bytes only, zero key bytes */
	if (off + n > ODI_GPON_AES_KEY_BYTES)
		n = ODI_GPON_AES_KEY_BYTES - off;	/* last, partial fragment */

	memcpy(&out->content[2], &key[off], n);
}

void odi_gpon_encode_rei(uint8_t onu_id, uint32_t error_count, uint8_t sequence,
			  struct odi_gpon_ploam *out)
{
	odi_gpon_ploam_clear(out, onu_id, ODI_GPON_US_REI);
	out->content[0] = (uint8_t)(error_count >> 24);
	out->content[1] = (uint8_t)(error_count >> 16);
	out->content[2] = (uint8_t)(error_count >> 8);
	out->content[3] = (uint8_t)(error_count & 0xffU);
	out->content[4] = sequence & 0x0fU;
}

void odi_gpon_encode_acknowledge(uint8_t onu_id, uint8_t acked_type,
				  const uint8_t acked_content_9[9], struct odi_gpon_ploam *out)
{
	odi_gpon_ploam_clear(out, onu_id, ODI_GPON_US_ACKNOWLEDGE);
	out->content[0] = acked_type;
	memcpy(&out->content[1], acked_content_9, 9U);
}
