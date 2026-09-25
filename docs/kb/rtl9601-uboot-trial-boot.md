# U-Boot has a one-shot trial-boot slot; use `sw_tryactive`, not `sw_commit`

`sw_tryactive` boots a firmware slot exactly once under the hardware
watchdog and reverts automatically if it never comes up; a successful trial
can also self-commit, so check `sw_commit` rather than assume it did not.

*Last verified: 2026-09-23*

---

## What

Setting `sw_tryactive` to `0` or `1` boots that firmware partition **exactly
once, with the hardware watchdog armed**. If the image does not come up, the
next boot returns to the last known-good partition with no intervention at
all.

    nv setenv sw_tryactive <n>    # 0 or 1. Never 2.
    reboot
    # once satisfied it works:
    nv setenv sw_commit <n>       # now it survives reboots

The bootloader env spells out the mechanism:

    bootcmd           = if sw_tryactive == 2; then boot_by_commit; else boot_by_tryactive; fi
    boot_by_commit    = if sw_commit == 0; then set_act0; b0; else set_act1; b1; fi
    boot_by_tryactive = if itest.s ${sw_tryactive} == 0;
      then setenv sw_tryactive 2;setenv sw_active 0;saveenv;run en_wdt;run b0;
      else setenv sw_tryactive 2;setenv sw_active 1;saveenv;run en_wdt;run b1;fi

`boot_by_tryactive` rewrites `sw_tryactive` back to `2` and runs `saveenv`
**before** arming the watchdog and handing over — confirmed against a full,
untruncated dump of the saved environment on more than one unit, identical in
every copy. The trial is therefore one-shot by construction and cannot loop:
a panicking or hanging image gets reset by the watchdog once, and `bootcmd`
then takes the `boot_by_commit` path straight back to the committed
partition. U-Boot itself is what arms the watchdog for the trial
(`en_wdt`, see below); nothing the replacement kernel does is what starts the
clock, only what has to answer it in time (see
[the watchdog-kicker note](rtl9601-watchdog-kicker.md)).

A trial that boots fine is *supposed* to be only a trial — U-Boot never
writes `sw_commit` itself, so on the bootloader's terms the next reboot
returns to whatever is already committed. **Do not rely on that.** On one
of our units, `sw_commit` changed from `1` to `0` on its own about 80
seconds after a trial image booted, with nothing done by hand in between —
the running firmware's own management stack (the GPON/OMCI software-image
activate-and-commit flow, driven by the network operator) had written it.
On another unit under the same procedure, `sw_commit` stayed untouched for
several minutes. This is likely operator-driven rather than something the
bootloader does on its own, though the mechanism has only been observed
this one way; always read `sw_commit` back after a trial rather than
assuming it is still a trial.

## Other things this environment settles

**The revert target is `sw_commit`, not `sw_active`.** `bootcmd` sends any
`sw_tryactive` value of `2` to `boot_by_commit`, which reads `sw_commit`
alone. So the slot a failed trial returns to is whatever `sw_commit` names,
and confirming that slot actually carries known-good firmware is a separate
precondition from simply not overwriting the running slot. `sw_active` is
U-Boot's own record of what it last booted, written by both paths — there is
never a need to set it by hand.

**The `sw_tryactive` test is not a strict match.** The tryactive branch only
tests `== 0`; with `bootcmd` routing `2` away, any value that is neither `0`
nor `2` — a typo, a corrupted byte — boots **slot 1** rather than erroring.
Set it to `0` or `1` and nothing else.

**The running/committed pair is marked read-only in the kernel command
line** — the partition table passed to the kernel flags the active kernel
and rootfs partitions read-only and the other pair writable, mirrored for
each slot. So a flashing tool that refuses to overwrite the running slot is
backed by a second, independent line of defence at the MTD layer.

A plain reboot does **not** move `sw_commit`: verified by rebooting a unit
holding `sw_commit=1` and reading `1` back afterwards. Whatever moves it is
specific to the trial-and-activation path (or to what the network operator's
management stack does around it), not general drift.

## Why it matters

The obvious-looking procedure — `nv setenv sw_commit <n>; reboot` — makes an
unproven image permanent *before it has booted even once*, throwing away a
safety net the bootloader provides for free. The stock firmware-update tool
writes both partitions' image contents but never touches a boot-select
variable itself, so getting the boot-select step right is entirely on
whoever flashes the image.

This is what makes flashing an experimental image cheap rather than a risk
of losing the unit, on a device whose only fallback path otherwise is a
hardware UART behind solder pads.

The revert covers an image that fails to **boot**. An image that boots and
is simply unreachable does not revert on its own — see
[the watchdog-kicker note](rtl9601-watchdog-kicker.md) for the software
deadline that closes that gap, and
[the DRAM ramlog note](rtl9601-dram-ramlog-console.md) for how to read what
such an image did before it died.

**Do not build an automatic health-check rollback on top of this
mechanism.** A unit with the fibre unplugged, or not yet provisioned for a
line, is indistinguishable from a broken image when judged from inside, so
a naive health check would roll back perfectly good builds. The trial slot
only protects against an image that fails to **boot**, which is the part
that can be judged locally and without ambiguity.

## Evidence

Read from a running unit with `nv getenv` (no argument dumps the whole
environment; note the usage string says `getenv`, so `nv printenv` silently
returns only usage text — a backup captured that instead of the real
environment and looked complete).

Steady state varies by unit: which slot carries the committed (currently
trusted) image is a property of how that unit was set up, not a fixed
convention — always read `sw_commit` rather than assume a slot number.
`sw_version0`/`sw_version1` name the two images.

`en_wdt` is `mw b8003268 e7c00000`, and the watchdog's own status interface
on a running system confirms it is armed with a multi-second kick interval —
the watchdog armed by the bootloader is the same one the kernel later takes
over kicking.

## See also

- [U-Boot environment format](rtl9601-uboot-env.md)
- [Two firmware partitions](rtl9601-dual-firmware-partitions.md)
- [Config store](rtl9601-config-store.md)
- [Watchdog kicker](rtl9601-watchdog-kicker.md)
