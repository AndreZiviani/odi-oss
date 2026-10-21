# Flashing and trial boot

The full procedure for putting an image built here onto a stick, and for
getting back out if it does not come up. Read "Safety model" in the top-level
README first if you have not already.

## Verifying a release

Every GitHub release (`.github/workflows/release.yml`) ships four files
beside the image tarball: `SHA256SUMS`, a CycloneDX SBOM (`sbom.cdx.json`,
built by `tools/generate-sbom.sh` from the pins already in this tree --
kernel, toolchain, busybox/dropbear/iproute2, and the metricsd/confd tags
this build actually fetched), and two GitHub attestations covering the
tarball: build provenance (this came from this repository's `release.yml`,
from this commit) and an SBOM attestation (this SBOM describes this
tarball). Checking them needs only `gh`, already authenticated:

    gh attestation verify odi-oss-<version>.tar -R AndreZiviani/odi-oss
    sha256sum -c SHA256SUMS

`gh attestation verify` checks both attestation types against the file on
disk and reports which workflow run produced them; a mismatch (wrong file,
tampered tarball, or a build from a fork) fails loudly rather than silently
serving a false positive. The SBOM itself is human-readable JSON --
`jq . sbom.cdx.json`, or open it in anything that reads CycloneDX -- and is
also covered by `gh attestation verify sbom.cdx.json -R AndreZiviani/odi-oss`.

## Backing up the whole flash

Do this once before the first flash of a stick, and again before anything that
rewrites the environment or the config store. It takes about ten seconds and
the result is the only way back from a mistake the two firmware slots cannot
absorb: they protect against a bad image, not against a lost config store, a
damaged bootloader environment or a wrong `flash_eraseall`.

**What is on the flash.** 8 MiB of SPI NOR, which the kernel exposes as one MTD
partition per region, in this order and with no gaps (read the names and sizes
from your own stick: `cat /proc/mtd`, and never trust the numbers, only the
names):

| partition | offset | size | what it holds |
|---|---|---|---|
| `boot` (mtd0) | 0 | 256 KiB | the bootloader |
| `env` (mtd1) | 256 KiB | 8 KiB | bootloader environment, first copy |
| `env2` (mtd2) | 264 KiB | 8 KiB | bootloader environment, second copy |
| `config` (mtd3) | 272 KiB | 240 KiB | the jffs2 config store: identity and every setting |
| `k0` (mtd4) / `r0` (mtd5) | 512 KiB / 1840 KiB | 1328 KiB / 2512 KiB | slot 0 kernel and root filesystem |
| `k1` (mtd6) / `r1` (mtd7) | 4352 KiB / 5680 KiB | 1328 KiB / 2512 KiB | slot 1 kernel and root filesystem |

The other MTD nodes (`Partition_008`... and `linux`/`rootfs`) are 4 KiB
placeholders and aliases of the slot that is running: not separate flash, and
not part of a backup.

**The config partition is the one that cannot be rebuilt.** It holds
`GPON_SN`, the PLOAM password, `ELAN_MAC_ADDR` and `MAC_KEY` (`docs/SETTINGS.md`),
the identity the OLT authenticates the line on, plus the host keys and the web
UI credentials. A new image does not recreate them, `flash_eraseall` on
`config` removes them for good, and a copy of it is as sensitive as the
stick: it is in clear text. Keep the dump out of any repository, mode 600,
in a directory only you can read.

**Read it.** Read the partitions in order and stream them to the host; do not
stage them on the stick, whose `/tmp` is RAM and cannot hold 8 MiB
(`docs/ACCESS.md`, "Copying files: scp"). Reading the `...ro` nodes cannot
change the flash:

    umask 077
    ssh root@<stick> 'for n in 0 1 2 3 4 5 6 7; do
        dd if=/dev/mtd${n}ro bs=4096 2>/dev/null; done' > flash-<stick>-$(date +%Y%m%d).bin
    ls -l flash-<stick>-*.bin        # must be 8388608 bytes

On our 6.18 kernel `dd` without a `count` stops at the end of the partition by
itself; it was checked with a bounded read (`timeout`), and the whole read
above takes about ten seconds. (The node numbers above are the order of
`/proc/mtd` on the images this tree builds. If yours differs, list the names
and read the eight regions in the order of the table.)

