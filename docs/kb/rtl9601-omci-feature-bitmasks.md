# The firmware's feature bitmasks are fully decodable from the shipped image

The four feature bitmasks that gate optional OMCI behavior are not undocumented magic numbers: each bit corresponds to a specific loadable plugin file shipped in the image, and the mapping can be regenerated directly from any given firmware image.

*Last verified: 2026-09-14*

---

## What

Four feature bitmasks each select a set of loadable plugins, and the plugins carry their own bit **in their filenames** — for example a plugin file named to indicate bit `0x4` of one mask, or bit `0x2` of another. The feature a plugin implements is exactly the behavior its code defines. Enumerating every plugin file shipped in an image yields a complete mapping from bitmask value to behavior, with nothing guessed — around 30 distinct entries across the four masks on one observed firmware base.

Three things that only become clear by building this mapping directly from a given image, rather than assuming it carries over from another:

- **A single bit can gate more than one feature.** Some plugin files implement two independent behaviors at once.
- **One capability bit is inert without a companion bit.** A particular bit in one mask only takes effect if a specific bit in a second mask is also set, loading the plugin that actually reads it; without that second bit the setting is silently ignored. That capability is also read as a single byte internally, so only the low byte of a larger identifier is used, not the whole value.
- **The stored values are decimal, not hexadecimal.** The configuration tool that sets these values accepts only decimal input, so a value typed as if it were hex silently stores something else.

## Why it matters

This is one of the least documented and most consequential groups of settings on the device — long-standing public discussion of one of these masks on community forums has gone unanswered with "no information on this." One of the mask's bits corresponds to a documented compatibility behavior (bypassing a particular per-connection sanity check) that had already been worked out independently from observed behavior, which corroborates the mapping method with a value that was already known.

The mapping is also per firmware image. Regenerating it against a different base tells you what *that* base implements, and a bit set with no matching plugin present in the image does nothing at all — a different failure mode from a bit that does something unwanted.

### Two sticks on the same firmware image can run different rule generators

These bitmasks are per-device configuration, not fixed by the image, and the consequence is bigger than "a feature is on or off". Measured on two otherwise-identical devices running the same firmware, one mask differed enough to select different plugin combinations on each, while a second mask was identical on both. One of the differing plugins overrides a default-drop behavior in the extended-VLAN rule generator described in [the extended-VLAN table note](rtl9601-omci-extvlan-table.md) — so one stick's class-171 rules go through a code path the other's do not. When a behavior is confirmed on one device and not another running nominally the same firmware, this per-device configuration difference is a candidate explanation to check before looking at anything the OLT provisioned.

A bit can also be set for a plugin that is not actually present in a given image; in that case the boot log records a failed file lookup for it, and the bit is silently a no-op.

A plugin's own behavior can be further configured by a second, independent mechanism: an option word read from a small config file, defaulting to "unconfigured" when that file has no relevant entry — and an unconfigured plugin behaves as if bit 0 of its own option word were set, not as if nothing were set. Reading option names in source order and assuming the first one listed is the default gets this backwards.

## Evidence

The full bitmask-to-plugin mapping can be produced directly from an unpacked firmware image by enumerating its plugin files and the behavior each one implements — this is real, checkable evidence rather than something inferred from external behavior. The result independently reproduces mappings previously published by community researchers (Anime4000/RTL960x issue #41 and #107 on the upstream tracker), including the multi-feature-bit cases, corroborating both.

## See also

- [Config store](rtl9601-config-store.md)
