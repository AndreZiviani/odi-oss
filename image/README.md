# Image assembly

    ./image/build.sh        ->  out/image/<version>.tar

Five members, the same as the vendor's, because the on-device upgrade path is
what unpacks it:

    fwu.sh    the flasher. OURS -- see below.
    fwu_ver   the version string, one line.
    md5.txt   one md5sum line per member except itself.
    rootfs    squashfs 4.0, XZ, 1 MiB blocks.
    uImage    LZMA-compressed kernel with a U-Boot header (U-Boot decompresses it).

## Format facts

The members keep the stock image's names and the stock `md5.txt` layout; the
formats inside them are this kernel's:

- `rootfs` is **squashfs 4.0, XZ, 256 KiB blocks** (`COMP`, `BS` in
  `build.sh`). The stock image uses LZMA with 1 MiB blocks, which only its
  patched vendor kernel reads; mainline squashfs has no LZMA. The smaller
  block is on purpose: 6.18 squashfs keeps caches of whole blocks, and every
  block of cache is RAM this 32 MB board does not have to spare.
- `uImage` is the kernel `kernel/build.sh` produces: LZMA-compressed, U-Boot
  header, load address `0x80000000`, entry point read from `System.map`.

Partition sizes come from `/proc/mtd` on a DFP-34X-2C2:

    k0 / k1   1,359,872 bytes   kernel
    r0 / r1   2,572,288 bytes   rootfs

Both are hard stops in `build.sh`, which prints the spare bytes of each. A
partition that does not fit cannot be flashed, and finding that out on the
device costs a recovery.

## What goes into the rootfs

`rootfs/skeleton/` is copied in first: `/etc` and the init chain, and the
three register-replay tables the kernel loads with `request_firmware()`
(`/lib/firmware/odi/sdkinit.bin`, `modload.bin`, `gpon_init.bin`, committed
data, not built here). Then busybox and its applet links, dropbear (with
`dropbear`, `dropbearkey` and `scp` links), iproute2, and every binary found
in `out/bin` (`diag`, `omcid`, `omcli` with its `omcicli` link, `omciprobe`,
`omcicap`, `nv`, `igmpd`, `metricsd`, `confd`) plus confd's web assets. A
missing package or confd asset set is fatal unless `ALLOW_PARTIAL=1`.

`build.sh` also writes `/etc/version` (read by confd), `/etc/odi-build` (the
image, kernel and component versions and md5s, read by the exporter's
`gpon_image_info`), and `/etc/kernel-config`, the `.config` the kernel was
built from. `rcS` gates platform init and `omcid` on that file, so a build
without one fails unless `ALLOW_NO_KCONFIG=1`.

Every ELF staged is run through `packages/isa-audit.sh` (instructions this
CPU traps on) and `packages/isa-allowlist.sh` (anything not confirmed to
execute) before it is squashed.

## `fwu.sh` is ours

The stock version is a short shell script and this one keeps its contract
exactly — `fwu.sh <slot> <tarball>`, the same member names, the same `md5.txt`
format — so an image built here is accepted by the same on-device mechanism.
It runs on the stock image's busybox too: `stock-commands.txt` lists what
that image has, and `test/fwu_stock_commands_test.sh` holds `fwu.sh` to it.
What it does differently from the stock one:

1. The stock one sets `set -e` and then tests `$?` after `diff`, so its
   friendly error is unreachable. Ours tests directly.
2. It refuses the running slot (from `root=` in `/proc/cmdline`, else
   `sw_active`), and refuses outright when it cannot tell.
3. It refuses a slot that `sw_commit` names, in either copy of the
   environment: the revert target must stay intact.
4. It checks both members' md5 **and** size before erasing anything, so a
   bad or oversized tarball never gets as far as erasing a partition.
5. It streams each member out of the tarball with `tar -O`, so `/tmp` needs
   only the tarball, `fwu.sh` and `md5.txt`.
6. It names the partition left unbootable if a write fails part-way.
7. It does not record `sw_version<slot>` unless `FWU_RECORD_VERSION=1`:
   that is an erase-and-rewrite of the environment for a cosmetic string.

It never writes `sw_tryactive` or `sw_commit`, and never touches the config
partition. `/etc/scripts/fwu_starter.sh` (in the rootfs) is what the web
UI's firmware write runs: it checks the target is the inactive slot,
extracts and md5-checks `fwu.sh` from the uploaded tarball, and runs it
detached, with its log in `/tmp/fwu.log` and its outcome in
`/tmp/fwu.state`.

## Device nodes

On this kernel `/dev` is devtmpfs, mounted before init runs
(`CONFIG_DEVTMPFS_MOUNT`), so every driver node is there by name.
`rootfs/devices.pseudo` is still applied (`mksquashfs -pf`, so the build
needs no root) and `build.sh` generates `/etc/scripts/make-devices.sh` from
it; `rcS` uses that pair only on a kernel without devtmpfs, where it mounts
a tmpfs on `/dev` and has to put the nodes back itself.

Nothing resolves a partition by number — `mount-config.sh` and `fwu.sh` both
look the name up in `/proc/mtd`. The board has fourteen partitions and the
numbering is not a promise.

## The root account

A shared default password baked into a public build is worth less than
nothing, so `build.sh` generates a 14-character one per build and writes it
to `out/image/root-password.txt` and, so the next build does not overwrite
it, `out/image/root-password-<version>.txt`. `ROOT_PW=...` sets your own;
`ROOT_PW=none` gives an empty password.

The hash is **MD5 crypt (`$1$`, `openssl passwd -1`)**. That was forced by
the vendor uClibc this image once linked, whose `crypt()` did DES and MD5
only; the uClibc-ng it links now has SHA-256/512 crypt
(`toolchain/README.md`), so the constraint is gone, but the hash has not
been changed.

## Flashing

The image is **inert** when written. On the stick:

    ./fwu.sh <slot> <version>.tar      # writes the slot it is given
    nv setenv sw_tryactive <slot>      # boots it ONCE, watchdog armed

A trial that does not come up reverts by itself: `odi_wdt` resets the board
at 120 s of uptime unless `rcS` has confirmed that the host side answers
ARP. Writing `sw_commit` up front throws away the only free safety net there
is. `docs/FLASHING.md` is the full procedure, including reading a failed
trial's log.

`/etc/config/modules.off`, set on the running stick before a trial (the
config partition survives the flash), makes the new image skip the OMCI
side at boot: no odi_switch platform init, no module-load replay, no
`omcid`. Management networking, ssh and the web UI still come up, so a
trial can be checked before it talks to the OLT. `apply.sh omci` refuses
while the flag is set.