**Check it.** Compare each partition on the stick against the same bytes of
the file, so a short or corrupt transfer is found now rather than during a
restore:

    ssh root@<stick> 'for n in 0 1 2 3 4 5 6 7; do
        printf "mtd$n "; dd if=/dev/mtd${n}ro bs=4096 2>/dev/null | md5sum; done'
    # on the host, one partition of the file (offset and size in KiB from the table):
    dd if=flash-<stick>-<date>.bin bs=1024 skip=272 count=240 2>/dev/null | md5sum   # config

The config partition is mounted and live: if a setting is saved between the
two reads the sums differ for that partition alone. Read it again.

A stick that is reachable only over telnet (the stock image) cannot do the
`ssh` form: have it listen and pull, `nc -l -p 12346 < /dev/mtd3ro` on the
stick and `nc -d -w 5 <stick> 12346 > config.bin` on the host. The `-d` is
mandatory, because without it the transfer is silently cut short. Check the size
and the md5 on both ends.

**Restore.** The write path is the one `fwu.sh` uses: erase the partition, write
the bytes to its `/dev/mtdN` node, read it back and compare. Nothing here has
been exercised as a whole-flash restore, only the read, so do it one partition
at a time and verify each before the next:

- **A firmware slot (`k0`+`r0` or `k1`+`r1`)**: never restore the slot you are
  running from (the kernel marks it read-only, and `fwu.sh` refuses it).
  Boot the other slot, then either flash a release tarball with `fwu.sh` (the
  normal way) or write the saved partitions back:

      ssh root@<stick> 'flash_eraseall /dev/mtd5 && cat > /dev/mtd5' < r0.bin
      ssh root@<stick> 'cmp - /dev/mtd5' < r0.bin    # "EOF on -" is the match

  with `r0.bin` cut out of the dump by offset and size, as above. The
  write above takes the slot's rootfs only; do the kernel (`k0`/`k1`) the
  same way, and write it **last**, as `fwu.sh` does, so that an interrupted
  restore leaves a kernel U-Boot refuses rather than a valid one over a
  half-written rootfs.

  **Before you point the next boot at a slot you wrote or restored by
  hand**, check it from the running slot. Nothing below is a CRC check: the
  stick has no CRC32 tool (`cksum` is the POSIX CRC, not the uImage one), so
  the header and data CRC that U-Boot verifies cannot be recomputed there.
  What can be checked, with the applets `fwu.sh` itself relies on:

  1. Which slot is running and what the environment says, so you do not
     touch the running slot or the committed one:

         cat /proc/cmdline; nv getenv sw_active; nv getenv sw_commit; nv fallback sw_commit

     The slot you are about to restore must be neither the running one nor
     the one `sw_commit` names, in both copies.
  2. The kernel partition holds a uImage, not erased flash: the first four
     bytes are the magic `27 05 19 56` (an erased partition reads `ff`):

         dd if=/dev/mtd6ro bs=4 count=1 2>/dev/null | md5sum
         printf '\047\005\031\126' | md5sum      # the two sums must be equal

     (`mtd6` is `k1`, `mtd4` is `k0`; take the number from `cat /proc/mtd`.)
  3. Both partitions of the slot are byte-for-byte what you meant to write,
     by md5 against the dump or the release you took them from. For a
     release tarball, compare the member against the partition, which is
     larger than the member: `tar -xf odi-oss-<version>.tar uImage -O | cmp - /dev/mtd6ro`
     says `EOF on -` when every byte of the member matches (as in the write
     check above). A dump taken with "Backing up the whole flash" compares
     whole partitions by md5.
  4. What is **not** checked: the uImage header CRC and data CRC, and that
     the kernel matches the rootfs next to it. Both are left to U-Boot at
     boot, which refuses a bad kernel and boots the committed slot
     instead, so the check that settles it is the trial below.

  Then take the safe path, never `sw_active`: `nv setenv sw_tryactive <slot>`
  and `reboot` boots that slot once under the watchdog and returns to the
  committed one on any failure ("Trial-booting it"); only after you have
  logged in and checked it, `nv commit <slot>` from that slot
  ("Committing"). Do not write `sw_active`, and do not write `sw_commit`
  before the trial.
- **The bootloader environment (`env`, `env2`)**: prefer `nv setenv`
  (`docs/TOOLS.md`), which rewrites one copy at a time. A raw restore is for a
  damaged environment: write `env2` first, check it, then `env`, so one copy is
  valid at every moment of the restore.
