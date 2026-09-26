# The host-side SerDes mode lives in one register, and "Fiber 1G" is a write sequence, not a single value

The host-side (LAN) SerDes mode is set by a specific 5-bit field in one
register, but getting to a given mode (such as Fiber 1G) is a whole ordered
sequence of register writes across several analog-block registers, not a
single value written once.

*Last verified: 2026-09-21*

---

## What

A working unit reports its host-side SerDes mode as "Fiber 1G" through the
stock firmware's own status interface. That name corresponds to an index
into a small fixed table of possible modes (1000Base-T PHY, 1000Base-FX,
SGMII in PHY or MAC mode, HiSGMII in PHY or MAC mode, 2500Base-X, and a
fixed-rate SGMII variant). The hardware does not simply hold that index
value anywhere read-only; reaching a given mode is an ordered sequence of
register writes, recovered by observing exactly what the stock firmware
writes during its own boot:

- Set one control bit in the main SerDes control register to park the mode
  first.
- Park the mode field to its "off" encoding and clear one other status
  bit in the same register.
- Clear one bit in a nearby "fiber" configuration register.
- Clear a 2-bit field in a second nearby "fiber extension" register.
- Write a specific sequence of values into six SerDes analog-block
  registers (all in the same small address range).
- Finally, in the main SerDes control register: clear the parking bit and
  the other status bit, and set the 5-bit mode field to the value for the
  desired mode (4 for Fiber 1G).
- Set one bit in an unrelated "GPHY patch done" register.

The 5-bit mode field's known values: an all-ones "off/parked" encoding,
4 = Fiber 1G, two SGMII encodings (with a companion bit for one of them),
one HiSGMII encoding shared by two related sub-modes, and one 2500Base-X
encoding. Reading this register block back on a working unit in Fiber 1G
mode, with two different tools, all registers involved hold exactly the
values the sequence above would produce.

## A register that looks like the mode register is not

An unrelated nearby register (offset `0x1d0`) reads a small value (`8`) on a
working unit and has a 5-bit field in some published register descriptions
that looks like it could be a mode selector. It is not the host-side mode
register described above — the host-side SerDes code never touches it.
That register is actually the *PON-side* SerDes mode selector, written
separately as part of PON initialization, and unrelated to the host link.
Comparing the wrong register when diagnosing a host-link SerDes problem
will find nothing wrong there and miss the real one.

## Why it matters

The host-side SerDes is the electrical link between the SoC and the rest of
the board — the only path to the on-board host interface — so getting this
sequence right (and in the right order — the analog-block writes between
the "park" step and the final mode selection are a single procedure, not a
set of independent, reorderable settings) is required for the on-board host
port to work at all. Whatever state the bootloader leaves the SerDes in is
what a kernel that never performs this sequence itself will be stuck with.

**Our own from-source firmware performs this same sequence during its own
network bring-up**, on every boot, and independently ends up at the same
result the stock firmware reaches — confirming the sequence is correct and
complete, not merely "good enough to link". See
`kernel/extra/drivers/net/ethernet/odi/` in this repository for that
driver.

This mode is **not persistent across a reset**: none of the registers
above survive a power cycle or reboot, so a userland component that always
re-applies the known-working sequence early in every boot (rather than
relying on a value stored in configuration) cannot end up bricking the host
link — a wrong value written at runtime only lasts until the next reset.
This matters because at least one community-known firmware issue for a
related product in this SoC family has been a boot-time SerDes mode
applied from stored configuration with no fallback if that stored value is
wrong; always re-deriving the mode at boot rather than trusting a stored
value avoids that class of problem entirely.

The non-Fiber-1G encodings above (the two SGMII variants, the two HiSGMII
sub-modes, and 2500Base-X) are recorded from the same register map but have
not been exercised end to end — there was no matching host-side link
partner available to validate them against.

## See also

- [Chip identity](rtl9602c-chip-identity.md)
- [LAN SDS mode key](rtl9601-lan-sds-mode-key.md)
