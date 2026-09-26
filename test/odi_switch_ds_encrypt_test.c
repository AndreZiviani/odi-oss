/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_ds_encrypt_test.c -- odi_switch_gpon_encrypt_port(), the one
 * path that sets the AES bit of a downstream GEM port: the GPON core calls
 * it on the Encrypted_Port-ID PLOAM of the OLT. Covers the gem_port_id to
 * DS slot lookup (odi_switch_ds_slot_record()/_find()), the set and the
 * clear, that the read-modify-write leaves every other FLAGS bit alone,
 * and that a GEM port with no slot yet is left alone.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"

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

/* The ISP1 data GEM ports, slots 1-5: FLAGS 0x02 (Ethernet) on ours before
 * the PLOAM, 0x12 (plus AES) on the stock image after it.
 */
static void test_capture_transition(void)
{
	static const uint16_t gem[6] = { 0xfff, 0x59a, 0x69a, 0x71a, 0x79a, 0x61a };
	unsigned int idx;

	odi_mock_reset();
	odi_switch_ds_slot_reset();
	for (idx = 0; idx < 6; idx++) {
		odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(idx), idx ? 0x00000002U : 0x00000003U);
		odi_switch_ds_slot_record(idx, gem[idx]);
	}
	for (idx = 1; idx < 6; idx++)
		CHECK(odi_switch_gpon_encrypt_port(gem[idx], 1) == 0, "a recorded data GEM port is found");
	for (idx = 1; idx < 6; idx++)
		CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(idx)) == 0x00000012U,
		      "slots 1-5: 0x02 becomes 0x12");
	CHECK(count_writes(ODI_SW_DSF_GEM_FLOW_TYPE(0)) == 1,
	      "slot 0 (OMCI/broadcast) only has the seed write");
}

static void test_clear(void)
{
	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(3), 0x00000012U);
	odi_switch_ds_slot_record(3, 0x71a);
	CHECK(odi_switch_gpon_encrypt_port(0x71a, 0) == 0, "clear: the port is found");
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(3)) == 0x00000002U, "aes 0 clears the bit");
}

/* A field-level read-modify-write: multicast, OMCI and bit 3 survive. */
static void test_rmw_preserves_other_bits(void)
{
	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_reg_write(ODI_SW_DSF_GEM_FLOW_TYPE(2), 0x0000000dU);
	odi_switch_ds_slot_record(2, 0x69a);
	(void)odi_switch_gpon_encrypt_port(0x69a, 1);
	CHECK(odi_reg_read(ODI_SW_DSF_GEM_FLOW_TYPE(2)) == 0x0000001dU,
	      "only bit 4 changes (0x0d -> 0x1d)");
	CHECK(count_writes(ODI_SW_DSF_GEM_FLOW_TYPE(2)) == 2, "one write besides the seed");
}

/* The PLOAM can arrive before cmd 25 created the port: nothing to write. */
static void test_unknown_port(void)
{
	odi_mock_reset();
	odi_switch_ds_slot_reset();
	odi_switch_ds_slot_record(1, 0x59a);
	odi_mock.log_n = 0;
	CHECK(odi_switch_gpon_encrypt_port(0x123, 1) == -1, "an unknown GEM port returns -1");
	CHECK(odi_mock.log_n == 0, "and writes nothing");
}

int main(void)
{
	test_capture_transition();
	test_clear();
	test_rmw_preserves_other_bits();
	test_unknown_port();
	CHECK(odi_mock_locks_idle(), "every switch lock released on return (odi_switch_mock.h)");

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_ds_encrypt_test: ok\n");
	return 0;
}
