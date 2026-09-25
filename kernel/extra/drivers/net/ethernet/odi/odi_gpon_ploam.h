/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_ploam.h -- downstream/upstream PLOAM message codec for the
 * RTL9602C GPON MAC block, against ITU-T G.984.3 clause 9 (PLOAM message
 * formats), cited by clause number below (see docs/REFERENCES.md for the
 * edition); this is not the author's own recollection for the fields the
 * standard actually defines.
 *
 * Wire shape (register-map fact, not a G.984.3 citation):
 * the hardware FIFO carries a 12-byte software message -- 1 byte ONU-ID,
 * 1 byte message type, 10 bytes of content -- packed into 6 of the 8
 * words the FIFO array declares, 2 content bytes per 16-bit word,
 * big-endian. G.984.3 clause 9.1's own generic message structure
 * (Figure 9-1) is 13 bytes -- the same ONU-ID + Message-ID + 10-byte
 * Data, plus a 1-byte CRC this MAC block's own hardware generates/checks
 * itself (no software-visible CRC field is documented for this block),
 * so the two shapes agree on the 10-byte content this header decodes.
 *
 * ONU-ID (clause 9.1.1): 0-253 once ranged, 0xFF for broadcast to all
 * ONUs and for "no ONU-ID assigned yet" (e.g., a pre-ranging
 * Serial_Number_ONU reply) -- ODI_GPON_ONU_ID_BROADCAST below.
 *
 * Every content-field byte offset below is cited to a G.984.3 clause
 * (9.2.3.x downstream, 9.2.4.x upstream) UNLESS marked otherwise. Two
 * messages this driver's own dispatch still carries a case for, in case an
 * OLT still sends one, have no field definition in this standard text
 * because the current revision deprecated them --
 * Serial_Number_Mask (clause 9.2.3.2) and Configure_VP/VC (clause
 * 9.2.3.7) -- those two structs are this author's own guess, marked
 * "layout per G.984.3, to be confirmed against a live OLT trace" same as
 * before, since the standard itself offers no definition to check them
 * against.
 */
#ifndef ODI_GPON_PLOAM_H
#define ODI_GPON_PLOAM_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* ---- Wire message: ONU-ID + type + 10-byte content ---- */

#define ODI_GPON_PLOAM_CONTENT_LEN	10U
#define ODI_GPON_PLOAM_WIRE_LEN		12U	/* onu_id + type + content, the software struct size */

/* G.984.3 clause 9.1.1: 0-253 once ranged; 0xFF for broadcast, and for
 * "no ONU-ID assigned yet" on a pre-ranging Serial_Number_ONU reply.
 */
#define ODI_GPON_ONU_ID_BROADCAST	0xffU

struct odi_gpon_ploam {
	uint8_t onu_id;
	uint8_t type;
	uint8_t content[ODI_GPON_PLOAM_CONTENT_LEN];
};

/* Not a G.984.3 message type: the standard's own trigger for an ONU to
 * send its Serial_Number_ONU reply in O3 (or O4, for a directed ranging
 * re-request) is a **bandwidth-map grant event** -- Alloc-ID 254 for the
 * broadcast SN request, the target ONU's own Alloc-ID for a directed
 * ranging request (G.984.3 clause 10.2.4 Table 10-1, clause 10.2.5.2) --
 * not a downstream PLOAM message at all. This driver's own FSM event set
 * (odi_gpon_fsm.h) has only ploam-rx/TO1/TO2/LOS/LOS-clear/activate/
 * deactivate, no distinct bandwidth-grant event, so it abstracts that
 * grant as a synthetic message type carried over the ordinary
 * ODI_GPON_EVENT_PLOAM_RX event ("SN request via ranging grant
 * abstraction"). 0x00 is unused by G.984.3 (real message IDs start at
 * 0x01), so it cannot collide with a real message.
 */
#define ODI_GPON_SN_REQUEST_GRANT	0x00U

/* ---- Downstream message IDs (G.984.3 clause 9.2.1/9.2.3, the octet-2
 * bit patterns given in each message's own subclause).
 */
