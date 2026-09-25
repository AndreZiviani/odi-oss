/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_test.c -- host-side unit test for odi_gpon_ploam.c/odi_gpon_fsm.c,
 * checked against ITU-T G.984.3 clauses 9 and 10 (see docs/REFERENCES.md
 * for the edition and how to fetch a local copy of the standard).
 *
 * Covers: PLOAM wire codec round trips, a full O1-O5 activation scenario
 * asserting the recorded action sequence, TO1 timeout reverting to O2,
 * Deactivate, Disable_Serial_Number to O7 and re-enable, the O6 POPUP
 * state entered by LOS and left by a directed POPUP message, Request_Key
 * producing the 2 Encryption_Key fragments an AES-128 key needs per
 * clause 9.2.4.5, and Key_switching_time scheduling.
 *
 * The FSM own every side effect goes through struct odi_gpon_fsm_ops; this
 * file own "mock" is a plain action recorder (kind + scalar payload +,
 * for send_us_ploam, the full wire message) rather than a register model --
 * there is no odi_gpon_hw.c leaf layer yet for the FSM to drive, so
 * recording the ops calls in order is the whole test contract for now.
 */
#include <stdio.h>
#include <string.h>
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* ---- Action recorder: the test own odi_gpon_fsm_ops implementation ---- */

enum action_kind {
	ACT_SET_STATE,
	ACT_SET_ONU_ID,
	ACT_SET_EQD,
	ACT_SET_UPSTREAM_OVERHEAD,
	ACT_SEND_US_PLOAM,
	ACT_START_TO1,
	ACT_STOP_TO1,
	ACT_START_TO2,
	ACT_STOP_TO2,
	ACT_LASER_ENABLE,
	ACT_LOAD_KEY,
	ACT_SWITCH_KEY,
	ACT_LINK_FORCE_UP,
	ACT_FLUSH_US_PLOAM_BUF,
};

struct action {
	enum action_kind kind;
	uint32_t a;
	uint32_t b;
	struct odi_gpon_ploam ploam;	/* valid for ACT_SEND_US_PLOAM */
	uint8_t key[16];		/* valid for ACT_LOAD_KEY */
};

#define MAX_ACTIONS 256

struct recorder {
	struct action log[MAX_ACTIONS];
	unsigned int n;
};

static void rec_push(struct recorder *r, enum action_kind kind, uint32_t a, uint32_t b,
		      const struct odi_gpon_ploam *ploam, const uint8_t *key)
{
	struct action *act;

	if (r->n >= MAX_ACTIONS) {
		fprintf(stderr, "recorder overflow\n");
		failures++;
		return;
	}
	act = &r->log[r->n++];
	memset(act, 0, sizeof(*act));
	act->kind = kind;
	act->a = a;
	act->b = b;
	if (ploam)
		act->ploam = *ploam;
	if (key)
		memcpy(act->key, key, 16U);
}

static void rec_reset(struct recorder *r)
{
	r->n = 0;
}

static void cb_set_state(void *ctx, enum odi_gpon_state state)
{
	rec_push((struct recorder *)ctx, ACT_SET_STATE, (uint32_t)state, 0, NULL, NULL);
}
static void cb_set_onu_id(void *ctx, uint8_t onu_id)
{
	rec_push((struct recorder *)ctx, ACT_SET_ONU_ID, onu_id, 0, NULL, NULL);
}
static void cb_set_eqd(void *ctx, uint32_t eqd_bits)
{
	rec_push((struct recorder *)ctx, ACT_SET_EQD, eqd_bits, 0, NULL, NULL);
}
static void cb_set_upstream_overhead(void *ctx, uint8_t guard_bits, uint8_t type1_preamble_bits,
				      uint8_t type2_preamble_bits, uint8_t type3_pattern,
				      const uint8_t delimiter[3], uint16_t preassigned_delay,
				      uint8_t power_level_mode)
{
	/* Packs enough of the fields to assert on without a dedicated
	 * struct field per argument: a = guard_bits:type1:type2:type3
	 * (8 bits each), b = preassigned_delay:power_level_mode. delimiter
	 * is intentionally not packed here (not asserted on by name below).
	 */
	uint32_t a = ((uint32_t)guard_bits << 24) | ((uint32_t)type1_preamble_bits << 16) |
		     ((uint32_t)type2_preamble_bits << 8) | (uint32_t)type3_pattern;
	uint32_t b = ((uint32_t)preassigned_delay << 8) | (uint32_t)power_level_mode;

	(void)delimiter;
	rec_push((struct recorder *)ctx, ACT_SET_UPSTREAM_OVERHEAD, a, b, NULL, NULL);
}
static void cb_send_us_ploam(void *ctx, const struct odi_gpon_ploam *msg)
{
	rec_push((struct recorder *)ctx, ACT_SEND_US_PLOAM, msg->type, msg->onu_id, msg, NULL);
}
static void cb_start_to1(void *ctx)
{
	rec_push((struct recorder *)ctx, ACT_START_TO1, 0, 0, NULL, NULL);
}
static void cb_stop_to1(void *ctx)
{
	rec_push((struct recorder *)ctx, ACT_STOP_TO1, 0, 0, NULL, NULL);
}
static void cb_start_to2(void *ctx)
{
	rec_push((struct recorder *)ctx, ACT_START_TO2, 0, 0, NULL, NULL);
}
static void cb_stop_to2(void *ctx)
{
	rec_push((struct recorder *)ctx, ACT_STOP_TO2, 0, 0, NULL, NULL);
}
static void cb_laser_enable(void *ctx, int enable)
{
	rec_push((struct recorder *)ctx, ACT_LASER_ENABLE, (uint32_t)enable, 0, NULL, NULL);
}
static void cb_load_key(void *ctx, unsigned int slot, const uint8_t key[16])
{
	rec_push((struct recorder *)ctx, ACT_LOAD_KEY, (uint32_t)slot, 0, NULL, key);
}
static void cb_switch_key(void *ctx, uint32_t switch_superframe)
{
	rec_push((struct recorder *)ctx, ACT_SWITCH_KEY, switch_superframe, 0, NULL, NULL);
}
static void cb_link_force_up(void *ctx, int up)
{
	rec_push((struct recorder *)ctx, ACT_LINK_FORCE_UP, (uint32_t)up, 0, NULL, NULL);
}
static void cb_flush_us_ploam_buf(void *ctx)
{
	rec_push((struct recorder *)ctx, ACT_FLUSH_US_PLOAM_BUF, 0, 0, NULL, NULL);
}

