// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_platform.c -- the switch-core state the stock OMCI kernel
 * modules set up when they loaded: the platform settings, each a
 * read-modify-write of the fields it owns, the clean of the ACL tables,
 * and the module-load replay.
 * rcS runs both once, through the switch_init write of /proc/odi_omci,
 * after the odi_init "switch" verb whose baseline they narrow and before
 * omcid starts (docs/SWITCH.md#platform-init, #module-load-replay). Board
 * ports: UNI 0, PON 2, CPU 3; port 1 is unused.
 */
#include "odi_switch_dal.h"
#include "odi_replay.h"
#include "odi_replay_blob.h"
#include "odi_switch_reg.h"

/* The CF rule, mask and action tables: 256 rows each. */
#define ODI_SW_CF_ROW_COUNT		256U

#define ODI_SWITCH_PLATFORM_UNI_PORT	0U
#define ODI_SWITCH_PLATFORM_PON_PORT	2U
#define ODI_SWITCH_PLATFORM_PORTS_N	4U

/* Unmatched upstream frames pass instead of needing a PON match. */
static void odi_switch_platform_classify(void)
{
	uint32_t reg = odi_reg_read(ODI_SW_CLASSIFY_SETUP_OFF);

	reg = ODI_SW_CLASSIFY_SETUP_US_NO_MATCH_ACTION_SET(reg, 1);
	odi_reg_write(ODI_SW_CLASSIFY_SETUP_OFF, reg);
}

/* The three ACL tables, 96 rows each: the rule pattern, the rule mask
 * (the same table kind, pattern rows 128 up) and the action.
 */
#define ODI_SW_ACL_ROW_COUNT		96U
#define ODI_SW_ACL_RULE_WORDS		5U
#define ODI_SW_ACL_ACTION_WORDS		3U

/* VALID of a rule: bit 144 of the pattern row, bit 16 of word 0 in the
 * order odi_switch_table_write() takes a row. The module-load replay
 * writes rule 1 with it set; a row with it clear matches nothing.
 */
#define ODI_SW_ACL_PATTERN_VALID	(1U << 16)

struct odi_sw_acl_count {
	unsigned int valid;	/* rules with VALID set */
	unsigned int used;	/* rows with a non-zero word in any of the three tables */
	unsigned int unread;	/* row reads that timed out */
	int first_valid;	/* the first rule with VALID set, -1 when none */
};

static const struct {
	uint32_t table;
	uint32_t words;
} odi_sw_acl_tables[] = {
	{ ODI_SW_TBL_ACL_PATTERN, ODI_SW_ACL_RULE_WORDS },
	{ ODI_SW_TBL_ACL_PATTERN_MASK, ODI_SW_ACL_RULE_WORDS },
	{ ODI_SW_TBL_ACL_ACTIONS, ODI_SW_ACL_ACTION_WORDS },
};

#define ODI_SW_ACL_TABLES \
	(sizeof(odi_sw_acl_tables) / sizeof(odi_sw_acl_tables[0]))

/* Reads every row of the three tables back, a table at a time. */
static void odi_switch_platform_acl_count(struct odi_sw_acl_count *c)
{
	uint32_t used[(ODI_SW_ACL_ROW_COUNT + 31U) / 32U] = { 0 };
	uint32_t row[ODI_SW_ACL_RULE_WORDS];
	uint32_t t, r, i;

	c->valid = 0;
	c->used = 0;
	c->unread = 0;
	c->first_valid = -1;
	for (t = 0; t < ODI_SW_ACL_TABLES; t++) {
		for (r = 0; r < ODI_SW_ACL_ROW_COUNT; r++) {
			if (odi_switch_table_read(odi_sw_acl_tables[t].table, r, row,
						  odi_sw_acl_tables[t].words)) {
				c->unread++;
				continue;
			}
			if (odi_sw_acl_tables[t].table == ODI_SW_TBL_ACL_PATTERN &&
			    (row[0] & ODI_SW_ACL_PATTERN_VALID)) {
				c->valid++;
				if (c->first_valid < 0)
					c->first_valid = (int)r;
			}
			for (i = 0; i < odi_sw_acl_tables[t].words; i++)
				if (row[i])
					used[r / 32U] |= 1U << (r % 32U);
		}
	}
	for (r = 0; r < ODI_SW_ACL_ROW_COUNT; r++)
		if (used[r / 32U] & (1U << (r % 32U)))
			c->used++;
}

/* Every row of the three tables written all-zero; the first failed write,
 * or 0.
 */
static int odi_switch_platform_acl_clear(void)
{
	static const uint32_t zero[ODI_SW_ACL_RULE_WORDS] = { 0 };
	uint32_t t, r;
	int rc = 0, w;

	for (t = 0; t < ODI_SW_ACL_TABLES; t++) {
		for (r = 0; r < ODI_SW_ACL_ROW_COUNT; r++) {
			w = odi_switch_table_write(odi_sw_acl_tables[t].table, r, zero,
						   odi_sw_acl_tables[t].words);
			if (w && !rc)
				rc = w;
		}
	}
	return rc;
}

/* The ACL tables live in RAM that neither a reset nor a reboot clears:
 * they come up holding random words after a power cut, and the rules of
 * whatever ran before after a warm boot (the stock firmware after a trial
 * fell back to it). ACL ingress is on for the UNI and PON ports (the next
 * step), and the module-load replay writes only rule 1, so every row is
 * cleared here, before either, and read back: a leftover rule with VALID
 * set would match ahead of everything on those ports. Two passes at most,
 * like the GPON tables; the counts go to the log.
 */
static void odi_switch_platform_acl_clean(void)
{
	struct odi_sw_acl_count before, after;
	unsigned int pass;
	int rc = 0;

	odi_switch_platform_acl_count(&before);
	ODI_SW_LOG("ACL rows before the clean: %u/%u rules valid (first %d), %u/%u rows not empty%s\n",
		   before.valid, ODI_SW_ACL_ROW_COUNT, before.first_valid, before.used,
		   ODI_SW_ACL_ROW_COUNT, before.unread ? " (a row read timed out)" : "");
	for (pass = 1; pass <= 2U; pass++) {
		rc = odi_switch_platform_acl_clear();
		odi_switch_platform_acl_count(&after);
		if (rc == 0 && (after.used || after.unread))
			rc = -1;
		if (rc == 0)
			break;
	}
	if (rc == 0) {
		ODI_SW_LOG("ACL rows clean after %u pass(es): 0/%u rules valid, 0/%u rows not empty\n",
			   pass, ODI_SW_ACL_ROW_COUNT, ODI_SW_ACL_ROW_COUNT);
		return;
	}
	ODI_SW_LOG("ACL rows NOT clean after 2 passes (%d): %u/%u rules valid (first %d), %u/%u rows not empty, %u row reads timed out\n",
		   rc, after.valid, ODI_SW_ACL_ROW_COUNT, after.first_valid, after.used,
		   ODI_SW_ACL_ROW_COUNT, after.unread);
}

/* ACL ingress on the UNI and PON ports, one port per write as captured. */
static void odi_switch_platform_acl(void)
{
	uint32_t reg;

	reg = odi_reg_read(ODI_SW_ACL_PORT_ENABLE_OFF);
	reg |= 1U << ODI_SWITCH_PLATFORM_UNI_PORT;
	odi_reg_write(ODI_SW_ACL_PORT_ENABLE_OFF, reg);
	reg = odi_reg_read(ODI_SW_ACL_PORT_ENABLE_OFF);
	reg |= 1U << ODI_SWITCH_PLATFORM_PON_PORT;
	odi_reg_write(ODI_SW_ACL_PORT_ENABLE_OFF, reg);
}

/* VLAN filtering off, PVID 0 on ports 0-1, the port-1 ingress check off,
 * every port an SVLAN uplink, SVLAN filtering off and untagged frames on
 * the port SVID. PORT_VLAN_INDEX of ports 2-3 and the per-port SVID are
 * not written: their register strides are not decoded.
 */
static void odi_switch_platform_vlan(void)
{
	uint32_t reg, port;

	reg = odi_reg_read(ODI_SW_VLAN_SETUP_OFF);
	reg = ODI_SW_VLAN_SETUP_FILTER_ON_SET(reg, 0);
	odi_reg_write(ODI_SW_VLAN_SETUP_OFF, reg);

	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(0), 0);
	odi_reg_write(ODI_SW_PORT_VLAN_INDEX(1), 0);

	reg = odi_reg_read(ODI_SW_VLAN_INGRESS_CHECK_BASE);
	reg = ODI_SW_VLAN_INGRESS_CHECK_ON_SET(reg, 1, 0);
	odi_reg_write(ODI_SW_VLAN_INGRESS_CHECK_BASE, reg);

	/* One read-modify-write per port, as captured. */
	for (port = 0; port < ODI_SWITCH_PLATFORM_PORTS_N; port++) {
		reg = odi_reg_read(ODI_SW_SVLAN_UPLINK_PORTS_BASE);
		reg = ODI_SW_SVLAN_UPLINK_PORTS_UPLINK_SET(reg, port, 1);
		odi_reg_write(ODI_SW_SVLAN_UPLINK_PORTS_BASE, reg);
	}

	reg = odi_reg_read(ODI_SW_SVLAN_SETUP_OFF);
	reg = ODI_SW_SVLAN_SETUP_FILTER_ON_SET(reg, 0);
	reg = ODI_SW_SVLAN_SETUP_UNTAGGED_ACTION_SET(reg, 2); /* port-based SVID */
	odi_reg_write(ODI_SW_SVLAN_SETUP_OFF, reg);
}

