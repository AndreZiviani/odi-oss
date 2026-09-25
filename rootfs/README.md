# Root filesystem

The skeleton is **ours**, not the vendor's. It is deliberately not a copy of
their `rc0`..`rc63` chain: most of what that starts does not exist in this
image, and one of the things it does start (`pondetect`) rewrites `PON_MODE`
and reboots after roughly 24 seconds of light without sync.

    skeleton/etc/inittab          sysinit rcS, the serial login, restart/shutdown
    skeleton/etc/init.d/rcS       mounts, networking, the SDK and PON steps, omcid
    skeleton/etc/init.d/services  metricsd, confd and dropbear, backgrounded
    skeleton/etc/scripts/         network.sh, mount-config.sh, flash, apply.sh,
                                  fwu_starter.sh, regreplay, parity-load.sh
    skeleton/etc/pon-steps        the PON verbs rcS runs, one per line
    skeleton/lib/firmware/odi/    the register-replay tables the kernel loads

## Two device facts the init chain has to respect

**`sysinit` runs to completion before any `respawn` entry starts.** The
serial `login` is the one `respawn` entry, and ssh only starts from
`services`, which `rcS` launches: anything that blocks in `rcS` costs every
way back into the device at once. `rcS` backgrounds `services` and every
slow step for exactly this reason. There is no telnet.

**dropbear needs the legacy BSD ptys.** It is built without `USE_DEV_PTMX`,
so an interactive ssh session takes a `/dev/ptypN`/`/dev/ttypN` pair
whether or not devpts is mounted. The kernel has both `CONFIG_LEGACY_PTYS`
and `CONFIG_UNIX98_PTYS`; `rcS` makes the sixteen BSD pairs by hand when
`/dev/ptyp0` is missing, and mounts devpts on `/dev/pts` as well. Without
the BSD nodes an ssh session has no tty.

## Where the rest of it comes from

- `etc/passwd` is **generated per build** by `image/build.sh`, because the root
  password is. `etc/group`, `etc/hosts` and `etc/hostname` are here.
- `/dev` is devtmpfs, mounted by the kernel before init
  (`CONFIG_DEVTMPFS_MOUNT`), so every driver node is there by name.
  `devices.pseudo` (applied by `mksquashfs -pf`, so the build needs no root)
  and the `make-devices.sh` generated from it are the fallback for a kernel
  without devtmpfs, where `rcS` mounts a tmpfs on `/dev` and registers
  `mdev` as the hotplug helper.
- `/var` is ramfs, and `/etc/config` is a link to `/var/config`, where
  `mount-config.sh` mounts the jffs2 `config` partition.
- `omcid` is started by `rcS` itself, from `omci_modules()` at the
  `omcimods` line of `pon-steps`, after the odi_switch platform init and
  module-load replay. It drives GPON provisioning over the netlink
  transport our own in-kernel switch/GPON drivers provide
  (`odi_switch`/`odi_omci`, built straight into the kernel — see
  `docs/KERNEL.md`); there is no loadable module.
  `/etc/config/modules.off` skips that whole step.