static const struct odi_gpon_fsm_ops test_ops = {
	.set_state = cb_set_state,
	.set_onu_id = cb_set_onu_id,
	.set_eqd = cb_set_eqd,
	.set_upstream_overhead = cb_set_upstream_overhead,
	.send_us_ploam = cb_send_us_ploam,
	.start_to1 = cb_start_to1,
	.stop_to1 = cb_stop_to1,
	.start_to2 = cb_start_to2,
	.stop_to2 = cb_stop_to2,
	.laser_enable = cb_laser_enable,
	.load_key = cb_load_key,
	.switch_key = cb_switch_key,
	.link_force_up = cb_link_force_up,
	.flush_us_ploam_buf = cb_flush_us_ploam_buf,
};

/* Finds the index of the first action of the given kind at or after
 * `from`, or -1 if none. Lets a test assert relative order without
 * hard-coding absolute action indices for every intervening call.
 */
static int find_action(const struct recorder *r, unsigned int from, enum action_kind kind)
{
	unsigned int i;

	for (i = from; i < r->n; i++)
		if (r->log[i].kind == kind)
			return (int)i;
	return -1;
}

/* ---- DS wire-message builders, matching each message own G.984.3
 * clause 9.2.3.x content layout (content[] index = octet-3, since octet 1
 * is the wire onu_id field and octet 2 the wire type field, both already
 * struct odi_gpon_ploam members). Serial_Number_Mask has no such clause
 * to follow (deprecated, content undefined by the standard) -- not built
 * here since this pass FSM does not act on it either.
 */

static struct odi_gpon_ploam make_ds(uint8_t onu_id, uint8_t type)
{
	struct odi_gpon_ploam m;

	memset(&m, 0, sizeof(m));
	m.onu_id = onu_id;
	m.type = type;
	return m;
}

/* Not a real G.984.3 message -- see ODI_GPON_SN_REQUEST_GRANT own comment
 * in odi_gpon_ploam.h. Kept only to assert that this FSM no longer acts
 * on it (odi_gpon_fsm.c own header comment, "the Serial_Number_ONU reply
 * is NOT sent by this FSM at all" -- hardware answers it autonomously,
 * with zero software involvement per grant).
 */
static struct odi_gpon_ploam ds_sn_request_grant(void)
{
	return make_ds(0, ODI_GPON_SN_REQUEST_GRANT);
}

static struct odi_gpon_ploam ds_upstream_overhead(void)
{
	struct odi_gpon_ploam m = make_ds(ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_UPSTREAM_OVERHEAD);

	m.content[0] = 8U;	/* guard bits */
	m.content[1] = 4U;	/* type1 (all-ones) preamble bits */
	m.content[2] = 4U;	/* type2 (all-zeroes) preamble bits */
	m.content[3] = 0x55U;	/* type3 pattern byte */
	m.content[4] = 0xb5U;	/* delimiter byte 1 */
	m.content[5] = 0x98U;	/* delimiter byte 2 */
	m.content[6] = 0x30U;	/* delimiter byte 3 */
	m.content[7] = 0x20U;	/* xxemsspp: e=1 (use pre-assigned delay), m=0, ss=0, pp=0 */
	m.content[8] = 0x00U;	/* pre-assigned delay MSB */
	m.content[9] = 0x10U;	/* pre-assigned delay LSB */
	return m;
}

