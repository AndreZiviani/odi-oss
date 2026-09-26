# OMCI table attributes on this firmware: an off-by-one index and a chunked Get-Next walk

This firmware's internal attribute index is offset by one from the standard G.988 attribute number, and any attribute wide enough to be a table is never returned inline in a Get response — it always diverts to a chunked Get-Next walk.

*Last verified: 2026-09-13*

---

## What

Two facts that anything reading or reimplementing this firmware's OMCI stack needs, and that give a plausible-looking wrong answer if guessed instead of checked.

**This firmware's internal attribute index counts the entity id itself as attribute 1.** So wherever this firmware refers to an attribute by index (for example in bitmask contexts), that index is the standard G.988 attribute number **plus one**. Verified across several unrelated managed-entity classes: a table attribute that G.988 numbers as attribute 1 is checked internally as index 2; a bridge-table attribute follows the same pattern; a VLAN-tagging table's table attribute (G.988 attribute 6) is checked as index 7, and its priority-mapping attribute (attribute 8) as index 9; a time-of-day attribute (attribute 4) is checked as index 5. Reading an internal index as if it were a direct G.988 attribute number points at the wrong attribute entirely — one such mismatch lands on a different, plausible-looking attribute, silently.

**A table attribute is never returned inline in an ordinary Get response.** This firmware routes a Get to a chunked Get-Next walk instead whenever the attribute's declared width reaches a threshold consistent with a table (or when its type is an octet-string/table type) — a plain Get response body only has room for a handful of small attributes' worth of data, so anything wider has to be walked. If more than one such attribute is requested in the same Get, only the first one in the request actually gets processed that way; the rest of the request is not looked at further.

## The Get-Next exchange

    request  contents +0  u16  attribute mask
                      +2  u16  sequence number
    response contents +0  u8   result
                      +1  u16  attribute mask, echoed
                      +3       up to 29 bytes of table data, or the remainder

The last chunk is whatever bytes remain, not padded out to 29. Before answering, the responder checks that the class id, the instance, and the attribute index all match what an earlier Get on the same table attribute established, so a Get-Next that does not properly follow its own preceding Get is refused rather than answered with some other entity's data. A sequence number requested past the end of the table is a failure result, not a zero-filled record.

Only a handful of managed-entity classes on this firmware have table attributes that go through this chunked path at all — among them the extended-VLAN tagging table, a general-purpose buffer entity, a generic-portal entity, the MAC bridge port's own bridge table, multicast operations profile and subscriber monitor entities, and a debug/log entity.

## What was ruled out

One attribute-flag bit looks, at first glance, like a "this is a table attribute" marker — it happens to be set on every attribute whose name suggests a table. **It is not actually what this firmware tests** when deciding whether to divert to Get-Next; the type/width check above is. Relying on that flag bit instead would incorrectly refuse the chunked Get-Next path for at least two known table attributes that do go through it in practice (the bridge port's own table attribute among them).

## See also

- [OMCI MIB model](rtl9601-omci-mib-model.md)
- [OMCI MIB readback](rtl9601-omci-mib-readback.md)
