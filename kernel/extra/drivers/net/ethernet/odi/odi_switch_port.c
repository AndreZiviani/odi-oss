// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_port.c -- port and L2 leaves: L2 ageing (cmd 62), the UNI PHY
 * auto-negotiation (cmd 30), the port force select (cmd 32), the flood
 * mask (cmd 64), and the PAUSE policy and the 1000BASE-X auto-negotiation
 * of the host port.
 * odi_switch_dal.h has the contract, docs/SWITCH.md#command-leaves the
 * capture each sequence reproduces.
 */
#include "odi_switch_dal.h"
#include "odi_switch_reg.h"

/* The three UNI PHY registers cmd 30 reads and writes, named by their
 * indirect PHY address: their fields are not decoded.
 */
#define ODI_SW_PHY_ADDR_A400		0xa400U
#define ODI_SW_PHY_ADDR_A408		0xa408U
#define ODI_SW_PHY_ADDR_A412		0xa412U

void odi_sw_l2_aging_set(uint32_t age_spd, uint32_t linkdown_ageout)
{
	uint32_t reg = odi_reg_read(ODI_SW_L2_LOOKUP_SETUP_OFF);

	reg = ODI_SW_L2_LOOKUP_SETUP_AGE_TICKS_SET(reg, age_spd);
	reg = ODI_SW_L2_LOOKUP_SETUP_AGE_ON_LINK_DOWN_SET(reg, linkdown_ageout);
	odi_reg_write(ODI_SW_L2_LOOKUP_SETUP_OFF, reg);
}

void odi_sw_port_autoneg_get(void)
{
	uint32_t reg;

	reg = ODI_SW_PHY_ACCESS_CMD_START_SET(0, 1);
	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A400);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);

	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A408);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);
	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A412);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);

	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A400);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);

	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A408);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);
	reg = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(reg, ODI_SW_PHY_ADDR_A412);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, reg);
}

/* The caller runs odi_sw_port_autoneg_get() first, in the same command,
 * as the capture does; the read command on a400 before its write is in
 * the capture too.
 */
