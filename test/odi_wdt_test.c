/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_wdt_test.c -- host-side unit test for odi_wdt.c, unity-build
 * (#include the .c directly): the pure encode/kick-value/force-reset-value
 * helpers, the deadline/heartbeat/stall-report decision function against
 * a fake clock, and the real arm/kick/disable/force-reset sequences
 * through odi_soc.c against the SoC window of test/odi_soc_mock.h.
 */
#include <stdio.h>
#include <string.h>

#include "odi_soc_mock.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_soc.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_wdt.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_wdt.c"

#define WDT_KICK	odi_soc_mock.regs[SOC_WDT_KICK / 4]
#define WDT_CTRL	odi_soc_mock.regs[SOC_WDT_CTRL / 4]

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

/* U-Boot own en_wdt (`mw b8003268 e7c00000`)
 * decodes to PRESCALE=3, TIMEOUT1=31, TIMEOUT2=0,
 * RESET_MODE=0, ENABLE=1 under the watchdog control register layout odi_wdt.h states.
 * odi_wdt_ctrl_encode() at the same operating point (ODI_WDT_PRESCALE etc.,
 * what odi_wdt_arm() below uses) must reproduce that exact value: the
 * kernel own kicker has to end the register where U-Boot left it, not a
 * shortened timeout.
 */
static void test_ctrl_encode_matches_uboot_en_wdt(void)
{
	uint32_t reg = odi_wdt_ctrl_encode(ODI_WDT_PRESCALE, ODI_WDT_TIMEOUT1,
					    ODI_WDT_TIMEOUT2, ODI_WDT_RESET_MODE, 1);

	CHECK(reg == 0xe7c00000U, "odi_wdt_ctrl_encode(3,31,0,0,1) == U-Boot en_wdt value 0xe7c00000");
}

static void test_ctrl_encode_disabled_clears_e_bit_only(void)
{
	uint32_t reg = odi_wdt_ctrl_encode(ODI_WDT_PRESCALE, ODI_WDT_TIMEOUT1,
					    ODI_WDT_TIMEOUT2, ODI_WDT_RESET_MODE, 0);

	CHECK(reg == 0x67c00000U, "enable=0 clears only bit 31, PRESCALE/TIMEOUT1 unchanged");
}

static void test_kick_value_is_read_modify_write(void)
{
	CHECK(odi_wdt_kick_value(0x00000000U) == 0x80000000U, "kick from a clear register sets bit 31 alone");
	CHECK(odi_wdt_kick_value(0x0000002aU) == 0x8000002aU, "kick preserves whatever else the kick register already held");
}

static void test_force_reset_value_is_e_bit_alone(void)
{
	CHECK(odi_wdt_force_reset_value() == 0x80000000U, "force-reset value is ENABLE alone, every other field 0 (fastest stage-1 timeout)");
}

/* ---- Deadline/heartbeat/stall-report decision function ----------------- */

/* free_kb well above ODI_WDT_MEM_FLOOR_KB, so it never interferes with a
 * test not about the memory floor. */
#define AMPLE_KB (ODI_WDT_MEM_FLOOR_KB + 1024U)

static void test_heartbeat_fires_every_60s_not_before(void)
{
	struct odi_wdt_deadline_state st;

	odi_wdt_deadline_state_init(&st);
	CHECK(!(odi_wdt_deadline_tick(&st, 0, AMPLE_KB) & ODI_WDT_ACTION_HEARTBEAT), "no heartbeat yet at uptime 0 (first one is due at 60 s)");
	CHECK(!(odi_wdt_deadline_tick(&st, 30, AMPLE_KB) & ODI_WDT_ACTION_HEARTBEAT), "no heartbeat again at 30 s");
	CHECK(!(odi_wdt_deadline_tick(&st, 59, AMPLE_KB) & ODI_WDT_ACTION_HEARTBEAT), "no heartbeat yet at 59 s");
	CHECK(odi_wdt_deadline_tick(&st, 60, AMPLE_KB) & ODI_WDT_ACTION_HEARTBEAT, "heartbeat fires again at 60 s");
	CHECK(!(odi_wdt_deadline_tick(&st, 119, AMPLE_KB) & ODI_WDT_ACTION_HEARTBEAT), "no heartbeat yet at 119 s");
	CHECK(odi_wdt_deadline_tick(&st, 120, AMPLE_KB) & ODI_WDT_ACTION_HEARTBEAT, "heartbeat fires again at 120 s");
}

/* Stall report: enabled, at least one kick observed, more than 15 s since
 * it, capped at 3 reports (odi_wdt.h has the why -- three reports at most).
 */
static void test_stall_report_fires_past_15s_capped_at_3(void)
{
	struct odi_wdt_deadline_state st;

	odi_wdt_deadline_state_init(&st);
	st.watchdog_enabled = 1;
	odi_wdt_note_kick(&st, 10);

	CHECK(!(odi_wdt_deadline_tick(&st, 24, AMPLE_KB) & ODI_WDT_ACTION_STALL_REPORT), "no stall report at 24 s (14 s since the kick)");
	CHECK(odi_wdt_deadline_tick(&st, 26, AMPLE_KB) & ODI_WDT_ACTION_STALL_REPORT, "stall report fires at 26 s (16 s since the kick)");
	CHECK(st.stall_reports == 1, "one stall report issued");
	CHECK(odi_wdt_deadline_tick(&st, 27, AMPLE_KB) & ODI_WDT_ACTION_STALL_REPORT, "second stall report at 27 s, still no new kick");
	CHECK(odi_wdt_deadline_tick(&st, 28, AMPLE_KB) & ODI_WDT_ACTION_STALL_REPORT, "third stall report at 28 s");
	CHECK(st.stall_reports == 3, "three stall reports issued");
	CHECK(!(odi_wdt_deadline_tick(&st, 29, AMPLE_KB) & ODI_WDT_ACTION_STALL_REPORT), "no fourth stall report -- capped at 3");

	odi_wdt_note_kick(&st, 29);
	CHECK(!(odi_wdt_deadline_tick(&st, 40, AMPLE_KB) & ODI_WDT_ACTION_STALL_REPORT), "a fresh kick keeps the cap from re-triggering (11 s since the new kick)");
}

/* A disabled watchdog never stalls or deadlines (a userland-driven
 * odi_wdt_disable() -- /proc/odi_wdt/watchdog_flag write of 0 --
 * turns both checks off, the same effect that write has on the stock
 * firmware).
 */
static void test_disabled_watchdog_never_stalls_or_deadlines(void)
{
	struct odi_wdt_deadline_state st;

	odi_wdt_deadline_state_init(&st);
	st.watchdog_enabled = 0;
	odi_wdt_note_kick(&st, 0);

	CHECK(!(odi_wdt_deadline_tick(&st, 200, AMPLE_KB) & ODI_WDT_ACTION_STALL_REPORT), "disabled watchdog: no stall report even long after the last kick");
	CHECK(!(odi_wdt_deadline_tick(&st, 200, AMPLE_KB) & ODI_WDT_ACTION_FORCE_RESET), "disabled watchdog: no forced reset even long past the deadline");
}

/* Userland deadline: fires once, past 120 s, only while userland_ok is
 * still 0 -- and only once (odi_wdt.h has the why: the real hardware path
 * spins forever right after, so a second tick never happens there).
 */
static void test_userland_deadline_fires_once_past_120s(void)
{
	struct odi_wdt_deadline_state st;

	odi_wdt_deadline_state_init(&st);
	st.watchdog_enabled = 1;

	CHECK(!(odi_wdt_deadline_tick(&st, 120, AMPLE_KB) & ODI_WDT_ACTION_FORCE_RESET), "no forced reset AT exactly 120 s (the check is uptime_s > deadline)");
	CHECK(odi_wdt_deadline_tick(&st, 121, AMPLE_KB) & ODI_WDT_ACTION_FORCE_RESET, "forced reset fires once uptime passes 120 s");
	CHECK(!(odi_wdt_deadline_tick(&st, 130, AMPLE_KB) & ODI_WDT_ACTION_FORCE_RESET), "not signaled again on a later tick");
}

static void test_userland_confirm_prevents_the_deadline(void)
{
	struct odi_wdt_deadline_state st;

	odi_wdt_deadline_state_init(&st);
	st.watchdog_enabled = 1;
	st.userland_ok = 1; /* rcS already wrote 1 to userland_ok before the deadline */

	CHECK(!(odi_wdt_deadline_tick(&st, 200, AMPLE_KB) & ODI_WDT_ACTION_FORCE_RESET), "userland_ok=1 suppresses the forced reset entirely");
}

/* ---- Per-client ping deadlines: register arms the deadline immediately,
 * counted from the registration uptime -- a client that never pings at all
 * is caught exactly like one that pinged once and then stalled -- and a
 * client never registered is never checked. ------------------------------ */

static void test_unregistered_client_ping_is_refused(void)
{
	struct odi_wdt_deadline_state st;

	odi_wdt_deadline_state_init(&st);
	CHECK(odi_wdt_client_ping(&st, "omcid", 5) < 0, "a ping from a name nobody registered is refused");
}

static void test_registered_but_never_pinged_client_still_resets(void)
{
	struct odi_wdt_deadline_state st;

	odi_wdt_deadline_state_init(&st);
	st.watchdog_enabled = 1;
	st.userland_ok = 1;
	CHECK(odi_wdt_client_register(&st, "omcid", 60, 10) >= 0, "register succeeds into a free slot");

	CHECK(!(odi_wdt_deadline_tick(&st, 69, AMPLE_KB) & ODI_WDT_ACTION_CLIENT_MISS),
	      "no miss before the registration-counted deadline elapses");
	CHECK(odi_wdt_deadline_tick(&st, 71, AMPLE_KB) & ODI_WDT_ACTION_CLIENT_MISS,
	      "a client that registers and never pings at all is still caught: rcS only "
	      "registers a client it is about to start (svc-omcid.sh gates that), so "
	      "there is no longer a legitimate registered-but-silent-forever case");
}

static void test_client_miss_fires_once_past_its_own_deadline(void)
{
	struct odi_wdt_deadline_state st;
	unsigned int actions;

	odi_wdt_deadline_state_init(&st);
	st.watchdog_enabled = 1;
	st.userland_ok = 1;
	odi_wdt_client_register(&st, "omcid", 60, 0);
	CHECK(odi_wdt_client_ping(&st, "omcid", 200) == 0, "a later ping still pushes the deadline out");

	CHECK(!(odi_wdt_deadline_tick(&st, 260, AMPLE_KB) & ODI_WDT_ACTION_CLIENT_MISS),
	      "no miss AT exactly the deadline (uptime_s > last_ping + deadline)");
	actions = odi_wdt_deadline_tick(&st, 261, AMPLE_KB);
	CHECK(actions & ODI_WDT_ACTION_CLIENT_MISS, "miss fires once the deadline is exceeded");
	CHECK(actions & ODI_WDT_ACTION_FORCE_RESET, "a client miss also carries FORCE_RESET");
	CHECK(!(odi_wdt_deadline_tick(&st, 500, AMPLE_KB) & ODI_WDT_ACTION_CLIENT_MISS), "not signaled again on a later tick");
}

static void test_client_ping_pushes_its_own_deadline_out(void)
{
	struct odi_wdt_deadline_state st;

	odi_wdt_deadline_state_init(&st);
	st.watchdog_enabled = 1;
	st.userland_ok = 1;
	odi_wdt_client_register(&st, "omcid", 60, 0);
	odi_wdt_client_ping(&st, "omcid", 0);
	odi_wdt_client_ping(&st, "omcid", 20); /* a fresh ping before the deadline elapses */

	CHECK(!(odi_wdt_deadline_tick(&st, 45, AMPLE_KB) & ODI_WDT_ACTION_CLIENT_MISS),
	      "a fresh ping pushes the deadline out (45 s since boot, 25 s since the last ping)");
}

static void test_client_register_is_idempotent_on_name(void)
{
	struct odi_wdt_deadline_state st;
	int slot1, slot2;

	odi_wdt_deadline_state_init(&st);
	slot1 = odi_wdt_client_register(&st, "omcid", 60, 0);
	slot2 = odi_wdt_client_register(&st, "omcid", 90, 5); /* re-registered with a new deadline */
	CHECK(slot1 == slot2, "the same name reuses its slot rather than taking a second one");
	CHECK(st.clients[slot2].deadline_s == 90, "re-registering updates the deadline");
}

static void test_client_reregister_resets_the_clock(void)
{
	struct odi_wdt_deadline_state st;

	odi_wdt_deadline_state_init(&st);
	st.watchdog_enabled = 1;
	st.userland_ok = 1;
	odi_wdt_client_register(&st, "omcid", 60, 0);

	CHECK(odi_wdt_deadline_tick(&st, 61, AMPLE_KB) & ODI_WDT_ACTION_CLIENT_MISS,
	      "sanity: the first registration's deadline has indeed passed");

	odi_wdt_deadline_state_init(&st); /* a fresh boot, not a stale in-memory reset */
	st.watchdog_enabled = 1;
	st.userland_ok = 1;
	odi_wdt_client_register(&st, "omcid", 60, 0);
	odi_wdt_client_register(&st, "omcid", 60, 55); /* re-registered late in the same boot */

	CHECK(!(odi_wdt_deadline_tick(&st, 61, AMPLE_KB) & ODI_WDT_ACTION_CLIENT_MISS),
	      "re-registering resets last_ping_s to the new registration time, not the old one");
}

static void test_client_slots_are_limited(void)
{
	struct odi_wdt_deadline_state st;
	char name[ODI_WDT_CLIENT_NAME_LEN];
	unsigned int i;

	odi_wdt_deadline_state_init(&st);
	for (i = 0; i < ODI_WDT_MAX_CLIENTS; i++) {
		name[0] = 'a' + (char)i;
		name[1] = '\0';
		CHECK(odi_wdt_client_register(&st, name, 60, 0) >= 0, "every slot up to the max registers fine");
	}
	CHECK(odi_wdt_client_register(&st, "one-too-many", 60, 0) < 0, "one past the max is refused, not silently dropped");
}

/* ---- Kernel-side memory floor: consecutive low samples, not one. ------- */

static void test_mem_floor_needs_consecutive_low_samples(void)
{
	struct odi_wdt_deadline_state st;

	odi_wdt_deadline_state_init(&st);
	st.watchdog_enabled = 1;
	st.userland_ok = 1;

	CHECK(!(odi_wdt_deadline_tick(&st, 10, ODI_WDT_MEM_FLOOR_KB - 1) & ODI_WDT_ACTION_MEM_FLOOR),
	      "one low sample alone does not reset (needs ODI_WDT_MEM_FLOOR_CONSEC in a row)");
	CHECK(!(odi_wdt_deadline_tick(&st, 15, AMPLE_KB) & ODI_WDT_ACTION_MEM_FLOOR),
	      "a single healthy sample in between resets the streak");
}

static void test_mem_floor_fires_after_consecutive_low_samples(void)
{
	struct odi_wdt_deadline_state st;
	unsigned int t, actions = 0;

	odi_wdt_deadline_state_init(&st);
	st.watchdog_enabled = 1;
	st.userland_ok = 1;

	for (t = 0; t < ODI_WDT_MEM_FLOOR_CONSEC; t++)
		actions = odi_wdt_deadline_tick(&st, 10 + t * ODI_WDT_TICK_INTERVAL_S, ODI_WDT_MEM_FLOOR_KB - 1);
	CHECK(actions & ODI_WDT_ACTION_MEM_FLOOR, "MEM_FLOOR fires on the Nth consecutive low sample");
	CHECK(actions & ODI_WDT_ACTION_FORCE_RESET, "a memory-floor miss also carries FORCE_RESET");
	CHECK(!(odi_wdt_deadline_tick(&st, 1000, 0) & ODI_WDT_ACTION_MEM_FLOOR), "not signaled again on a later tick");
}

/* ---- Real arm/kick/disable/force-reset sequences, against the SoC
 * window of test/odi_soc_mock.h. ---------------------------------------- */

static void test_arm_writes_the_uboot_matching_value(void)
{
	odi_soc_mock_reset();
	odi_wdt_arm();
	CHECK(WDT_CTRL == 0xe7c00000U, "odi_wdt_arm() writes the watchdog control register to the U-Boot en_wdt value");
	/* U-Boot arms the watchdog before decompressing the kernel, which uses
	 * most of the timeout: arm has to kick once, or the kicker thread first
	 * kick comes too late.
	 */
	CHECK(WDT_KICK != 0U, "odi_wdt_arm() kicks the watchdog counter register once");
}

static void test_kick_is_real_read_modify_write_on_kick_reg(void)
{
	odi_soc_mock_reset();
	WDT_KICK = 0x0000002aU; /* some prior bits, as a real boot would leave */
	odi_wdt_kick();
	CHECK(WDT_KICK == 0x8000002aU, "odi_wdt_kick() reads the kick register back before writing (RMW), not a blind write");
}

static void test_disable_clears_only_the_e_bit(void)
{
	odi_soc_mock_reset();
	WDT_CTRL = 0xe7c00000U; /* armed, as odi_wdt_arm() left it */
	odi_wdt_disable();
	CHECK(WDT_CTRL == 0x67c00000U, "odi_wdt_disable() clears ENABLE, leaves PRESCALE/TIMEOUT1/TIMEOUT2/RESET_MODE untouched");
}

static void test_force_reset_overwrites_ctrlr_to_e_bit_alone(void)
{
	odi_soc_mock_reset();
	WDT_CTRL = 0xe7c00000U; /* armed at the normal (41.6 s) timeout */
	odi_wdt_force_reset();
	CHECK(WDT_CTRL == 0x80000000U, "the deadline force-reset re-arms at TIMEOUT1=0 -- the fastest the hardware can time out");
	CHECK(WDT_KICK == 0x80000000U, "the force-reset kicks after the control write, so the short timeout counts from zero");
}

static void test_allowlist_refuses_addresses_outside_the_window_list(void)
{
	uint32_t v;

	odi_soc_mock_reset();
	v = odi_wdt_reg_read(SOC_WDT_STATUS); /* allowlisted, unused elsewhere */
	CHECK(v == 0U, "an allowlisted read of the status register returns the (zeroed) mock value");
	CHECK(odi_soc_mock.reads == 1U, "the status register is on the allowlist -- the read reaches the window");

	odi_wdt_reg_write(0x3300U, 0xdeadbeefU); /* a nearby SoC offset -- NOT on the list */
	CHECK(odi_soc_mock.writes == 0U, "a write outside the allowlist never reaches the window");
	CHECK(odi_soc_write(0x3300U, 0) != 0, "and odi_soc_write() says it refused");
}

int main(void)
{
	test_ctrl_encode_matches_uboot_en_wdt();
	test_ctrl_encode_disabled_clears_e_bit_only();
	test_kick_value_is_read_modify_write();
	test_force_reset_value_is_e_bit_alone();

	test_heartbeat_fires_every_60s_not_before();
	test_stall_report_fires_past_15s_capped_at_3();
	test_disabled_watchdog_never_stalls_or_deadlines();
	test_userland_deadline_fires_once_past_120s();
	test_userland_confirm_prevents_the_deadline();

	test_unregistered_client_ping_is_refused();
	test_registered_but_never_pinged_client_still_resets();
	test_client_miss_fires_once_past_its_own_deadline();
	test_client_ping_pushes_its_own_deadline_out();
	test_client_register_is_idempotent_on_name();
	test_client_reregister_resets_the_clock();
	test_client_slots_are_limited();

	test_mem_floor_needs_consecutive_low_samples();
	test_mem_floor_fires_after_consecutive_low_samples();

	test_arm_writes_the_uboot_matching_value();
	test_kick_is_real_read_modify_write_on_kick_reg();
	test_disable_clears_only_the_e_bit();
	test_force_reset_overwrites_ctrlr_to_e_bit_alone();
	test_allowlist_refuses_addresses_outside_the_window_list();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_wdt_test: ok\n");
	return 0;
}
