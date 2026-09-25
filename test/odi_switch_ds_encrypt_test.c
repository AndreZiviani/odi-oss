/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_ds_encrypt_test.c -- odi_switch_ds_encrypt() ("DS GEM
 * encryption flag"): mask selection, that bit 4 (EN_AES) is the
 * only bit touched (every other FLAGS bit already set for a
 * port survives the read-modify-write untouched), and the exact
 * transition a full-stream capture recorded for the real
 * data GEM ports (0x02 -> 0x12, this codebase's own captured "before"
 * value against the stock image captured "after").
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_hw.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_tbl.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_dal.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_replay_blob.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

static unsigned int count_writes(uint32_t addr)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == addr)
			n++;
	return n;
}

/* mask == 0 must be a documented no-op, same contract as every other
 * trigger this codebase has.
 */
static void test_empty_mask(void)
{
	odi_mock_reset();
	odi_switch_ds_encrypt(0);
	CHECK(odi_mock.log_n == 0, "empty mask: no writes of any kind");
}

/* The v8-vs-s15 finding itself: DSF_GEM_FLOW_TYPE[1..5] reads 0x02
 * on odi_switch (is_eth set, AES clear -- odi_sw_gpon_usflow_set()'s
 * own traffic_cfg=2 for a data port) and 0x12 on the stock stack (the
 * same is_eth bit plus bit 4, AES enable). odi_switch_ds_encrypt()
 * must turn the captured "before" into exactly the captured "after" for
 * every one of those five ports, and touch no other GEM port slot.
 */
static void test_matches_v8_capture_transition(void)
{
	unsigned int idx;
	uint32_t v;

	odi_mock_reset();
	for (idx = 1; idx <= 5; idx++)
		odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(idx), 0x00000002U);

	odi_switch_ds_encrypt((1U << 1) | (1U << 2) | (1U << 3) | (1U << 4) | (1U << 5));

	for (idx = 1; idx <= 5; idx++) {
		v = odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(idx));
		CHECK(v == 0x00000012U, "GEM port slot 1-5: 0x02 (captured before) becomes 0x12 (captured after)");
	}

	/* Slot 0 (OMCI/broadcast, traffic_cfg=3 per odi_sw_gpon_usflow_set()'s
	 * own doc) was never in the mask -- must be untouched.
	 */
	CHECK(count_writes(ODI_SW_DSF_GEM_FLOW_TYPE(0)) == 0,
	      "GEM port slot 0 (not in the mask) is never written");
}

/* Every other bit of FLAGS (multicast, OMCI, and whatever a
 * future field might use bit 3 for) must survive the read-modify-write
 * exactly -- this is a field-level RMW, not a blind overwrite.
 */
static void test_rmw_preserves_other_bits(void)
{
	uint32_t v;

	odi_mock_reset();
	/* is_mcast=1, is_eth=0, is_omci=1, bit3=1 (reserved, seeded to
	 * prove it survives too), AES=0 -- 0b01101 = 0x0d.
	 */
	odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(2), 0x0000000dU);
	odi_switch_ds_encrypt(1U << 2);

	v = odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(2));
	CHECK(v == 0x0000001dU, "RMW sets only bit 4, every other bit (0x0d) survives untouched (-> 0x1d)");
}

/* Mask bit selection: only the named slots are touched, in mask order,
 * and each entry does exactly one read-modify-write (not two).
 */
static void test_mask_selects_only_named_slots(void)
{
	odi_mock_reset();
	odi_switch_ds_encrypt((1U << 3) | (1U << 7));

	CHECK(count_writes(ODI_SW_DSF_GEM_FLOW_TYPE(3)) == 1, "slot 3 (in the mask) written exactly once");
	CHECK(count_writes(ODI_SW_DSF_GEM_FLOW_TYPE(7)) == 1, "slot 7 (in the mask) written exactly once");
	CHECK(count_writes(ODI_SW_DSF_GEM_FLOW_TYPE(1)) == 0, "slot 1 (not in the mask) untouched");
	CHECK(count_writes(ODI_SW_DSF_GEM_FLOW_TYPE(4)) == 0, "slot 4 (not in the mask) untouched");

	/* AES actually landed for both selected slots. */
	CHECK(ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_DECRYPT_GET(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(3))) == 1,
	      "slot 3: AES bit set");
	CHECK(ODI_SW_DSF_GEM_FLOW_TYPE_FLAGS_DECRYPT_GET(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(7))) == 1,
	      "slot 7: AES bit set");
}

int main(void)
{
	test_empty_mask();
	test_matches_v8_capture_transition();
	test_rmw_preserves_other_bits();
	test_mask_selects_only_named_slots();
	CHECK(odi_mock_locks_idle(), "every switch lock released on return (odi_switch_mock.h)");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_ds_encrypt_test: ok\n");
	return 0;
}
