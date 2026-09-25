# The die and the marketed part number are different names for the same chip

The ODI DFP-34X-2C2's SoC answers to two different names depending on what
you ask it, and both are correct.

*Last verified: 2026-09-16*

---

## What

On this device, a chip-identity query through the stock firmware's `diag`
tool returns two different names:

```
diag debug get version detail
Chip probe result : RTL9602C (ID = 0x96030002)
RTL9601D-VA3
```

Both are correct and answer different questions. `RTL9602C` is the silicon
die identity (chip id `0x96030002`); `RTL9601D-VA3` is the marketed part
number, one of several bonded variants of the same die. Which name matters
depends on what you're looking up: driver and register-map work should key
off the die (`RTL9602C`); ordering and datasheet lookups should key off the
part number (`RTL9601D-VA3`). A superficially similar part name,
`RTL9601C`, is a *different* bonded variant of the same die family and does
not describe this device.

## Why it matters: a chip-id check that reads backwards

The stock firmware's own `diag` CLI has at least one command whose internal
chip-identity check is inverted: it branches on "is the chip id equal to a
specific constant" to decide whether to apply a chip-specific adjustment,
and on this hardware the check answers the opposite of what its numeric
value suggests at a glance (the constant it compares against looks like it
names a *different, unsupported* chip, when it in fact names this one).
Observed effects on the stock CLI: an egress-bandwidth reporting command
applied its adjustment backwards, and a physical port was silently missing
from a "show all ports" listing. Anyone writing code that keys behavior off
this device's chip id constant should verify against the device's own
`debug get version detail` output rather than trusting an id value's
apparent meaning.

## See also

- [rtl9602c-register-table-from-stock-binary](rtl9602c-register-table-from-stock-binary.md)