static struct odi_gpon_ploam ds_assign_onu_id(uint8_t onu_id, const uint8_t sn[8])
{
	struct odi_gpon_ploam m = make_ds(ODI_GPON_ONU_ID_BROADCAST, ODI_GPON_DS_ASSIGN_ONU_ID);

	m.content[0] = onu_id;
	memcpy(&m.content[1], sn, 8U);
	return m;
}

static struct odi_gpon_ploam ds_ranging_time(uint8_t onu_id, uint32_t eqd)
{
	struct odi_gpon_ploam m = make_ds(onu_id, ODI_GPON_DS_RANGING_TIME);

	m.content[0] = 0U;	/* main path */
	m.content[1] = (uint8_t)(eqd >> 24);
	m.content[2] = (uint8_t)(eqd >> 16);
	m.content[3] = (uint8_t)(eqd >> 8);
	m.content[4] = (uint8_t)(eqd & 0xffU);
	return m;
}

static struct odi_gpon_ploam ds_deactivate_onu_id(uint8_t onu_id)
{
	return make_ds(onu_id, ODI_GPON_DS_DEACTIVATE_ONU_ID);	/* content unspecified, clause 9.2.3.5 */
}

static struct odi_gpon_ploam ds_disable_serial_number(uint8_t code, const uint8_t sn[8])
{
	struct odi_gpon_ploam m = make_ds(ODI_GPON_ONU_ID_BROADCAST,
					  ODI_GPON_DS_DISABLE_SERIAL_NUMBER);

	m.content[0] = code;
	memcpy(&m.content[1], sn, 8U);
	return m;
}

static struct odi_gpon_ploam ds_popup(uint8_t onu_id)
{
	return make_ds(onu_id, ODI_GPON_DS_POPUP);	/* content unspecified, clause 9.2.3.12 */
}

static struct odi_gpon_ploam ds_request_key(uint8_t onu_id)
{
	return make_ds(onu_id, ODI_GPON_DS_REQUEST_KEY);	/* content unspecified, clause 9.2.3.13 */
}

static struct odi_gpon_ploam ds_key_switching_time(uint8_t onu_id, uint32_t ssf30)
{
	struct odi_gpon_ploam m = make_ds(onu_id, ODI_GPON_DS_KEY_SWITCHING_TIME);

	m.content[0] = (uint8_t)((ssf30 >> 24) & 0x3fU);
	m.content[1] = (uint8_t)(ssf30 >> 16);
	m.content[2] = (uint8_t)(ssf30 >> 8);
	m.content[3] = (uint8_t)(ssf30 & 0xffU);
	return m;
}

/* ---- Codec round trips ---- */

static void test_wire_pack_unpack_roundtrip(void)
{
	struct odi_gpon_ploam msg, back;
	uint16_t words[6];
	unsigned int i;

	msg.onu_id = 0xa5U;
	msg.type = ODI_GPON_DS_ASSIGN_ONU_ID;
	for (i = 0; i < ODI_GPON_PLOAM_CONTENT_LEN; i++)
		msg.content[i] = (uint8_t)(0x10U + i);

	odi_gpon_ploam_pack_words(&msg, words);

	/* Big-endian, 2 bytes per word: word[0] = onu_id:type. */
	CHECK(words[0] == ((uint16_t)msg.onu_id << 8 | msg.type),
	      "word 0 packs onu_id (high byte) and type (low byte)");
	CHECK(words[1] == ((uint16_t)msg.content[0] << 8 | msg.content[1]),
	      "word 1 packs content bytes 0-1 big-endian");

	odi_gpon_ploam_unpack_words(words, &back);
	CHECK(back.onu_id == msg.onu_id, "unpack restores onu_id");
	CHECK(back.type == msg.type, "unpack restores type");
	CHECK(memcmp(back.content, msg.content, ODI_GPON_PLOAM_CONTENT_LEN) == 0,
	      "unpack restores content bytes");
}

