# A file missing from the firmware image may be a symlink into RAM

The root filesystem is read-only; several paths that look absent from the
image are actually symlinks into a writable RAM-backed filesystem mounted at
`/var` — "not in the image" is not the same as "not on the running system".

*Last verified: 2026-09-16*

---

## What

The root filesystem is read-only (squashfs); `/var` is a separate,
writable, RAM-backed filesystem (ramfs). Several paths that look missing
from the image actually point into it:

- **`/etc/config` is a symlink to a persistent config partition** (backed by
  flash, not RAM — see [the config-store note](rtl9601-config-store.md)), so
  flag files there toggle behaviour without rebuilding an image, and a
  firmware update does not touch it: serial number, LOID and OMCI settings
  survive a reflash.
- **`/etc/passwd` is a symlink into `/var`**, generated at boot. It is absent
  from the image and present at runtime.
- **`/tmp` is a symlink into `/var/tmp`**, which is ramfs.

The general lesson: **a file absent from the read-only image may be a
symlink into `/var`.** `/etc/passwd`, `/etc/config` and the pseudo-terminal
device directory all looked missing from a plain image listing and were not
actually missing at runtime. Telling "absent from the image" apart from
"absent from the running system" costs one look at the symlink target.

## Why it matters

Because `/tmp` is a symlink, **listing `/tmp` (without a trailing slash)
prints the symlink itself, not its contents.** A check that lists `/tmp` and
greps for leftovers can silently pass over whatever is actually in there —
list `/tmp/` (with the trailing slash) instead.

`/var` being RAM-backed has a hard edge: pushing something into it larger
than roughly half of free memory can reboot the stock firmware outright.
Drop caches and check free memory before writing a large file there.

## Evidence

The RAM-fill reboot: pushing about 1.5 MB into roughly 1.1 MB of free memory
reliably rebooted a stock stick.

## See also

- [Config is jffs2 files under a persistent partition](rtl9601-config-store.md) — what lives behind `/etc/config`
- [The vendor kernel has no devpts, only legacy BSD ptys](rtl9601-legacy-bsd-ptys-only.md)
- [The stock userland's networking gaps](rtl9601-userland.md)
