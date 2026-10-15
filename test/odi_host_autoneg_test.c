/* SPDX-License-Identifier: GPL-2.0 */
/*
 * odi_host_autoneg_test.c -- the 1000BASE-X auto-negotiation of the host
 * port (docs/SWITCH.md#host-port-auto-negotiation), against the register
 * mock: what odi_sw_host_autoneg_apply() writes to HOST_FIBER_CTRL
 * (0x021c00) in each direction, that it writes nothing when the bit
 * already matches, that the platform settings touch the word only when
 * the parameter is off, and what odi_sw_host_link_up() reads.
 *
 * Seeded with the word a running stick reads (0x1140: AUTONEG,
 * FULL_DUPLEX, SPEED_MSB) and PORT_ABILITY(0) = 0xfe (link up).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odi_switch_unity.h"

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		failures++; \
	} else { \
		printf("ok    %s\n", msg); \
	} \
} while (0)

#define CTRL	ODI_SW_HOST_FIBER_CTRL_OFF

static uint32_t reg(uint32_t off)
{
	return odi_mock.regs[odi_mock_slot(off)];
}

static void seed(uint32_t ctrl)
{
	odi_mock_reset();
	odi_mock.regs[odi_mock_slot(CTRL)] = ctrl;
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_ABILITY(0))] = 0xfeU;
}

/* Writes to one offset since the last seed. */
static unsigned int writes_to(uint32_t off, uint32_t *last)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++) {
		if (odi_mock.log[i].kind == 'W' && odi_mock.log[i].addr == off) {
			n++;
			if (last)
				*last = odi_mock.log[i].val;
		}
	}
	return n;
}

static unsigned int writes_total(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < odi_mock.log_n; i++)
		n += odi_mock.log[i].kind == 'W';
	return n;
}

static void test_off_from_running(void)
{
	uint32_t last = 0;

	seed(0x1140U);
	odi_sw_host_autoneg = false;
	odi_sw_host_autoneg_apply();
	CHECK(reg(CTRL) == 0x0140U, "off: 0x1140 -> 0x0140 (AUTONEG cleared, 1000 full duplex kept)");
	CHECK(writes_to(CTRL, &last) == 1 && last == 0x0140U, "off: exactly one write, of 0x0140");
	CHECK(writes_total() == 1, "off: nothing else written");

	odi_sw_host_autoneg_apply();
	CHECK(writes_to(CTRL, NULL) == 1, "off twice: the second apply writes nothing");
}

static void test_on_from_forced(void)
{
	uint32_t last = 0;

	seed(0x0140U);
	odi_sw_host_autoneg = true;
	odi_sw_host_autoneg_apply();
	CHECK(writes_to(CTRL, &last) == 1 && last == 0x1340U,
	      "on: one write of 0x1340 (AUTONEG and AUTONEG_RESTART)");
	CHECK(writes_total() == 1, "on: nothing else written");
}

static void test_on_when_on(void)
{
	seed(0x1140U);
	odi_sw_host_autoneg = true;
	odi_sw_host_autoneg_apply();
	CHECK(writes_total() == 0, "on while already on: no write, so no renegotiation and no flap");
}

/* Every other bit survives: the read-modify-write moves bit 12 (and bit 9
 * when turning on) only.
 */
static void test_other_bits_kept(void)
{
	seed(0x1140U | 0x0800U | 0x0020U);
	odi_sw_host_autoneg = false;
	odi_sw_host_autoneg_apply();
	CHECK(reg(CTRL) == (0x0140U | 0x0800U | 0x0020U), "off keeps the other bits (POWER_DOWN, bit 5)");
	odi_sw_host_autoneg = true;
	odi_sw_host_autoneg_apply();
	CHECK(reg(CTRL) == (0x1340U | 0x0800U | 0x0020U), "on keeps them too");
}

/* The platform settings: the default touches nothing in the word, not
 * even a read (the boot goldens pin every read), and off applies it.
 */
static void test_platform(void)
{
	unsigned int i, reads = 0;

	seed(0x1140U);
	odi_mock_log_reads = 1;
	odi_sw_host_pause = false;
	odi_sw_host_autoneg = true;
	odi_switch_init_platform();
	for (i = 0; i < odi_mock.log_n; i++)
		reads += odi_mock.log[i].addr == CTRL;
	CHECK(reads == 0 && reg(CTRL) == 0x1140U, "platform, default: HOST_FIBER_CTRL neither read nor written");

	seed(0x1140U);
	odi_sw_host_autoneg = false;
	odi_switch_init_platform();
	CHECK(reg(CTRL) == 0x0140U, "platform, host_autoneg=0 on the command line: forced");
	odi_mock_log_reads = 0;
	odi_sw_host_autoneg = true;
}

static void test_link(void)
{
	seed(0x1140U);
	CHECK(odi_sw_host_link_up() == 1, "link: PORT_ABILITY(0) 0xfe reads as up");
	odi_mock.regs[odi_mock_slot(ODI_SW_PORT_ABILITY(0))] = 0xeeU;
	CHECK(odi_sw_host_link_up() == 0, "link: LINK_UP (bit 4) clear reads as down");
}

int main(void)
{
	test_off_from_running();
	test_on_from_forced();
	test_on_when_on();
	test_other_bits_kept();
	test_platform();
	test_link();

	if (failures) {
		fprintf(stderr, "odi_host_autoneg_test: %d check(s) failed\n", failures);
		return 1;
	}
	printf("odi_host_autoneg_test: ok\n");
	return 0;
}
