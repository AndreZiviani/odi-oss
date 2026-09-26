# Two DRAM pages survive a watchdog reset and can carry a kernel log across it

This device has no serial console, but two specific DRAM pages survive a
watchdog reset (though not a full power cycle): mirroring the kernel log
into them lets the next boot read the previous, failed kernel's last words.

*Last verified: 2026-09-25*

---

## What

On this board's 32 MB RAM layout, two small windows are left out of the
memory Linux manages at all, near the top of RAM. The very last page of each
of those windows (a 4 KB-aligned physical address) is touched by nothing:
not either kernel, not U-Boot (which relocates itself to a different
address), not the boot-time preloader after a warm reset, and not the
PON-side memory-mapped hardware block whose DRAM windows sit nearby (that
block's windows end just short of these pages, in a small unused gap).
Written through the CPU's uncached address window (so a reset cannot lose
anything already flushed to DRAM), both pages survive `reboot` and a
hardware watchdog reset byte-for-byte. They do **not** survive a full power
cycle: something in the boot path fills DRAM with a fixed pattern on a cold
start.

The console this gives: mirror every kernel log line, as it is printed,
into these two pages — one holding a small header (a magic value and a
running byte count) plus the start of the log, the other a ring buffer of
the most recent output. A page that still shows only its pre-boot tag after
a reset means the handover to that stage of boot never happened at all.
After a revert, boot back into the previous (working) image and read both
pages with a small standalone tool that reads physical memory directly.

## Why it matters

This device has no serial console. Without this technique, a failed trial
boot is one bit of information: it reverted, with the watchdog kicking in
and no other detail. With it, failed trial boots have yielded the exact
panic message from an early memory-allocator failure, the exact point a
hardware probe returned an unexpected value, and the exact reset line from
a deadline timer — each otherwise invisible on this hardware.

Practical rules: tag both pages with a known pattern before a trial so
stale content from a previous run cannot be mistaken for a fresh log: and
on the currently-running image, check free memory before pushing a
log-reading tool into the on-device temporary filesystem — pushing a tool
larger than available free memory has rebooted a unit outright in testing.

## On the current kernel, this is a small dedicated console driver

On the from-source replacement kernel (current mainline-based build), the
same technique is implemented as a small console driver that registers as
early as possible in boot and hands over to the normal console once one
becomes available — so these two pages specifically capture the log of a
kernel that never got far enough to have a working console at all, which is
exactly the failure mode where a log is most valuable. An earlier
(2.6.30-era) build phase of this project used a different implementation —
hooking the kernel's own log-output routine directly rather than a
dedicated console driver — with the same DRAM pages and the same
survive-a-reset property; that earlier mechanism is now build history, but
the two-pages-survive-a-reset hardware fact holds for both.

Once both slots can run this image, the boot after a failed trial is ours
too, and its own console would overwrite the failed log at startup. So the
driver first copies both pages into a static buffer, before it writes
anything, and keeps that copy readable for the whole boot as
`/proc/odi_ramlog_prev` (decoded) and `/proc/odi_ramlog_prev_raw` (the
8192 raw bytes, page A then page B), both root only. It reaches one boot
back. The last 64 bytes of page A hold a small per-boot metadata block (a
boot counter that survives warm resets, the slot from the last `root=`,
the image build id, and the early-crumb pair of the previous boot), so the
copy says which boot and which image it came from; `docs/KERNEL.md` has
the layout.

## Telling a timed event from a stalled instruction

The log captured in the ramlog trails the actual point of failure by
roughly one line, and the exact last line printed can shift between
otherwise-identical trials depending on unrelated timing (adding more print
statements shifts it further). Recording a raw, monotonically increasing
tick counter alongside each log line resolves this ambiguity: if the last
line printed changes between trials but the tick count at the moment of
death does not, the death is a **timed event** (a watchdog or deadline
firing, for example), not something wrong with the code at that particular
last line. Keeping a small ring of (tag, step, tick-count) triples written
directly into the reserved header space — rather than relying on the
regular log ring alone — has caught this distinction in practice, including
one case where the printed log alone would have pointed at the wrong line
entirely.

## See also

- [Trial boot](rtl9601-uboot-trial-boot.md)
- [Watchdog kicker](rtl9601-watchdog-kicker.md)
- [PBO IP enable before switch init](rtl9602c-pbo-ip-enable-before-switch-init.md)
