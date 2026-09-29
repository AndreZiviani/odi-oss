/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_replay_test.c -- drives the gpondeact verb, then the gponact
 * verb, then feeds the downstream PLOAM messages the stock-image capture
 * decodes to (dummy serial number substituted, see below), calling
 * odi_gpon_isr() once per message occurrence exactly like a real
 * interrupt pass would, and dumps the resulting write log for
 * tools/regtrace/gpon_replay_compare.py to diff against
 * test/fixtures/gpon-react-260922-excerpt.txt (an ordered write-stream
 * comparison, not compare.py own --stream final-value-only mode -- see
 * that script own docstring for why).
 *
 * Scenario: a FORCED re-activation of an ALREADY-active ONU (the
 * fixture own react.txt capture, not a fresh boot) -- this file seeds
 * the FSM and the mock registers to look like O5 with a GEM port and five
 * Alloc-IDs already up before calling odi_gpon_verb_deactivate(), the same
 * starting condition a forced-deactivation trial on real hardware
 * captured.
 *
 * Dummy serial number: react.txt own Assign_ONU-ID content carries this
 * stick own real 8-byte serial number (the only place it appears as
 * anything the register trace touches, and only as a READ, never a
 * WRITE -- confirmed by grep before this fixture was cut, and again here:
 * the fixture has no W line containing it). This test still substitutes a
 * fixed dummy value for both the fed message and this FSM own configured
 * serial_number, since a fixture derived from this same capture could
 * change to include it later without this file own author noticing.
 *
 * Message feed counts: every downstream message in G.984.3 is sent 3x
 * (react.txt matches this), and this test feeds each one 3 times to
 * match -- one exception: Request_Key is fed once, since this driver own
 * response to it (odi_gpon_fsm.c) already sends each of its 2 key
 * fragments 3 times per single Request_Key event (the evidenced fragment-
 * repeat count, not the triggering message own read count); feeding the
 * trigger 3 times as well would triple that again, which react.txt does
 * not show (one key exchange only).
 */
#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_isr.c"

#include <stdio.h>

static const uint8_t dummy_sn[8] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
static const uint8_t dummy_pw[10];	/* empty -- matches this stick own empty gponpw config */
/* The AES key react.txt shows this ONU report on its one Request_Key
 * cycle -- not this stick own persistent identity (a fresh, ONU-
 * generated session key every activation; only the serial number is
 * redacted here), replayed as captured so the Encryption_Key fragment
 * CONTENT (compared, since it lands in the 0x700000-0x706fff window)
 * matches.
 */
static const uint8_t captured_aes_key[16] = {
	0x08, 0xe0, 0x96, 0xdd, 0x1d, 0x2e, 0xa0, 0xd9,
	0xed, 0xbb, 0xb4, 0x74, 0xfb, 0x27, 0x0a, 0xbe,
};

#define TEST_ONU_ID	0x1aU	/* this box own assigned ID in the capture -- not sensitive */
#define TEST_GEM_PORT	0x01aU

struct fixture_msg {
	uint8_t onu_id;
	uint8_t type;
	uint8_t content[10];
	unsigned int repeats;
};

