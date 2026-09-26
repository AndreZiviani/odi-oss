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

	out_fmt("\n%s\n", failures ? "FAILURES" : "all ok");
	out_flush();
	return failures ? 1 : 0;
}
