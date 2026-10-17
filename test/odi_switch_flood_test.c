/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_switch_flood_test.c -- cmd 64 (setFloodingPortMask) against the
 * register mock. The stock OMCI layer reads the mask of the type it is
 * given and adds or removes only the ports of portMask; the old cmd 64
 * wrote 0x7 or 0 whatever it was told, so a class 47 delete (or ME 45
 * DiscardUnknown = 1) took every port out of unknown-unicast flooding.
 */
#include <stdio.h>
#include <string.h>

#include "odi_switch_unity.h"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_cmd.c"
#include "../kernel/extra/drivers/net/ethernet/odi/odi_switch_bdgconn.c"
#include "../kernel/extra/drivers/net/ethernet/odi/uapi/omci_bridgeport.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} \
} while (0)

#define UCAST	0x1c028U
#define BCAST	0x1c020U
#define MCAST	0x1c024U

static uint32_t reg(uint32_t addr)
{
	return odi_mock.regs[odi_mock_slot(addr)];
}

static int flood(uint32_t sel, uint32_t enable, uint32_t port_mask)
{
	struct omci_flood f = { .sel = sel, .enable = enable, .portMask = port_mask };

	return odi_switch_cmd(OMCI_FLOOD_CMD, &f, sizeof f);
}

static unsigned int writes(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W')
			n++;
	return n;
}

static void setup(void)
{
	odi_mock_reset();
	odi_switch_cmd_reset_state();
	odi_mock_flood_boot_state();
}

/* ISP1: the PON port added on a module-load 0xf gives the captured 0x7,
 * four writes of the one word.
 */
static void test_isp1(void)
{
	unsigned int i, n = 0;

	setup();
	CHECK(flood(0, 1, 1U << 2) == 0, "enable of the PON port accepted");
	CHECK(reg(UCAST) == 0x7U, "0xf -> 0x7: the PON kept, the CPU out as before");
	for (i = 0; i < odi_mock.log_n; i++)
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == UCAST &&
		    odi_mock.log[i].val == 0x7U)
			n++;
	CHECK(n == 4U && writes() == 4U, "four writes of 0x7, as the capture");
	CHECK(reg(BCAST) == 0xfU && reg(MCAST) == 0xfU, "the other two masks untouched");
}

/* A delete, or DiscardUnknown = 1, removes its one port. */
static void test_delete_one_port(void)
{
	setup();
	(void)flood(0, 1, 1U << 2);
	CHECK(flood(0, 0, 1U << 0) == 0, "a removal of the UNI accepted");
	CHECK(reg(UCAST) == 0x6U, "only the UNI bit goes: 0x7 -> 0x6");
	CHECK(flood(0, 1, 1U << 0) == 0, "and it comes back");
	CHECK(reg(UCAST) == 0x7U, "0x6 -> 0x7");
	CHECK(flood(0, 0, 1U << 2) == 0 && reg(UCAST) == 0x3U, "removing the PON leaves the UNI");
}

/* The type picks the mask; the CPU bit is only taken out of unknown unicast. */
static void test_types(void)
{
	setup();
	CHECK(flood(1, 0, 1U << 0) == 0 && reg(BCAST) == 0xeU, "broadcast mask, UNI removed, CPU kept");
	CHECK(flood(2, 0, 1U << 2) == 0 && reg(MCAST) == 0xbU, "multicast mask, PON removed, CPU kept");
	CHECK(reg(UCAST) == 0xfU, "unknown unicast untouched by them");
}

/* Trap (2), an unknown type, and bits past the four ports. */
static void test_refused(void)
{
	setup();
	odi_mock.log_n = 0;
	CHECK(flood(0, 2, 1U << 0) != 0, "act 2 (trap) refused, as stock");
	CHECK(flood(3, 1, 1U << 0) != 0, "an unknown type refused");
	CHECK(writes() == 0U, "and nothing written");
	CHECK(flood(1, 1, 0xf0U) == 0 && reg(BCAST) == 0xfU, "bits past port 3 ignored");
}

/* No argument: the replay harness, the captured call. */
static void test_no_argument(void)
{
	setup();
	CHECK(odi_switch_cmd(OMCI_FLOOD_CMD, NULL, 0) == 0 && reg(UCAST) == 0x7U,
	      "no argument is the captured enable of the PON port");
}

int main(void)
{
	test_isp1();
	test_delete_one_port();
	test_types();
	test_refused();
	test_no_argument();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_switch_flood_test: ok\n");
	return 0;
}
