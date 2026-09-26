# Two firmware partitions, so flashing is reversible

An update always writes the inactive firmware slot, so the running image is
never overwritten; roll back with `sw_commit`, never touch `sw_active` by
hand, and trial a new image with `sw_tryactive`.

*Last verified: 2026-09-16*

---

## What

**Two firmware partitions.** An update always writes to the *inactive* one,
so the running firmware is never overwritten. Each kernel/rootfs slot pair
is a fixed, identically-sized region of flash.

## Why it matters

Flashing is reversible. Roll back by reading the current boot-select
variable, then setting **`sw_commit`** to the other value and rebooting —
`sw_active` is U-Boot's own bookkeeping and never needs setting by hand. To
boot a *new* image, use the one-shot trial slot `sw_tryactive` instead, so a
failure reverts itself; see [trial boot](rtl9601-uboot-trial-boot.md).

## Evidence

Confirmed directly on the device: firmware-update tooling writes only the
inactive slot, and the trial-boot mechanics are verified separately.

## See also

- [Trial boot](rtl9601-uboot-trial-boot.md)
- [U-Boot environment](rtl9601-uboot-env.md)
- [Userland overview](rtl9601-userland.md)
