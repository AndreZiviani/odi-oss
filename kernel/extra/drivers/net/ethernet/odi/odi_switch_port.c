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

/* The PON port isolated from the CPU side, both ways, with
 * odi_sw_pon_isolate:
 * - the PON port's mask loses the CPU port and every extension port (the
 *   CPU side): a frame that enters from the PON may be forwarded to the
 *   UNIs only, whatever the L2 lookup, a flood mask or a classifier row
 *   decides;
 * - the CPU port's mask loses the PON port: a frame the CPU sends that the
 *   switch forwards by lookup (a broadcast, an unknown unicast) never
 *   leaves by the PON. The OMCI replies are not forwarded by lookup: they
 *   carry PORT_SEL and the PON port mask (odi_omci.c), a directed TX, which
 *   the vendor forwarding engine filters by isolation in software because
 *   the hardware does not (docs/SWITCH.md#pon-port-isolation). The frames
 *   of the IP stack carry a host-port mask (odi_nic_stack_tx_mask()), so
 *   this half only matters should the switch forward those by lookup.
 * Without it the bits are set again, the all-open reset value. Ports 2
 * and 3 share the word: one read-modify-write. Ingress from the UNIs is
 * untouched: the host reaches the CPU and the fibre.
 */
#define ODI_SW_PON_PORT			2U
#define ODI_SW_CPU_PORT			3U
#define ODI_SW_ISOLATION_CPU_SIDE	(0x7f0U | (1U << ODI_SW_CPU_PORT))	/* extension ports, CPU port */
#define ODI_SW_ISOLATION_PON		(1U << ODI_SW_PON_PORT)
/* The two ports share one word, so the apply is one write. */
typedef char odi_sw_pon_cpu_share_a_word[(ODI_SW_PORT_ISOLATION(ODI_SW_PON_PORT) ==
					 ODI_SW_PORT_ISOLATION(ODI_SW_CPU_PORT)) ? 1 : -1];

void odi_sw_pon_isolate_apply(void)
{
	uint32_t off = ODI_SW_PORT_ISOLATION(ODI_SW_PON_PORT);
	uint32_t pon_item = ODI_SW_PON_PORT % 2U, cpu_item = ODI_SW_CPU_PORT % 2U;
	uint32_t reg, pon, cpu;

	reg = odi_reg_read(off);
	pon = ODI_SW_PORT_ISOLATION_EGRESS_GET(reg, pon_item);
	cpu = ODI_SW_PORT_ISOLATION_EGRESS_GET(reg, cpu_item);
	if (odi_sw_pon_isolate) {
		pon &= ~ODI_SW_ISOLATION_CPU_SIDE;
		cpu &= ~ODI_SW_ISOLATION_PON;
	} else {
		pon |= ODI_SW_ISOLATION_CPU_SIDE;
		cpu |= ODI_SW_ISOLATION_PON;
	}
	reg = ODI_SW_PORT_ISOLATION_EGRESS_SET(reg, pon_item, pon);
	reg = ODI_SW_PORT_ISOLATION_EGRESS_SET(reg, cpu_item, cpu);
	odi_reg_write(off, reg);

	reg = odi_reg_read(off);
	pon = ODI_SW_PORT_ISOLATION_EGRESS_GET(reg, pon_item);
	cpu = ODI_SW_PORT_ISOLATION_EGRESS_GET(reg, cpu_item);
	ODI_SW_LOG("PON port isolation %s: the PON port forwards to 0x%03x%s, the CPU port to 0x%03x%s\n",
		   odi_sw_pon_isolate ? "on" : "off", (unsigned int)pon,
		   (pon & ODI_SW_ISOLATION_CPU_SIDE) ? " (the CPU included)" : " (never the CPU)",
		   (unsigned int)cpu,
		   (cpu & ODI_SW_ISOLATION_PON) ? " (the PON included)" : " (never the PON by lookup)");
}

/* SA_MOVED (0x01c000), two bits a port: what the switch does with a frame
 * whose source MAC is a learned row on another port. 0 (the reset value
 * and the stock image's) moves the row to the port the frame came in by;
 * 1 drops the frame and leaves the row where it is. Set to 1 on the PON
 * port only: the fibre is the far side, no host moves from the UNI to it,
 * but a frame the OLT reflects, or one forged there, carrying a host's
 * source MAC would otherwise move that host's row to the PON port, and
 * traffic to the host would then be switched toward the fibre. Another open
 * implementation measured that on a reflecting OLT (pings failed both ways
 * until the PON port was set to 1). A static row, the stick's own pin on
 * the CPU port, is not moved either way in that implementation's reading;
 * this setting is a second guard, and keeps the frame itself from moving
 * anything. The UNI and CPU ports stay at
 * move: a host really can change port there. One read-modify-write, only
 * when the field differs.
 */
#define ODI_SW_SA_MOVED_OFF		0x1c000U
#define ODI_SW_SA_MOVED_DROP		1U

void odi_sw_sa_moved_apply(void)
{
	uint32_t shift = 2U * ODI_SW_PON_PORT;
	uint32_t reg = odi_reg_read(ODI_SW_SA_MOVED_OFF);

	if (((reg >> shift) & 3U) != ODI_SW_SA_MOVED_DROP) {
		reg = (reg & ~(3U << shift)) | (ODI_SW_SA_MOVED_DROP << shift);
		odi_reg_write(ODI_SW_SA_MOVED_OFF, reg);
	}
	ODI_SW_LOG("SA_MOVED 0x%08x: a frame from the PON port with a source MAC learned elsewhere is dropped, the row stays\n",
		   (unsigned int)odi_reg_read(ODI_SW_SA_MOVED_OFF));
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
