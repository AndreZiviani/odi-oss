// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_port.c -- port and L2 leaves: L2 ageing (cmd 62), the UNI PHY
 * auto-negotiation (cmd 30), the port force select (cmd 32), the flood
 * mask (cmd 64), the I2C select of the transceiver read (cmd 10), and the
 * PAUSE policy of the host port.
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

/* cmd 10: the device-select word of the port-1 I2C master, and the two
 * GPIO pins of the transceiver I2C select. Pin 29 takes gpio_lo, then
 * gpio_hi; pin 31 is always driven to 1, its role not decoded.
 */
#define ODI_SW_I2C_PORT1_DEVSEL		0x234413aU
#define ODI_SW_GPIO_PIN_I2C_SELECT	29U
#define ODI_SW_GPIO_PIN_I2C_STROBE	31U

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

void odi_sw_ponmac_transceiver_get(uint32_t gpio_lo, uint32_t gpio_hi)
{
	unsigned int i;

	/* I2C_MASTER_SETUP(1) is the device select of the I2C master a DDM
	 * read drives, so a DDM read in progress would carry on with this
	 * select word: odi_i2c_lock (odi_switch.c).
	 */
	mutex_lock(&odi_i2c_lock);
	for (i = 0; i < 2; i++) {
		odi_reg_write(ODI_SW_I2C_MASTER_SETUP(1), ODI_SW_I2C_PORT1_DEVSEL);
		odi_reg_write(ODI_SW_PIN_GPIO_SELECT(ODI_SW_GPIO_PIN_I2C_SELECT), gpio_lo);
		odi_reg_write(ODI_SW_PIN_GPIO_SELECT(ODI_SW_GPIO_PIN_I2C_STROBE), 1);
		gpio_lo = gpio_hi;
	}
	mutex_unlock(&odi_i2c_lock);
}