enum odi_gpon_ds_ploam_type {
	ODI_GPON_DS_UPSTREAM_OVERHEAD		= 0x01,	/* clause 9.2.3.1 */
	ODI_GPON_DS_SERIAL_NUMBER_MASK		= 0x02,	/* clause 9.2.3.2 -- deprecated, no content definition */
	ODI_GPON_DS_ASSIGN_ONU_ID		= 0x03,	/* clause 9.2.3.3 */
	ODI_GPON_DS_RANGING_TIME		= 0x04,	/* clause 9.2.3.4 */
	ODI_GPON_DS_DEACTIVATE_ONU_ID		= 0x05,	/* clause 9.2.3.5 */
	ODI_GPON_DS_DISABLE_SERIAL_NUMBER	= 0x06,	/* clause 9.2.3.6 */
	ODI_GPON_DS_CONFIGURE_VP_VC		= 0x07,	/* clause 9.2.3.7 -- deprecated, no content definition */
	ODI_GPON_DS_ENCRYPTED_PORT_ID		= 0x08,	/* clause 9.2.3.8 */
	ODI_GPON_DS_REQUEST_PASSWORD		= 0x09,	/* clause 9.2.3.9 */
	ODI_GPON_DS_ASSIGN_ALLOC_ID		= 0x0a,	/* clause 9.2.3.10 */
	ODI_GPON_DS_NO_MESSAGE			= 0x0b,	/* clause 9.2.3.11 */
	ODI_GPON_DS_POPUP			= 0x0c,	/* clause 9.2.3.12 */
	ODI_GPON_DS_REQUEST_KEY			= 0x0d,	/* clause 9.2.3.13 */
	ODI_GPON_DS_CONFIGURE_PORT_ID		= 0x0e,	/* clause 9.2.3.14 */
	ODI_GPON_DS_PHYSICAL_EQUIPMENT_ERROR	= 0x0f,	/* clause 9.2.3.15 */
	ODI_GPON_DS_CHANGE_POWER_LEVEL		= 0x10,	/* clause 9.2.3.16 */
	ODI_GPON_DS_PST				= 0x11,	/* clause 9.2.3.17 */
	ODI_GPON_DS_BER_INTERVAL		= 0x12,	/* clause 9.2.3.18 */
	ODI_GPON_DS_KEY_SWITCHING_TIME		= 0x13,	/* clause 9.2.3.19 */
	ODI_GPON_DS_EXT_BURST_LENGTH		= 0x14,	/* clause 9.2.3.20 */
	/* Vendor/ZTE-interop variant of Disable_Serial_Number; not a
	 * G.984.3 message ID.
	 */
	ODI_GPON_DS_DISABLE_SERIAL_NUMBER_ZTE	= 0x81,
};

/* ---- Upstream message IDs (G.984.3 clause 9.2.2/9.2.4) ---- */
enum odi_gpon_us_ploam_type {
	ODI_GPON_US_SERIAL_NUMBER_ONU	= 0x01,	/* clause 9.2.4.1 */
	ODI_GPON_US_PASSWORD		= 0x02,	/* clause 9.2.4.2 */
	ODI_GPON_US_DYING_GASP		= 0x03,	/* clause 9.2.4.3 */
	ODI_GPON_US_NO_MESSAGE		= 0x04,	/* clause 9.2.4.4 */
	ODI_GPON_US_ENCRYPTION_KEY	= 0x05,	/* clause 9.2.4.5 */
	ODI_GPON_US_PEE			= 0x06,	/* clause 9.2.4.6, Physical_Equipment_Error */
	ODI_GPON_US_PST			= 0x07,	/* clause 9.2.4.7 */
	ODI_GPON_US_REI			= 0x08,	/* clause 9.2.4.8, Remote Error Indication */
	ODI_GPON_US_ACKNOWLEDGE		= 0x09,	/* clause 9.2.4.9 */
};

/* Disable_Serial_Number content[0] codes (clause 9.2.3.6). */
#define ODI_GPON_DISABLE_SN_ENABLE		0x00U	/* re-enable this serial number */
#define ODI_GPON_DISABLE_SN_ENABLE_ALL		0x0fU	/* re-enable every previously-denied ONU; bytes 1-9 irrelevant */
#define ODI_GPON_DISABLE_SN_DISABLE		0xffU	/* deny this serial number upstream access */

