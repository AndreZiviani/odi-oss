/* The downstream IGMP and multicast TCI of a multicast operations profile
 * (class 309 attribute 16, G.988 9.3.27) as the bridge rules of cmd 51.
 *
 * The attribute is three bytes: a control type, then the TCI (priority 15..13,
 * DEI 12, VID 11..0) the types that write a tag use.
 *
 *   0  pass the multicast as it comes           (the VLAN rules decide)
 *   1  strip the outer tag
 *   2  add a tag (the TCI)
 *   3  replace the outer tag (the TCI)
 *   4  replace its VID only, priority and DEI kept
 *   5-7  as 2-4 with the VID of the subscriber's service package table row
 *
 * The switch cannot tell a multicast frame from a unicast one: a downstream
 * classifier row compares the tags, never the GEM port or the destination
 * address (docs/SWITCH.md, "CF rows"). So the treatment goes on the VLANs the
 * profile itself says carry its groups downstream, the VLAN (ANI) of each
 * part 0 row of its two access control lists, one rule per VLAN, and only
 * where that VLAN is a multicast VLAN and nothing else:
 *
 *   - VLAN (ANI) 0 (untagged groups), 4097 (any tagged frame) and 0xffff
 *     (unspecified) name no VLAN of their own: a rule on them would act on the
 *     unicast frames too;
 *   - a VID that a unicast rule of the line also names is left to that rule;
 *   - "add a tag" to a frame that already has its C-tag can only put an S-tag
 *     (TPID 0x88a8) in front: the classifier replaces a C-tag, it does not
 *     stack a second one;
 *   - a TCI with DEI 1 is not written (the classifier has no DEI field), and
 *     types 5 to 7 are not done: the service package table does not select
 *     anything by VID here (the multicast policy says so too, mcast.c).
 *
 * What is left out keeps the rules that applied before: the caller adds these
 * in front of them. Pure: no store, no driver, unit-tested on its own
 * (test/mcastvlan_test.c).
 */
#ifndef ODI_OMCI_MCASTVLAN_H
#define ODI_OMCI_MCASTVLAN_H

#include <stdint.h>
#include "omci_bdgconn.h"

/* The control type, the first byte of the attribute. */
#define MCV_PASS            0
#define MCV_STRIP           1
#define MCV_ADD             2
#define MCV_REPLACE         3
#define MCV_REPLACE_VID     4
#define MCV_ADD_PKG         5
#define MCV_REPLACE_PKG     6
#define MCV_REPLACE_VID_PKG 7

/* VLAN (ANI) of an access control list row, the values that are not a VID. */
#define MCV_ANI_UNTAGGED    0
#define MCV_ANI_ANY_TAGGED  4097
#define MCV_ANI_UNSPECIFIED 0xffff

/* The rules one profile can make, and the VLANs said about. */
#define MCV_RULES_MAX       8
#define MCV_SKIPS_MAX       8

/* The bit map of the VIDs a unicast rule names (mcv_mark()). */
#define MCV_VIDMAP_BYTES    512

struct mcv_in {
	uint8_t tci[3];          /* attribute 16 as the OLT set it */
	int force_strip;         /* OMCI_CUSTOM_MCAST bit 0x1: type 1, whatever
				  * the OLT set (the stock cf_force_ds_mcast_untagged) */
	const uint16_t *ani;     /* VLAN (ANI) of each part 0 row of both lists */
	int nani;
	const uint8_t *unicast;  /* MCV_VIDMAP_BYTES, or 0 for none */
};

struct mcv_rule {
	uint16_t vid;            /* the multicast VLAN the rule matches */
	const char *kind;        /* for the log: mcast-strip, mcast-add, ... */
	struct omci_vlan_oper vr;   /* rule_gen OMCI_VLAN_OPER_EXTVLAN_GEN,
				     * for a downstream-only descriptor */
};

struct mcv_skip {
	uint16_t vid;            /* the VLAN (ANI) value as the row has it */
	const char *why;
};

struct mcv_out {
	int type;                /* the control type in effect */
	const char *why;         /* no rule at all for a reason that is not one
				  * VLAN's (0: nothing to say, as for type 0) */
	struct mcv_rule rule[MCV_RULES_MAX];
	int nrule;
	struct mcv_skip skip[MCV_SKIPS_MAX];
	int nskip;
};

void mcv_plan(const struct mcv_in *in, struct mcv_out *out);

/* The VIDs a bridge rule names, in its filter or in a tag it writes, set in
 * `map` (MCV_VIDMAP_BYTES). The caller marks every unicast rule of the line,
 * and hands the map to mcv_plan(). */
void mcv_mark(uint8_t *map, const struct omci_vlan_oper *vr);

#endif
