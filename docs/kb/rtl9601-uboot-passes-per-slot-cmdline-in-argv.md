# U-Boot passes a per-slot boot command line as argv; a replacement kernel must read it and let it win

U-Boot hands the kernel a slot-specific command line (which root partition,
which flash layout) as boot arguments rather than baking it into the
kernel; a kernel that ignores this can silently boot the wrong slot's root
filesystem.

*Last verified: 2026-09-23*

---

## What

U-Boot's `bootm` passes the boot command line as the standard MIPS
boot-argument registers, built per active slot: booting slot 1 produces a
line naming slot 1's root device and a flash-partition layout marked
slot-aware (slot 1's kernel and rootfs partitions read-only, slot 0's
writable, or vice versa). A kernel that never reads this and instead relies
solely on its own compiled-in default command line boots using slot 0's
values regardless of which slot U-Boot actually selected. A current
mainline kernel, once configured to accept a bootloader-supplied command
line, by default *appends* its own built-in line after the bootloader's —
and the last repeated setting wins, so an unlucky ordering can silently
re-select the wrong slot even when the bootloader line was read correctly.

## Why it matters

A trial boot of slot 1 can silently mount slot 0's root filesystem instead
— or nothing at all. The failure looks exactly like an ordinary "cannot
open root device" panic, with nothing pointing at the actual cause.

## Fix

Read the bootloader-supplied command line as early as possible during
kernel boot, and configure the kernel so its own built-in command line acts
only as a fallback, applied *before* the bootloader's rather than after —
so the bootloader's per-slot values always take precedence when present.
Separately, make sure the kernel is built with in-kernel support for the
flash block-device major number the partition parser needs; without it,
the partition list comes back empty with a low-level error rather than a
clear message.

## Evidence

One trial booted with only the kernel's own built-in command line and
panicked. A later trial's boot log showed U-Boot's slot-1 line followed by
the kernel's own built-in slot-0 line, and the slot-0 partitions were used
— confirming the append-and-last-wins behaviour. Reordering so the
bootloader's line is authoritative fixed it: the correct slot's kernel and
root filesystem partitions were found and mounted.

## See also

- [Trial boot](rtl9601-uboot-trial-boot.md)
- [MTD map name rewrite](rtl9601-mtd-map-name-rewrite.md)
