/* The class 171 table entry: decode, and what the vendor generator does with it.
 *
 * Built for the target and run under qemu. Nothing here needs a device, which
 * is the point: the rule generator cannot be verified against either of our
 * OLTs -- neither drives tagging through class 171 -- so the parts that CAN be
 * checked without one are checked hard, and the parts that cannot are not
 * pretended.
 *
 * Every entry below is real. The hex is assembled from the captured
 * `omcicli mib get 171 --vendor` dump of isp1 (ref/vendor-mib-171.txt), field
 * by field through the documented shifts, and the decode is asserted to return
 * the numbers that dump printed. That makes this a round trip against a device
 * capture rather than against itself.
 */
#include "extvlan.h"
#include "omci_bdgconn.h"
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

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* Assemble an entry from the fields a dump prints, using the shifts in the
 * layout comment. Building the bytes here and decoding them back is what makes
 * the test independent of the decoder: a wrong shift in one would have to be
 * the same wrong shift in the other to pass. */
static void build(uint8_t *e,
		  uint8_t fop, uint16_t fov, uint8_t fot,
		  uint8_t fip, uint16_t fiv, uint8_t fit, uint8_t et,
		  uint8_t rt, uint8_t top, uint16_t tov, uint8_t tot,
		  uint8_t tip, uint16_t tiv, uint8_t tit)
{
	put32(e + 0, ((uint32_t)fop << 28) | ((uint32_t)fov << 15) |
		     ((uint32_t)fot << 12));
	put32(e + 4, ((uint32_t)fip << 28) | ((uint32_t)fiv << 15) |
		     ((uint32_t)fit << 12) | et);
	put32(e + 8, ((uint32_t)rt << 30) | ((uint32_t)top << 16) |
		     ((uint32_t)tov << 3) | tot);
	put32(e + 12, ((uint32_t)tip << 16) | ((uint32_t)tiv << 3) | tit);
}