static void test_ds_decoders(void)
{
	static const uint8_t sn[8] = {0x01, 0x02, 0x03, 0x04, 0x41, 0x42, 0x43, 0x44};
	struct odi_gpon_ploam msg;
	struct odi_gpon_ds_assign_onu_id assign;
	struct odi_gpon_ds_ranging_time ranging;
	struct odi_gpon_ds_disable_serial_number dsn;
	struct odi_gpon_ds_key_switching_time kst;
	struct odi_gpon_ds_upstream_overhead boh;

	msg = ds_upstream_overhead();
	odi_gpon_decode_upstream_overhead(&msg, &boh);
	CHECK(boh.guard_bits == 8U, "upstream_overhead decodes guard_bits");
	CHECK(boh.type3_pattern == 0x55U, "upstream_overhead decodes the type3 pattern byte");
	CHECK(boh.preassigned_delay_en == 1U, "upstream_overhead decodes the e flag bit");
	CHECK(boh.preassigned_delay == 0x0010U, "upstream_overhead decodes the 16-bit pre-assigned delay");

	msg = ds_assign_onu_id(0x07U, sn);
	odi_gpon_decode_assign_onu_id(&msg, &assign);
	CHECK(assign.onu_id == 0x07U, "assign_onu_id decodes the assigned ID from content[0]");
	CHECK(memcmp(assign.serial_number, sn, 8U) == 0,
	      "assign_onu_id decodes the serial number from content[1..8]");

	msg = ds_ranging_time(0x07U, 0x001a2b3cU);
	odi_gpon_decode_ranging_time(&msg, &ranging);
	CHECK(ranging.protection_path == 0U, "ranging_time decodes the main/protection path bit");
	CHECK(ranging.eqd == 0x001a2b3cU, "ranging_time decodes the 32-bit eqd value");

	msg = ds_disable_serial_number(ODI_GPON_DISABLE_SN_DISABLE, sn);
	odi_gpon_decode_disable_serial_number(&msg, &dsn);
	CHECK(dsn.code == ODI_GPON_DISABLE_SN_DISABLE, "disable_serial_number decodes the code byte");
	CHECK(memcmp(dsn.serial_number, sn, 8U) == 0,
	      "disable_serial_number decodes the serial number from content[1..8]");

	msg = ds_key_switching_time(0x07U, 0x3fffffffU);
	odi_gpon_decode_key_switching_time(&msg, &kst);
	CHECK(kst.switch_superframe == 0x3fffffffU,
	      "key_switching_time decodes the full 30-bit superframe count");
}

