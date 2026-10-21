/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_gpon_hold_test.c -- the kept Emergency Stop (GPON_KEEP_DISABLE) in the
 * GPON leaf. The OLT disables the ONU (G.984.3 Disable_Serial_Number, O7);
 * with the feature on, this side's own deactivation and activation (the
 * omcid respawn, a serial number or password change, the boot) end in O7
 * with the TX-disable line asserted, and only the OLT enable (or the
 * release by hand) leaves it. With the feature off the behaviour is the one
 * test/odi_gpon_o7_test.c covers: nothing is tracked, and gpondeact and
 * gponact leave O7. The TX-disable line is a fake hook, as there.
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

static void setup(enum odi_gpon_state state, int keep, struct odi_gpon_fsm *fsm)
{
	odi_mock_reset();
	odi_gpon_hw_reset();
	odi_switch_ds_slot_reset();
	odi_gpon_hw_set_tx_disable(fake_tx_disable);
	odi_gpon_hw_hold_enable(keep);
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

/* One downstream message and the tracking odi_gpon.c does after it. */
static enum odi_gpon_hold_change olt_sends(struct odi_gpon_fsm *fsm, uint8_t code, const uint8_t *serial)
{
	enum odi_gpon_state prev = fsm->state;

	disable_sn(fsm, code, serial);
	return odi_gpon_hw_hold_track(prev, fsm->state);
}

static unsigned int state_reg(void)
{
	return ODI_GPON_DSF_ONU_STATE_ACTIVATION_STATE_GET(odi_reg_read(ODI_GPON_DSF_ONU_STATE_OFF));
}

/* Off: nothing is tracked, nothing is kept, and the verbs are the ones that
 * leave O7, as before the feature.
 */
static void test_off_is_unchanged(void)
{
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O5, 0, &fsm);
	CHECK(olt_sends(&fsm, 0xff, sn) == ODI_GPON_HOLD_UNCHANGED, "off: the disable is not tracked");
	CHECK(fsm.state == ODI_GPON_STATE_O7 && line == 1, "off: O7, line asserted");
	CHECK(!odi_gpon_hw_hold_get(), "off: nothing held");
	odi_gpon_verb_deactivate(&fsm);
	CHECK(fsm.state == ODI_GPON_STATE_O1 && line == 0 && state_reg() == ODI_GPON_STATE_O1,
	      "off: gpondeact goes to O1 and releases the line");
	odi_gpon_verb_activate(&fsm);
	CHECK(fsm.state == ODI_GPON_STATE_O2, "off: gponact goes to O2");
	CHECK(olt_sends(&fsm, 0xff, sn) == ODI_GPON_HOLD_UNCHANGED && !odi_gpon_hw_hold_get(),
	      "off: a second disable is not tracked either");
}

/* On: the OLT disable starts the hold; gpondeact and gponact keep O7, the
 * line stays asserted from first to last, and the interrupt masks still go
 * on so the enable is heard.
 */
static void test_restarts_keep_o7(void)
{
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O5, 1, &fsm);
	CHECK(olt_sends(&fsm, 0xff, sn) == ODI_GPON_HOLD_RECORDED, "on: the disable is recorded");
	CHECK(odi_gpon_hw_hold_get() && fsm.state == ODI_GPON_STATE_O7, "on: held, in O7");
	CHECK(olt_sends(&fsm, 0xff, sn) == ODI_GPON_HOLD_UNCHANGED, "on: a repeat records nothing more");

	calls = 0;
	odi_gpon_verb_deactivate(&fsm);
	CHECK(fsm.state == ODI_GPON_STATE_O7 && fsm.onu_id == 0U, "gpondeact: still O7, no ONU-ID");
	CHECK(state_reg() == ODI_GPON_STATE_O7, "gpondeact: the state register says O7");
	CHECK(line == 1 && odi_gpon_hw_tx_disabled(), "gpondeact: the line stays asserted");
	CHECK(calls == 0U, "gpondeact: the line is not driven at all");

	odi_gpon_verb_activate(&fsm);
	CHECK(fsm.state == ODI_GPON_STATE_O7 && state_reg() == ODI_GPON_STATE_O7, "gponact: still O7");
	CHECK(line == 1 && calls == 0U, "gponact: the line never let go");
	CHECK(odi_reg_read(ODI_GPON_DSF_IRQ_ENABLE_OFF) != 0U, "gponact: the downstream masks are on again");

	/* An omcid respawn is exactly this pair, repeated. */
	odi_gpon_verb_deactivate(&fsm);
	odi_gpon_verb_activate(&fsm);
	CHECK(fsm.state == ODI_GPON_STATE_O7 && line == 1 && calls == 0U, "a second round: unchanged");
	CHECK(odi_gpon_hw_hold_get(), "restarts leave the hold in force");
}

/* The OLT enable (this serial number, or enable-all) ends the hold, from
 * the O7 a restart left as well as the one it reached.
 */