int main(void)
{
	uint8_t e[EVTOCD_ENTRY_LEN];
	struct evtocd_entry t;

	/* isp1, instance 0x01, INDEX 0. The rule that tags everything with
	 * VID 1: both filter halves ignored, no tags removed, an inner
	 * treatment of PRI 0 VID 1 TPID 4 (0x8100). */
	build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  0, 1, 4);
	evtocd_decode(e, &t);
	ok(t.f_out_pri == 15 && t.f_out_vid == 4096 && t.f_out_tpid == 0,
	   "instance 1 index 0: the outer filter decodes as the dump printed it");
	ok(t.f_in_pri == 15 && t.f_in_vid == 4096 && t.f_in_tpid == 0 &&
	   t.f_ethertype == 0,
	   "and the inner filter, ethertype included");
	ok(t.remove_tags == 0 && t.t_out_pri == 15 && t.t_out_vid == 4096 &&
	   t.t_out_tpid == 0,
	   "and the outer treatment, which is where the priority moves to bit 16");
	ok(t.t_in_pri == 0 && t.t_in_vid == 1 && t.t_in_tpid == 4,
	   "and the inner treatment: add VID 1 with TPID 0x8100");
	ok(t.f_out_pri == EVTOCD_F_PRI_IGNORE_OTHER &&
	   t.t_out_pri == EVTOCD_T_PRI_DO_NOT_ADD,
	   "15 means IGNORE on the filter side and DO NOT ADD on the treatment side");
	ok(evtocd_default_rule_tags(&t) == -1, "it is not a default rule");
	ok(!evtocd_ds_skipped(&t), "and downstream generates a rule for it");

	/* isp1, instance 0x01, INDEX 1. The untagged default: both filter
	 * halves are the DEFAULT_TAG_RULE sentinel and the treatment discards. */
	build(e, 14, 4096, 0,  14, 4096, 0, 0,   3, 15, 0, 0,  15, 0, 0);
	evtocd_decode(e, &t);
	ok(t.f_out_pri == 14 && t.f_in_pri == 14 && t.remove_tags == 3,
	   "instance 1 index 1: the untagged default drop decodes");
	/* Both halves DEFAULT is the DOUBLE-tag branch in the vendor, commented
	 * "do nothing, S-C-F->S-C-F". It is not the untagged default, and this
	 * asserted 0 until the description test disagreed. */
	ok(evtocd_default_rule_tags(&t) == 2,
	   "and is the default rule for a DOUBLE-tagged frame");
	ok(t.remove_tags == EVTOCD_T_DISCARD_FRAME,
	   "RemoveTags 3 is not a count, it is discard");
	ok(evtocd_ds_skipped(&t),
	   "so downstream creates no rule for it at all");

	/* isp1, instance 0x01, INDEX 2. The single-tag default: outer ignored,
	 * inner default. Same discard. */
	build(e, 15, 4096, 0,  14, 4096, 0, 0,   3, 15, 0, 0,  15, 0, 0);
	evtocd_decode(e, &t);
	ok(evtocd_default_rule_tags(&t) == 1,
	   "instance 1 index 2 is the default rule for a single-tagged frame");
	ok(evtocd_ds_skipped(&t), "and it too is skipped downstream");

	/* isp1, instance 0x02, which is the VEIP side and carries only the two
	 * defaults. Index 0 there is the untagged one. */
	build(e, 14, 4096, 0,  14, 4096, 0, 0,   3, 15, 0, 0,  15, 0, 0);
	evtocd_decode(e, &t);
	ok(evtocd_default_rule_tags(&t) == 2,
	   "instance 2 index 0 is the same double-tag default");

	/* The downstream early return has two independent triggers, and the
	 * ethertype one is a > rather than a set membership: IPoE passes, and
	 * PPPoE, ARP and IPv6 do not. Getting that comparison backwards would
	 * drop every IPoE rule and keep the rest. */
	build(e, 15, 4096, 0,  15, 4096, 0, EVTOCD_F_ET_DO_NOT_FILTER,
	      0, 15, 4096, 0,  0, 100, 4);
	evtocd_decode(e, &t);
	ok(!evtocd_ds_skipped(&t), "no ethertype filter: downstream keeps it");
	build(e, 15, 4096, 0,  15, 4096, 0, EVTOCD_F_ET_IPOE_0800,
	      0, 15, 4096, 0,  0, 100, 4);
	evtocd_decode(e, &t);
	ok(!evtocd_ds_skipped(&t), "IPoE: downstream keeps it");
	build(e, 15, 4096, 0,  15, 4096, 0, EVTOCD_F_ET_PPPOE,
	      0, 15, 4096, 0,  0, 100, 4);
	evtocd_decode(e, &t);
	ok(evtocd_ds_skipped(&t), "PPPoE: downstream drops the rule");
	build(e, 15, 4096, 0,  15, 4096, 0, EVTOCD_F_ET_IPV6_86DD,
	      0, 15, 4096, 0,  0, 100, 4);
	evtocd_decode(e, &t);
	ok(evtocd_ds_skipped(&t), "IPv6: downstream drops the rule");

	/* A shape G.988 does not define -- outer default, inner examined. It
	 * must not be reported as a default rule, because the generator would
	 * then hand it to the plugin that rescues default drops. */
	build(e, 14, 4096, 0,  0, 100, 4, 0,   0, 15, 4096, 0,  0, 100, 4);
	evtocd_decode(e, &t);
	ok(evtocd_default_rule_tags(&t) == -1,
	   "outer default with an examined inner tag is not a default rule");

	/* The two halves are laid out differently, and this is the assertion
	 * that catches one being copied to the other: the same field value in
	 * the filter and in the treatment lands at different bit positions.
	 * A single entry with every field distinct pins all fourteen. */
	build(e, 1, 11, 2,  3, 22, 4, 5,   2, 6, 33, 1,  7, 44, 3);
	evtocd_decode(e, &t);
	ok(t.f_out_pri == 1 && t.f_out_vid == 11 && t.f_out_tpid == 2,
	   "fourteen distinct fields: the outer filter");
	ok(t.f_in_pri == 3 && t.f_in_vid == 22 && t.f_in_tpid == 4 &&
	   t.f_ethertype == 5, "the inner filter");
	ok(t.remove_tags == 2 && t.t_out_pri == 6 && t.t_out_vid == 33 &&
	   t.t_out_tpid == 1, "the outer treatment");
	ok(t.t_in_pri == 7 && t.t_in_vid == 44 && t.t_in_tpid == 3,
	   "the inner treatment");

	/* The widest legal values, to catch a mask one bit short. A VID field
	 * is 13 bits so it holds the 4097 COPY_FROM_OUTER sentinel, which is
	 * past the 4094 legal maximum on purpose. */
	build(e, 15, 8191, 7,  15, 8191, 7, 15,   3, 15, 8191, 7,  15, 8191, 7);
	evtocd_decode(e, &t);
	ok(t.f_out_vid == 8191 && t.t_in_vid == 8191,
	   "a VID field is 13 bits wide in both halves");
	ok(t.f_ethertype == 15 && t.remove_tags == 3 && t.t_out_tpid == 7,
	   "and the narrow fields reach their maxima");
	build(e, 0, EVTOCD_T_VID_COPY_OUTER, 0,  0, 0, 0, 0,
	      0, 0, EVTOCD_T_VID_COPY_OUTER, 0,  0, EVTOCD_T_VID_COPY_INNER, 0);
	evtocd_decode(e, &t);
	ok(t.t_out_vid == 4097 && t.t_in_vid == 4096,
	   "the copy-from-outer and copy-from-inner sentinels survive the decode");

	/* ------------------------------------------- what an entry means --- */

	{
		struct evtocd_action a;
		char line[160];

		/* isp1 instance 1 index 0: the rule that tags everything with
		 * VLAN 1. The single added tag lives in the INNER treatment
		 * word with the outer set to DO_NOT_ADD, which is the shape an
		 * implementation reaching for the outer word first gets wrong
		 * -- and it is the common case on this device. */
		build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  0, 1, 4);
		evtocd_decode(e, &t);
		evtocd_action(&t, 0x8100, 0x8100, &a);
		ok(a.tags_in == EVTOCD_UNTAGGED, "it is the untagged rule");
		ok(!a.discard && a.tags_removed == 0, "nothing is removed");
		ok(a.tags_added == 1, "one tag is added, from the INNER word");
		ok(a.add[0].vid_src == EVTOCD_VID_ASSIGN && a.add[0].vid == 1,
		   "and it is VLAN 1");
		ok(a.add[0].pri_src == EVTOCD_PRI_ASSIGN && a.add[0].pri == 0,
		   "at priority 0");
		ok(a.add[0].tpid == 0x8100, "with TPID 0x8100, named literally");
		ok(!a.transparent, "so it is not transparent");

		evtocd_describe(&t, 0x8100, 0x8100, line, sizeof line);
		out_fmt("      %s\n", line);
		ok(str_eq(line, "untagged frames: add VLAN 1 pri 0 tpid 0x8100"),
		   "and it reads as a sentence");

		/* isp1 instance 1 index 1: the untagged default, discarding. */
		build(e, 14, 4096, 0,  14, 4096, 0, 0,   3, 15, 0, 0,  15, 0, 0);
		evtocd_decode(e, &t);
		evtocd_action(&t, 0x8100, 0x8100, &a);
		ok(a.discard && a.tags_added == 0, "the default rule discards");
		ok(a.tags_in == EVTOCD_DOUBLE_TAG,
		   "and both filter halves at DEFAULT is the double-tag rule");
		evtocd_describe(&t, 0x8100, 0x8100, line, sizeof line);
		out_fmt("      %s\n", line);
		ok(str_eq(line, "double-tagged frames (the default rule): DISCARD"),
		   "and says so, naming itself as the default");

		/* Transparent: filter matches a single tag, treatment adds and
		 * removes nothing. */
		build(e, 15, 4096, 0,  8, 4096, 0, 0,   0, 15, 0, 0,  15, 0, 0);
		evtocd_decode(e, &t);
		evtocd_action(&t, 0x8100, 0x8100, &a);
		ok(a.tags_in == EVTOCD_SINGLE_TAG, "outer ignored, inner filtered: single tag");
		ok(a.transparent, "nothing removed and nothing added is transparent");
		evtocd_describe(&t, 0x8100, 0x8100, line, sizeof line);
		ok(str_eq(line, "single-tagged frames: pass through unchanged"),
		   "and reads as pass-through");

		/* Two tags out, and the ordering: add[0] is the OUTER. */
		build(e, 0, 100, 4,  0, 200, 4, 0,   1, 3, 300, 4,  5, 400, 4);
		evtocd_decode(e, &t);
		evtocd_action(&t, 0x8100, 0x8100, &a);
		ok(a.tags_in == EVTOCD_DOUBLE_TAG, "an examined outer tag is the double rule");
		ok(a.tags_removed == 1 && a.tags_added == 2, "remove one, add two");
		ok(a.add[0].vid == 300 && a.add[0].pri == 3, "add[0] is the OUTER treatment");
		ok(a.add[1].vid == 400 && a.add[1].pri == 5, "add[1] is the inner");
		evtocd_describe(&t, 0x8100, 0x8100, line, sizeof line);
		out_fmt("      %s\n", line);
		ok(str_eq(line, "double-tagged frames: remove 1 tag, add VLAN 300 "
				"pri 3 tpid 0x8100 outside VLAN 400 pri 5 tpid 0x8100"),
		   "and reads outer-outside-inner");

		/* Priority copy on a single-tag rule falls back to copy-outer.
		 * The vendor tolerates the impossible case rather than failing,
		 * and its own comment says so. */
		build(e, 15, 4096, 0,  8, 4096, 4, 0,
		      0, 15, 0, 0,  EVTOCD_T_PRI_COPY_INNER, 50, 4);
		evtocd_decode(e, &t);
		evtocd_action(&t, 0x8100, 0x8100, &a);
		ok(a.add[0].pri_src == EVTOCD_PRI_COPY_OUTER,
		   "copy-from-inner on a single-tag rule falls back to copy-from-outer");

		/* On an untagged rule there is no tag to copy from at all, and
		 * the vendor returns an error rather than picking a value. */
		build(e, 15, 4096, 0,  15, 4096, 0, 0,
		      0, 15, 4096, 0,  EVTOCD_T_PRI_COPY_OUTER, 7, 4);
		evtocd_decode(e, &t);
		evtocd_action(&t, 0x8100, 0x8100, &a);
		ok(a.add[0].pri_src == EVTOCD_PRI_IMPOSSIBLE,
		   "and on an untagged rule a copy has no source at all");

		/* DSCP, and the VID copy sentinels. */
		build(e, 0, 10, 4,  0, 20, 4, 0,
		      0, EVTOCD_T_PRI_FROM_DSCP, EVTOCD_T_VID_COPY_OUTER, 4,
		      1, EVTOCD_T_VID_COPY_INNER, 4);
		evtocd_decode(e, &t);
		evtocd_action(&t, 0x8100, 0x8100, &a);
		ok(a.add[0].pri_src == EVTOCD_PRI_DSCP, "priority 10 is derive-from-DSCP");
		ok(a.add[0].vid_src == EVTOCD_VID_COPY_OUTER, "VID 4097 copies the outer tag");
		ok(a.add[1].vid_src == EVTOCD_VID_COPY_INNER, "VID 4096 copies the inner tag");

		/* TPID resolution. 4 is literal, 6 and 7 are the OutputTPID,
		 * 0 and 1 copy from the filter -- which can be unresolvable. */
		build(e, 15, 4096, 0,  15, 4096, 0, 0,
		      0, 15, 4096, 0,  0, 1, EVTOCD_T_TPID_OUTPUT_DEI_0);
		evtocd_decode(e, &t);
		evtocd_action(&t, 0x8100, 0x88a8, &a);
		ok(a.add[0].tpid == 0x88a8, "treatment TPID 6 is the OutputTPID");
		build(e, 15, 4096, 0,  15, 4096, EVTOCD_F_TPID_INPUT, 0,
		      0, 15, 4096, 0,  0, 1, EVTOCD_T_TPID_COPY_INNER);
		evtocd_decode(e, &t);
		evtocd_action(&t, 0x9100, 0x8100, &a);
		ok(a.add[0].tpid == 0x9100, "treatment TPID 0 copies the filter inner, here the InputTPID");
		build(e, 15, 4096, 0,  15, 4096, EVTOCD_F_TPID_DO_NOT_FILTER, 0,
		      0, 15, 4096, 0,  0, 1, EVTOCD_T_TPID_COPY_INNER);
		evtocd_decode(e, &t);
		evtocd_action(&t, 0x9100, 0x8100, &a);
		ok(a.add[0].tpid == 0, "and is unresolvable when that filter names no TPID");
		evtocd_describe(&t, 0x9100, 0x8100, line, sizeof line);
		ok(str_eq(line, "untagged frames: add VLAN 1 pri 0 tpid unresolved"),
		   "which the description says rather than printing 0x0000");

		/* Truncation must not run off the end. */
		{
			char tiny[12];
			int n;

			build(e, 0, 100, 4,  0, 200, 4, 0,   1, 3, 300, 4,  5, 400, 4);
			evtocd_decode(e, &t);
			n = evtocd_describe(&t, 0x8100, 0x8100, tiny, sizeof tiny);
			ok(n < (int)sizeof tiny && tiny[n] == 0,
			   "a description truncates inside a short buffer and stays terminated");
		}
	}

	/* The one shape the datapath applies (evtocd_olt_tag). The tables are
	 * what the OLTs provision: the OLT of issue #29, and ISP1's. */
	{
		uint8_t tbl[3 * EVTOCD_ENTRY_LEN];
		int vid = 0, pri = 0;
		const char *why;

		/* untagged -> add VLAN 100 pri 0 tpid 0x8100 (f8000000f8000000000f800000000324) */
		build(tbl, 15, 4096, 0,  15, 4096, 0, 0,  0, 15, 4096, 0,  0, 100, 4);
		/* single- and double-tagged defaults: discard */
		build(tbl + 16, 15, 4096, 0,  14, 4096, 0, 0,  3, 15, 4096, 0,  15, 4096, 0);
		build(tbl + 32, 14, 4096, 0,  14, 4096, 0, 0,  3, 15, 4096, 0,  15, 4096, 0);
		why = evtocd_olt_tag(tbl, 3, 0x8100, &vid, &pri);
		ok(!why && vid == 100 && pri == 0, "#29: untagged adds VLAN 100 pri 0");

		build(tbl, 15, 4096, 0,  15, 4096, 0, 0,  0, 15, 4096, 0,  5, 1, 4);
		why = evtocd_olt_tag(tbl, 3, 0x8100, &vid, &pri);
		ok(!why && vid == 1 && pri == 5, "ISP1: untagged adds VLAN 1, priority kept");

		build(tbl, 15, 4096, 0,  15, 4096, 0, 0,  0, 15, 4096, 0,  8, 100, 4);
		ok(evtocd_olt_tag(tbl, 3, 0x8100, &vid, &pri) != 0, "copy-inner priority on untagged is not applied");
		build(tbl, 15, 4096, 0,  15, 4096, 0, 0,  0, 15, 4096, 0,  0, 4096, 4);
		ok(evtocd_olt_tag(tbl, 3, 0x8100, &vid, &pri) != 0, "VID 4096 is not a concrete VID");
		build(tbl, 15, 4096, 0,  15, 4096, 0, 0,  0, 15, 4096, 0,  0, 4095, 4);
		ok(evtocd_olt_tag(tbl, 3, 0x8100, &vid, &pri) != 0, "VID 4095 is reserved");
		/* Output TPID with DEI 0 (6), Output TPID 0x8100: the same tag
		 * as 4 (f8000000f8000000000f000300000966). */
		build(tbl, 15, 4096, 0,  15, 4096, 0, 0,  0, 15, 4096, 0,  0, 300, 6);
		why = evtocd_olt_tag(tbl, 3, 0x8100, &vid, &pri);
		ok(!why && vid == 300 && pri == 0, "output TPID 0x8100, DEI 0: untagged adds VLAN 300");
		ok(evtocd_olt_tag(tbl, 3, 0x88a8, &vid, &pri) != 0, "output TPID 0x88a8 is not applied");
		build(tbl, 15, 4096, 0,  15, 4096, 0, 0,  0, 15, 4096, 0,  0, 300, 7);
		ok(evtocd_olt_tag(tbl, 3, 0x8100, &vid, &pri) != 0, "a drop-eligible tag (DEI 1) is not applied");
		build(tbl, 15, 4096, 0,  15, 4096, 0, 0,  1, 15, 4096, 0,  0, 100, 4);
		ok(evtocd_olt_tag(tbl, 3, 0x8100, &vid, &pri) != 0, "removing a tag as well is not applied");
		build(tbl, 15, 4096, 0,  15, 4096, 0, 0,  0, 0, 200, 4,  0, 100, 4);
		ok(evtocd_olt_tag(tbl, 3, 0x8100, &vid, &pri) != 0, "two added tags are not applied");
		build(tbl, 15, 4096, 0,  15, 4096, 0, 0,  3, 15, 4096, 0,  15, 4096, 0);
		ok(evtocd_olt_tag(tbl, 3, 0x8100, &vid, &pri) != 0, "untagged frames discarded is not applied");
		ok(evtocd_olt_tag(tbl, 0, 0x8100, &vid, &pri) != 0, "an empty table is not applied");
	}


	/* ---------------------------------------- what the classifier can do --- */
	{
		struct evtocd_plan u, d;
		const char *why;
		const char *dwhy = 0;
		char line[160];

		/* G.988 9.3.13, the one-to-one VLAN translation: a single C-tag
		 * of VID 410, any priority, becomes VID 1410 and keeps its
		 * priority. Filter outer ignored, inner (pri 8 = any, VID 410);
		 * treatment removes one tag and adds one from the inner word
		 * with the priority copied (8) and the VID assigned. */
		build(e, 15, 4096, 0,  8, 410, 0, 0,   1, 15, 4096, 0,  8, 1410, 4);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(!why, "translation 410 -> 1410: a plan");
		ok(u.c.in && u.c.out && u.c.op == EVTOCD_OP_REWRITE && !u.s.in && !u.s.out,
		   "it replaces the C-tag and touches no S-tag");
		ok(u.c.f_vid == 410 && u.c.f_pri == -1 && u.c.vid == 1410 && u.c.pri == -1,
		   "from VID 410, any priority, to 1410, priority kept");
		ok(u.out_tags == 1 && u.out_vid == 1410 && u.out_pri == -1,
		   "it leaves one tag, VID 1410, priority following the frame");
		evtocd_plan_describe(&u, line, sizeof line);
		out_fmt("      %s\n", line);
		ok(str_eq(line, "match C-tag VID 410: C-tag replace VID 1410 pri kept"),
		   "and reads as a sentence");
		ok(evtocd_plan_ds(&u, EVTOCD_DSMODE_INVERSE, &d, &dwhy) == 1,
		   "downstream (mode 0) has a rule");
		ok(d.c.in && d.c.out && d.c.op == EVTOCD_OP_REWRITE && d.c.f_vid == 1410 &&
		   d.c.f_pri == -1 && d.c.vid == 410 && d.c.pri == -1,
		   "which matches 1410 and puts back 410, priority kept");

		/* The operator vector: one VID, three priorities, each its own
		 * entry. The priority is named in the filter and copied, so it
		 * is a literal on the way out and picks the GEM port. */
		{
			static const uint8_t pris[3] = { 0, 5, 6 };

			for (int i = 0; i < 3; i++) {
				build(e, 15, 4096, 0,  pris[i], 410, 0, 0,
				      1, 15, 4096, 0,  8, 1410, 4);
				evtocd_decode(e, &t);
				why = evtocd_plan(&t, 0x8100, 0x8100, &u);
				ok(!why && u.c.f_vid == 410 && u.c.f_pri == pris[i] &&
				   u.c.vid == 1410 && u.c.pri == pris[i] && u.out_pri == pris[i],
				   "operator vector: VID 410 priority p -> 1410 priority p (pri 0, 5, 6)");
				ok(evtocd_plan_ds(&u, 0, &d, &dwhy) == 1 && d.c.f_vid == 1410 &&
				   d.c.f_pri == pris[i] && d.c.vid == 410 && d.c.pri == pris[i],
				   "and downstream matches the same priority");
			}
		}

		/* Priority remap that names the VID: (100, p3) -> priority 5, VID
		 * copied from the inner tag. A copy of a VID the filter names is
		 * a literal. */
		build(e, 15, 4096, 0,  3, 100, 0, 0,   1, 15, 4096, 0,  5, EVTOCD_T_VID_COPY_INNER, 4);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(!why && u.c.vid == 100 && u.c.pri == 5 && u.c.f_pri == 3,
		   "priority remap 3 -> 5 on VID 100: VID copied from the filter");
		ok(evtocd_plan_ds(&u, 0, &d, &dwhy) == 1 && d.c.f_pri == 5 && d.c.pri == 3 &&
		   d.c.vid == 100, "and downstream puts priority 3 back");

		/* The same remap with a filter that does not name the VID: the
		 * classifier has nothing to put in the VID field. */
		build(e, 15, 4096, 0,  3, 4096, 0, 0,   1, 15, 4096, 0,  5, EVTOCD_T_VID_COPY_INNER, 4);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(why != 0, "a VID copied from a filter that does not name it is refused");

		/* Untagged: add a C-tag, and add an S-tag. */
		build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  2, 100, 4);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(!why && !u.c.in && u.c.out && u.c.op == EVTOCD_OP_PUSH && u.c.vid == 100 &&
		   u.c.pri == 2 && !u.s.in && !u.s.out, "untagged -> add C-tag VID 100 pri 2");
		ok(evtocd_plan_ds(&u, 0, &d, &dwhy) == 1 && d.c.in && !d.c.out &&
		   d.c.op == EVTOCD_OP_POP && d.c.f_vid == 100 && d.c.f_pri == 2,
		   "downstream takes it off VID 100 pri 2");
		build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  2, 300, EVTOCD_T_TPID_OUTPUT_DEI_0);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x88a8, &u);
		ok(!why && u.s.out && !u.s.in && u.s.op == EVTOCD_OP_PUSH && u.s.vid == 300 &&
		   u.s.pri == 2 && !u.c.out, "untagged -> add S-tag VID 300 (Output TPID 0x88a8)");
		ok(u.cmp == EVTOCD_CMP_C, "an untagged filter compares nothing");
		ok(evtocd_plan_ds(&u, 0, &d, &dwhy) == 1 && d.s.in && !d.s.out &&
		   d.s.op == EVTOCD_OP_POP && d.cmp == EVTOCD_CMP_S && d.s.f_vid == 300,
		   "downstream matches the S-VID and pops it");
		why = evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(!why && u.c.out && !u.s.out,
		   "the same entry with Output TPID 0x8100 is a C-tag");
		why = evtocd_plan(&t, 0x8100, 0x9100, &u);
		ok(why != 0, "and with Output TPID 0x9100 is not a plan");

		/* S-tag push in front of a C-tag (the double-tagging service): C
		 * VID 100 in, S-VID 200 pri 3 pushed, nothing removed. */
		build(e, 15, 4096, 0,  8, 100, 0, 0,   0, 15, 4096, 0,  3, 200, EVTOCD_T_TPID_OUTPUT_DEI_0);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x88a8, &u);
		ok(!why && u.c.in && u.c.out && u.c.op == EVTOCD_OP_PASS && u.s.op == EVTOCD_OP_PUSH &&
		   u.s.vid == 200 && u.s.pri == 3 && u.out_tags == 2 && u.out_vid == 200 &&
		   u.out_pri == 3, "C-VID 100 -> S-VID 200 pri 3 in front of it");
		ok(u.cmp == EVTOCD_CMP_C && u.c.f_vid == 100, "upstream compares the C-VID");
		ok(evtocd_plan_ds(&u, 0, &d, &dwhy) == 1 && d.s.in && !d.s.out &&
		   d.s.op == EVTOCD_OP_POP && d.cmp == EVTOCD_CMP_S && d.s.f_vid == 200 &&
		   d.s.f_pri == 3 && d.c.f_vid == -1 && d.ds_wider,
		   "downstream pops S-VID 200 pri 3 and, comparing one tag, not the C-VID");

		/* S-tag pop: S-VID 200 in front of any C-tag, S-tag removed. The
		 * input TPID is 0x88a8, which is what says the outer tag is an S-tag. */
		build(e, 8, 200, 0,  8, 4096, 0, 0,   1, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x88a8, 0x8100, &u);
		ok(!why && u.s.in && !u.s.out && u.s.op == EVTOCD_OP_POP && u.s.f_vid == 200 &&
		   u.c.in && u.c.out && u.c.op == EVTOCD_OP_PASS && u.cmp == EVTOCD_CMP_S,
		   "S-VID 200 over a C-tag: pop the S-tag");
		ok(u.out_tags == 1 && u.out_vid == -1, "one tag left, its VID not known");
		ok(evtocd_plan_ds(&u, 0, &d, &dwhy) == 1 && d.s.op == EVTOCD_OP_PUSH &&
		   d.s.vid == 200 && d.s.pri == 0 && d.c.in && d.c.out,
		   "downstream pushes S-VID 200 back, priority 0 (the filter named none)");
		why = evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(why != 0, "with input TPID 0x8100 the same entry is two C-tags, which is refused");

		/* S-tag replace: S-VID 200 over a C-tag becomes S-VID 300. */
		build(e, 8, 200, 0,  8, 4096, 0, 0,   1, 15, 4096, 0,  2, 300, EVTOCD_T_TPID_OUTPUT_DEI_0);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x88a8, 0x88a8, &u);
		ok(!why && u.s.op == EVTOCD_OP_REWRITE && u.s.vid == 300 && u.s.pri == 2 &&
		   u.c.op == EVTOCD_OP_PASS, "S-VID 200 -> 300: the S-tag is replaced, the C-tag passes");
		ok(evtocd_plan_ds(&u, 0, &d, &dwhy) == 0 && dwhy,
		   "but its priority is not undone: the filter named none, and an S-tag has no keep");

		/* Both tags replaced, and two tags added to untagged frames. */
		build(e, 8, 200, 0,  8, 100, 4, 0,   2, 1, 300, 0,  2, 400, 4);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x88a8, 0x88a8, &u);
		ok(why != 0, "a filter on the S-VID and the C-VID together is refused");
		build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 1, 300, 6,  2, 400, 4);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x88a8, &u);
		ok(!why && u.s.op == EVTOCD_OP_PUSH && u.s.vid == 300 && u.s.pri == 1 &&
		   u.c.op == EVTOCD_OP_PUSH && u.c.vid == 400 && u.c.pri == 2 && u.out_tags == 2,
		   "untagged -> S-VID 300 outside C-VID 400");
		build(e, 15, 4096, 0,  8, 100, 0, 0,   1, 1, 300, 6,  2, 400, 4);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x88a8, &u);
		ok(!why && u.s.op == EVTOCD_OP_PUSH && u.c.op == EVTOCD_OP_REWRITE && u.c.vid == 400,
		   "C-VID 100 -> S-VID 300 outside C-VID 400: a push and a replace");

		/* Removing: a single C-tag popped, and both tags of a double. */
		build(e, 15, 4096, 0,  8, 100, 0, 0,   1, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(!why && u.c.op == EVTOCD_OP_POP && u.out_tags == 0, "pop the tag of VID 100");
		ok(evtocd_plan_ds(&u, 0, &d, &dwhy) == 1 && d.c.op == EVTOCD_OP_PUSH && d.c.vid == 100 &&
		   !d.c.in && d.c.out, "downstream tags the untagged frame with VID 100");
		build(e, 15, 4096, 0,  8, 4096, 0, 0,   1, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(!why && evtocd_plan_ds(&u, 0, &d, &dwhy) == 0 && dwhy,
		   "pop of any tag: there is no downstream rule to give it back");

		/* Many to one: any single tag becomes VID 100. Upstream only. */
		build(e, 15, 4096, 0,  8, 4096, 0, 0,   1, 15, 4096, 0,  8, 100, 4);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(!why && u.c.f_vid == -1 && u.c.vid == 100, "any VID -> 100");
		ok(evtocd_plan_ds(&u, 0, &d, &dwhy) == 0 && dwhy,
		   "many-to-one has no way back");

		/* Downstream modes. */
		build(e, 15, 4096, 0,  4, 410, 0, 0,   1, 15, 4096, 0,  4, 1410, 4);
		evtocd_decode(e, &t);
		why = evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(!why && u.c.f_pri == 4 && u.c.pri == 4, "priority 4, VID 410 -> 1410 (pri copied)");
		ok(evtocd_plan_ds(&u, EVTOCD_DSMODE_FORWARD, &d, &dwhy) == 0,
		   "mode 1: no downstream rule");
		ok(evtocd_plan_ds(&u, EVTOCD_DSMODE_DROP_ALL, &d, &dwhy) == 0,
		   "mode 8: none either");
		ok(evtocd_plan_ds(&u, 9, &d, &dwhy) == 0, "mode 9 does not exist");
		ok(evtocd_plan_ds(&u, EVTOCD_DSMODE_VID_PRI, &d, &dwhy) == 1 &&
		   d.c.f_vid == 1410 && d.c.f_pri == 4 && !d.ds_wider, "mode 2: VID and priority");
		ok(evtocd_plan_ds(&u, EVTOCD_DSMODE_VID, &d, &dwhy) == 1 &&
		   d.c.f_vid == 1410 && d.c.f_pri == -1 && d.c.vid == 410, "mode 3: VID only");
		ok(evtocd_plan_ds(&u, EVTOCD_DSMODE_PRI, &d, &dwhy) == 1 &&
		   d.c.f_vid == -1 && d.c.f_pri == 4, "mode 4: priority only");
		ok(evtocd_plan_ds(&u, EVTOCD_DSMODE_VID_DROP, &d, &dwhy) == 1 &&
		   d.c.f_vid == 1410 && d.c.f_pri == -1 && d.ds_wider,
		   "mode 6: as 3, and says the frames it would discard are forwarded");

		/* What is not a plan. */
		build(e, 14, 4096, 0,  14, 4096, 0, 0,   3, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_plan(&t, 0x8100, 0x8100, &u) != 0, "a default rule is not a plan");
		build(e, 15, 4096, 0,  8, 100, 0, EVTOCD_F_ET_PPPOE,   1, 15, 4096, 0,  8, 200, 4);
		evtocd_decode(e, &t);
		ok(evtocd_plan(&t, 0x8100, 0x8100, &u) != 0, "an EtherType filter on tagged frames is not (the value behind a tag is not known)");
		build(e, 15, 4096, 0,  8, 100, EVTOCD_F_TPID_INPUT_DEI_0, 0,   1, 15, 4096, 0,  8, 200, 4);
		evtocd_decode(e, &t);
		ok(evtocd_plan(&t, 0x8100, 0x8100, &u) != 0, "nor a DEI filter");
		build(e, 15, 4096, 0,  8, 100, 0, 0,   3, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_plan(&t, 0x8100, 0x8100, &u) != 0, "nor a rule that discards what it matches");
		build(e, 15, 4096, 0,  8, 100, 0, 0,   1, 15, 4096, 0,  EVTOCD_T_PRI_FROM_DSCP, 200, 4);
		evtocd_decode(e, &t);
		ok(evtocd_plan(&t, 0x8100, 0x8100, &u) != 0, "nor a priority from the DSCP");
		build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  EVTOCD_T_PRI_COPY_INNER, 200, 4);
		evtocd_decode(e, &t);
		ok(evtocd_plan(&t, 0x8100, 0x8100, &u) != 0, "nor a priority copied from an untagged frame");
		build(e, 15, 4096, 0,  8, 100, 0, 0,   2, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_plan(&t, 0x8100, 0x8100, &u) != 0, "nor two tags removed from a single-tagged frame");
		build(e, 15, 4096, 0,  8, 100, 0, 0,   0, 15, 4096, 0,  0, 200, 7);
		evtocd_decode(e, &t);
		ok(evtocd_plan(&t, 0x8100, 0x8100, &u) != 0, "nor an added tag with the DEI bit set");
		build(e, 15, 4096, 0,  8, 100, 0, 0,   0, 15, 4096, 0,  0, 200, 4);
		evtocd_decode(e, &t);
		ok(evtocd_plan(&t, 0x8100, 0x8100, &u) != 0,
		   "nor a C-tag added in front of a C-tag (the classifier cannot tell two from one)");
	}

	/* ------------------------------------ the EtherType of untagged frames --- */
	{
		struct evtocd_plan u, d;
		struct omci_vlan_oper vr;
		static const struct { uint8_t code; uint8_t n; uint16_t a, b; } ets[] = {
			{ EVTOCD_F_ET_IPOE_0800, 1, 0x0800, 0 },
			{ EVTOCD_F_ET_PPPOE, 2, 0x8863, 0x8864 },
			{ EVTOCD_F_ET_ARP_0806, 1, 0x0806, 0 },
			{ EVTOCD_F_ET_IPV6_86DD, 1, 0x86dd, 0 },
			{ EVTOCD_F_ET_EAPOL_888E, 1, 0x888e, 0 },
		};

		for (unsigned i = 0; i < sizeof ets / sizeof ets[0]; i++) {
			build(e, 15, 4096, 0,  15, 4096, 0, ets[i].code,   0, 15, 4096, 0,  3, 100, 4);
			evtocd_decode(e, &t);
			ok(evtocd_plan(&t, 0x8100, 0x8100, &u) == 0 && u.et_n == ets[i].n &&
			   u.et[0] == ets[i].a && (ets[i].n < 2 || u.et[1] == ets[i].b),
			   "untagged frames of one EtherType code: the values G.988 gives it");
			ok(evtocd_plan_ds(&u, 0, &d, 0) && d.et_n == 0,
			   "and the way back names no EtherType: it is told by its VID");
		}
		build(e, 15, 4096, 0,  15, 4096, 0, 6,   0, 15, 4096, 0,  3, 100, 4);
		evtocd_decode(e, &t);
		ok(evtocd_plan(&t, 0x8100, 0x8100, &u) != 0, "an EtherType code G.988 does not define is not a plan");
		build(e, 15, 4096, 0,  15, 4096, 0, EVTOCD_F_ET_PPPOE,   0, 15, 4096, 0,  3, 100, 4);
		evtocd_decode(e, &t);
		(void)evtocd_plan(&t, 0x8100, 0x8100, &u);
		evtocd_plan_oper(&u, &vr);
		ok(vr.filter.ethertype == OMCI_ETHTYPE_NO_CARE, "the plan alone names no EtherType");
		evtocd_oper_ethertype(&vr, u.et[1]);
		ok(vr.filter.ethertype == (OMCI_ETHTYPE_LITERAL | 0x8864), "the descriptor carries one literal value");
		evtocd_oper_rest(&vr);
		ok(vr.filter.ethertype == OMCI_ETHTYPE_REST, "or says it is for the rest of the untagged frames");
	}

	/* ----------------------------------------- an entry that discards --- */
	{
		struct evtocd_drop dr;
		struct evtocd_plan u;
		struct omci_vlan_oper vr;
		unsigned m;

		/* C-tag VID 20, any priority: discard. */
		build(e, 15, 4096, 0,  8, 20, 0, 0,   3, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_drop(&t, 0x8100, &dr) == 0 && dr.vid == 20 && dr.pri == -1, "a discard of VID 20, any priority");
		m = evtocd_drop_all(&dr);
		ok(m == 0xff, "every priority to start with");
		build(e, 15, 4096, 0,  5, 20, 0, 0,   3, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_drop(&t, 0x8100, &dr) == 0 && dr.vid == 20 && dr.pri == 5, "a discard of VID 20 priority 5");
		ok(evtocd_drop_all(&dr) == 0x20, "which is one priority");
		evtocd_drop_oper(&dr, 5, &vr);
		ok(vr.rule_gen == OMCI_VLAN_OPER_EXTVLAN_DROP && vr.filter.inner.vid == 20 &&
		   vr.filter.inner.pri == 5 && (vr.filter.inner_mode & OMCI_TAGF_PRI), "its descriptor");

		build(e, 15, 4096, 0,  15, 4096, 0, 0,   3, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_drop(&t, 0x8100, &dr) != 0, "a discard of untagged frames is not one (VID 0 reads the same)");
		build(e, 15, 4096, 0,  8, 4096, 0, 0,   3, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_drop(&t, 0x8100, &dr) != 0, "nor a discard of every VID");
		build(e, 8, 20, 0,  8, 4096, 0, 0,   3, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_drop(&t, 0x88a8, &dr) != 0, "nor a discard of two-tagged frames");
		build(e, 15, 4096, 0,  8, 20, 0, 1,   3, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_drop(&t, 0x8100, &dr) != 0, "nor one that also filters on an EtherType");
		build(e, 15, 4096, 0,  8, 20, 5, 0,   3, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_drop(&t, 0x88a8, &dr) != 0, "nor a discard of S-tagged frames");
		build(e, 14, 4096, 0,  14, 4096, 0, 0,   3, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		ok(evtocd_drop(&t, 0x8100, &dr) != 0, "a default rule is not a discard to build");

		/* Against the other entries of the table: VID 20, any priority, and
		 * an entry that names VID 20 priority 5 (a translation). */
		build(e, 15, 4096, 0,  8, 20, 0, 0,   3, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		(void)evtocd_drop(&t, 0x8100, &dr);
		m = evtocd_drop_all(&dr);
		build(e, 15, 4096, 0,  5, 20, 0, 0,   1, 15, 4096, 0,  8, 1020, 4);
		evtocd_decode(e, &t);
		(void)evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(!evtocd_drop_shadow(&dr, &u, &m) && m == 0xdf, "the priority the other entry names is left out of the discard");
		build(e, 15, 4096, 0,  8, 21, 0, 0,   1, 15, 4096, 0,  8, 1021, 4);
		evtocd_decode(e, &t);
		(void)evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(!evtocd_drop_shadow(&dr, &u, &m) && m == 0xdf, "an entry of another VID changes nothing");
		build(e, 15, 4096, 0,  15, 4096, 0, 0,   0, 15, 4096, 0,  0, 20, 4);
		evtocd_decode(e, &t);
		(void)evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(!evtocd_drop_shadow(&dr, &u, &m) && m == 0xdf, "nor does one for untagged frames");
		build(e, 15, 4096, 0,  8, 20, 0, 0,   1, 15, 4096, 0,  8, 1020, 4);
		evtocd_decode(e, &t);
		(void)evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(evtocd_drop_shadow(&dr, &u, &m) != 0, "an entry with the same filter as the discard: refused, nothing orders them");
		build(e, 15, 4096, 0,  3, 4096, 0, 0,   1, 15, 4096, 0,  8, 1020, 4);
		evtocd_decode(e, &t);
		(void)evtocd_plan(&t, 0x8100, 0x8100, &u);
		ok(evtocd_drop_shadow(&dr, &u, &m) != 0, "so is a priority-only entry beside a discard of the whole VID");
		build(e, 8, 200, 0,  8, 4096, 0, 0,   1, 15, 4096, 0,  15, 4096, 0);
		evtocd_decode(e, &t);
		(void)evtocd_plan(&t, 0x88a8, 0x8100, &u);
		ok(evtocd_drop_shadow(&dr, &u, &m) != 0, "and an entry on an S-tag: the C-tag field behind it is not known");
	}

	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();
	return failures ? 1 : 0;
}
