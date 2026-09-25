# An 802.1p-mapped service carries its p-bit in the out-style tag too, not just the filter

A service reached through an 802.1p mapper needs its priority bit written into the bridge rule's output tag as well as its input filter — leaving the output at the default "ignore" value still forwards, so nothing but a byte-for-byte comparison against a known-good rule catches the omission.

*Last verified: 2026-09-21*

---

## What

In the bridge connection rule, a service reached through an 802.1p mapper (OMCI class 130) with a VID+priority filter (class 84) needs the filter's priority bit written into the tag priority filter field, **and** the same priority bit written into the output-style tag's priority field (for example, priority 4 for one VID, 5 for another on one observed device). Services filtered by VID alone use the "ignore" sentinel value (8) in that same output-style priority field. An untagged handoff service uses an add-tag rule with priority 0, taken from the config store.

## Why it matters

A bridge-rule implementation that copies the filter half correctly but leaves the output-style priority at the ignore sentinel for every service still forwards traffic — so nothing tells you it is wrong except a field-by-field comparison against a known-good rule dump from the stock firmware on the same provisioning. Keep such a capture on hand; it is the only reference answer for these rules.

## Evidence

- A known-good rule dump showing output-style `(PRI,TPID,VID)` as `(4,COPY_FROM_INNER,13)` and `(5,...,12)` for the two mapped services, and `(8,...,14)`, `(8,...,10)` for the VID-only ones.
- A replacement OMCI daemon, after the fix, reproducing output-style priority 4 / 5 on the two mapped services, 8 on the VID-only ones, and 0 on the untagged one.

## See also

- [GEM flows do not forward on their own](rtl9601-omci-bridge-connection.md)
- [Manual VLAN tagging](rtl9601-onu-manual-vlan.md)
