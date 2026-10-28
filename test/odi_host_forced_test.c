/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_host_forced_test.c -- the forced host port of the 2500BASE-X and
 * HiSGMII host SerDes modes (docs/SWITCH.md#host-port-serdes-mode), against
 * the register mock: what odi_sw_host_forced_apply() writes to
 * PORT_FORCE_ABILITY(0) and PORT_FORCE_SELECT(0) in each direction, with
 * the PAUSE policy either way; that cmd 32 (the OLT admin lock and unlock)
 * moves the forced LINK value and keeps the select on a forced port; that
 * the whole ISP1 provisioning leaves the force in place; that turning it
 * off puts both words back as the force found them, and writing it off
 * while off touches nothing; that an admin lock survives the force being
 * turned on and off; and that the platform
 * settings touch neither word for it unless it is on.
 *
 * Seeded as odi_host_pause_test.c is: port 0 not forced, its forced-ability
 * word 0x7f (so a write that clobbers a bit shows), ports 2 and 3 forced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"

#include "../kernel/extra/drivers/net/ethernet/odi/uapi/omci_gemflow.h"
#include "../kernel/extra/drivers/net/ethernet/odi/uapi/omci_bdgconn.h"
#include "bdgconn_rules.h"
#include "isp1_boot5_cmds.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} else { \
		printf("ok    %s\n", msg); \
	} \
} while (0)

#define ABILITY		ODI_SW_PORT_FORCE_ABILITY(0)
#define SELECT		ODI_SW_PORT_FORCE_SELECT(0)
#define LINK_BIT	(1U << 4)
#define TX_PAUSE_BIT	(1U << 6)

static uint32_t reg(uint32_t off)
{
	return odi_mock.regs[odi_mock_slot(off)];
}

static void seed(void)
{
	/* The words a force saved belong to the driver: an off clears them,
	 * whatever an earlier test left; the mock reset then wipes its writes.
	 */
	odi_sw_host_forced = false;
	odi_sw_host_forced_apply();
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	odi_mock.regs[odi_mock_slot(ABILITY)] = 0x7fU;
	odi_mock.regs[odi_mock_slot(SELECT)] = 0x0U;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_ABILITY(0))] = 0xfeU;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_FORCE_ABILITY(2))] = 0x16U;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_FORCE_SELECT(2))] = 0xfffU;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_FORCE_ABILITY(3))] = 0x16U;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_FORCE_SELECT(3))] = 0xfffU;
	odi_sw_host_pause = false;
	odi_sw_host_forced = false;
}

static void call(uint32_t cmd, void *buf, uint32_t len)
{
	(void)odi_switch_cmd(cmd, buf, len);
}

struct cmd32_arg {
	uint32_t port;
	uint32_t state;
};

static int cmd32(uint32_t port, uint32_t state)
{
	struct cmd32_arg a = { port, state };

	return odi_switch_cmd(OMCI_PORT_STATE_CMD, &a, sizeof a);
}

static unsigned int writes_to(uint32_t off)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		n += odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == off;
	return n;
}

/* 1 when the last write to ABILITY comes before the last one to SELECT. */
static int value_before_select(void)
{
	unsigned int i, val_at = 0, sel_at = 0;

	for (i = 0; i < odi_mock.log_n; i++) {
		if (odi_mock.log[i].kind != 'W')
			continue;
		if (odi_mock.log[i].addr == ABILITY)
			val_at = i + 1;
		if (odi_mock.log[i].addr == SELECT)
			sel_at = i + 1;
	}
	return val_at && sel_at && val_at < sel_at;
}

static void check_other_ports_untouched(const char *when)
{
	char msg[160];

	snprintf(msg, sizeof(msg), "%s: ports 2 and 3 keep their forced ability", when);
	CHECK(reg(ODI_SW_PORT_FORCE_ABILITY(2)) == 0x16U && reg(ODI_SW_PORT_FORCE_SELECT(2)) == 0xfffU &&
	      reg(ODI_SW_PORT_FORCE_ABILITY(3)) == 0x16U && reg(ODI_SW_PORT_FORCE_SELECT(3)) == 0xfffU,
	      msg);
}

