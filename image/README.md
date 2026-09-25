# Image assembly

    ./image/build.sh        ->  out/image/<version>.tar

Five members, the same as the vendor's, because the on-device upgrade path is
what unpacks it:

    fwu.sh    the flasher. OURS -- see below.
    fwu_ver   the version string, one line.
    md5.txt   one md5sum line per member except itself.
    rootfs    squashfs 4.0, XZ, 1 MiB blocks.
    uImage    LZMA-compressed kernel with a U-Boot header (U-Boot decompresses it).

## Format facts, read off the stock image rather than assumed

The rootfs is **squashfs 4.0 with XZ compression** and a 1 MiB block size; the vendor image used LZMA, which mainline squashfs does not read. The rest of this paragraph is the vendor's arrangement, kept for the record —
read out of the superblock of the image the stick shipped with. That
combination is unusual: mainline squashfs dropped LZMA in favour of XZ before
4.0 existed, so this kernel carries Realtek's patched version. Debian's
`squashfs-tools` still writes it, even though `--help` does not list `lzma`.

Partition sizes come from `/proc/mtd` on a DFP-34X-2C2:

    k0 / k1   1,359,872 bytes   kernel
    r0 / r1   2,572,288 bytes   rootfs

Both are hard stops in `build.sh`. A partition that does not fit cannot be
flashed, and finding that out on the device costs a recovery.

## `fwu.sh` is ours

The vendor's version is a short shell script and this one keeps its contract
exactly — `fwu.sh <slot> <tarball>`, the same member names, the same `md5.txt`
format — so an image built here is accepted by the same on-device mechanism.
Three things are fixed rather than reproduced:

1. Theirs sets `set -e` and then tests `$?` after `diff`. Under `set -e` the
   failing `diff` kills the script first, so the friendly error is unreachable
   dead code.
2. Theirs records the version with `nv setenv` unconditionally. A missing `nv`
   must not fail a flash that already succeeded.
3. Theirs says nothing when a write fails part-way. Ours names the partition
   left unbootable, because that decides whether the stick still boots.

It also verifies **both** members before writing **either**: a tarball whose
rootfs is corrupt must never get as far as erasing the kernel partition.

## Device nodes

`rootfs/devices.pseudo` is a `mksquashfs -pf` file, so the build creates
character and block devices **without root**. They have to be in the image:
`/dev` is a tmpfs that `rcS` mounts, which needs `/dev/null` and a console
before `mdev` has run, and the MTD character devices are not exposed through
sysfs in a way `mdev` acts on.

Nothing resolves a partition by number — `mount-config.sh` and `fwu.sh` both
look the name up in `/proc/mtd`. The board has fourteen partitions and the
numbering is not a promise.

## The root account

A shared default password baked into a public build is worth less than
nothing, so `build.sh` generates one per build and writes it to
`out/image/root-password.txt`. `ROOT_PW=...` sets your own; `ROOT_PW=none`
gives an empty password.

The hash is **MD5 crypt (`$1$`)**, not SHA-512. The libc on this image is
uClibc 0.9.30.3 from 2010 and its `crypt()` does DES and MD5 only — a `$6$`
hash would never match and the account would simply be unloginable. That
constraint disappears with the uClibc-ng toolchain; see `toolchain/README.md`.

## Flashing

The image is **inert** when written. On the stick:

    ./fwu.sh <slot> <version>.tar      # writes the slot it is given
    nv setenv sw_tryactive <slot>      # boots it ONCE, watchdog armed

`sw_tryactive` reverts by itself if the image does not come up. Writing
`sw_commit` up front throws away the only free safety net there is.

### Before the FIRST trial boot of a new image

    touch /etc/config/modules.off

on the **running** stick. `/etc/config` is the jffs2 partition and `fwu.sh`
never writes it, so the flag is already in place when the new image comes up.

Without it, `services` loads `omcidrv`, `pf_rtk` and `igmp_drv` at boot. None
of the three has ever been loaded on this hardware, and a module that panics
takes the trial boot with it -- so the first boot tells you "it did not come
up" and nothing about which half was at fault.

With it, boot, check `/proc/mtd`, `/var/config`, `/proc/config.gz` and the
network, and then:

    /etc/scripts/load-modules.sh

which loads them in order, skips what is already in, and reports an `insmod`
failure without skipping the rest. Remove the flag once that has worked.
