/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_host_pause_test.c -- the PAUSE policy of the host port
 * (docs/SWITCH.md#host-port-pause), against the register mock: the final
 * value of the TX_PAUSE field in PORT_FORCE_ABILITY(0) and
 * PORT_FORCE_SELECT(0) after the platform settings, after the whole ISP1
 * provisioning (the 91 boot5 brackets, cmd 32 among them) through
 * odi_switch_cmd(), and with the policy turned on and off again, as the
 * odi_switch.host_pause parameter does on a running stick.
 *
 * The registers are seeded with the state a running stick reads before
 * any of it (a live register dump): port 0 not forced at all, its
 * negotiated ability 0xfe (1000 full duplex, link, PAUSE both ways),
 * ports 2 and 3 forced. Every other bit of the two words must survive.
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

#define TX_PAUSE_BIT	(1U << 6)

static uint32_t reg(uint32_t off)
{
	return odi_mock.regs[odi_mock_slot(off)];
}

/* A live stick before the platform settings: port 0 runs on the
 * negotiated ability, with a forced-ability word that has TX_PAUSE set
 * (and the other bits set too, so a read-modify-write that clobbers them
 * shows); port 2 (PON) and port 3 (CPU) forced, as the sdkinit and
 * module-load replays leave them.
 */
static void seed_live_stick(void)
{
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_FORCE_ABILITY(0))] = 0x7fU;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_FORCE_SELECT(0))] = 0x0U;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_ABILITY(0))] = 0xfeU;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_FORCE_ABILITY(2))] = 0x16U;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_FORCE_SELECT(2))] = 0xfffU;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_FORCE_ABILITY(3))] = 0x16U;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_FORCE_SELECT(3))] = 0xfffU;
}

static void check_other_ports_untouched(const char *when)
{
	char msg[160];

	snprintf(msg, sizeof(msg), "%s: ports 2 and 3 keep their forced ability", when);
	CHECK(reg(ODI_SW_PORT_FORCE_ABILITY(2)) == 0x16U && reg(ODI_SW_PORT_FORCE_SELECT(2)) == 0xfffU &&
	      reg(ODI_SW_PORT_FORCE_ABILITY(3)) == 0x16U && reg(ODI_SW_PORT_FORCE_SELECT(3)) == 0xfffU,
	      msg);
}

static void call(uint32_t cmd, void *buf, uint32_t len)
{
	(void)odi_switch_cmd(cmd, buf, len);
}

/* The default: the platform settings force TX_PAUSE off on port 0, the
 * forced value written before the select, and nothing else moves.
 */
static void test_platform_default(void)
{
	unsigned int i, last_val = 0, last_sel = 0;

	seed_live_stick();
	odi_sw_host_pause = false;
	odi_switch_init_platform();

	CHECK(reg(ODI_SW_PORT_FORCE_ABILITY(0)) == (0x7fU & ~TX_PAUSE_BIT),
	      "platform: PORT_FORCE_ABILITY(0).TX_PAUSE = 0, the other fields kept");
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(0)) == TX_PAUSE_BIT,
	      "platform: PORT_FORCE_SELECT(0) = TX_PAUSE only (0x40)");
	check_other_ports_untouched("platform");

	for (i = 0; i < odi_mock.log_n; i++) {
		if (odi_mock.log[i].kind != 'W')
			continue;
		if (odi_mock.log[i].addr == ODI_SW_PORT_FORCE_ABILITY(0))
			last_val = i + 1;
		if (odi_mock.log[i].addr == ODI_SW_PORT_FORCE_SELECT(0))
			last_sel = i + 1;
	}
	CHECK(last_val && last_sel && last_val < last_sel,
	      "platform: the forced value is written before the select");
}

/* cmd 32 writes PORT_FORCE_SELECT(0) at every provisioning; the stock
 * image writes 0 there, which would hand TX_PAUSE back to the
 * negotiation. The whole ISP1 provisioning must leave it forced.
 */