/* Assign_Alloc-ID content[2] Alloc-ID type codes (clause 9.2.3.10). */
#define ODI_GPON_ALLOC_ID_TYPE_GEM		0x01U
#define ODI_GPON_ALLOC_ID_TYPE_DEALLOCATE	0xffU

/* ---- FIFO word pack/unpack (register-map fact) ---- */

/* Packs a 12-byte wire message into 6 16-bit words, 2 bytes per word,
 * big-endian -- the exact shape both the downstream FIFO read and the
 * upstream FIFO write use (a register-map fact, not a
 * G.984.3 one: the standard's own 13-byte wire format, Figure 9-1, is a
 * bit-serial GTC field, not a 16-bit-word FIFO). words[] must hold at
 * least 6 entries; only the low 16 bits of each are meaningful.
 */
void odi_gpon_ploam_pack_words(const struct odi_gpon_ploam *msg, uint16_t words[6]);

/* Inverse of odi_gpon_ploam_pack_words(). */
void odi_gpon_ploam_unpack_words(const uint16_t words[6], struct odi_gpon_ploam *msg);

/* ---- Typed downstream content decoders ----
 *
 * Byte offsets below are content[] indices, i.e. octet-3 of each
 * message's own G.984.3 table (octet 1 = ONU-ID, octet 2 = Message-ID,
 * both already carried by struct odi_gpon_ploam's own onu_id/type
 * fields, not repeated in content[]).
 */

/* Upstream_Overhead (clause 9.2.3.1). Broadcast (onu_id = 0xFF). */
struct odi_gpon_ds_upstream_overhead {
	uint8_t guard_bits;		/* content[0]: number of guard bits */
	uint8_t type1_preamble_bits;	/* content[1]: all-ones preamble bit count */
	uint8_t type2_preamble_bits;	/* content[2]: all-zeroes preamble bit count */
	uint8_t type3_pattern;		/* content[3]: type 3 preamble fill pattern byte */
	uint8_t delimiter[3];		/* content[4..6]: delimiter bytes 1-3 */
	uint8_t preassigned_delay_en;	/* content[7] bit 5 (the e flag): use preassigned_delay below */
	uint8_t sn_mask_enabled;	/* content[7] bit 4 (the m flag): deprecated, shall be 0 (clause 9.2.3.1 Note 5) */
	uint8_t extra_sn_tx;		/* content[7] bits 3:2 (the ss field): deprecated, shall be 0 (Note 6) */
	uint8_t power_level_mode;	/* content[7] bits 1:0 (the pp field): 0=normal, 1=-3dB, 2=-6dB */
	uint16_t preassigned_delay;	/* content[8..9]: pre-assigned delay, 32-byte units */
};
void odi_gpon_decode_upstream_overhead(const struct odi_gpon_ploam *msg,
					struct odi_gpon_ds_upstream_overhead *out);

/* Serial_Number_Mask (clause 9.2.3.2): deprecated in this revision of the
 * standard, which defines no content fields for it any more. This
 * driver's own PLOAM dispatch (odi_gpon_isr.c) has no case for it -- an
 * unverified decoder for it was removed, unused, with no capture ever
 * confirming a content layout; re-add one against a live OLT trace if
 * this message type is ever actually seen.
 */

/* Assign_ONU-ID (clause 9.2.3.3). Broadcast (onu_id = 0xFF). */
struct odi_gpon_ds_assign_onu_id {
	uint8_t onu_id;			/* content[0]: the ONU-ID being assigned */
	uint8_t serial_number[8];	/* content[1..8] */
	/* content[9] unspecified */
};
void odi_gpon_decode_assign_onu_id(const struct odi_gpon_ploam *msg,
				    struct odi_gpon_ds_assign_onu_id *out);

