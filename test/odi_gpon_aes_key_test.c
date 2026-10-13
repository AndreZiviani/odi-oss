/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_aes_key_test.c -- the Request_Key path end to end, FSM and
 * register leaves together, against test/odi_switch_mock.h: each
 * Request_Key draws a new key (G.984.3 clause 12.3), the Encryption_Key
 * fragments pushed into the upstream PLOAM FIFO carry it, and the words
 * written into the key engine for the next slot are the same 16 bytes.
 * Two cycles must give two different, non-zero keys under two Key_Index
 * values. The host build of the key draw is a fixed pseudo-random
 * sequence (odi_gpon_hw.c, hw_new_key()); the kernel one is the RNG.
 */
#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_isr.c"

#include <stdio.h>

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

#define TEST_ONU_ID	0x1aU

/* What the write hook saw during one Request_Key. */
static struct {
	struct odi_gpon_ploam us[16];	/* every upstream PLOAM pushed */
	unsigned int n_us;
	uint8_t key_words[16];		/* the key engine words, by WORD_INDEX */
	unsigned int key_word_mask;	/* which word indexes were committed */
	int next_slot;			/* the DSK_SWITCH_ARM bracket chose the next slot */
} seen;

static void capture_hook(uint32_t off, uint32_t val)
{
	if (off == ODI_GPON_USF_PLOAM_TX_CTL_OFF && ODI_GPON_USF_PLOAM_TX_CTL_PUSH_GET(val)) {
		uint16_t words[6];
		unsigned int i;

		for (i = 0; i < 6U; i++)
			words[i] = (uint16_t)odi_mock.regs[odi_mock_slot(ODI_GPON_USF_PLOAM_TX_WORD(i))];
		if (seen.n_us < 16U)
			odi_gpon_ploam_unpack_words(words, &seen.us[seen.n_us++]);
	} else if (off == ODI_GPON_DSK_KEY_LOAD_OFF && ODI_GPON_DSK_KEY_LOAD_WRITE_REQ_GET(val)) {
		unsigned int i = ODI_GPON_DSK_KEY_LOAD_WORD_INDEX_GET(val);
		uint32_t w = odi_mock.regs[odi_mock_slot(ODI_GPON_DSK_KEY_WORD_OFF)];

		seen.key_words[2U * i] = (uint8_t)(w >> 8);
		seen.key_words[2U * i + 1U] = (uint8_t)w;
		seen.key_word_mask |= 1U << i;
	} else if (off == ODI_GPON_DSK_SWITCH_ARM_OFF && ODI_GPON_DSK_SWITCH_ARM_ARM_GET(val)) {
		seen.next_slot = !ODI_GPON_DSK_SWITCH_ARM_ACTIVE_SLOT_GET(val);
	}
}

/* One Request_Key through the FSM with the real leaves. Fills key with the
 * key the fragments reported and returns their Key_Index, or -1 if the
 * fragments disagree among themselves.
 */
static int request_key(struct odi_gpon_fsm *fsm, uint8_t key[16])
{
	struct odi_gpon_ploam msg;
	unsigned int i, n_frag = 0;
	int key_index = -1;

	memset(&seen, 0, sizeof(seen));
	memset(&msg, 0, sizeof(msg));
	msg.onu_id = TEST_ONU_ID;
	msg.type = ODI_GPON_DS_REQUEST_KEY;
	odi_gpon_fsm_handle_event(fsm, odi_gpon_hw_ops(), NULL, ODI_GPON_EVENT_PLOAM_RX, &msg);

	memset(key, 0, 16U);
	for (i = 0; i < seen.n_us; i++) {
		const struct odi_gpon_ploam *m = &seen.us[i];
		unsigned int frag = m->content[1];

		if (m->type != ODI_GPON_US_ENCRYPTION_KEY || frag >= ODI_GPON_KEY_FRAGMENTS)
			continue;
		if (n_frag++ == 0)
			key_index = m->content[0];
		else if (m->content[0] != key_index)
			return -1;
		if (n_frag <= ODI_GPON_KEY_FRAGMENTS) {
			memcpy(&key[frag * ODI_GPON_KEY_FRAGMENT_BYTES], &m->content[2],
			       ODI_GPON_KEY_FRAGMENT_BYTES);
		} else {
			CHECK(memcmp(&key[frag * ODI_GPON_KEY_FRAGMENT_BYTES], &m->content[2],
				     ODI_GPON_KEY_FRAGMENT_BYTES) == 0,
			      "the three copies of a fragment carry the same bytes");
		}
	}
	CHECK(n_frag == 3U * ODI_GPON_KEY_FRAGMENTS, "six Encryption_Key fragments pushed upstream");
	CHECK(seen.key_word_mask == 0xffU, "all eight key words committed to the key engine");
	CHECK(seen.next_slot, "the key goes into the next (shadow) slot");
	CHECK(memcmp(seen.key_words, key, 16U) == 0,
	      "the key written into the key engine is the key sent upstream");
	return key_index;
}

static int all_zero(const uint8_t key[16])
{
	unsigned int i;

	for (i = 0; i < 16U; i++)
		if (key[i])
			return 0;
	return 1;
}

static void test_two_request_key_cycles(void)
{
	struct odi_gpon_fsm fsm;
	uint8_t k1[16], k2[16];
	int idx1, idx2;

	odi_mock_reset();
	odi_gpon_hw_reset();
	odi_gpon_fsm_init(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	odi_mock_write_hook = capture_hook;

	idx1 = request_key(&fsm, k1);
	idx2 = request_key(&fsm, k2);

	CHECK(!all_zero(k1), "the first key is not all zero");
	CHECK(!all_zero(k2), "the second key is not all zero");
	CHECK(memcmp(k1, k2, 16U) != 0, "each Request_Key reports a different key");
	CHECK(idx1 >= 0 && idx2 >= 0, "every fragment of one key carries one Key_Index");
	CHECK(idx2 == ((idx1 + 1) & 0xff), "the second key carries the next Key_Index (G.984.3 12.3)");

	odi_mock_write_hook = NULL;
}

/* A key engine that never reports a word taken: the load still writes all
 * eight words, bounded, and leaves no lock held.
 */
static void test_load_not_done_is_bounded(void)
{
	struct odi_gpon_fsm fsm;
	uint8_t k[16];

	odi_mock_reset();
	odi_gpon_hw_reset();
	odi_gpon_fsm_init(&fsm);
	fsm.state = ODI_GPON_STATE_O5;
	fsm.onu_id = TEST_ONU_ID;
	odi_mock.poll_stuck = 1;
	odi_mock_write_hook = capture_hook;
	(void)request_key(&fsm, k);
	odi_mock_write_hook = NULL;
	CHECK(odi_mock_locks_idle(), "no lock left held");
}

int main(void)
{
	test_two_request_key_cycles();
	test_load_not_done_is_bounded();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_aes_key_test: ok\n");
	return 0;
}