static void test_provisioning_keeps_it(void)
{
	seed_live_stick();
	odi_sw_host_pause = false;
	odi_switch_init_platform();
	isp1_boot5_replay(call);

	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(0)) == TX_PAUSE_BIT,
	      "after the ISP1 provisioning (cmd 32 included): PORT_FORCE_SELECT(0) = 0x40");
	CHECK((reg(ODI_SW_PORT_FORCE_ABILITY(0)) & TX_PAUSE_BIT) == 0,
	      "after the ISP1 provisioning: PORT_FORCE_ABILITY(0).TX_PAUSE = 0");
	check_other_ports_untouched("provisioning");

	/* A second provisioning in the same boot (MIB reset, respawned
	 * omcid) is the same.
	 */
	isp1_boot5_replay(call);
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(0)) == TX_PAUSE_BIT,
	      "after a second provisioning: PORT_FORCE_SELECT(0) = 0x40");
}

/* cmd 32 on a port other than the host port keeps the stock 0. */
static void test_cmd32_other_ports(void)
{
	seed_live_stick();
	odi_sw_host_pause = false;
	odi_sw_port_force_set(1);
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(1)) == 0,
	      "cmd 32 leaf on port 1: PORT_FORCE_SELECT(1) = 0");
	odi_sw_port_force_set(0);
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(0)) == TX_PAUSE_BIT,
	      "cmd 32 leaf on port 0: PORT_FORCE_SELECT(0) = 0x40");
}

/* odi_switch.host_pause=1: the stock behaviour, PAUSE left to the
 * negotiation, and cmd 32 writes the stock 0.
 */
static void test_policy_on(void)
{
	seed_live_stick();
	odi_sw_host_pause = true;
	odi_switch_init_platform();
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(0)) == 0,
	      "host_pause=1, platform: PORT_FORCE_SELECT(0) = 0 (negotiated)");
	isp1_boot5_replay(call);
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(0)) == 0,
	      "host_pause=1, after the ISP1 provisioning: PORT_FORCE_SELECT(0) = 0");
	check_other_ports_untouched("host_pause=1");
}

/* The parameter written on a running stick: off, on, off again, each one
 * apply, and the select follows; the forced value stays 0 throughout.
 */
static void test_toggle(void)
{
	seed_live_stick();
	odi_sw_host_pause = false;
	odi_switch_init_platform();

	odi_sw_host_pause = true;
	odi_sw_host_pause_apply();
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(0)) == 0,
	      "toggle on: PORT_FORCE_SELECT(0).TX_PAUSE cleared");
	CHECK((reg(ODI_SW_PORT_FORCE_ABILITY(0)) & TX_PAUSE_BIT) == 0,
	      "toggle on: PORT_FORCE_ABILITY(0).TX_PAUSE stays 0");

	odi_sw_host_pause = false;
	odi_sw_host_pause_apply();
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(0)) == TX_PAUSE_BIT,
	      "toggle off again: PORT_FORCE_SELECT(0) = 0x40");
	odi_sw_host_pause_apply();
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(0)) == TX_PAUSE_BIT,
	      "apply twice: idempotent");
	check_other_ports_untouched("toggle");
}

/* A select word with other fields already forced (the link, say) keeps
 * them: the apply is a read-modify-write of the one bit.
 */
static void test_select_rmw(void)
{
	seed_live_stick();
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_FORCE_SELECT(0))] = 0x10U;
	odi_sw_host_pause = false;
	odi_sw_host_pause_apply();
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(0)) == 0x50U,
	      "apply keeps another forced field (0x10 -> 0x50)");
	odi_sw_host_pause = true;
	odi_sw_host_pause_apply();
	CHECK(reg(ODI_SW_PORT_FORCE_SELECT(0)) == 0x10U,
	      "apply with host_pause=1 clears only TX_PAUSE (0x50 -> 0x10)");
}

int main(void)
{
	test_platform_default();
	test_provisioning_keeps_it();
	test_cmd32_other_ports();
	test_policy_on();
	test_toggle();
	test_select_rmw();

	if (failures) {
		fprintf(stderr, "odi_host_pause_test: %d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_host_pause_test: ok\n");
	return 0;
}
