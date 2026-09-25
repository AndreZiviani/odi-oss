# A modern busybox telnetd cannot serve even one session without Unix98 PTY support

On the stock 2.6.30-era kernel line, `telnetd`'s fallback path never actually
worked: without Unix98 pseudo-terminal support in the kernel config, the
whole daemon dies on every inbound connection.

*Last verified: 2026-09-15*

---

## What

A modern busybox build (with device-filesystem PTY support enabled) opens a
pseudo-terminal purely through the modern Unix98 API (`/dev/ptmx`) — the
older BSD-style `/dev/ptyXX` scan is compiled out entirely in that
configuration.

If the kernel it's running on does not have Unix98 PTY support enabled, that
open call fails — and `telnetd`'s session setup has no failure path for it,
so **the whole daemon dies on every inbound connection**. The init system
respawns it in time for the next connection attempt to kill it again.

## Why it matters

This is silent, and it's asymmetric. **A modern dropbear SSH daemon built
against the older BSD PTY API is unaffected** — it finds the legacy nodes
just fine. So SSH works perfectly while telnet — typically documented as
*the fallback for when SSH doesn't come up* — has never actually worked at
all on the stock 2.6.30-era kernel. Nobody exercises a fallback path until
the day they actually need it, which is exactly the wrong day to discover
this.

Unix98 PTY support is normally on by default upstream, gated behind an
"embedded systems" config prompt; vendors commonly turn it off to save
memory. Turning it back on also brings in the pseudo-terminal filesystem
gated on the same option, so a proper `/dev/pts` tree comes with it. Three
things are needed together: the kernel option itself, a `/dev/ptmx` device
node, and the pseudo-terminal filesystem actually mounted at `/dev/pts`.

**A from-source replacement image using a current mainline kernel does not
have this problem** — Unix98 PTY support and `devpts` are standard there.
This repository's image ships no telnetd at all (busybox is built without
it); ssh is its only network login.

## The neighbouring trap

A related kernel option caps how many legacy BSD-style PTY pairs exist; the
upstream default is generous but vendor configs commonly cut it down to a
tiny number. With an SSH daemon relying on the legacy PTY path, that count
becomes a hard ceiling on concurrent SSH sessions — and one leaked/stuck
session can cut the effective ceiling in half, often during exactly the kind
of debugging session where a second shell matters most.

## Evidence

Verified by executing the fix under emulation against the real root
filesystem on the stock 2.6.30-era kernel: after enabling Unix98 PTY
support, sixteen pseudo-terminal master devices opened successfully and
`devpts` showed up mounted read-write. Before the change, the shipped
busybox build's strings showed only the Unix98 path and a "can't find free
pty" failure string, with no legacy PTY device names present at all.

## See also

- [The vendor kernel has no devpts, only legacy BSD ptys](rtl9601-legacy-bsd-ptys-only.md)
- [A modern busybox `mdev -s` needs a newer kernel](busybox-mdev-s-needs-devname-uevent.md)
