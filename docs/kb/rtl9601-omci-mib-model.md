# The stock firmware's OMCI information model is a set of independently loadable plugins

The stock firmware holds its entire OMCI managed-information model as a set of independently loadable modules — on the order of 80 of them, covering several hundred attributes in total — and three fields per attribute decide how the firmware treats it.

*Last verified: 2026-09-12*

---

## What

The stock firmware's OMCI information model is organized as a set of independently loadable plugin modules, each implementing one managed-entity class. On this firmware base there are on the order of 80 such modules, together covering several hundred attributes, with names, data types, widths, access flags, and the set of OMCI actions each managed entity accepts.

## Three fields that decide behavior

- **A data-type/width field** selects how wide an attribute's value is. Numeric types are straightforward; the two variable-width types are strings and raw octet arrays. **For a string attribute, the stored size and the on-the-wire size differ by one** — the wire format does not carry a stored attribute's terminating byte, while the local storage does. Using the stored size where the wire size is needed shifts every field that follows the first string attribute in a record.
- **An access-flags field** encodes, among other things, whether an attribute is settable at creation. A `create` operation for a managed entity carries, in order and with no gaps, exactly the attributes marked "settable at creation" — nothing else. An attribute that is read-only (for example, a UNI's own traffic counter) is correctly absent from that list; treating a `create` payload as "every attribute of the entity, in order" shifts every field after the first such gap.
- **A "who creates this" field** partitions the whole model in two: some managed entities are created autonomously by the ONU itself (these accept only `set` and `get`, never `create`); others are provisioned by the OLT. Checked against a live read of the whole MIB, every entity table lands cleanly on one side of that partition with nothing left over.

That partition is exactly what a MIB upload has to report correctly. After a MIB reset, the ONU's MIB is not empty — it holds whatever the ONU built for itself autonomously, and that is how the OLT learns what hardware and capabilities it has. An ONU that reports nothing there gets the whole identification cycle indefinitely, never fully provisioned.

## Why it matters

Any reimplementation of the OMCI stack needs to get all three of these right to interoperate: getting the string wire-size off-by-one wrong desyncs message parsing past the first string attribute; getting the create-attribute-order wrong desyncs message parsing past the first read-only attribute; and getting the autonomous/provisioned partition wrong stalls provisioning at the OLT entirely (see the [related note](rtl9601-omci-olt-provisions-only-against-vendor-attribute-values.md) on what happens when an ONU reports default values for its autonomous entities).

## Evidence

The complete model — every managed entity, its attributes, their types, widths, access flags, and creation ownership — can be extracted directly and mechanically from the firmware image, cross-checked internally (each entity's declared row size against the sum of its attribute widths) and spot-checked against the public G.988 standard for several well-known entity classes (an ONU-G-equivalent accepting `get` plus software-download actions, entities accepting only `set`/`get`, and the extended-VLAN entity additionally accepting a table-attribute walk).

## See also

- [OMCI MIB readback](rtl9601-omci-mib-readback.md)
- [OLT provisions only against vendor-observed attribute values](rtl9601-omci-olt-provisions-only-against-vendor-attribute-values.md)