static void test_us_encoders(void)
{
	static const uint8_t pw[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
	static const uint8_t key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
	struct odi_gpon_ploam msg;
	unsigned int i;
	uint8_t rebuilt[16];

	odi_gpon_encode_password(0x07U, pw, &msg);
	CHECK(msg.type == ODI_GPON_US_PASSWORD, "password encodes the right type");
	CHECK(memcmp(msg.content, pw, ODI_GPON_PLOAM_CONTENT_LEN) == 0,
	      "password content carries all 10 bytes");

	/* Clause 9.2.4.5 Note: "Currently, only two fragments are required
	 * for AES-128" -- 8 key bytes per fragment, 16/8 = 2.
	 */
	CHECK(ODI_GPON_KEY_FRAGMENTS == 2U, "a 16-byte AES-128 key needs 2 fragments of 8 bytes");
	memset(rebuilt, 0, sizeof(rebuilt));
	for (i = 0; i < ODI_GPON_KEY_FRAGMENTS; i++) {
		unsigned int off = i * ODI_GPON_KEY_FRAGMENT_BYTES;

		odi_gpon_encode_encryption_key_fragment(0x07U, 0U, key, i, &msg);
		CHECK(msg.type == ODI_GPON_US_ENCRYPTION_KEY, "encryption_key encodes the right type");
		CHECK(msg.content[0] == 0U, "encryption_key content[0] carries Key_Index");
		CHECK(msg.content[1] == i, "encryption_key content[1] carries Frag_Index");
		memcpy(&rebuilt[off], &msg.content[2], ODI_GPON_KEY_FRAGMENT_BYTES);
	}
	CHECK(memcmp(rebuilt, key, 16U) == 0, "the 2 fragments reassemble the original 16-byte key");
}

/* ---- FSM scenarios ---- */

static const uint8_t test_sn[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
static const uint8_t test_pw[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
static const uint8_t test_key[16] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};
#define TEST_ASSIGNED_ONU_ID	0x2aU

static void fsm_setup(struct odi_gpon_fsm *fsm, struct recorder *rec)
{
	odi_gpon_fsm_init(fsm);
	odi_gpon_fsm_set_serial_number(fsm, test_sn);
	odi_gpon_fsm_set_password(fsm, test_pw);
	odi_gpon_fsm_set_aes_key(fsm, test_key);
	rec_reset(rec);
}

/* Drives fsm from O1 all the way to O5, per G.984.3 Table 10-1: LOS_CLEAR
 * (O1->O2), Upstream_Overhead (O2->O3, starts TO1), the bandwidth-map
 * SN-request grant abstraction (O3, produces nothing -- see
 * ds_sn_request_grant() own comment), Assign_ONU-ID (O3->O4), Ranging_Time
 * (O4->O5).
 */
static void drive_to_o5(struct odi_gpon_fsm *fsm, struct recorder *rec)
{
	struct odi_gpon_ploam msg;

	odi_gpon_fsm_handle_event(fsm, &test_ops, rec, ODI_GPON_EVENT_ACTIVATE, NULL);
	odi_gpon_fsm_handle_event(fsm, &test_ops, rec, ODI_GPON_EVENT_LOS_CLEAR, NULL);

	msg = ds_upstream_overhead();
	odi_gpon_fsm_handle_event(fsm, &test_ops, rec, ODI_GPON_EVENT_PLOAM_RX, &msg);

	/* The SN-request grant is fed here too, asserted (below) to produce
	 * no action at all -- hardware answers it on its own now, not this
	 * FSM (see ds_sn_request_grant() own comment).
	 */
	msg = ds_sn_request_grant();
	odi_gpon_fsm_handle_event(fsm, &test_ops, rec, ODI_GPON_EVENT_PLOAM_RX, &msg);

	msg = ds_assign_onu_id(TEST_ASSIGNED_ONU_ID, test_sn);
	odi_gpon_fsm_handle_event(fsm, &test_ops, rec, ODI_GPON_EVENT_PLOAM_RX, &msg);

	msg = ds_ranging_time(TEST_ASSIGNED_ONU_ID, 0x00050001U);	/* an arbitrary raw eqd bit-delay */
	odi_gpon_fsm_handle_event(fsm, &test_ops, rec, ODI_GPON_EVENT_PLOAM_RX, &msg);
}

static void test_activation_scenario(void)
{
	struct odi_gpon_fsm fsm;
	struct recorder rec;
	int i_overhead, i_to1_start, i_o3, i_onu_id, i_o4;
	int i_stop_to1, i_eqd, i_flush, i_o5, i_link_up;

	fsm_setup(&fsm, &rec);
	drive_to_o5(&fsm, &rec);

	CHECK(fsm.state == ODI_GPON_STATE_O5, "activation scenario ends in O5");
	CHECK(fsm.onu_id == TEST_ASSIGNED_ONU_ID, "the OLT-assigned ONU-ID stuck");

	/* Upstream_Overhead: set_upstream_overhead, start_to1, => O3, all
	 * from the one message (G.984.3 Table 10-1 own Standby row).
	 */
	i_overhead = find_action(&rec, 0, ACT_SET_UPSTREAM_OVERHEAD);
	CHECK(i_overhead >= 0, "Upstream_Overhead produced a set_upstream_overhead action");
	i_to1_start = find_action(&rec, (unsigned int)(i_overhead + 1), ACT_START_TO1);
	i_o3 = find_action(&rec, (unsigned int)(i_overhead + 1), ACT_SET_STATE);
	CHECK(i_to1_start >= 0 && i_o3 >= 0 && i_to1_start < i_o3,
	      "Upstream_Overhead starts TO1 before the O2->O3 transition");
	CHECK(rec.log[i_o3].a == ODI_GPON_STATE_O3, "Upstream_Overhead own transition sets state O3");

	/* The SN-request grant abstraction produces nothing at all now --
	 * hardware answers it autonomously (this file own header comment,
	 * odi_gpon_fsm.c own header comment): the very next action after the
	 * O2->O3 transition is Assign_ONU-ID own set_onu_id, with nothing
	 * (in particular no ACT_SEND_US_PLOAM) from the grant in between.
	 */
	CHECK((unsigned int)(i_o3 + 1) < rec.n && rec.log[i_o3 + 1].kind == ACT_SET_ONU_ID,
	      "the SN-request grant produces no action (hardware answers it, not this FSM)");

	/* Assign_ONU-ID: set_onu_id then => O4 (this driver own documented ordering). */
	i_onu_id = find_action(&rec, (unsigned int)(i_o3 + 1), ACT_SET_ONU_ID);
	CHECK(i_onu_id >= 0 && rec.log[i_onu_id].a == TEST_ASSIGNED_ONU_ID,
	      "Assign_ONU-ID writes the assigned ID");
	i_o4 = find_action(&rec, (unsigned int)(i_onu_id + 1), ACT_SET_STATE);
	CHECK(i_o4 >= 0 && rec.log[i_o4].a == ODI_GPON_STATE_O4,
	      "the ONU-ID write is followed by the O3->O4 transition");

	/* Ranging_Time: stop_to1, set_eqd, flush, => O5, link up, in order. */
	i_stop_to1 = find_action(&rec, (unsigned int)(i_o4 + 1), ACT_STOP_TO1);
	i_eqd = find_action(&rec, (unsigned int)(i_o4 + 1), ACT_SET_EQD);
	i_flush = find_action(&rec, (unsigned int)(i_o4 + 1), ACT_FLUSH_US_PLOAM_BUF);
	i_o5 = find_action(&rec, (unsigned int)(i_o4 + 1), ACT_SET_STATE);
	i_link_up = find_action(&rec, (unsigned int)(i_o4 + 1), ACT_LINK_FORCE_UP);

	CHECK(i_stop_to1 >= 0 && i_eqd >= 0 && i_flush >= 0 && i_o5 >= 0 && i_link_up >= 0,
	      "Ranging_Time produces stop_to1, set_eqd, flush, set_state(O5) and link_force_up");
	CHECK(i_stop_to1 < i_eqd && i_eqd < i_flush && i_flush < i_o5 && i_o5 < i_link_up,
	      "Ranging_Time own O4->O5 actions run in this driver own documented order");
	CHECK(rec.log[i_o5].a == ODI_GPON_STATE_O5, "the O4->O5 transition sets state O5");
	CHECK(rec.log[i_link_up].a == 1U, "the port link is forced up entering O5");
	CHECK(rec.log[i_eqd].a == 0x00050001U,
	      "set_eqd receives the raw ranging-time delay bits, unsplit (odi_gpon_hw.c splits it)");
}

static void test_to1_timeout_reverts(void)
{
	struct odi_gpon_fsm fsm;
	struct recorder rec;
	struct odi_gpon_ploam msg;
	int i_stop, i_state;

	fsm_setup(&fsm, &rec);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_ACTIVATE, NULL);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_LOS_CLEAR, NULL);
	msg = ds_upstream_overhead();
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_PLOAM_RX, &msg);
	CHECK(fsm.state == ODI_GPON_STATE_O3, "setup reaches O3 before the O3 timeout test");

	rec_reset(&rec);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_TO1_EXPIRE, NULL);

	CHECK(fsm.state == ODI_GPON_STATE_O2, "TO1 expiry in O3 reverts to O2 (Table 10-1)");
	i_stop = find_action(&rec, 0, ACT_STOP_TO1);
	i_state = find_action(&rec, 0, ACT_SET_STATE);
	CHECK(i_stop >= 0, "TO1 expiry stops the TO1 timer");
	CHECK(i_state >= 0 && rec.log[i_state].a == ODI_GPON_STATE_O2,
	      "TO1 expiry writes state O2");

	/* Re-enter O3, then reach O4, then expire TO1 again there. */
	msg = ds_upstream_overhead();
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_PLOAM_RX, &msg);
	msg = ds_assign_onu_id(TEST_ASSIGNED_ONU_ID, test_sn);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_PLOAM_RX, &msg);
	CHECK(fsm.state == ODI_GPON_STATE_O4, "setup reaches O4 before the O4 timeout test");

	rec_reset(&rec);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_TO1_EXPIRE, NULL);
	CHECK(fsm.state == ODI_GPON_STATE_O2, "TO1 expiry in O4 also reverts to O2 (Table 10-1)");
}