/* The default: the platform settings write port 0 for the PAUSE policy
 * only, exactly as before the parameter existed.
 */
static void test_platform_default(void)
{
	seed();
	odi_switch_init_platform();
	CHECK(reg(ABILITY) == (0x7fU & ~TX_PAUSE_BIT) && reg(SELECT) == TX_PAUSE_BIT,
	      "platform, host_forced=N: only the PAUSE policy (0x3f, 0x40)");
	CHECK(writes_to(ABILITY) == 1 && writes_to(SELECT) == 1,
	      "platform, host_forced=N: one write to each word, the PAUSE ones");

	seed();
	odi_sw_host_forced = true;
	odi_switch_init_platform();
	CHECK(reg(ABILITY) == 0x16U && reg(SELECT) == 0xfffU,
	      "platform, host_forced=Y on the command line: forced (0x16, 0xfff)");
	check_other_ports_untouched("platform");
}

/* On: the stock HSGQ values, the value before the select; the PAUSE
 * policy keeps its say on the TX_PAUSE select bit.
 */
static void test_on(void)
{
	seed();
	odi_switch_init_platform();
	odi_mock.log_n = 0;
	odi_sw_host_forced = true;
	odi_sw_host_forced_apply();
	CHECK(reg(ABILITY) == 0x16U, "on: PORT_FORCE_ABILITY(0) = 0x16 (link up, full duplex, 1000)");
	CHECK(reg(SELECT) == 0xfffU, "on: PORT_FORCE_SELECT(0) = 0xfff");
	CHECK(value_before_select(), "on: the value is written before the select");

	odi_sw_host_forced_apply();
	CHECK(reg(ABILITY) == 0x16U && reg(SELECT) == 0xfffU, "on twice: the same values");

	odi_sw_host_pause = true;
	odi_sw_host_forced_apply();
	CHECK(reg(ABILITY) == 0x16U && reg(SELECT) == 0xfbfU,
	      "on with host_pause=Y: TX_PAUSE left unforced (0xfbf)");
	check_other_ports_untouched("on");
}

/* Off again: both words back to what they held before the force, the
 * PAUSE force kept. Twice, from the word a running stick reads (0x0) and
 * from one with every other bit set, so a bit not put back shows.
 */
static void test_off(void)
{
	uint32_t seeds[2] = { 0x0U, 0x7fU }, before_val, before_sel;
	unsigned int k;
	char msg[160];

	for (k = 0; k < 2; k++) {
		seed();
		odi_mock.regs[odi_mock_slot(ABILITY)] = seeds[k];
		odi_switch_init_platform();
		before_val = reg(ABILITY);
		before_sel = reg(SELECT);
		odi_sw_host_forced_set(true, true);
		odi_sw_host_forced_set(true, true);
		odi_sw_host_forced_set(false, true);
		snprintf(msg, sizeof(msg), "on, on, off from 0x%02x: PORT_FORCE_ABILITY(0) back to 0x%02x (is 0x%02x)",
			 (unsigned int)seeds[k], (unsigned int)before_val, (unsigned int)reg(ABILITY));
		CHECK(reg(ABILITY) == before_val, msg);
		snprintf(msg, sizeof(msg), "on, on, off from 0x%02x: PORT_FORCE_SELECT(0) back to 0x%02x (is 0x%02x)",
			 (unsigned int)seeds[k], (unsigned int)before_sel, (unsigned int)reg(SELECT));
		CHECK(reg(SELECT) == before_sel, msg);
	}
	CHECK(reg(SELECT) == TX_PAUSE_BIT, "off: PORT_FORCE_SELECT(0) back to TX_PAUSE only (0x40)");
	CHECK((reg(ABILITY) & TX_PAUSE_BIT) == 0, "off: the forced TX_PAUSE value stays 0");
	check_other_ports_untouched("off");

	/* The second force saves afresh: a word changed between two forces
	 * is what the second off puts back.
	 */
	seed();
	odi_mock.regs[odi_mock_slot(ABILITY)] = 0x0U;
	odi_switch_init_platform();
	odi_sw_host_forced_set(true, true);
	odi_sw_host_forced_set(false, true);
	odi_mock.regs[odi_mock_slot(ABILITY)] = 0x01U;
	odi_sw_host_forced_set(true, true);
	odi_sw_host_forced_set(false, true);
	CHECK(reg(ABILITY) == 0x01U, "a second on and off puts back the word the second on found");
}

