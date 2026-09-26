# The stock CLI's `all` port keyword is a fixed device port class, not a live probe

The stock switch/optics CLI has seven logical ports, three of them
populated on this board — and its `all` port keyword resolves to a
different fixed subset of those seven depending on which command you run,
not to "every port that answers."

*Last verified: 2026-09-13*

---

## What

This SoC exposes **seven logical ports (0-6)**; this board populates three
of them (0 host SerDes, 2 PON, 3 CPU). The stock CLI's `all` keyword, used
in commands like `port get port all state` or `vlan get ext-pvid port all`,
does not mean "every port that responds" — it resolves to a fixed **port
class** read from a small on-device info structure (exposed through a
sockopt call, option `0x2f57`, a 656-byte buffer whose first word is the
chip id), and different commands pass a different class:

    class 6           every logical port (0-6)
    class 7           the CPU port alone (port 3)
    everything else    ports 0 and 2 only

That's why `vlan get ext-pvid port all` covers all seven ports while
`port get port all state` covers only two (0 and 2) — the two commands ask
for different classes, not a different live port set.

**So the CPU port is not "omitted by convention"** — it simply isn't in the
default class most commands use. A counter-style read that iterates ports
directly still finds it, because the CPU port does keep counters; it just
isn't part of the class the `all` keyword resolves to for most commands.

## Why it matters

Some commands — mostly plain register-table style reads — have no way to
refuse a port that the board doesn't actually populate; a naive loop over
one of those will happily report a value for a port that doesn't physically
exist on this board, with nothing indicating an error. Only a command that
resolves through the port-class mechanism above enforces the real,
board-specific port set.

If you're reimplementing this behaviour, prefer reading the port-class
structure directly over probing live ports as a substitute — probing is a
reasonable fallback only when that structure can't be read at all, and it's
what produced a wrong earlier theory here (that the vendor CLI deliberately
excludes the CPU port "by convention," when in fact it just isn't in the
default class).

## Evidence

Measured directly on this hardware:

    switch get small-pkt port all state
      vendor CLI: port 0, port 2
      (an early probe-based reimplementation incorrectly also reported port 1)

    bandwidth get egress port all
      vendor CLI: port 0, port 2
      (an early probe-based reimplementation reported port 0, port 3 — CPU port
       skipped in one path, PON port missing in another)

Reading the port-class structure directly reproduces the vendor CLI's
output exactly in both cases.

## See also

- [Only the switch MIB counters show whether the ONU is forwarding](rtl9601-forwarding-visibility.md)