static void test_deactivate(void)
{
	struct odi_gpon_fsm fsm;
	struct recorder rec;
	int i_link_down, i_laser_off, i_onu_id, i_state;

	fsm_setup(&fsm, &rec);
	drive_to_o5(&fsm, &rec);
	CHECK(fsm.state == ODI_GPON_STATE_O5, "setup reaches O5 before the deactivate test");

	rec_reset(&rec);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_DEACTIVATE, NULL);

	/* Clause 9.2.3.5 own "Effect of receipt": laser off, ONU-ID
	 * discarded, moves to Standby (O2) -- not O1.
	 */
	CHECK(fsm.state == ODI_GPON_STATE_O2, "deactivate from O5 forces O2 (Standby), not O1");
	CHECK(fsm.onu_id == 0U, "deactivate clears the ONU-ID");
	i_link_down = find_action(&rec, 0, ACT_LINK_FORCE_UP);
	i_laser_off = find_action(&rec, 0, ACT_LASER_ENABLE);
	i_onu_id = find_action(&rec, 0, ACT_SET_ONU_ID);
	i_state = find_action(&rec, 0, ACT_SET_STATE);
	CHECK(i_link_down >= 0 && rec.log[i_link_down].a == 0U,
	      "deactivate from O5 forces the port link down");
	CHECK(i_laser_off >= 0 && rec.log[i_laser_off].a == 0U, "deactivate switches the laser off");
	CHECK(i_onu_id >= 0 && rec.log[i_onu_id].a == ODI_GPON_ONU_ID_BROADCAST,
	      "deactivate writes the broadcast ID to hardware, not a bare 0"
	      " (every capture shows 0xff, never 0x00, as the unassigned ONU-ID)");
	CHECK(i_state >= 0 && rec.log[i_state].a == ODI_GPON_STATE_O2,
	      "deactivate writes state O2");

	/* A Deactivate_ONU-ID PLOAM addressed to this ONU does the same
	 * thing from O5.
	 */
	fsm_setup(&fsm, &rec);
	drive_to_o5(&fsm, &rec);
	rec_reset(&rec);
	{
		struct odi_gpon_ploam msg = ds_deactivate_onu_id(TEST_ASSIGNED_ONU_ID);

		odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_PLOAM_RX, &msg);
	}
	CHECK(fsm.state == ODI_GPON_STATE_O2, "a Deactivate_ONU-ID PLOAM also forces O2");

	/* ...but is a no-op from O2/O3 (Table 10-1 shows dashes there). */
	fsm_setup(&fsm, &rec);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_ACTIVATE, NULL);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_LOS_CLEAR, NULL);
	rec_reset(&rec);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_DEACTIVATE, NULL);
	CHECK(fsm.state == ODI_GPON_STATE_O2, "deactivate from O2 is a no-op (Table 10-1 dash)");
	CHECK(rec.n == 0, "deactivate from O2 produces no actions at all");
}

