/* Programming the switch, over the accessors in switch_opt.c.
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
 * The MAC path only. The IP path -- rtk_l2_ipMcastAddr_* on a 68-byte entry --
 * has two fields nothing was observed to write, and an entry with a guessed
 * field in it is exactly the failure above.
 */
#include "program.h"
#include "entry.h"
#include "switch_opt.h"
#include "io.h"

static int hw_enabled;
static int hw_vid_valid = 1;

void igmp_hw_set_enabled(int on)
{
	hw_enabled = on;
}

int igmp_hw_enabled(void)
{
	return hw_enabled;
}

/* The vendor writes the VID into the entry only when ipmcMode is 0. Asked once
 * at startup rather than per entry: it is a mode, not a per-group property,
 * and the accessor is a 516-byte socket round trip.
 *
 * A failure to read it leaves the VID in place, which is the behaviour in the
 * mode this device is expected to be in, and says so. */
int igmp_hw_probe(void)
{
	uint32_t mode = 0;
	int rc = rtk_l2_ipmcMode_get(&mode);

	/* -99 is ENOPROTOOPT: the vendor switch driver behind this sockopt is
	 * not in the kernel (a 6.18 image), and odi_switch has no netlink op
	 * that reads the multicast lookup mode back. Say that, rather than a
	 * bare error number that reads like a transient failure. */
	if (rc == -99) {
		out("ipmcMode: no netlink equivalent on this kernel (the vendor "
		    "switch sockopt is absent); assuming the vid applies\n");
		hw_vid_valid = 1;
		return rc;
	}
	if (rc != 0) {
		out_fmt("ipmcMode unreadable (%d); assuming the vid applies\n", rc);
		hw_vid_valid = 1;
		return rc;
	}
	hw_vid_valid = (mode == 0);
	out_fmt("ipmcMode %d: the entry vid %s\n", mode,
		hw_vid_valid ? "applies" : "is left zero");
	return 0;
}

static void show(const char *what, uint16_t vid, uint32_t group, uint32_t ports)
{
	uint8_t mac[6];

	igmp_group_mac(mac, group);
	out_fmt("  %s vid %d ", what, vid);
	for (int i = 0; i < 6; i++) {
		if (i)
			out_char(':');
		out_hex(mac[i], 2);
	}
	out_fmt(" ports 0x%x%s\n", ports, hw_enabled ? "" : "  (not written)");
}

int igmp_hw_group_set(uint16_t vid, uint32_t group, uint32_t ports)
{
	uint32_t e[IGMP_MAC_ENTRY_WORDS];
	int rc;

	show("set", vid, group, ports);
	if (!hw_enabled)
		return 0;
	igmp_mac_entry(e, vid, group, ports, hw_vid_valid);
	/* add is an in/out call: librtk copies the whole entry back over ours
	 * after the sockopt. Nothing is read from it here, but the buffer has
	 * to be writable and per-call, which is why it is a local. */
	rc = rtk_l2_mcastAddr_add(e);
	if (rc)
		out_fmt("  rtk_l2_mcastAddr_add: %d\n", rc);
	return rc;
}

int igmp_hw_group_del(uint16_t vid, uint32_t group)
{
	uint32_t e[IGMP_MAC_ENTRY_WORDS];
	int rc;

	show("del", vid, group, 0);
	if (!hw_enabled)
		return 0;
	/* The port mask is not part of the key -- the vendor delete path never
	 * writes it -- so zero, which is also what the zeroed entry holds. */
	igmp_mac_entry(e, vid, group, 0, hw_vid_valid);
	rc = rtk_l2_mcastAddr_del(e);
	if (rc)
		out_fmt("  rtk_l2_mcastAddr_del: %d\n", rc);
	return rc;
}
