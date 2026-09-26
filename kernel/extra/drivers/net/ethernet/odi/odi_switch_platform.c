// SPDX-License-Identifier: GPL-2.0
/*
 * odi_switch_platform.c -- the switch-core state the stock OMCI kernel
 * modules set up when they loaded: the platform settings, each a
 * read-modify-write of the fields it owns, and the module-load replay.
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

static const struct {
	void (*fn)(void);
	const char *name;
} odi_switch_platform_steps[] = {
	{ odi_switch_platform_classify, "classify" },
	{ odi_switch_platform_acl, "acl" },
	{ odi_switch_platform_vlan, "vlan" },
	{ odi_switch_platform_l2, "l2" },
	{ odi_switch_platform_cf_sweep, "cf sweep" },
	{ odi_switch_platform_reserved_vid, "reserved vid" },
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

void odi_switch_init_modload(const struct odi_replay_blob *table)
{
	const struct odi_replay_opts opts = { .rmw = 1, .verb = ODI_REPLAY_ALL_VERBS };

	ODI_SW_LOG("init_modload: %u records\n", table->count);
	(void)odi_replay_run(table, &opts);
	ODI_SW_LOG("init_modload: done\n");
}