static void test_disable_serial_number_to_o7_and_reenable(void)
{
	struct odi_gpon_fsm fsm;
	struct recorder rec;
	struct odi_gpon_ploam msg;
	int i_laser_off, i_state_o7, i_laser_on, i_state_o2;

	fsm_setup(&fsm, &rec);
	drive_to_o5(&fsm, &rec);
	rec_reset(&rec);

	msg = ds_disable_serial_number(ODI_GPON_DISABLE_SN_DISABLE, test_sn);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_PLOAM_RX, &msg);

	CHECK(fsm.state == ODI_GPON_STATE_O7, "Disable_Serial_Number(disable) forces O7");
	i_laser_off = find_action(&rec, 0, ACT_LASER_ENABLE);
	i_state_o7 = find_action(&rec, 0, ACT_SET_STATE);
	CHECK(i_laser_off >= 0 && rec.log[i_laser_off].a == 0U,
	      "entering O7 disables the laser");
	CHECK(i_state_o7 >= 0 && rec.log[i_state_o7].a == ODI_GPON_STATE_O7,
	      "entering O7 writes state O7");

	rec_reset(&rec);
	msg = ds_disable_serial_number(ODI_GPON_DISABLE_SN_ENABLE, test_sn);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_PLOAM_RX, &msg);

	CHECK(fsm.state == ODI_GPON_STATE_O2, "Disable_Serial_Number(enable) from O7 re-enables to O2");
	i_laser_on = find_action(&rec, 0, ACT_LASER_ENABLE);
	i_state_o2 = find_action(&rec, 0, ACT_SET_STATE);
	CHECK(i_laser_on >= 0 && rec.log[i_laser_on].a == 1U, "re-enable turns the laser back on");
	CHECK(i_state_o2 >= 0 && rec.log[i_state_o2].a == ODI_GPON_STATE_O2,
	      "re-enable writes state O2");
}

static void test_popup_to_o6_and_back(void)
{
	struct odi_gpon_fsm fsm;
	struct recorder rec;
	struct odi_gpon_ploam msg;
	int i_to2_start, i_o6, i_to2_stop, i_o5;

	/* G.984.3 Table 10-1: LOS/LOF from Operation (O5) is what enters
	 * POPUP (O6), not a POPUP PLOAM -- a POPUP message is what the OLT
	 * sends to ONUs already sitting in O6 to bring them back out.
	 */
	fsm_setup(&fsm, &rec);
	drive_to_o5(&fsm, &rec);
	rec_reset(&rec);

	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_LOS, NULL);

	CHECK(fsm.state == ODI_GPON_STATE_O6, "LOS from O5 enters O6 (POPUP state)");
	i_to2_start = find_action(&rec, 0, ACT_START_TO2);
	i_o6 = find_action(&rec, 0, ACT_SET_STATE);
	CHECK(i_to2_start >= 0, "entering O6 starts TO2");
	CHECK(i_o6 >= 0 && rec.log[i_o6].a == ODI_GPON_STATE_O6, "entering O6 writes state O6");

	/* A directed POPUP message (this ONU own ID) returns straight to
	 * Operation (O5), keeping EqD/ONU-ID/Alloc-IDs.
	 */
	rec_reset(&rec);
	msg = ds_popup(TEST_ASSIGNED_ONU_ID);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_PLOAM_RX, &msg);

	CHECK(fsm.state == ODI_GPON_STATE_O5, "a directed POPUP from O6 returns to O5");
	i_to2_stop = find_action(&rec, 0, ACT_STOP_TO2);
	i_o5 = find_action(&rec, 0, ACT_SET_STATE);
	CHECK(i_to2_stop >= 0, "leaving O6 via a directed POPUP stops TO2");
	CHECK(i_o5 >= 0 && rec.log[i_o5].a == ODI_GPON_STATE_O5, "leaving O6 writes state O5");

	/* A broadcast POPUP (onu_id = 0xFF) instead sends O6 to O4 (Ranging),
	 * starting TO1.
	 */
	fsm_setup(&fsm, &rec);
	drive_to_o5(&fsm, &rec);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_LOS, NULL);
	CHECK(fsm.state == ODI_GPON_STATE_O6, "setup reaches O6 before the broadcast-POPUP test");
	rec_reset(&rec);
	msg = ds_popup(ODI_GPON_ONU_ID_BROADCAST);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_PLOAM_RX, &msg);
	CHECK(fsm.state == ODI_GPON_STATE_O4, "a broadcast POPUP from O6 moves to O4 (Ranging)");
	CHECK(find_action(&rec, 0, ACT_START_TO1) >= 0, "the O6->O4 broadcast-POPUP path starts TO1");

	/* TO2 expiry (failed to reacquire sync in time) falls all the way
	 * back to O1, not O5 (clause 10.2.2 f)).
	 */
	fsm_setup(&fsm, &rec);
	drive_to_o5(&fsm, &rec);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_LOS, NULL);
	CHECK(fsm.state == ODI_GPON_STATE_O6, "setup reaches O6 before the TO2 expiry test");
	rec_reset(&rec);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_TO2_EXPIRE, NULL);
	CHECK(fsm.state == ODI_GPON_STATE_O1, "TO2 expiry in O6 falls back to O1, not O5");
}