/* Ranging_Time (clause 9.2.3.4). Directed (msg->onu_id names the ONU). */
struct odi_gpon_ds_ranging_time {
	uint8_t protection_path;	/* content[0] bit 0: 0=main path EqD, 1=protection path EqD */
	uint32_t eqd;			/* content[1..4]: equalization delay, bits, MSB first */
	/* content[5..9] unspecified */
};
void odi_gpon_decode_ranging_time(const struct odi_gpon_ploam *msg,
				   struct odi_gpon_ds_ranging_time *out);

/* Deactivate_ONU-ID (clause 9.2.3.5): all 10 content bytes are
 * unspecified -- the targeted (or broadcast) ONU-ID is msg->onu_id
 * itself, not a content field. No decoder needed; dispatch on msg->type
 * and msg->onu_id directly.
 */

/* Disable_Serial_Number (clause 9.2.3.6). Broadcast (onu_id = 0xFF). */
struct odi_gpon_ds_disable_serial_number {
	uint8_t code;			/* content[0]: ODI_GPON_DISABLE_SN_* */
	uint8_t serial_number[8];	/* content[1..8] -- irrelevant when code == ENABLE_ALL */
	/* content[9] unspecified */
};
void odi_gpon_decode_disable_serial_number(const struct odi_gpon_ploam *msg,
					    struct odi_gpon_ds_disable_serial_number *out);

/* Encrypted_Port-ID (clause 9.2.3.8). Directed. */
struct odi_gpon_ds_encrypted_port_id {
	uint8_t valid;		/* content[0] bit 1 (the b flag): 0 = message should be ignored */
	uint8_t encrypted;	/* content[0] bit 0 (the a flag): 1 = encrypted, 0 = not */
	uint16_t port_id;	/* content[1] << 4 | content[2] >> 4, a 12-bit GEM Port-ID */
};
void odi_gpon_decode_encrypted_port_id(const struct odi_gpon_ploam *msg,
					struct odi_gpon_ds_encrypted_port_id *out);

/* Request_Password (clause 9.2.3.9): all 10 content bytes unspecified --
 * a query, triggers an upstream Password reply. No decoder needed;
 * dispatch on msg->type alone.
 */

/* Assign_Alloc-ID (clause 9.2.3.10). Directed. */
struct odi_gpon_ds_assign_alloc_id {
	uint16_t alloc_id;	/* content[0] << 4 | content[1] >> 4, a 12-bit Alloc-ID */
	uint8_t alloc_id_type;	/* content[2]: ODI_GPON_ALLOC_ID_TYPE_* (1=GEM, 255=deallocate) */
};
void odi_gpon_decode_assign_alloc_id(const struct odi_gpon_ploam *msg,
				      struct odi_gpon_ds_assign_alloc_id *out);

/* POPUP (clause 9.2.3.12): all 10 content bytes unspecified -- the
 * targeted (or broadcast, 0xFF) ONU-ID is msg->onu_id itself. No decoder
 * needed; dispatch on msg->type and msg->onu_id directly.
 */

/* Request_Key (clause 9.2.3.13): all 10 content bytes unspecified -- a
 * query, directed via msg->onu_id, triggers an upstream Encryption_Key
 * reply sequence. No decoder needed; dispatch on msg->type alone.
 */

/* Configure_Port-ID (clause 9.2.3.14). Directed. */
struct odi_gpon_ds_configure_port_id {
	uint8_t activate;	/* content[0] bit 0 (the a flag): 1 = activate this Port-ID, 0 = deactivate */
	uint16_t port_id;	/* content[1] << 4 | content[2] >> 4, a 12-bit Port-ID (both directions) */
};
void odi_gpon_decode_configure_port_id(const struct odi_gpon_ploam *msg,
					struct odi_gpon_ds_configure_port_id *out);

/* BER_interval (clause 9.2.3.18). Directed or broadcast (0xFF). */
struct odi_gpon_ds_ber_interval {
	uint32_t interval_frames;	/* content[0..3]: downstream frame count, MSB first */
};
void odi_gpon_decode_ber_interval(const struct odi_gpon_ploam *msg,
				   struct odi_gpon_ds_ber_interval *out);

