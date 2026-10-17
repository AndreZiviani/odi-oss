/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_o7_test.c -- the laser in Emergency Stop (O7): the GPON leaf
 * asserts the module's TX-disable line when it writes state O7 and
 * releases it when any other state is written after it, whichever way O7
 * is left (G.984.3 10.2.2 g; the stock GPON module drives its TX-disable
 * GPIO on exactly these transitions). And it never touches the line on
 * the paths the stock module leaves alone: Deactivate_ONU-ID and loss of
 * signal, where a laser turned off would stay off. The line is a fake hook
 * here (odi_gpon_hw_set_tx_disable()); test/odi_board_test.c covers the
 * GPIO behind it.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_ploam.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_fsm.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_gpon_hw.c"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

#define ONU_ID	26U

static const uint8_t sn[8] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };

static int line;	/* the fake TX-disable line: 1 = laser off */
static unsigned int calls;
static int fail_next;

static int fake_tx_disable(int disable)
{
	calls++;
	if (fail_next) {
		fail_next = 0;
		return -5;
	}
	line = disable;
	return 0;
}

static void setup(enum odi_gpon_state state, struct odi_gpon_fsm *fsm)
{
	odi_mock_reset();
	odi_gpon_hw_reset();
	odi_switch_ds_slot_reset();
	odi_gpon_hw_set_tx_disable(fake_tx_disable);
	line = 0;
	calls = 0;
	fail_next = 0;
	odi_gpon_fsm_init(fsm);
	odi_gpon_fsm_set_serial_number(fsm, sn);
	fsm->state = state;
	fsm->onu_id = state >= ODI_GPON_STATE_O4 && state <= ODI_GPON_STATE_O6 ? ONU_ID : 0U;
}

static void disable_sn(struct odi_gpon_fsm *fsm, uint8_t code, const uint8_t *serial)
{
	struct odi_gpon_ploam msg;

	memset(&msg, 0, sizeof msg);
	msg.onu_id = 0xff;
	msg.type = ODI_GPON_DS_DISABLE_SERIAL_NUMBER;
	msg.content[0] = code;
	if (serial)
		memcpy(&msg.content[1], serial, 8U);
	odi_gpon_fsm_handle_event(fsm, odi_gpon_hw_ops(), NULL, ODI_GPON_EVENT_PLOAM_RX, &msg);
}

/* O5 -> O7 by a disable with our serial number: laser off; the enable back
 * to O2: laser on again.
 */
static void test_disable_and_enable(void)
{
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O5, &fsm);
	disable_sn(&fsm, 0xff, sn);
	CHECK(fsm.state == ODI_GPON_STATE_O7, "disable: O7");
	CHECK(line == 1 && odi_gpon_hw_tx_disabled(), "O7: TX-disable asserted");
	disable_sn(&fsm, 0xff, sn);
	CHECK(calls == 1U, "a repeat of the disable drives nothing more");
	disable_sn(&fsm, 0x00, sn);
	CHECK(fsm.state == ODI_GPON_STATE_O2, "enable: O2");
	CHECK(line == 0 && !odi_gpon_hw_tx_disabled(), "left O7: released");
}

/* Enable-all (0x0f) leaves O7 too; a disable for another serial number
 * does nothing.
 */
static void test_enable_all_and_other_sn(void)
{
	static const uint8_t other[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O3, &fsm);
	disable_sn(&fsm, 0xff, other);
	CHECK(fsm.state == ODI_GPON_STATE_O3 && calls == 0U, "another ONU's disable: nothing");
	disable_sn(&fsm, 0xff, sn);
	CHECK(line == 1, "O3 disable: asserted");
	disable_sn(&fsm, 0x0f, other);
	CHECK(fsm.state == ODI_GPON_STATE_O2 && line == 0, "enable-all: released");
}

/* The driver verbs leave O7 as well: gpondeact writes O1, gponact O1. */
static void test_verbs_release(void)
{
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O5, &fsm);
	disable_sn(&fsm, 0xff, sn);
	odi_gpon_verb_deactivate(&fsm);
	CHECK(line == 0, "gpondeact from O7: released");

	setup(ODI_GPON_STATE_O5, &fsm);
	disable_sn(&fsm, 0xff, sn);
	odi_gpon_fsm_handle_event(&fsm, odi_gpon_hw_ops(), NULL, ODI_GPON_EVENT_ACTIVATE, NULL);
	CHECK(fsm.state == ODI_GPON_STATE_O1 && line == 0, "activate from O7: O1, released");
}

/* Deactivate_ONU-ID (O5 -> O2) and LOS (-> O1) do not touch the line: the
 * stock module leaves the laser on there, and O2 has no way back on.
 */
static void test_no_gate_outside_o7(void)
{
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O5, &fsm);
	odi_gpon_fsm_handle_event(&fsm, odi_gpon_hw_ops(), NULL, ODI_GPON_EVENT_DEACTIVATE, NULL);
	CHECK(fsm.state == ODI_GPON_STATE_O2 && calls == 0U, "Deactivate: O2, line untouched");
	setup(ODI_GPON_STATE_O5, &fsm);
	odi_gpon_fsm_handle_event(&fsm, odi_gpon_hw_ops(), NULL, ODI_GPON_EVENT_LOS, NULL);
	CHECK(calls == 0U, "LOS: line untouched");
	odi_gpon_verb_deactivate(&fsm);
	CHECK(calls == 0U, "gpondeact outside O7: line untouched");
}

/* A release that failed is tried again at the next state written. */
static void test_failed_release_retried(void)
{
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O5, &fsm);
	disable_sn(&fsm, 0xff, sn);
	fail_next = 1;
	disable_sn(&fsm, 0x00, sn);
	CHECK(line == 1 && odi_gpon_hw_tx_disabled(), "release failed: still asserted");
	odi_gpon_fsm_handle_event(&fsm, odi_gpon_hw_ops(), NULL, ODI_GPON_EVENT_ACTIVATE, NULL);
	CHECK(line == 0 && !odi_gpon_hw_tx_disabled(), "the next state written releases it");
}

/* No hook (host tests, or a build without the board): nothing driven. */
static void test_no_hook(void)
{
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O5, &fsm);
	odi_gpon_hw_set_tx_disable(NULL);
	disable_sn(&fsm, 0xff, sn);
	CHECK(fsm.state == ODI_GPON_STATE_O7 && calls == 0U && !odi_gpon_hw_tx_disabled(),
	      "no hook: O7 reached, nothing driven");
}

int main(void)
{
	test_disable_and_enable();
	test_enable_all_and_other_sn();
	test_verbs_release();
	test_no_gate_outside_o7();
	test_failed_release_retried();
	test_no_hook();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_o7_test: ok\n");
	return 0;
}