/* N while already N: nothing read, nothing written. */
static void test_off_while_off(void)
{
	seed();
	odi_switch_init_platform();
	odi_mock_log_reads = 1;
	odi_mock.log_n = 0;
	odi_sw_host_forced_set(false, true);
	odi_sw_host_forced_set(false, true);
	CHECK(odi_mock.log_n == 0, "N while N: no register read or written");
	odi_mock_log_reads = 0;
	CHECK(!odi_sw_host_forced, "N while N: still off");
}

/* cmd 32 on a forced port: the LINK value moves, the select stays. */
static void test_cmd32(void)
{
	seed();
	odi_switch_init_platform();
	odi_sw_host_forced = true;
	odi_sw_host_forced_apply();

	odi_mock.log_n = 0;
	CHECK(cmd32(0, 0) == 0, "forced, lock accepted");
	CHECK(reg(ABILITY) == (0x16U & ~LINK_BIT), "forced, lock: the forced LINK value is 0 (0x06)");
	CHECK(reg(SELECT) == 0xfffU, "forced, lock: the select stays 0xfff");
	CHECK(value_before_select(), "forced, lock: the value is written before the select");

	CHECK(cmd32(0, 1) == 0, "forced, unlock accepted");
	CHECK(reg(ABILITY) == 0x16U, "forced, unlock: the forced LINK value is 1 again (0x16)");
	CHECK(reg(SELECT) == 0xfffU, "forced, unlock: the select stays 0xfff, the link stays forced up");

	CHECK(cmd32(1, 0) == 0, "forced port 0, lock of port 1 accepted");
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(1)) == LINK_BIT,
	      "forced port 0: another UNI port still gets the plain lock");
	check_other_ports_untouched("cmd 32");
}

/* The whole ISP1 provisioning, cmd 32 unlock among it, twice. */
static void test_provisioning(void)
{
	seed();
	odi_switch_init_platform();
	odi_sw_host_forced = true;
	odi_sw_host_forced_apply();
	isp1_boot5_replay(call);
	CHECK(reg(ABILITY) == 0x16U && reg(SELECT) == 0xfffU,
	      "after the ISP1 provisioning: still forced (0x16, 0xfff)");
	isp1_boot5_replay(call);
	CHECK(reg(ABILITY) == 0x16U && reg(SELECT) == 0xfffU,
	      "after a second provisioning: still forced");
	check_other_ports_untouched("provisioning");
}

/* An admin lock is carried across the force, both ways. */
static void test_lock_across(void)
{
	seed();
	odi_switch_init_platform();
	CHECK(cmd32(0, 0) == 0, "lock before the force accepted");
	odi_sw_host_forced = true;
	odi_sw_host_forced_apply();
	CHECK(reg(ABILITY) == 0x06U && reg(SELECT) == 0xfffU,
	      "locked, then forced: the forced LINK value stays 0 (0x06, 0xfff)");
	odi_sw_host_forced = false;
	odi_sw_host_forced_apply();
	CHECK(reg(SELECT) == (LINK_BIT | TX_PAUSE_BIT) && (reg(ABILITY) & LINK_BIT) == 0,
	      "locked, forced, then not: still locked (select 0x50, LINK value 0)");
	CHECK(cmd32(0, 1) == 0 && reg(SELECT) == TX_PAUSE_BIT,
	      "and the unlock after it frees the port (0x40)");
}

int main(void)
{
	test_platform_default();
	test_on();
	test_off();
	test_off_while_off();
	test_cmd32();
	test_provisioning();
	test_lock_across();

	if (failures) {
		fprintf(stderr, "odi_host_forced_test: %d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_host_forced_test: ok\n");
	return 0;
}