/* Key_switching_time (clause 9.2.3.19). Directed or broadcast (0xFF).
 * The 30-bit superframe counter is packed 6+8+8+8 bits across content[0..3]
 * (content[0] top 2 bits are reserved/0) -- matches DSK_SWITCH_FRAME
 * own 30-bit SUPERFRAME field width (odi_gpon_hw.h), a useful
 * cross-check between the register map and the standard's own bit count.
 */
struct odi_gpon_ds_key_switching_time {
	uint32_t switch_superframe;	/* the 30-bit value, right-justified in a uint32_t */
};
void odi_gpon_decode_key_switching_time(const struct odi_gpon_ploam *msg,
					 struct odi_gpon_ds_key_switching_time *out);

/* ---- Typed upstream content encoders ----
 *
 * Each builds a full struct odi_gpon_ploam (onu_id/type/content) ready for
 * odi_gpon_ploam_pack_words(). Byte offsets cited to G.984.3 clause
 * 9.2.4.x as for the decoders above.
 */

/* Serial_Number_ONU (clause 9.2.4.1): this driver never sends one -- the
 * hardware transmits it on its own during ranging (G.984.3 clause 9.2.4.1
 * is a one-time autonomous message at O2/O3, before the FSM has anything
 * to say upstream). An unused encoder for it was removed; re-add one if a
 * future need to send this message under software control ever exists.
 */

/* Password (clause 9.2.4.2): the 10-byte password fills the content exactly. */
void odi_gpon_encode_password(uint8_t onu_id, const uint8_t password[10],
			       struct odi_gpon_ploam *out);

/* No unused encoders for Dying_Gasp (clause 9.2.4.3) or No_Message (clause
 * 9.2.4.4): this driver has never had a caller for either, and none is
 * planned; add one back with its own caller if that changes.
 *
 * Encryption_Key (clause 9.2.4.5): content[0] = Key_Index (which key this
 * ONU is reporting -- one key generation per Request_Key cycle in this
 * driver, so always 0 for now), content[1] = Frag_Index, content[2..9] =
 * 8 key bytes for that fragment. A 128-bit (16-byte) AES key needs
 * exactly ODI_GPON_KEY_FRAGMENTS (2) fragments of ODI_GPON_KEY_FRAGMENT_BYTES
 * (8) bytes each (clause 9.2.4.5 Note: "Currently, only two fragments are
 * required for AES-128").
 */
#define ODI_GPON_KEY_FRAGMENT_BYTES	8U
#define ODI_GPON_AES_KEY_BYTES		16U
#define ODI_GPON_KEY_FRAGMENTS	\
	(((ODI_GPON_AES_KEY_BYTES) + (ODI_GPON_KEY_FRAGMENT_BYTES) - 1U) / (ODI_GPON_KEY_FRAGMENT_BYTES))
void odi_gpon_encode_encryption_key_fragment(uint8_t onu_id, uint8_t key_index,
					      const uint8_t key[16], unsigned int fragment_index,
					      struct odi_gpon_ploam *out);

/* No unused encoders for PEE (clause 9.2.4.6, Physical_Equipment_Error) or
 * PST (clause 9.2.4.7, line number plus the K1/K2 APS control bytes,
 * [b-ITU-T G.841]) either, for the same reason.
 *
 * REI (clause 9.2.4.8, Remote Error Indication): the 32-bit downstream
 * bit-error count this ONU observed, plus a 4-bit sequence number
 * incremented on every REI sent.
 */
void odi_gpon_encode_rei(uint8_t onu_id, uint32_t error_count, uint8_t sequence,
			  struct odi_gpon_ploam *out);

/* Acknowledge (clause 9.2.4.9): acked_type is the acknowledged downstream
 * message's own Message-ID; acked_content_9 is that message's own
 * content[0..8] (9 of its 10 content bytes), echoed back per the
 * standard's own DMBYTE1..DMBYTE9 fields.
 */
void odi_gpon_encode_acknowledge(uint8_t onu_id, uint8_t acked_type,
				  const uint8_t acked_content_9[9], struct odi_gpon_ploam *out);

#endif /* ODI_GPON_PLOAM_H */
