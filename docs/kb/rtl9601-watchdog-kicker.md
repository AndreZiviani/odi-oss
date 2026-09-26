# A replacement kernel must kick the hardware watchdog at arm, not one interval later

U-Boot arms a hardware watchdog (~41.6-46s) before every trial boot; a
replacement kernel must start kicking it from init, and its very first kick
must land immediately, not after a full interval, or a slow boot loses the
race.

*Last verified: 2026-09-23*

---

## What

U-Boot arms the hardware watchdog just before handing over to the kernel on
every trial boot (see [trial-boot](rtl9601-uboot-trial-boot.md)). Measured
timeout: about 41.6 seconds under one kernel, about 46 seconds under
another, with no kicks. If nothing kicks it before that timeout, the board
resets and the one-shot trial mechanism reverts to the last known-good
image — which is the correct safety behaviour, but it means a kernel that
would otherwise have booted fine can look indistinguishable from one that
crashed, costing a trial and teaching nothing.

On the stock 2.6.30-era firmware the kernel does not keep the watchdog
alive by itself on this board: the kicking is driven from the stock boot
scripts — so a from-scratch replacement kernel that assumes "the stock
kernel kicks it, so mine should too" silently ends up with a kernel that
never kicks. On our own
from-source image, the equivalent behaviour is implemented as a small
always-on kernel component that starts kicking immediately at init and
exposes its status at `/proc/odi_wdt` (`userland_ok`, `watchdog_flag`,
a roughly 5-second kick interval, and a roughly 120-second higher-level
deadline described below).

## The first kick has to land at arm, not after the first interval

U-Boot's own handover — including decompressing the kernel image — eats
most of the ~41-46 second window before the kernel code that would start
kicking ever runs, leaving only a few seconds of margin. A kicker whose
first kick comes one full interval after its own thread starts (rather than
immediately, at the moment it takes over) loses the race under any load
that delays scheduling that thread even slightly. The symptom is a board
that resets silently at almost exactly the same very-early boot tick count
every single trial, whatever the last log line printed happens to be —
which looks exactly like a hang at that log line, and is not. Kicking once,
immediately, at the point the kicker takes over — rather than waiting for
its first scheduled interval — closes this gap; moving the kick interval
back afterwards changes nothing.

## A software deadline on top of the low-level kicker

The low-level kicker alone only proves the kernel is scheduling threads; it
says nothing about whether the system has actually come up. A second,
higher-level deadline is worth adding on top: a timer that forces a reset by
writing directly to the watchdog control register itself at a fixed number
of seconds of uptime (about 120s in our own image) unless userland has
separately confirmed the system is reachable. Two things worth getting
right when building one:

- **Confirm reachability, not just interface state.** A network interface
  simply holding an IP address is not the same as being reachable — confirm
  it with a positive response (an ARP reply, for example) from another host
  on the same segment.
- **Watch for uptime accounting starting from a large offset.** Depending on
  how the kernel's internal tick counter is initialised, a raw tick count
  read very early in boot can appear to be an enormous number rather than a
  small one close to zero — use a proper "time since boot" value, not a raw
  internal counter, when computing a deadline.

This deadline is confirmed working: it reset the board on cue at the
configured 120-second mark in testing when nothing answered, and stood down
correctly once the network came up within the window.

## An always-on kicker thread shows up as load average 1.00, harmlessly

A kicker implemented as a thread that sleeps uninterruptibly between kicks
counts as a "busy" (D-state-like) task for load-average purposes, so an
otherwise completely idle system with this kicker running can show a load
average of 1.00 at 100% idle CPU. This is harmless, but it means load
average alone cannot be trusted to indicate "something is stuck" on this
platform without also checking CPU idle time — and it can mask an
unrelated genuinely-stuck process sharing the same load-average signal
(making the sleep interruptible removes this artifact without changing the
kick timing, since a kernel-internal thread does not receive external
signals anyway).

## Open question: does the hardware watchdog recover a bus stall?

Separately from the above (which covers a kernel that hangs cleanly or
never starts), whether the hardware watchdog alone can recover the board
from a full bus stall — for example a stalled register write into a
powered-down IP block, see
[the PBO IP-enable note](rtl9602c-pbo-ip-enable-before-switch-init.md) — is
**not established**. In testing, a hang of that specific kind needed a
power cycle; the watchdog did not appear to fire. Do not assume the
hardware watchdog is a universal recovery mechanism for every kind of hang;
treat "kernel never started kicking" and "bus completely stalled" as two
different failure modes with two different (and not equally reliable)
recovery paths.

## See also

- [Trial boot](rtl9601-uboot-trial-boot.md)
- [DRAM ramlog console](rtl9601-dram-ramlog-console.md)
- [PBO IP enable before switch init](rtl9602c-pbo-ip-enable-before-switch-init.md)
