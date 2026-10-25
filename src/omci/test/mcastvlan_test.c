/* Class 309 attribute 16, the downstream IGMP and multicast TCI, as bridge
 * rules (../mcastvlan.c).
 *
 * Built for the target and run under qemu, like extvlan_test.c. Neither of our
 * lines carries multicast, and the two profiles they provision name no VLAN
 * in an access control list, so the rule shapes cannot be seen on a stick;
 * what can be pinned without one is pinned here: which VLANs get a rule, what
 * each control type writes into the descriptor the driver reads, and what is
 * left alone and why.
 *
 * The control types are G.988 9.3.27: 0 pass, 1 strip the outer tag, 2 add a
 * tag, 3 replace it, 4 replace its VID only, 5 to 7 the same with the service
 * package VID. ISP1 provisions 01 00 00, ISP2 00 00 00.
 */
#include "mcastvlan.h"
#include "io.h"

static int failures;

static void ok(int cond, const char *what)
{
	if (cond) {
		out_fmt("ok    %s\n", what);
	} else {
		out_fmt("FAIL  %s\n", what);
		failures++;
	}
}

static struct mcv_out o;

static void run(uint8_t type, uint16_t tci, int force, const uint16_t *ani, int nani,
		const uint8_t *unicast)
{
	struct mcv_in in;

	in.tci[0] = type;
	in.tci[1] = (uint8_t)(tci >> 8);
	in.tci[2] = (uint8_t)tci;
	in.force_strip = force;
	in.ani = ani;
	in.nani = nani;
	in.unicast = unicast;
	mcv_plan(&in, &o);
}

/* What every rule shares: a downstream-only rule of the class 171 encoding,
 * marked multicast, matching a C-tag of its VLAN with no S-tag in front and
 * no priority compared. */
static int matches_vlan(const struct mcv_rule *r, unsigned vid)
{
	const struct omci_vlan_oper *v = &r->vr;

	return r->vid == vid && v->rule_gen == OMCI_VLAN_OPER_EXTVLAN_GEN &&
	       v->out.is_mcast == 1 &&
	       v->filter.outer_mode == OMCI_TAGF_UNTAGGED &&
	       v->filter.inner_mode == (OMCI_TAGF_TAGGED | OMCI_TAGF_VID) &&
	       v->filter.inner.vid == vid && v->filter.ethertype == OMCI_ETHTYPE_NO_CARE;
}

static void set_vid(uint8_t *map, unsigned vid)
{
	map[vid >> 3] |= (uint8_t)(1u << (vid & 7));
}