- **The bootloader (`boot`)**: do not restore it unless it is damaged. A
  power cut or a bad write during the erase leaves a stick that does not
  boot at all, and this tree has no way to bring one back (see "If it does not
  come up": recovery needs the board's UART header).
- **The config partition (`config`)**: it is mounted, and a raw write under a
  mounted jffs2 is not safe. Restore settings with the web UI's Restore (or
  the copy `tools/config-backup.sh` keeps, `docs/SETTINGS.md`), which carries
  every key. Keep the raw `config` copy as the last-resort record of the
  identity: with it, `GPON_SN`, `ELAN_MAC_ADDR` and `MAC_KEY` can be read back
  out and typed in again if the partition is ever lost.

## Before anything

1. **Find out which slot is running**, and flash the *other* one:

       cat /proc/cmdline

   `root=31:5` is slot 0, `root=31:7` is slot 1.

2. **Decide whether the trial should revert when it is unreachable.** This
   image arms the watchdog on every boot, and rcS confirms userland to it
   once its own steps have run, without looking at the network; with no
   confirmation within 120 s of uptime the kernel resets the board. So a
   trial that boots but never answers on the network stays up until you
   power-cycle it. For development, `: > /etc/config/confirm-arp` makes rcS
   confirm only after an ARP reply from the `.2` address of the management
   subnet (`192.168.1.2` for the default `192.168.1.1`; see
   `docs/SETTINGS.md`), so an unreachable trial reverts by itself and keeps
   its ramlog. Only set it when the host port really has that address: on
   a trial, no reply means a fall-back to the committed slot; on a
   committed slot of this image, a reboot loop.

3. **Record the bootloader environment**, so there is something to compare
   against afterwards, and so you know what to restore if something goes
   wrong with the environment itself rather than the image:

       nv getenv

   Keep `sw_tryactive`, `sw_commit`, `sw_active`, and the version strings.
   **`sw_commit` must already equal the running slot.** That is what the
   revert path (`boot_by_commit`) reads on the next boot; `fwu.sh` itself
   refuses to write the slot named by `sw_commit` (in either environment
   copy), but confirm it yourself — the failure this protects against is a
   trial reverting *into* the unproven image instead of away from it.

4. **Copy the tarball to the stick.** `/tmp` is ramfs on a device with a few
   MB of free RAM, so do not unpack the whole tarball — `fwu.sh` streams
   each member out of it with `tar -O` and needs only itself and the
   checksum file on disk beside it:

       scp -O odi-oss-<version>.tar root@<stick>:/tmp/    # from this image, see docs/ACCESS.md
       cd /tmp && tar xf odi-oss-<version>.tar fwu.sh md5.txt
       md5sum odi-oss-<version>.tar     # compare against the value the build printed

## Flashing

    ./fwu.sh <slot> odi-oss-<version>.tar

`fwu.sh` is ours, and it:

- refuses to write the slot it is currently running from, cross-checked
  against `/proc/cmdline` and `sw_active`, and refuses outright rather than
  guessing if it cannot tell;
- names up front any tool it needs that the running image lacks (`tar`,
  `sed`, `cksum`, `md5sum`, `cmp`; it uses no `cut`, which some stock
  firmwares do not have), before it touches anything;
- verifies both members (kernel and rootfs) and checks they fit their
  partitions **before** erasing anything, so a bad tarball never leaves you
  with an erased partition and nothing to put in it;
- writes in an order that makes an interrupted flash unbootable instead of
  half-valid: it erases the kernel and rootfs partitions first, writes the
  rootfs, and writes the kernel **last**. U-Boot refuses a kernel that is
  erased or short (bad magic or data CRC), so a power cut at any point leaves
  a slot the bootloader will not start, never a valid new kernel over a
  missing or old rootfs;
- reports by name which partition is left unbootable if a write does fail
  partway, because that is exactly the information that decides whether the
  stick still boots at all;
- never touches the config partition, so host keys, the web UI credential
  and feature flags on the stick you are flashing survive.

Clean up afterward:

    rm /tmp/odi-oss-<version>.tar /tmp/fwu.sh /tmp/md5.txt

## Trial-booting it

    nv setenv sw_tryactive <slot>
    reboot

This boots the slot **exactly once**, with the hardware watchdog armed.
U-Boot rewrites `sw_tryactive` back to "don't retry" and saves *before*
handing over, so the trial cannot loop.

