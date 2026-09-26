# Reading a G.988 extended-VLAN tagging entry: the single added tag comes from the inner treatment word

Each entry of the extended VLAN tagging operation table (G.988 class 171) packs its filter and treatment halves differently — a single-VLAN rule's tag comes from the inner treatment word, and the sentinel value 4096 means two different things depending on which half it appears in.

*Last verified: 2026-09-14*

---

## What

Each entry of the extended VLAN tagging operation table (G.988 class 171) is four big-endian words, and **the filter and treatment halves are not laid out the same way** — a filter priority is in the top nibble, a treatment priority at bit 16:

    word0  filter outer      PRI 31..28  VID 27..15  TPID 14..12
    word1  filter inner      as word0, plus EthType 3..0
    word2  treatment outer   RemoveTags 31..30  PRI 19..16  VID 15..3  TPID 2..0
    word3  treatment inner   PRI 19..16  VID 15..3  TPID 2..0

What the entry *does*, observed on this firmware's rule generator:

    tags the entry is about, from the FILTER side
      outer PRI 15 (IGNORE) and inner 15   untagged frames
      outer 15, inner anything else        single-tagged
      otherwise                            double-tagged

    what comes out, from the TREATMENT side
      tOut.pri 15 and tIn.pri 15   transparent, nothing added
      tOut.pri 15                  add ONE tag, FROM tIn
      otherwise                    add two, outer from tOut, inner from tIn

    RemoveTags  0/1/2 tags removed; 3 is not a count, it DISCARDS the frame

    priority    8 copy inner, 9 copy outer, 10 from DSCP, 15 do not add,
                anything else the literal value
    VID         4096 copy from inner, 4097 copy from outer, else literal
    TPID        0 copy the filter INNER tpid, 1 the filter OUTER tpid,
                4 literally 0x8100, and 2/3/6/7 all the OutputTPID

## Why it matters

**A rule that adds one VLAN carries it in the inner treatment word with the outer set to "do not add".** Code that reaches for the outer word first gets every single-VLAN rule wrong, and single-VLAN is the ordinary case on an ONU.

**4096 is "do not filter" in a filter VID and "copy from the inner tag" in a treatment VID** — same number, opposite halves, unrelated meanings. Anything that compares a bare 4096 has to say which half it is in.

**Which default rule an entry is, is not what it looks like.** Both filter halves at the DEFAULT sentinel (14) is the **double-tag** default, not the untagged one. Outer IGNORE with inner DEFAULT is the single-tag default. There is **no untagged default**: the untagged branch is reached by outer IGNORE with inner IGNORE and tests the treatment for DISCARD directly. An earlier description of this table, derived only from field values without tracing which branch of the firmware's logic actually gets taken, got this exactly backwards.

Two edges this firmware's rule generator handles and a naive reading does not: copy-from-inner on a single-tag rule falls back to copy-from-**outer**, tolerating the otherwise-impossible case; and either copy on an untagged rule is treated as an error, because there is no tag to copy from.

## Why it matters for a replacement implementation

What this firmware's rule generator *builds* internally from a class-171 entry is a lower-level switch-fabric rule that reaches the hardware through the bridge-connection command (see [the bridge-connection note](rtl9601-omci-bridge-connection.md)). The semantics above transfer to any reimplementation; the internal switch-fabric representation does not need to be reproduced exactly, and doing so is only worth it against an OLT that actually drives class 171 directly.

## Evidence

Layout and semantics decoded from the standard G.988 class-171 attribute definition and confirmed against this firmware's own behavior. Cross-checked against a captured read of class 171 from a live stick, whose five entries across two instances come out as: one rule adding a VLAN to untagged frames, and four **default drops** — a shape that a firmware feature module can override (see [feature bitmasks](rtl9601-omci-feature-bitmasks.md)), and which is enabled on some devices and not others.

## See also

- [Manual VLAN tagging](rtl9601-onu-manual-vlan.md)
- [OMCI table attributes](rtl9601-omci-table-attributes.md)
- [Feature bitmasks](rtl9601-omci-feature-bitmasks.md)