int main(void)
{
	static uint8_t uc[MCV_VIDMAP_BYTES];
	const uint16_t v4000[] = { 4000 };
	const uint16_t two[] = { 4000, 401 };

	/* ---- the two lines: no access control list row, so no VLAN named */
	run(MCV_PASS, 0x0000, 0, 0, 0, 0);
	ok(o.type == MCV_PASS && o.nrule == 0 && o.nskip == 0 && !o.why,
	   "ISP2 00 00 00: no rule and nothing to say, the line's rules decide");
	run(MCV_STRIP, 0x0000, 0, 0, 0, 0);
	ok(o.type == MCV_STRIP && o.nrule == 0 && o.why != 0,
	   "ISP1 01 00 00, no ACL row: no rule, and it says why (no VLAN named)");
	run(MCV_PASS, 0x0000, 1, 0, 0, 0);
	ok(o.type == MCV_STRIP && o.nrule == 0 && o.why != 0,
	   "OMCI_CUSTOM_MCAST bit 1 on 00 00 00: type 1, still no VLAN to put it on");

	/* ---- type 0 with a VLAN named: nothing either */
	run(MCV_PASS, 0x0000, 0, v4000, 1, 0);
	ok(o.nrule == 0 && o.nskip == 0 && !o.why,
	   "type 0 with VLAN 4000 named: no rule, the VLAN rules apply as G.988 says");

	/* ---- type 1, strip */
	run(MCV_STRIP, 0x0000, 0, v4000, 1, 0);
	ok(o.nrule == 1 && matches_vlan(&o.rule[0], 4000),
	   "strip, VLAN 4000: one rule on C-VID 4000, no S-tag");
	ok(o.rule[0].vr.inner_act.tag_op == OMCI_TAGOP_POP &&
	   o.rule[0].vr.outer_act.tag_op == OMCI_TAGOP_PASS &&
	   o.rule[0].vr.out.tag_count == 0,
	   "and it pops the C-tag, leaves no tag");
	run(MCV_STRIP, 0xa064, 0, v4000, 1, 0);
	ok(o.nrule == 1 && o.rule[0].vr.inner_act.tag_op == OMCI_TAGOP_POP,
	   "strip ignores the TCI bytes");
	run(MCV_REPLACE, 0x2064, 1, v4000, 1, 0);
	ok(o.type == MCV_STRIP && o.nrule == 1 &&
	   o.rule[0].vr.inner_act.tag_op == OMCI_TAGOP_POP,
	   "OMCI_CUSTOM_MCAST bit 1 overrides type 3: stripped, as the stock plugin does");

	/* ---- type 3, replace: TCI pri 5 VID 100 (0xa064) */
	run(MCV_REPLACE, 0xa064, 0, v4000, 1, 0);
	ok(o.nrule == 1 && matches_vlan(&o.rule[0], 4000),
	   "replace, VLAN 4000: one rule on C-VID 4000");
	ok(o.rule[0].vr.inner_act.tag_op == OMCI_TAGOP_REWRITE &&
	   o.rule[0].vr.inner_act.vid_op == OMCI_VIDOP_SET &&
	   o.rule[0].vr.inner_act.pri_op == OMCI_PRIOP_SET &&
	   o.rule[0].vr.inner_act.set_tag.vid == 100 &&
	   o.rule[0].vr.inner_act.set_tag.pri == 5 &&
	   o.rule[0].vr.outer_act.tag_op == OMCI_TAGOP_PASS &&
	   o.rule[0].vr.out.tag_count == 1,
	   "and rewrites the C-tag to VID 100 priority 5");

	/* ---- type 4, replace the VID: the priority stays the frame's */
	run(MCV_REPLACE_VID, 0xa064, 0, v4000, 1, 0);
	ok(o.nrule == 1 && matches_vlan(&o.rule[0], 4000) &&
	   o.rule[0].vr.inner_act.tag_op == OMCI_TAGOP_REWRITE &&
	   o.rule[0].vr.inner_act.set_tag.vid == 100 &&
	   o.rule[0].vr.inner_act.pri_op == OMCI_PRIOP_PASS,
	   "replace VID: VID 100, the priority kept (PRIOP_PASS), the TCI's 5 not used");
	run(MCV_REPLACE_VID, 0xb064, 0, v4000, 1, 0);
	ok(o.nrule == 1, "replace VID with DEI 1 in the TCI: done, DEI is the frame's own");

	/* ---- type 2, add: an S-tag in front of the C-tag */
	run(MCV_ADD, 0x6064, 0, v4000, 1, 0);
	ok(o.nrule == 1 && matches_vlan(&o.rule[0], 4000) &&
	   o.rule[0].vr.inner_act.tag_op == OMCI_TAGOP_PASS &&
	   o.rule[0].vr.outer_act.tag_op == OMCI_TAGOP_PUSH &&
	   o.rule[0].vr.outer_act.pri_op == OMCI_PRIOP_SET &&
	   o.rule[0].vr.outer_act.set_tag.vid == 100 &&
	   o.rule[0].vr.outer_act.set_tag.pri == 3 &&
	   o.rule[0].vr.out.tag_count == 2,
	   "add: the C-tag kept, an S-tag of VID 100 priority 3 pushed in front");

	/* ---- what the TCI cannot say here */
	run(MCV_REPLACE, 0xafff, 0, v4000, 1, 0);
	ok(o.nrule == 0 && o.why != 0, "replace with VID 4095: refused, said why");
	run(MCV_ADD, 0x1064, 0, v4000, 1, 0);
	ok(o.nrule == 0 && o.why != 0, "add with DEI 1: refused (no DEI field), said why");
	run(MCV_REPLACE, 0x1064, 0, v4000, 1, 0);
	ok(o.nrule == 0 && o.why != 0, "replace with DEI 1: refused too");

	/* ---- types 5 to 7, reserved values */
	for (uint8_t t = MCV_ADD_PKG; t <= MCV_REPLACE_VID_PKG; t++) {
		run(t, 0x0064, 0, v4000, 1, 0);
		ok(o.type == t && o.nrule == 0 && o.why != 0,
		   "types 5 to 7: not done (no VID selection of the service package), said why");
	}
	run(8, 0x0064, 0, v4000, 1, 0);
	ok(o.nrule == 0 && o.why != 0, "type 8, reserved: no rule, said why");
	run(0xff, 0x0064, 0, v4000, 1, 0);
	ok(o.nrule == 0 && o.why != 0, "type 255, reserved: no rule");

	/* ---- which VLANs the ACL rows name */
	run(MCV_STRIP, 0, 0, two, 2, 0);
	ok(o.nrule == 2 && matches_vlan(&o.rule[0], 4000) && matches_vlan(&o.rule[1], 401),
	   "two VLANs named: a rule each, in the order of the rows");
	{
		const uint16_t dup[] = { 4000, 4000, 401, 4000 };

		run(MCV_STRIP, 0, 0, dup, 4, 0);
		ok(o.nrule == 2 && o.nskip == 0, "a VLAN named by several rows: one rule");
	}
	{
		const uint16_t odd[] = { MCV_ANI_UNTAGGED, MCV_ANI_ANY_TAGGED,
					 MCV_ANI_UNSPECIFIED, 4095, 4096, 4000 };

		run(MCV_STRIP, 0, 0, odd, 6, 0);
		ok(o.nrule == 1 && matches_vlan(&o.rule[0], 4000),
		   "VLAN (ANI) 0, 4097, 0xffff, 4095, 4096: no rule of their own; 4000 still gets one");
		ok(o.nskip == 5 && o.skip[0].vid == 0 && o.skip[1].vid == 4097 &&
		   o.skip[2].vid == 0xffff && o.skip[0].why && o.skip[1].why,
		   "and each is said, with why");
		ok(!o.why, "a list that names a VLAN is not 'no VLAN named'");
	}
	{
		const uint16_t only0[] = { MCV_ANI_UNTAGGED };

		run(MCV_ADD, 0x0064, 0, only0, 1, 0);
		ok(o.nrule == 0 && o.nskip == 1 && o.skip[0].vid == 0,
		   "add on untagged groups: no rule, since a rule on untagged frames takes unicast too");
	}

	/* ---- a VLAN the unicast rules use is left to them */
	set_vid(uc, 401);
	run(MCV_REPLACE, 0x0064, 0, two, 2, uc);
	ok(o.nrule == 1 && matches_vlan(&o.rule[0], 4000) &&
	   o.nskip == 1 && o.skip[0].vid == 401 && o.skip[0].why,
	   "VLAN 401 is a unicast VLAN too: no rule on it, said; 4000 still gets its rule");

	/* ---- the cap */
	{
		uint16_t many[MCV_RULES_MAX + 2];

		for (int i = 0; i < MCV_RULES_MAX + 2; i++)
			many[i] = (uint16_t)(1000 + i);
		run(MCV_STRIP, 0, 0, many, MCV_RULES_MAX + 2, 0);
		ok(o.nrule == MCV_RULES_MAX && o.nskip == 2,
		   "more VLANs than rules kept: the first eight, the rest said");
	}

	/* ---- mcv_mark: the VIDs a unicast rule names */
	{
		static uint8_t m[MCV_VIDMAP_BYTES];
		struct omci_vlan_oper v;

		for (unsigned i = 0; i < sizeof v / 4; i++)
			((uint32_t *)&v)[i] = 0;
		v.filter.outer_mode = OMCI_TAGF_UNTAGGED;
		v.filter.inner_mode = OMCI_TAGF_UNTAGGED;
		v.filter.inner.vid = 0;
		v.inner_act.tag_op = OMCI_TAGOP_PUSH;
		v.inner_act.set_tag.vid = 11;
		v.outer_act.tag_op = OMCI_TAGOP_PASS;
		v.out.out_tag.vid = 11;
		mcv_mark(m, &v);
		ok((m[11 >> 3] >> (11 & 7) & 1) && !(m[0] & 1),
		   "the manual add-tag rule marks the VID it pushes (11), not the untagged filter");
		v.filter.inner_mode = OMCI_TAGF_VID;
		v.filter.inner.vid = 12;
		v.inner_act.tag_op = OMCI_TAGOP_PASS;
		v.inner_act.set_tag.vid = 99;
		v.out.out_tag.vid = OMCI_VID_ANY;
		mcv_mark(m, &v);
		ok((m[12 >> 3] >> (12 & 7) & 1) && !(m[99 >> 3] >> (99 & 7) & 1),
		   "a VID filter marks its VID; a tag it does not write is not marked");
	}

	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();
	return failures ? 1 : 0;
}