**Check the laser on the trial.** The image switches the laser on only when
the laser driver chip reads back as the Semtech GN25L95 (id 0x6a) it is
tested with. On a stick with another laser driver it stays off and GPON
activation is refused, so the ONU never leaves O1 and the trial looks like a
dead line: `dmesg | grep 'odi_board: laser'` says which, and
`grep '^laser' /proc/odi_gpon/status` shows `laser on 0 ldd_id <id> unverified 0`.
`flash set ODI_LASER_UNVERIFIED 1` and a reboot override it (`docs/SETTINGS.md`);
do that only on a stick you know. Check this before you commit.

**What this catches, and what it does not.** A kernel that panics, hangs,
or never mounts a root filesystem stops kicking the watchdog, and the board
resets. A kernel that is alive keeps kicking it, but it also holds a
**userland deadline** (`docs/KERNEL.md`): unless rcS has confirmed within
120 s of uptime, the kernel forces the reset itself. So a boot whose
scripts hang reverts too. rcS confirms without looking at the network, so
a boot whose management path never comes up stays up; power-cycle it. For
development, `/etc/config/confirm-arp` makes rcS confirm only after an ARP
reply from the `.2` address of the `br0` subnet, so an unreachable trial
reverts by itself; never leave it on a stick whose host is not on `.2`. Because U-Boot cleared `sw_tryactive` before
handing over, every one of those resets lands on the committed slot, with
nothing lost and nothing to do by hand. What it cannot catch is an image
that confirms and then misbehaves (unreachable, or answers ARP but not ssh): that
stays up until you power-cycle it, which also brings the committed image
back -- as long as the trial has not been committed ("Committing",
below).

**Never write `sw_commit` before this.** Making the trial slot permanent
before it has booted throws away the only free safety net there is.

A trial image can itself be re-tried as often as you like: from the
committed image, `nv setenv sw_tryactive <slot>` and `reboot` again. The
slot is not rewritten, so each boot tests the same image.

**Rebooting this image.** `reboot` shuts down and then resets the board
through the watchdog a third of a second later (`docs/KERNEL.md`). The
link drops about 4 s after the command, and on ISP1 the committed stock
image answered on port 80 again about 75 s after it, nearly all of that
U-Boot and the stock boot. `halt` and `poweroff` do not stop the stick
for good: it has no power switch, so both leave the watchdog armed and
hang, and the board resets about 42 s later. From a trial slot, every one of these lands on the
committed slot.

## Committing

**The update procedure ends with you committing the trial by hand**, from
the running trial image, once you have checked it yourself: logged in,
the ONU in O5 (`omcli state`), traffic flowing, whatever the change was
meant to do doing it.

    nv commit <slot>          # the running slot; nv refuses any other
    nv getenv sw_commit; nv fallback sw_commit     # both name the new slot
    slot-state.sh             # clears the login notice and gpon_uncommitted

`nv commit` (`docs/TOOLS.md`) writes `sw_commit` into the primary
environment copy, reads it back, then into the fallback copy, reads it
back, then reads both once more. It refuses a slot that is not the running
one or not `sw_active`, and an environment whose two copies are not both
valid; it never writes `sw_active`, and it is safe to interrupt at every
step. `nv setenv sw_commit <slot>` also still works but writes only the
primary copy; nv then prints that the fallback copy still names the old
slot, and `nv setenv -c <copy> sw_commit <slot>` makes it agree. `nv
commit` does both, in the safe order, and checks them.

The same two writes set `mupgrade_en=0`. The stock bootloader ships it as 1
and, after the boot countdown, listens on the host link for a firmware image
sent by multicast and writes it to flash without any authentication, so
anyone on the LAN side during a reboot could replace the firmware. Every
install of this image ends in `nv commit`, so every stick gets it turned off.

**Know what committing costs.** Once committed, every reset boots this
image. An image that later crashes, hangs or fails to confirm -- next
boot, or next month -- resets into itself again, forever: nothing on the
stick sends it back to the other slot. The only way out is a boot that
gets far enough for you to log in and point `sw_commit` back, or the UART.
So commit only an image you have watched work.

### The reminders

Nothing commits a trial for you. So that an uncommitted image is never
mistaken for a committed one, `/etc/scripts/slot-state.sh` (an
`/etc/inittab` `once` entry) reads the environment at every boot and, while
`sw_commit` in either copy names another slot than the running one, says
so in three places:

