# `/proc/cmdline` is not what U-Boot passed: the stock kernel renames the flash device

The stock kernel silently renames the MTD flash device before parsing
partitions from the boot command line, so `/proc/cmdline` on a running unit
shows a different device name than what U-Boot actually passed — and a
replacement kernel keyed on the wrong one finds no root filesystem.

*Last verified: 2026-09-15*

---

## What

U-Boot passes a partition table keyed to one MTD device name. `/proc/cmdline`
on the same running unit shows the *same* partition table keyed to a
**different** device name — every other byte identical: same sizes, same
flags, same aliases, same root device.

Both readings are real. The **stock kernel renames the flash device**
internally before its command-line partition parser ever sees it, because
the stock kernel's own flash driver registers under that different name.
The rewrite is not part of the openly-published kernel sources for this
platform family — it is specific to the exact kernel actually shipped on
this device, and is small enough (a needle-and-replacement string pair,
with a message announcing the rewrite) that its evidence survives intact in
the decompressed kernel image itself.

## Why it matters

Command-line-based partition parsing on Linux applies partitions only to a
flash device whose registered name matches the prefix given before the
colon in the partition table string. A replacement kernel that registers
its own flash device under the name shown in `/proc/cmdline` — rather than
the name U-Boot actually passed — matches nothing, and falls back to a
built-in default partition layout meant for a *different* board entirely.
The boot-time root device then names nothing that exists.

The failure is a kernel panic with no root filesystem found, and it looks
exactly like any other early-boot panic — nothing points at the actual
cause. On a device whose only recovery mechanism is a self-reverting trial
boot (see [trial boot](rtl9601-uboot-trial-boot.md)), this costs an entire
trial and teaches nothing about what actually went wrong.

Build a replacement kernel's own flash device registered under the name
U-Boot actually passes, not the name a *running stock kernel's*
`/proc/cmdline` happens to show.

## The general form

The standing rule already was "the stock kernel is evidence about the
stock kernel, not about what the bootloader does" — and that's correct.
The stronger version is that **`/proc/cmdline` is not reliable evidence of
what the bootloader passed either** — it is what the kernel decided to keep
after its own processing. The only fully reliable source of truth is the
bootloader's own saved environment, read directly rather than inferred from
a live kernel.

## Evidence

Confirmed against full, checksum-verified dumps of every flash partition on
more than one unit, cross-checked against `/proc/cmdline` on a live system.
Independently confirmed by execution: an emulated environment carrying the
same partition layout, using U-Boot's actual device name rather than the
kernel's rewritten one, found and mounted every expected partition.

## See also

- [U-Boot passes per-slot cmdline in argv](rtl9601-uboot-passes-per-slot-cmdline-in-argv.md)
- [U-Boot environment](rtl9601-uboot-env.md)
