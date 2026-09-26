# The RTL9602C register table comes from the stock firmware, not from a published one

Published register tables for this SoC family are the wrong revision for
the chip on this stick. Ours is read out of the stock firmware binary,
the only source that matches the silicon.

*Last verified: 2026-09-24*

---

## What

Register tables published for this SoC family are not one stable table.
One of them lists 1357 registers; the table the stock `librtk.so` on the
device carries has 1530. About 1200 of the smaller table's entries agree
with the binary's exactly; the rest drift -- same general shape, a
different address, width or bit position.

So this project does not use a published table at all.
`src/diag/tools/regmap-extract.py` reads the register table straight out
of the stock `librtk.so`: every register's address, width, array and port
range, and every field's bit position, width and id. The binary carries no
names, so every name is ours (`src/diag/tools/regnames.txt`, or a default
built from the address and bit range), and the kernel register headers use
the same names.

## Why it matters

A mismatched table does not fail loudly. An access built from an entry
that has drifted resolves to some other, plausible-looking register or
field and reads or writes the wrong thing, with no error. About one
register in ten misresolved this way when a published table was checked
against behaviour on the device.

A couple of spot checks do not catch it: well-known registers such as the
chip identity word read back correctly under a right and a wrong table
alike, purely because those low-numbered entries happen to agree. Check
rows that sit where the revisions differ (rows 1 and 1225) instead. While
`diag` carried a generated copy of the table, a test pinned those two rows;
it carries none now, so a listing read out of `librtk.so` for
`tools/regtrace` needs the same check by hand.

One related pitfall: some registers on this family return a meaningful
value only after an explicit unlock write (a bit pattern in an otherwise
unused nibble). An all-zero read-back is not by itself a sign that the
table entry is wrong.

## See also

- [Chip identity](rtl9602c-chip-identity.md)
- [Register access via sockopt](rtl9601-register-access-via-sockopt.md)
