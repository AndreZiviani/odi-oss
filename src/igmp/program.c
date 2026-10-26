/* Programming the switch, over the /dev/odi_sw ioctls in switch_opt.c.
 *
 * Named program.c rather than hw.c on purpose: diag owns a hw.h on the same
 * include path, and two headers with one name is a collision that resolves
 * differently depending on -I order.
 *
 * Every write here is OFF by default. Nothing in this daemon has ever run on a
 * device, and a wrong L2 multicast entry does not fail visibly -- it forwards
 * traffic to the wrong port, or stops forwarding it at all. So the daemon
 * prints what it would do until it is told otherwise, and the first run on a
 * stick can be watched before it is trusted.
 *
 * The MAC path only: static L2 multicast entries keyed on the group MAC.
 * With IPv4 multicast looked up on MAC + VID/FID -- the stock setting, which
 * our init replays -- that is what the switch consults for multicast data. An
 * IPv4 route entry (keyed on the group address) would serve the other lookup
 * mode, which nothing on this image selects.
 */
#include "program.h"
#include "entry.h"
#include "switch_opt.h"
#include "io.h"

static int hw_enabled;

void igmp_hw_set_enabled(int on)
{
	hw_enabled = on;
}

int igmp_hw_enabled(void)
{
	return hw_enabled;
}

/* The entry is keyed on filtering id 0 (SVL), in either lookup mode, and
 * the VID plays no part in the key.
 *
 * Why not the VID (IVL): whether the switch looks a destination up on
 * MAC + VID or on MAC + filtering id follows how the VLAN it arrived on
 * is set up, and on this image every VLAN is shared. The stick shows it
 * in its own table: every address it learned is an SVL row on filtering
 * id 0, the one learned from VID-14-tagged frames off the PON included
 * (docs/KERNEL.md, "The L2 table"). An IVL entry keyed on a VID would sit
 * in the table and never match a frame.
 *
 * The lookup mode is still read and printed: with IPv4 multicast looked
 * up on the group address rather than the MAC, a MAC-keyed entry is not
 * consulted at all, and that is worth saying before writing one. */
int igmp_hw_probe(void)
{
	uint32_t mode = 0;
	int rc = igmp_sw_mode(&mode);

	if (rc == -25) {
		out("lookup mode: this kernel has no L2 multicast ioctl on "
		    "/dev/odi_sw (ENOTTY)\n");
		return rc;
	}
	if (rc != 0) {
		out_fmt("lookup mode unreadable (%d)\n", rc);
		return rc;
	}
	out_fmt("IPv4 multicast looked up on %s: the entry is keyed on "
		"filtering id 0 (SVL)%s\n",
		mode ? "the group address" : "MAC + VID/FID",
		mode ? ", and this mode does not consult it" : "");
	return 0;
}

static void show(const char *what, uint16_t vid, const uint8_t mac[6], uint32_t ports)
{
	out_fmt("  %s vid %d ", what, vid);
	for (int i = 0; i < 6; i++) {
		if (i)
			out_char(':');
		out_hex(mac[i], 2);
	}
	out_fmt(" ports 0x%x%s\n", ports, hw_enabled ? "" : "  (not written)");
}

static const char *sw_err(int rc)
{
	switch (rc) {
	case -1:  return "refused: the reports are not taken, or not by this process";
	case -2:  return "no /dev/odi_sw";
	case -16: return "busy: the table engine, or another snooper holds the lease";
	case -22: return "refused by the driver";
	case -25: return "no L2 multicast ioctl in this kernel";
	case -28: return "hash bucket full";
	}
	return "failed";
}

int igmp_hw_snoop(uint32_t flags)
{
	int rc;

	out_fmt("snooping lease: %s%s%s%s%s\n",
		!(flags & ODI_SW_MC_SNOOP_COPY) ? "released" :
		flags & ODI_SW_MC_SNOOP_TRAP ? "take host IGMP to the CPU, igmpd sends it up" :
		"copy host IGMP to the CPU",
		flags & ODI_SW_MC_SNOOP_MLD ? ", and its MLD" : "",
		flags & ODI_SW_MC_SNOOP_PON_DROP ? ", drop unregistered IPv4 multicast from the PON" : "",
		flags & ODI_SW_MC_SNOOP_PON_DROP6 ?
		", drop unregistered IPv6 multicast from the PON (ff02::/16 and solicited-node flooded)" : "",
		hw_enabled ? "" : "  (not written)");
	if (!hw_enabled)
		return 0;
	rc = igmp_sw_snoop(flags);
	if (rc)
		out_fmt("  snoop ioctl: %d (%s)\n", rc, sw_err(rc));
	return rc;
}

int igmp_hw_group_set(uint16_t vid, uint32_t group, uint32_t ports)
{
	uint8_t mac[6];

	igmp_group_mac(mac, group);
	return igmp_hw_mac_set(vid, mac, ports);
}

int igmp_hw_group_del(uint16_t vid, uint32_t group)
{
	uint8_t mac[6];

	igmp_group_mac(mac, group);
	return igmp_hw_mac_del(vid, mac);
}

int igmp_hw_mac_set(uint16_t vid, const uint8_t mac[6], uint32_t ports)
{
	struct odi_sw_l2_mcast m;
	int rc;

	show("set", vid, mac, ports);
	if (!hw_enabled)
		return 0;
	if (igmp_mac_entry_mac(&m, vid, mac, ports, 0) != 0) {
		out_fmt("  not written: ports 0x%x name a port the switch does not have\n",
			ports);
		return -22;
	}
	rc = igmp_sw_mcast_add(&m);
	if (rc)
		out_fmt("  l2 multicast add: %d (%s)\n", rc, sw_err(rc));
	else
		out_fmt("  written, row 0x%x\n", m.index);
	return rc;
}

int igmp_hw_mac_del(uint16_t vid, const uint8_t mac[6])
{
	struct odi_sw_l2_mcast m;
	int rc;

	show("del", vid, mac, 0);
	if (!hw_enabled)
		return 0;
	/* The member mask is not part of the key, so zero. */
	igmp_mac_entry_mac(&m, vid, mac, 0, 0);
	rc = igmp_sw_mcast_del(&m);
	if (rc)
		out_fmt("  l2 multicast del: %d (%s)\n", rc, sw_err(rc));
	else if (!m.found)
		out("  no such entry in the switch\n");
	return rc;
}

int igmp_hw_route(struct odi_sw_mc_route *r)
{
	int rc = igmp_sw_route(r);

	if (rc)
		out_fmt("  route ioctl: %d (%s)\n", rc, sw_err(rc));
	return rc;
}

int igmp_hw_send(const uint8_t *frame, uint32_t len, uint32_t flow)
{
	static struct odi_sw_mc_send m;
	int rc;

	if (len > sizeof m.frame)
		return -22;
	m.len = len;
	m.flow = flow;
	for (uint32_t i = 0; i < len; i++)
		m.frame[i] = frame[i];
	rc = igmp_sw_send(&m);
	if (rc)
		out_fmt("  send ioctl: %d (%s)\n", rc, sw_err(rc));
	return rc;
}
