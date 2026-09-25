# Root filesystem

The skeleton is **ours**, not the vendor's. It is deliberately not a copy of
their `rc0`..`rc63` chain: most of what that starts does not exist in this
image, and one of the things it does start (`pondetect`) rewrites `PON_MODE`
and reboots after roughly 24 seconds of light without sync.

    skeleton/etc/inittab          three entries; order matters
    skeleton/etc/init.d/rcS       mounts, mdev, ptys, then hands off
    skeleton/etc/init.d/services  our daemons, backgrounded

## Two device facts the init chain has to respect

**`sysinit` runs to completion before any `respawn` entry starts.** Both ways
back into the device — the serial console and ssh — are `respawn` entries,
so anything that blocks in `rcS` costs you every one of them at once. `rcS`
backgrounds `services` for exactly this reason.

**This kernel has no devpts.** `/proc/filesystems` lists jffs2, proc, ramfs,
rootfs, squashfs, sysfs and tmpfs; mounting devpts fails with `ENODEV`. It has
legacy BSD ptys instead, and `mdev` does not create those nodes because they
are not exposed through sysfs — so `rcS` makes them by hand. Without them an
interactive ssh session has no tty.

## Where the rest of it comes from

- `etc/passwd` is **generated per build** by `image/build.sh`, because the root
  password is. `etc/group`, `etc/hosts` and `etc/hostname` are here.
- Device nodes are in `devices.pseudo`, applied by `mksquashfs -pf` so the
  build needs no root. They must be in the image rather than made at boot:
  `/dev` is a tmpfs `rcS` mounts, which needs `/dev/null` and a console before
  `mdev` has run, and MTD character devices are not exposed through sysfs in a
  way `mdev` acts on.
- `omcid` is started from `services` and is live: it drives GPON provisioning
  over the netlink transport our own in-kernel switch/GPON drivers provide
  (`odi_switch`/`odi_omci`, built straight into the kernel — see
  `docs/KERNEL.md`), not a loadable module.
