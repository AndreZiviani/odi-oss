# A PON-side IP block must be explicitly powered on before switch-core init, or the bus stalls permanently

Before touching switch-core registers on this SoC, a specific IP-enable bit
must be set (bit 5 of a SoC control register at 0xB800063C), or the very
first switch-core write into the powered-down PON/PBO block stalls the
memory bus with no way back short of a power cycle.

*Last verified: 2026-09-16*

---

## What

Before any switch-core initialization can proceed on this SoC, a
specific PON-related IP block needs to be explicitly powered on: reading
SoC control register `0xB800063C`, setting bit 5 ("enable PON/PBO IP"),
and writing it back. Skipping this step and going straight into
switch-core initialization stalls the very first register write that
lands inside that still-powered-down block: the CPU sits with interrupts
off, an already-armed timer never fires, and (separately, see
[the watchdog-kicker note](rtl9601-watchdog-kicker.md#open-question-does-the-hardware-watchdog-recover-a-bus-stall))
the hardware watchdog did not recover it either — in testing, this
specific hang needed a power cycle, repeatedly, across multiple
multi-hour attempts. With the bit set first, the same initialization
sequence completes in well under a second.

On a unit running the stock firmware, this same register reads as already
enabled during normal operation, confirming the stock firmware performs
this same enable step itself as part of its own bring-up, before this
register value is otherwise expected to change.

## Why it matters

This is the difference between a kernel that reaches userland in a few
seconds and one that hangs unrecoverably — and it is easy to miss, because
nothing about it shows up by inspecting switch-core initialization code in
isolation: it's a board/IP-enable prerequisite that has to be satisfied
*before* switch-core init runs, not part of switch-core init itself. Any
from-scratch bring-up of this switch/GPON core on this specific board needs
this enable step before its own switch-core initialization; treating an
apparently-correct switch-core init sequence as sufcient without it will
still hang.

## Evidence

Debug output added around this specific initialization step showed the
sequence proceeding cleanly through every subsequent step once the enable
bit was set first, and stalling silently with no further output when it
was not. The affected register read the "disabled" value before the fix
and the "enabled" value afterward, matching what a live read of the same
register on the stock firmware shows during normal operation.

## See also

- [Watchdog kicker](rtl9601-watchdog-kicker.md)
- [GPON LOS needs RX_SD_POR_SEL](rtl9602c-gpon-los-needs-rx-sd-por-sel.md)
- [Chip identity](rtl9602c-chip-identity.md)
