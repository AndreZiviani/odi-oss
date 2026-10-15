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

uint32_t odi_sw_port_force_get(uint32_t port)
{
	return odi_reg_read(ODI_SW_PORT_FORCE_SELECT(port));
}

bool odi_sw_host_pause;

/* The FORCE_SELECT bits a port keeps through cmd 32: TX_PAUSE on the host
 * port while PAUSE toward the host is off, nothing otherwise.
 */
static uint32_t odi_sw_port_force_kept(uint32_t port)
{
	if (port != ODI_SW_HOST_PORT || odi_sw_host_pause)
		return 0;
	return ODI_SW_PORT_FORCE_SELECT_TX_PAUSE_SET(0, 1);
}

/* Every other FORCE_* bit clear: speed, duplex and link come from the
 * PHY or the SerDes auto-negotiation. The stock cmd 32 writes 0; on the
 * host port this keeps the TX_PAUSE force of odi_sw_host_pause_apply(),
 * which a 0 would undo at every provisioning.
 */
void odi_sw_port_force_set(uint32_t port)
{
	odi_reg_write(ODI_SW_PORT_FORCE_SELECT(port), odi_sw_port_force_kept(port));
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

uint32_t odi_sw_l2_flood_mask_get(void)
{
	return odi_reg_read(ODI_SW_FLOOD_UNKN_UCAST_PORTS_BASE);
}

/* One word holds the bit of every port; the capture writes that same word
 * four times, and the host replay compares the repeat count too.
 */
void odi_sw_l2_flood_mask_set(uint32_t mask)
{
	unsigned int i;

	for (i = 0; i < ODI_SW_PORT_COUNT; i++)
		odi_reg_write(ODI_SW_FLOOD_UNKN_UCAST_PORTS_BASE, mask);
}