void odi_sw_port_autoneg_set(uint32_t reg_a408, uint32_t reg_a412, uint32_t reg_a400)
{
	uint32_t cmd;

	odi_reg_write(ODI_SW_PHY_ACCESS_DATA_OFF, ODI_SW_PHY_ACCESS_DATA_WRITE_DATA_SET(0, reg_a408));
	cmd = ODI_SW_PHY_ACCESS_CMD_WRITE_SET(0, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_START_SET(cmd, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(cmd, ODI_SW_PHY_ADDR_A408);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, cmd);

	odi_reg_write(ODI_SW_PHY_ACCESS_DATA_OFF, ODI_SW_PHY_ACCESS_DATA_WRITE_DATA_SET(0, reg_a412));
	cmd = ODI_SW_PHY_ACCESS_CMD_WRITE_SET(0, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_START_SET(cmd, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(cmd, ODI_SW_PHY_ADDR_A412);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, cmd);

	cmd = ODI_SW_PHY_ACCESS_CMD_START_SET(0, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(cmd, ODI_SW_PHY_ADDR_A400);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, cmd);

	odi_reg_write(ODI_SW_PHY_ACCESS_DATA_OFF, ODI_SW_PHY_ACCESS_DATA_WRITE_DATA_SET(0, reg_a400));
	cmd = ODI_SW_PHY_ACCESS_CMD_WRITE_SET(0, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_START_SET(cmd, 1);
	cmd = ODI_SW_PHY_ACCESS_CMD_ADDRESS_SET(cmd, ODI_SW_PHY_ADDR_A400);
	odi_reg_write(ODI_SW_PHY_ACCESS_CMD_OFF, cmd);
}

bool odi_sw_host_pause;

/* The stock admin enable of the RTL9602C SDK, as its OMCI layer reaches it
 * for a port with no forced ability (dal_rtl9602c_port_adminEnable_set(),
 * read, not copied): a lock forces the LINK ability to down, the value
 * first and then its select bit; an unlock clears the select bit and the
 * port follows its PHY or SerDes again. Field writes of bit 4 only, so
 * the host-port TX_PAUSE force (below) and anything else forced is kept.
 * The capture's unlock is this read and write on a word that held 0.
 */
void odi_sw_port_admin_set(uint32_t port, int enable)
{
	uint32_t reg;

	if (!enable) {
		reg = odi_reg_read(ODI_SW_PORT_FORCE_ABILITY(port));
		odi_reg_write(ODI_SW_PORT_FORCE_ABILITY(port), ODI_SW_PORT_FORCE_ABILITY_LINK_SET(reg, 0));
	}
	reg = odi_reg_read(ODI_SW_PORT_FORCE_SELECT(port));
	odi_reg_write(ODI_SW_PORT_FORCE_SELECT(port), ODI_SW_PORT_FORCE_SELECT_LINK_SET(reg, enable ? 0U : 1U));
}

void odi_sw_host_pause_apply(void)
{
	uint32_t reg;

	/* The value first, then the select, so the port never runs with a
	 * stale forced value; the value is 0 whichever way the select goes.
	 */
	reg = odi_reg_read(ODI_SW_PORT_FORCE_ABILITY(ODI_SW_HOST_PORT));
	reg = ODI_SW_PORT_FORCE_ABILITY_TX_PAUSE_SET(reg, 0);
	odi_reg_write(ODI_SW_PORT_FORCE_ABILITY(ODI_SW_HOST_PORT), reg);

	reg = odi_reg_read(ODI_SW_PORT_FORCE_SELECT(ODI_SW_HOST_PORT));
	reg = ODI_SW_PORT_FORCE_SELECT_TX_PAUSE_SET(reg, odi_sw_host_pause ? 0 : 1);
	odi_reg_write(ODI_SW_PORT_FORCE_SELECT(ODI_SW_HOST_PORT), reg);

	reg = odi_reg_read(ODI_SW_PORT_ABILITY(ODI_SW_HOST_PORT));
	ODI_SW_LOG("host port PAUSE toward the host %s: ability 0x%03x (link %u, tx pause %u, rx pause %u)\n",
		   odi_sw_host_pause ? "negotiated" : "off", (unsigned int)reg,
		   (unsigned int)ODI_SW_PORT_ABILITY_LINK_UP_GET(reg),
		   (unsigned int)ODI_SW_PORT_ABILITY_TX_PAUSE_GET(reg),
		   (unsigned int)ODI_SW_PORT_ABILITY_RX_PAUSE_GET(reg));
}

bool odi_sw_pon_isolate = true;

/* The PON port's isolation mask: with odi_sw_pon_isolate, the CPU port and
 * every extension port (the CPU side) are cleared from it, so a frame that
 * enters from the PON may be forwarded to the UNIs only, whatever the L2
 * lookup, a flood mask or a classifier row decides. Without it they are
 * set again, the all-open reset value. One read-modify-write of the PON
 * port's 11 bits; the other port in the word is kept. Ingress from every
 * other port is untouched: the host reaches the CPU, and the CPU reaches
 * the PON (OMCI replies).
 */
#define ODI_SW_PON_PORT			2U
#define ODI_SW_ISOLATION_CPU_SIDE	(0x7f0U | (1U << 3))	/* extension ports, CPU port */

void odi_sw_pon_isolate_apply(void)
{
	uint32_t off = ODI_SW_PORT_ISOLATION(ODI_SW_PON_PORT);
	uint32_t item = ODI_SW_PON_PORT % 2U;
	uint32_t reg, mask;

	reg = odi_reg_read(off);
	mask = ODI_SW_PORT_ISOLATION_EGRESS_GET(reg, item);
	if (odi_sw_pon_isolate)
		mask &= ~ODI_SW_ISOLATION_CPU_SIDE;
	else
		mask |= ODI_SW_ISOLATION_CPU_SIDE;
	odi_reg_write(off, ODI_SW_PORT_ISOLATION_EGRESS_SET(reg, item, mask));

	mask = ODI_SW_PORT_ISOLATION_EGRESS_GET(odi_reg_read(off), item);
	ODI_SW_LOG("PON port isolation %s: the PON port forwards to 0x%03x%s\n",
		   odi_sw_pon_isolate ? "on" : "off", (unsigned int)mask,
		   (mask & ODI_SW_ISOLATION_CPU_SIDE) ? " (the CPU included)" : " (never the CPU)");
}

bool odi_sw_host_autoneg = true;

/* Only the AUTONEG bit of the PCS control word moves; speed (1000) and
 * duplex (full) stay as the Fiber 1G mode set left them, so clearing it
 * leaves the PCS forced to 1000BASE-X full duplex. Turning it back on
 * also sets AUTONEG_RESTART (self-clearing), so the PCS negotiates again
 * at once rather than at the next loss of sync. Nothing is written when
 * the bit already holds the wanted value: a write that restarts the
 * negotiation would flap a working link.
 */
void odi_sw_host_autoneg_apply(void)
{
	uint32_t before, reg;

	before = odi_reg_read(ODI_SW_HOST_FIBER_CTRL_OFF);
	if (ODI_SW_HOST_FIBER_CTRL_AUTONEG_GET(before) == (odi_sw_host_autoneg ? 1U : 0U)) {
		ODI_SW_LOG("host port auto-negotiation already %s: control 0x%04x\n",
			   odi_sw_host_autoneg ? "on" : "off (forced 1000BASE-X full duplex)",
			   (unsigned int)before);
		return;
	}
	reg = ODI_SW_HOST_FIBER_CTRL_AUTONEG_SET(before, odi_sw_host_autoneg ? 1U : 0U);
	reg = ODI_SW_HOST_FIBER_CTRL_AUTONEG_RESTART_SET(reg, odi_sw_host_autoneg ? 1U : 0U);
	odi_reg_write(ODI_SW_HOST_FIBER_CTRL_OFF, reg);
	ODI_SW_LOG("host port auto-negotiation %s: control 0x%04x -> 0x%04x, ability 0x%03x\n",
		   odi_sw_host_autoneg ? "on" : "off (forced 1000BASE-X full duplex)",
		   (unsigned int)before, (unsigned int)reg,
		   (unsigned int)odi_reg_read(ODI_SW_PORT_ABILITY(ODI_SW_HOST_PORT)));
}

uint32_t odi_sw_host_link_up(void)
{
	return ODI_SW_PORT_ABILITY_LINK_UP_GET(odi_reg_read(ODI_SW_PORT_ABILITY(ODI_SW_HOST_PORT)));
}

static uint32_t odi_sw_l2_flood_reg(uint32_t type)
{
	switch (type) {
	case ODI_SW_FLOOD_BCAST:
		return ODI_SW_FLOOD_BCAST_PORTS_BASE;
	case ODI_SW_FLOOD_UNKN_MCAST:
		return ODI_SW_FLOOD_UNKN_MCAST_PORTS_BASE;
	default:
		return ODI_SW_FLOOD_UNKN_UCAST_PORTS_BASE;
	}
}

uint32_t odi_sw_l2_flood_mask_get(uint32_t type)
{
	return odi_reg_read(odi_sw_l2_flood_reg(type));
}

/* One word holds the bit of every port. The stock SDK writes it once per
 * port, each a field write of that port's bit, so the capture shows the
 * same word four times; the host replay compares the repeat count too.
 */
void odi_sw_l2_flood_mask_set(uint32_t type, uint32_t mask)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_PORT_COUNT; i++)
		odi_reg_write(odi_sw_l2_flood_reg(type), mask);
}
