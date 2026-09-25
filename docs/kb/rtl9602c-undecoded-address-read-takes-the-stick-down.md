# Reading an undecoded SoC address from the running stock firmware takes the whole unit down

Reading a physical address the SoC doesn't decode never completes: no bus
error, the CPU simply stops responding, and the unit drops off the network
until something eventually recovers it — recovery in this case took about
three minutes, and what actually causes that recovery is not established.

*Last verified: 2026-09-23*

---

## What

On this SoC, a memory read from an address nothing decodes never completes:
there is no bus error or exception, the CPU simply stops. Using the stock
firmware's own low-level register/memory read tool to probe an address that
turns out to be undecoded hangs that tool, and then the whole system: no
ping, no SSH, nothing — until the unit came back on its own about three
minutes later. That duration does not match any watchdog timeout known on
this hardware (those run in the tens of seconds, not minutes), so what
specifically causes this recovery (a watchdog, or something else) was
**not confirmed** in this instance — see the related open question in
[the watchdog-kicker note](rtl9601-watchdog-kicker.md#open-question-does-the-hardware-watchdog-recover-a-bus-stall).

## Why it matters

Reading registers from the stock firmware while it's running is a normal
and generally safe way to get reference values for known register blocks.
Probing a guessed or unconfirmed address is not safe: it costs a full
network outage and an unplanned reboot of whatever the unit was doing at
the time. Confirm an address is actually decoded — from a bootloader
environment dump, a known-working reference driver, or documented register
map — before reading it live.

## Evidence

Probing one specific undecoded address hung the read tool immediately, with
no response from the unit on the network for about three minutes, after
which it came back on its own with no other apparent effect. Reads of
several other, previously-confirmed addresses in the same session completed
normally with no issue.

## See also

- [NOR MMIO window](rtl9602c-nor-mmio-window-is-0x14000000.md)
- [Watchdog kicker](rtl9601-watchdog-kicker.md)