/* The L2 CAM on (CAM_OFF is a disable bit). */
static void odi_switch_platform_l2(void)
{
	uint32_t reg = odi_reg_read(ODI_SW_L2_LOOKUP_SETUP_OFF);

	reg = ODI_SW_L2_LOOKUP_SETUP_CAM_OFF_SET(reg, 0);
	odi_reg_write(ODI_SW_L2_LOOKUP_SETUP_OFF, reg);
}

/* Every CF rule and action row cleared (an all-zero row is invalid: bit 16
 * of word 0 is the valid flag in every row decoded). CLS_MASK_B stays.
 */
static void odi_switch_platform_cf_sweep(void)
{
	static const uint32_t zero2[2] = { 0, 0 };
	static const uint32_t zero3[3] = { 0, 0, 0 };
	uint32_t i;

	for (i = 0; i < ODI_SW_CF_ROW_COUNT; i++) {
		(void)odi_switch_table_write(ODI_SW_TBL_CLS_RULE_B, i, zero2, 2);
		(void)odi_switch_table_write(ODI_SW_TBL_CLS_DS_ACTION, i, zero3, 3);
		(void)odi_switch_table_write(ODI_SW_TBL_CLS_US_ACTION, i, zero3, 3);
	}
}

/* VIDs 0 and 4095 tagged, SVLAN priority taken from the C-TAG. */
static void odi_switch_platform_reserved_vid(void)
{
	uint32_t reg;

	reg = odi_reg_read(ODI_SW_VLAN_SETUP_OFF);
	reg = ODI_SW_VLAN_SETUP_VID0_MODE_SET(reg, 1);
	reg = ODI_SW_VLAN_SETUP_VID4095_MODE_SET(reg, 1);
	odi_reg_write(ODI_SW_VLAN_SETUP_OFF, reg);

	reg = odi_reg_read(ODI_SW_SVLAN_SETUP_OFF);
	reg = ODI_SW_SVLAN_SETUP_PRIO_SOURCE_SET(reg, 1);
	odi_reg_write(ODI_SW_SVLAN_SETUP_OFF, reg);
}