- **the ssh login banner** (`/etc/motd`, printed by dropbear at every
  interactive login):

      *** TRIAL BOOT: running slot 1, which is not committed (sw_commit=0 in
      both copies). The next reboot returns to slot 0. Commit with: nv commit
      1 && slot-state.sh

  or `HALF COMMITTED` when only the fallback copy still names the other
  slot (a plain `nv setenv sw_commit` did that), naming which copy says
  what;
- **the exporter**: `gpon_uncommitted 1`, with `gpon_boot_slot` and
  `gpon_committed_slot{copy=...}` beside it (odi-sfp-exporter's
  `docs/METRICS.md`), from the state file `/var/run/odi-slot`
  (`docs/TOOLS.md`, "Slot state");
- **an alert**: `OdiUncommittedImage`, a warning once `gpon_uncommitted`
  has been 1 for 30 minutes (odi-sfp-exporter's `prometheus/alerts.yml`).

The file is written at boot: after `nv commit`, run `slot-state.sh` (it is
on the ssh `PATH`) to clear the banner and the metric without a reboot.

## If it does not come up

There is no serial console on this device, so a trial that never answers on
the network is otherwise indistinguishable from one that never booted at
all.

- **Turn on breadcrumbs before the trial, from the currently running
  stick**: `: > /etc/config/breadcrumbs.on`. `rcS` and `services` then
  append one timestamped line per boot stage to `/etc/config/breadcrumbs` —
  config partition mounted, network address assigned, `rcS` done,
  `services` done. That file lives on the config partition, which
  `fwu.sh` never writes, so it **survives the revert**: after a failed
  trial, read `/etc/config/breadcrumbs` from the old (now running again)
  image to see exactly how far the new one got. Leave it off for a first
  trial of a brand-new image if you would rather not add any write to the
  config partition until you have seen the image boot at all; turn it on
  for the next one.
- **Read the failed boot's console from DRAM.** The ramlog pages survive
  the watchdog reset (not a power cycle) and hold the first 4 KB and the
  last 4 KB of the failed boot's console, the last early-boot crumb it
  reached (`docs/KERNEL.md`, "Early crumbs"), and, with
  `print-fatal-signals=1`, the registers of any process that died of a
  signal.
  - **From the stock image**, which knows nothing of these pages: push
    `tools/memprobe` and read them back
    (`tools/memprobe/README.md`, `tools/memprobe/ramlog-read.sh`).
  - **From this image** (a committed slot of ours, or a later trial of
    ours): the kernel copied both pages before its own ramlog wrote
    anything, so

        cat /proc/odi_ramlog_prev          # decoded
        cat /proc/odi_ramlog_prev_raw > p  # 8192 raw bytes, page A then B

    The first line is this boot (`boot=N slot=S`), the second the previous
    one (`boot=N-1 slot=... build=... crumb=... reason=...`): check that
    slot and build id are the trial's before reading on. `reason=` is why
    it reset, as the kernel recorded it: `wdt_client:<name>`, `wdt_mem`,
    `wdt_cpu_rx`, `wdt_userland`, `reboot`, `halt`, `poweroff`, `panic`, `oops`, `power`
    (DRAM lost: a power cycle) or `unknown` (the pages survived, nothing
    recorded one: a hang the hardware watchdog caught). `docs/HACKING.md`,
    "Reading a boot you could not see", has the full table. It reaches one boot back
    only, and a boot of the stock image in between writes nothing, so the
    boot counter says which boot of ours it was.
- **Power-cycle the stick** if nothing else answers after a few minutes —
  see "What this catches" above for why that alone can be enough.
- If both slots end up unbootable, recovery needs the board's UART header;
  there is no other way in. This is the reason for every rule above, and the
  reason to never commit an image you have not actually watched come up.

## What answers what, once you are in

    omcli state              serial, device identity, ONU state, MIB sync
    omcli conn                the bridge connections omcid built
    omcli get sn                the same command shapes the stock CLI answers
    diag                       the switch/optics CLI, batched on stdin
    cat /var/log/omcid.log     every OMCI frame and driver call this boot
    cat /proc/odi_ramlog_prev  the previous boot of this image, from DRAM

`docs/ACCESS.md` covers getting in over ssh or the web UI, and moving
files on and off the stick.