static const struct fixture_msg fixture_messages[] = {
	/* Upstream_Overhead (broadcast, O2->O3). */
	{ 0xff, ODI_GPON_DS_UPSTREAM_OVERHEAD,
	  { 0x20, 0x00, 0x00, 0xaa, 0xab, 0x59, 0x83, 0x20, 0x00, 0x00 }, 3 },
	/* Assign_ONU-ID (broadcast, dummy SN, O3->O4). content[0] filled below. */
	{ 0xff, ODI_GPON_DS_ASSIGN_ONU_ID, { 0 }, 3 },
	/* Ranging_Time (O4->O5). */
	{ TEST_ONU_ID, ODI_GPON_DS_RANGING_TIME,
	  { 0x00, 0x00, 0x03, 0x77, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00 }, 3 },
	/* Configure_Port-ID (Port-ID 0x01a, activate). */
	{ TEST_ONU_ID, ODI_GPON_DS_CONFIGURE_PORT_ID,
	  { 0x01, 0x01, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 3 },
	/* Encrypted_Port-ID (same Port-ID, not encrypted). */
	{ TEST_ONU_ID, ODI_GPON_DS_ENCRYPTED_PORT_ID,
	  { 0x02, 0x01, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 3 },
	/* BER_Interval (10.000 s). */
	{ TEST_ONU_ID, ODI_GPON_DS_BER_INTERVAL,
	  { 0x00, 0x01, 0x38, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 3 },
	/* Request_Key -- fed once, see this file own header comment. */
	{ TEST_ONU_ID, ODI_GPON_DS_REQUEST_KEY, { 0 }, 1 },
	/* Key_Switching_Time. */
	{ TEST_ONU_ID, ODI_GPON_DS_KEY_SWITCHING_TIME,
	  { 0x0c, 0x5c, 0xc4, 0xa9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 3 },
	/* Assign_Alloc-ID x5, in arrival order. */
	{ TEST_ONU_ID, ODI_GPON_DS_ASSIGN_ALLOC_ID,
	  { 0x11, 0xa0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 3 },
	{ TEST_ONU_ID, ODI_GPON_DS_ASSIGN_ALLOC_ID,
	  { 0x31, 0xa0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 3 },
	{ TEST_ONU_ID, ODI_GPON_DS_ASSIGN_ALLOC_ID,
	  { 0x41, 0xa0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 3 },
	{ TEST_ONU_ID, ODI_GPON_DS_ASSIGN_ALLOC_ID,
	  { 0x51, 0xa0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 3 },
	/* Repeats 1, not 3: the fixture excerpt ends (react.txt line 1303,
	 * test/fixtures/gpon-react-260922-excerpt.txt own header comment)
	 * right after this fifth Alloc-ID own FIRST Acknowledge, before its
	 * second and third reads -- a fixture-boundary truncation, not a
	 * claim that this message is only ever read once in the real
	 * capture (every other message here is read three times).
	 */
	{ TEST_ONU_ID, ODI_GPON_DS_ASSIGN_ALLOC_ID,
	  { 0x21, 0xa0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 1 },
};

static void poke_reg(uint32_t off, uint32_t val)
{
	odi_mock.regs[odi_mock_slot(off)] = val;
}

/* One simulated interrupt pass carrying exactly one queued PLOAM message
 * -- odi_gpon_isr.c drains at most one message per call (this pass own
 * finding, its header comment), so the outer "while messages remain" loop
 * this function own caller runs is this test own, not odi_gpon_isr() own.
 */
static void feed_one(struct odi_gpon_fsm *fsm, const struct fixture_msg *m)
{
	uint16_t words[6];
	struct odi_gpon_ploam msg;
	unsigned int i;
	unsigned int n;

	msg.onu_id = m->onu_id;
	msg.type = m->type;
	memcpy(msg.content, m->content, sizeof(msg.content));
	odi_gpon_ploam_pack_words(&msg, words);

	poke_reg(ODI_GPON_PONMAC_IRQ_PENDING_OFF, ODI_GPON_PONMAC_IRQ_PENDING_DS_FRAMER);
	poke_reg(ODI_GPON_DSF_IRQ_EVENT_OFF, ODI_GPON_DSF_IRQ_EVENT_PLOAM_RX);
	poke_reg(ODI_GPON_DSF_PLOAM_RX_CTL_OFF, 0U);	/* FIFO_EMPTY clear -- a message is queued */
	for (i = 0; i < 6U; i++)
		poke_reg(ODI_GPON_DSF_PLOAM_RX_WORD(i), words[i]);

	n = odi_gpon_isr_poll(fsm, NULL);
	if (n != 1U) {
		fprintf(stderr, "odi_gpon_replay_test: odi_gpon_isr() drained %u messages, expected 1\n", n);
		exit(1);
	}
}

int main(int argc, char **argv)
{
	struct odi_gpon_fsm fsm;
	unsigned int seed_alloc_rows[] = { 0, 1, 2, 3, 4, 16 };
	unsigned int i, r;
	struct fixture_msg assign_onu_id_msg;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <out-file>\n", argv[0]);
		return 2;
	}

	odi_mock_reset();
	odi_gpon_hw_reset();
	odi_gpon_fsm_init(&fsm);
	odi_gpon_fsm_set_serial_number(&fsm, dummy_sn);
	odi_gpon_fsm_set_password(&fsm, dummy_pw);
	odi_gpon_fsm_set_aes_key(&fsm, captured_aes_key);

	/* Seed "already active" state -- both the FSM own fields and the
	 * mock registers a real re-activation trial would already have set
	 * from the PREVIOUS activation cycle (this file own header comment).
	 */
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	odi_gpon_hw_test_seed_active(TEST_GEM_PORT, seed_alloc_rows,
				      sizeof(seed_alloc_rows) / sizeof(seed_alloc_rows[0]));
	poke_reg(ODI_GPON_DSF_ONU_STATE_OFF, 0x1a05U);
	poke_reg(ODI_GPON_USF_ONU_ID_OFF, 0x1a00U);
	poke_reg(ODI_GPON_USF_MIN_RESP_DELAY_OFF, 0x9132U);	/* gpondrv own fixed constant, unchanged across activations in the capture */
	poke_reg(ODI_GPON_DSF_SETUP_OFF, 0x620U);		/* re-asserted unchanged, odi_gpon_hw.h own comment */
	poke_reg(ODI_GPON_USF_PLOAM_TX_SETUP_OFF, 0x13U);	/* pre-ranging value flush_us_ploam_buf() toggles from */
	poke_reg(ODI_GPON_USF_IRQ_ENABLE_OFF, 0xa4U);	/* never touched by gpondeact -- persists */
	poke_reg(ODI_SW_DSF_GEM_FLOW_TYPE(64U), 0x14U);
	poke_reg(ODI_SW_US_GEM_PORT_MAP(64U), TEST_GEM_PORT);
	poke_reg(ODI_SW_PONQ_STREAM_VALID(2U), 1U);

	odi_gpon_verb_deactivate(&fsm);
	odi_gpon_verb_activate(&fsm);

	for (i = 0; i < sizeof(fixture_messages) / sizeof(fixture_messages[0]); i++) {
		const struct fixture_msg *m = &fixture_messages[i];

		if (m->type == ODI_GPON_DS_ASSIGN_ONU_ID) {
			assign_onu_id_msg = *m;
			assign_onu_id_msg.content[0] = TEST_ONU_ID;
			memcpy(&assign_onu_id_msg.content[1], dummy_sn, 8U);
			m = &assign_onu_id_msg;
		}
		for (r = 0; r < m->repeats; r++)
			feed_one(&fsm, m);
	}

	/* The replay went through every GPON CAM sequence that takes
	 * odi_switch_dsf_lock; a nested acquire would already have aborted
	 * (odi_switch_mock.h), this checks none was left held.
	 */
	if (!odi_mock_locks_idle()) {
		fprintf(stderr, "odi_gpon_replay_test: a switch lock is still held after the replay\n");
		return 1;
	}

	{
		FILE *f = fopen(argv[1], "w");

		if (!f) {
			fprintf(stderr, "odi_gpon_replay_test: cannot open %s\n", argv[1]);
			return 1;
		}
		fprintf(f, "# actual write log, odi_gpon replay test\n");
		odi_mock_dump(f);
		fclose(f);
	}

	/* After the captured sequence, so none of this is in the compared
	 * write log: Assign_Alloc-ID type 255 releases an Alloc-ID (G.984.3
	 * 9.2.3.9), read three times like every downstream message, and the
	 * next assignment takes the freed row. /proc/odi_gpon (alloc_ids) is
	 * odi_gpon_get_alloc_ids(), which omcid binds its T-CONTs to.
	 */
	{
		static const struct fixture_msg release_794 = { TEST_ONU_ID,
			ODI_GPON_DS_ASSIGN_ALLOC_ID,
			{ 0x31, 0xa0, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 3 };
		static const struct fixture_msg assign_1000 = { TEST_ONU_ID,
			ODI_GPON_DS_ASSIGN_ALLOC_ID,
			{ 0x3e, 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 3 };
		static const uint16_t after_release[] = { 282, 1050, 1306, 538 };
		static const uint16_t after_assign[] = { 282, 1000, 1050, 1306, 538 };
		uint16_t ids[32];
		unsigned int n;

		n = odi_gpon_get_alloc_ids(ids, 32);
		if (n != 5 || ids[0] != 282 || ids[1] != 794 || ids[4] != 538) {
			fprintf(stderr, "odi_gpon_replay_test: %u Alloc-IDs after the capture, expected the five assigned\n", n);
			return 1;
		}
		for (r = 0; r < release_794.repeats; r++)
			feed_one(&fsm, &release_794);
		n = odi_gpon_get_alloc_ids(ids, 32);
		if (n != 4 || memcmp(ids, after_release, sizeof after_release)) {
			fprintf(stderr, "odi_gpon_replay_test: deallocate 794 left %u Alloc-IDs, expected 282 1050 1306 538\n", n);
			return 1;
		}
		for (r = 0; r < assign_1000.repeats; r++)
			feed_one(&fsm, &assign_1000);
		n = odi_gpon_get_alloc_ids(ids, 32);
		if (n != 5 || memcmp(ids, after_assign, sizeof after_assign)) {
			fprintf(stderr, "odi_gpon_replay_test: assign 1000 after the release gave %u Alloc-IDs, expected it in the freed row\n", n);
			return 1;
		}
		if (!odi_mock_locks_idle()) {
			fprintf(stderr, "odi_gpon_replay_test: a switch lock is still held after the release\n");
			return 1;
		}
		printf("odi_gpon_replay_test: Alloc-ID release and reuse ok\n");
	}

	return 0;
}