/* PAUSE toward the host per odi_sw_host_pause: off by default, so a
 * congested switch drops at its own buffers instead of stalling the host
 * NIC (docs/SWITCH.md#host-port-pause). Ours: the stock image lets the
 * SerDes auto-negotiation turn it on.
 */
static void odi_switch_platform_host_pause(void)
{
	odi_sw_host_pause_apply();
}

/* The host-port auto-negotiation, only when odi_switch.host_autoneg was
 * turned off on the kernel command line: the default writes and reads
 * nothing, so a stick that never sets it runs the stock 0x1140. Userland
 * normally sets it through sysfs earlier in the boot (host-autoneg.sh),
 * and then this finds the bit already clear and writes nothing.
 */
static void odi_switch_platform_host_autoneg(void)
{
	if (!odi_sw_host_autoneg)
		odi_sw_host_autoneg_apply();
}

static const struct {
	void (*fn)(void);
	const char *name;
} odi_switch_platform_steps[] = {
	{ odi_switch_platform_classify, "classify" },
	{ odi_switch_platform_acl_clean, "acl clean" },
	{ odi_switch_platform_acl, "acl" },
	{ odi_switch_platform_vlan, "vlan" },
	{ odi_switch_platform_l2, "l2" },
	{ odi_switch_platform_cf_sweep, "cf sweep" },
	{ odi_switch_platform_reserved_vid, "reserved vid" },
	{ odi_switch_platform_host_pause, "host pause" },
	{ odi_switch_platform_host_autoneg, "host autoneg" },
};

