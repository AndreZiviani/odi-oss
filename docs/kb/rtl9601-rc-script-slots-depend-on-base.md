# The free boot-script slot on this device is a property of the image, not the platform

The stock boot process runs every numbered boot-hook script that exists in
sequence; which slot number is actually free for a new hook to use depends
entirely on which firmware base you started from, not on the SoC or board.

*Last verified: 2026-09-16*

---

## What

The stock boot sequence runs every existing numbered boot-hook script in
order, as a separate shell process each. **In the stock (OEM) vendor image**,
the highest one in use leaves a specific slot free, so dropping in a new
hook at that slot needs no existing file patched.

**This does not hold for every base you might start from.** For example, the
well-known community "Anime4000" firmware-modification base for this device
family already uses that same slot for its own purposes, so installing a new
hook there is a *replacement*, not an addition — whatever that base's script
did at that slot stops happening.

## Why it matters

A replacement is often exactly what you want (it's a normal way to disable
another base's own boot-time behaviour), but it has to be a deliberate
choice: a build check that verifies "did I only add files" without
comparing contents can wrongly refuse a legitimate build, and a check that
allows overwriting without saying so can silently discard the base's boot
hook without anyone noticing.

Check whether your chosen slot is already used by the base you're building
from before assuming it's free. The general, reusable rule: the boot
sequence runs whatever numbered scripts exist, so "the next free slot" is a
property of the specific image in front of you, not of the platform.

## See also

- [The stock userland's networking gaps](rtl9601-userland.md)
- [The stock web server only runs because an earlier boot step failed](rtl9601-boa-launched-from-startup-failure-path.md)