static void test_olt_enable_releases(void)
{
	static const uint8_t other[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O5, 1, &fsm);
	olt_sends(&fsm, 0xff, sn);
	odi_gpon_verb_deactivate(&fsm);
	odi_gpon_verb_activate(&fsm);
	CHECK(olt_sends(&fsm, 0x00, other) == ODI_GPON_HOLD_UNCHANGED && fsm.state == ODI_GPON_STATE_O7,
	      "an enable for another serial number does nothing");
	CHECK(olt_sends(&fsm, 0x00, sn) == ODI_GPON_HOLD_RELEASED, "the enable ends the hold");
	CHECK(fsm.state == ODI_GPON_STATE_O2 && line == 0 && !odi_gpon_hw_hold_get(),
	      "enabled: O2, line released, nothing held");
	odi_gpon_verb_deactivate(&fsm);
	CHECK(fsm.state == ODI_GPON_STATE_O1, "after the enable the verbs are the usual ones again");

	setup(ODI_GPON_STATE_O3, 1, &fsm);
	olt_sends(&fsm, 0xff, sn);
	CHECK(olt_sends(&fsm, 0x0f, other) == ODI_GPON_HOLD_RELEASED && fsm.state == ODI_GPON_STATE_O2,
	      "enable-all ends it too");
}

/* The boot with the marker: the line goes up when the hold is set, before
 * the activation; the boot activation ends in O7 (odi_gpon.c), and the
 * OLT enable then ranges the ONU as usual.
 */
static void test_boot_restore(void)
{
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O1, 1, &fsm);
	odi_gpon_hw_hold_set(1);
	CHECK(line == 1 && odi_gpon_hw_hold_get(), "restore: the line is high at once");
	odi_gpon_hw_hold_enter(&fsm);
	CHECK(fsm.state == ODI_GPON_STATE_O7 && state_reg() == ODI_GPON_STATE_O7, "restore: O7");
	CHECK(odi_gpon_hw_hold_track(ODI_GPON_STATE_O1, ODI_GPON_STATE_O7) == ODI_GPON_HOLD_UNCHANGED,
	      "restore: entering O7 on its own account records nothing");
	CHECK(olt_sends(&fsm, 0xff, sn) == ODI_GPON_HOLD_UNCHANGED && fsm.state == ODI_GPON_STATE_O7,
	      "restore: a second disable in O7 is a dash");
	CHECK(olt_sends(&fsm, 0x00, sn) == ODI_GPON_HOLD_RELEASED && fsm.state == ODI_GPON_STATE_O2 && line == 0,
	      "restore: the OLT enable resumes normal activation");

	/* The line did not take at the first try: the state write retries it. */
	setup(ODI_GPON_STATE_O1, 1, &fsm);
	fail_next = 1;
	odi_gpon_hw_hold_set(1);
	CHECK(line == 0 && odi_gpon_hw_hold_get(), "restore: a failed line is not a failed hold");
	odi_gpon_hw_hold_enter(&fsm);
	CHECK(line == 1, "restore: entering O7 asserts it again");
}

/* The release by hand: the flag and the line go, an ONU in O7 is activated
 * as the OLT enable would have (O2), and the verbs are the usual ones.
 */
static void test_local_release(void)
{
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O5, 1, &fsm);
	olt_sends(&fsm, 0xff, sn);
	odi_gpon_hw_hold_set(0);
	CHECK(!odi_gpon_hw_hold_get() && line == 0, "release: flag cleared, line released");
	odi_gpon_verb_activate(&fsm);
	CHECK(fsm.state == ODI_GPON_STATE_O2 && line == 0, "release: the activation that follows ends in O2");
	CHECK(odi_gpon_hw_hold_track(ODI_GPON_STATE_O7, ODI_GPON_STATE_O2) == ODI_GPON_HOLD_UNCHANGED,
	      "release: nothing left to track");
}

/* The test hook: a disable the OLT never sent. It moves the ONU as the real
 * message does, sends nothing upstream, and only O2 to O6 respond.
 */
static void test_simulated_disable(void)
{
	struct odi_gpon_fsm fsm;
	unsigned int rx0, tx0, rx1, tx1;

	setup(ODI_GPON_STATE_O5, 1, &fsm);
	odi_gpon_get_ploam_counts(&rx0, &tx0);
	odi_gpon_hw_hold_simulate_disable(&fsm);
	CHECK(odi_gpon_hw_hold_track(ODI_GPON_STATE_O5, fsm.state) == ODI_GPON_HOLD_RECORDED &&
	      fsm.state == ODI_GPON_STATE_O7 && line == 1, "simulated: O7, recorded, line high");
	odi_gpon_get_ploam_counts(&rx1, &tx1);
	CHECK(rx0 == rx1 && tx0 == tx1, "simulated: no PLOAM sent, none counted");

	setup(ODI_GPON_STATE_O1, 1, &fsm);
	odi_gpon_hw_hold_simulate_disable(&fsm);
	CHECK(fsm.state == ODI_GPON_STATE_O1 && line == 0, "simulated: from O1 nothing happens (a dash)");
}

/* No hook (a build without the board): the hold is kept all the same. */
static void test_no_hook(void)
{
	struct odi_gpon_fsm fsm;

	setup(ODI_GPON_STATE_O5, 1, &fsm);
	odi_gpon_hw_set_tx_disable(NULL);
	olt_sends(&fsm, 0xff, sn);
	odi_gpon_verb_deactivate(&fsm);
	CHECK(fsm.state == ODI_GPON_STATE_O7 && odi_gpon_hw_hold_get() && calls == 0U,
	      "no hook: O7 and the hold are kept");
}

int main(void)
{
	test_off_is_unchanged();
	test_restarts_keep_o7();
	test_olt_enable_releases();
	test_boot_restore();
	test_local_release();
	test_simulated_disable();
	test_no_hook();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_gpon_hold_test: ok\n");
	return 0;
}