/* The log names the step before it touches hardware, so a log cut short
 * names the step that was running.
 */
void odi_switch_init_platform(void)
{
	unsigned int i;

	for (i = 0; i < sizeof(odi_switch_platform_steps) / sizeof(odi_switch_platform_steps[0]); i++) {
		ODI_SW_LOG("platform: %s\n", odi_switch_platform_steps[i].name);
		odi_switch_platform_steps[i].fn();
	}
	ODI_SW_LOG("platform: done\n");
}

/* After the replay, unknown multicast is not flooded to the CPU port. The
 * replay writes the three flood masks 0xf, every port with the CPU, where
 * the stock capture has 0x7 (docs/SWITCH.md, module-load replay: without
 * the CPU in the broadcast mask, LAN broadcast stopped reaching it). The
 * CPU needs broadcast (ARP); it has no use for unknown multicast: the
 * kernel has no IPv6, so no neighbour discovery, nothing on the stick
 * joins a group, and the reserved 01:80:C2 groups have their own actions
 * (LINK_MCAST_*). With IGMP snooping off every group is unknown, so a
 * downstream IPTV stream would be copied to the CPU at stream rate,
 * thousands of frames a second, against the CPU-RX guards. So the
 * unknown-multicast mask takes the capture's value back: CPU bit off,
 * the other ports as the replay left them.
 */
#define ODI_SW_MODLOAD_CPU_PORT_BIT	(1U << 3)

void odi_switch_init_modload(const struct odi_replay_blob *table)
{
	const struct odi_replay_opts opts = { .rmw = 1, .verb = ODI_REPLAY_ALL_VERBS };
	uint32_t reg;

	ODI_SW_LOG("init_modload: %u records\n", table->count);
	(void)odi_replay_run(table, &opts);
	reg = odi_reg_read(ODI_SW_FLOOD_UNKN_MCAST_PORTS_BASE);
	odi_reg_write(ODI_SW_FLOOD_UNKN_MCAST_PORTS_BASE, reg & ~ODI_SW_MODLOAD_CPU_PORT_BIT);
	ODI_SW_LOG("init_modload: done, unknown multicast flood mask 0x%x\n",
		   (unsigned int)(reg & ~ODI_SW_MODLOAD_CPU_PORT_BIT));
}
