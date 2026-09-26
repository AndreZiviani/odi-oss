# The stock firmware cannot keep time and has no log but the kernel ring buffer

On the stock (OEM) firmware there is no real-time clock, no NTP client and no
syslog — the kernel's own ring buffer is the only log there is, and reading
it the obvious way destroys it.

*Last verified: 2026-09-16*

---

## What

Three absences on the stock firmware shape any tooling built against it:

- **It cannot keep time and cannot learn it.** There is no real-time-clock
  device and no `hwclock`, and no working NTP client either — the
  configuration daemon has a slot for one in its process table, but the
  corresponding binary isn't actually shipped in the image. There is an OMCI
  managed entity that lets the OLT push SNTP settings, and the corresponding
  persistent config keys (timezone string, NTP interface) survive a reflash
  even though the clock they describe is never actually set on this image.

- **There is no syslog daemon and no `dmesg` command.** The kernel ring
  buffer is the only log that exists.

- **`ping` and `traceroute` exist**, but in bridge/SFU mode the stick can
  only reach its own management LAN and nothing past it — including no path
  to a public time server.

## Why it matters

In bridge mode the stick has no route to a time server at all, so the only
workable time source is a client that already knows the time (for example, a
browser posting its own clock to the device).

Reading the kernel log the obvious way is a trap: consuming the kernel
message device the naive way both **destroys** the log history and then
**blocks** waiting for more — a "show me the log" command that both wipes
and hangs. Reading it through the kernel's log-read syscall interface
instead reads it without clearing it.

**A from-source replacement image is not bound by any of this** — it can add
a real NTP client, `dmesg`, and a syslog daemon. Treat this note as a
description of what the stock firmware lacks, not a hardware limitation.

## See also

- [The stock userland's networking gaps](rtl9601-userland.md)
- [Flash part identity, and reading the boot line before it rotates out](rtl9601-nor-flash-part.md)
- [MIPS o32 ABI traps](mips-o32-syscall-abi-traps.md)
