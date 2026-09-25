# DDM optical readings follow plain SFF-8472 scaling

The stock firmware's optical diagnostics (DDM) readout uses the standard
SFF-8472 conversions, nothing vendor-specific — useful if you are decoding the
same raw DDMI block yourself.

*Last verified: 2026-09-12*

---

## What

The stock firmware reads a 24-byte DDMI block and formats it for display.
Every conversion is the standard SFF-8472 scaling; nothing about the math is
proprietary or specific to this device.

## Why it matters

A reimplementation does not need to guess at scaling constants or treat this
as reverse-engineering risk: the formulas are the specification's own, and
three of the four values are exact in integer arithmetic — only the optical
power reading (dBm) needs a logarithm.

## The conversions

All values are read big-endian from the first two bytes of the relevant
24-byte block.

| field | conversion | typical format |
|---|---|---|
| temperature | signed; `hi + lo × 0.00390625` (1/256 °C) | `"%f C"` |
| voltage | `v / 10000.0` | `"%f V"` |
| bias current | `(v + v) / 1000.0`, i.e. 2 µA units | `"%f mA"` |
| tx power, rx power | `10.0 × log10(v / 10000.0)` | `"%f  dBm"` (two spaces) |

The stock firmware's own display prints six decimal places (`%f`) rather than
the two you might expect for a human-readable reading, and its dBm line has
two spaces before the unit — worth matching if you want byte-identical output
next to the stock CLI.

**Temperature sign handling has a one-off quirk.** The sign check uses a
threshold of `0x81` rather than the more obvious `0x80`, so a raw high byte of
exactly `0x80` is treated as positive. This has no practical effect, since
that value corresponds to roughly -128 °C — outside any value the sensor will
actually report — but it is worth knowing if you are trying to match the
stock firmware's output exactly rather than just "correct" SFF-8472 rounding.

## See also

- [RTL9601 register access is getsockopt on a raw socket](rtl9601-register-access-via-sockopt.md)
- [Fork the CLI rather than link the vendor library](rtl9601-cli-over-vendor-lib.md)