static void test_request_key_two_fragments(void)
{
	struct odi_gpon_fsm fsm;
	struct recorder rec;
	struct odi_gpon_ploam msg;
	unsigned int i, count = 0, expect_idx = 0;
	uint8_t rebuilt[16];
	int i_load = -1;

	fsm_setup(&fsm, &rec);
	drive_to_o5(&fsm, &rec);
	rec_reset(&rec);

	msg = ds_request_key(TEST_ASSIGNED_ONU_ID);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_PLOAM_RX, &msg);

	CHECK(fsm.state == ODI_GPON_STATE_O5, "Request_Key does not change state");

	/* Each of the 2 fragments sent 3x, alternating (0,1,0,1,0,1) -- the
	 * observed order, not the 2-total order this driver sent before this
	 * pass.
	 */
	memset(rebuilt, 0, sizeof(rebuilt));
	for (i = 0; i < rec.n; i++) {
		if (rec.log[i].kind == ACT_SEND_US_PLOAM &&
		    rec.log[i].ploam.type == ODI_GPON_US_ENCRYPTION_KEY) {
			unsigned int idx = rec.log[i].ploam.content[1];	/* Frag_Index */
			unsigned int off = idx * ODI_GPON_KEY_FRAGMENT_BYTES;

			CHECK(rec.log[i].ploam.content[0] == fsm.key_index,
			      "each Encryption_Key fragment carries this ONU own Key_Index");
			CHECK(idx == expect_idx % ODI_GPON_KEY_FRAGMENTS,
			      "the fragments alternate 0,1,0,1,0,1, not 3 of 0 then 3 of 1");
			expect_idx++;
			memcpy(&rebuilt[off], &rec.log[i].ploam.content[2],
			       ODI_GPON_KEY_FRAGMENT_BYTES);
			count++;
		} else if (rec.log[i].kind == ACT_LOAD_KEY) {
			i_load = (int)i;
		}
	}
	CHECK(count == 3U * ODI_GPON_KEY_FRAGMENTS,
	      "Request_Key produces exactly 6 Encryption_Key fragment sends (2 fragments x3)");
	CHECK(memcmp(rebuilt, test_key, 16U) == 0,
	      "the fragments reassemble this ONU own configured AES key");
	CHECK(i_load >= 0 && (unsigned int)i_load == rec.n - 1U,
	      "load_key runs last, after all six fragment sends (the observed order)");
	CHECK(i_load >= 0 && rec.log[i_load].a == ODI_GPON_DSK_SLOT_NEXT,
	      "the generated key is loaded into the inactive (next) hardware slot");
	CHECK(i_load >= 0 && memcmp(rec.log[i_load].key, test_key, 16U) == 0,
	      "load_key receives the same key the fragments reported upstream");
}

static void test_key_switching_time(void)
{
	struct odi_gpon_fsm fsm;
	struct recorder rec;
	struct odi_gpon_ploam msg;
	int i_switch;

	fsm_setup(&fsm, &rec);
	drive_to_o5(&fsm, &rec);
	rec_reset(&rec);

	msg = ds_key_switching_time(TEST_ASSIGNED_ONU_ID, 0x00123456U);
	odi_gpon_fsm_handle_event(&fsm, &test_ops, &rec, ODI_GPON_EVENT_PLOAM_RX, &msg);

	CHECK(fsm.state == ODI_GPON_STATE_O5, "Key_switching_time does not change state");
	i_switch = find_action(&rec, 0, ACT_SWITCH_KEY);
	CHECK(i_switch >= 0 && rec.log[i_switch].a == 0x00123456U,
	      "Key_switching_time schedules the switch at the OLT-supplied superframe count");
}

int main(void)
{
	test_wire_pack_unpack_roundtrip();
	test_ds_decoders();
	test_us_encoders();
	test_activation_scenario();
	test_to1_timeout_reverts();
	test_deactivate();
	test_disable_serial_number_to_o7_and_reenable();
	test_popup_to_o6_and_back();
	test_request_key_two_fragments();
	test_key_switching_time();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_test: all checks passed\n");
	return 0;
}
