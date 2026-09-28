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
- verifies both members (kernel and rootfs) and checks they fit their
  partitions **before** erasing anything, so a bad tarball never leaves you
  with an erased partition and nothing to put in it;
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
back.

**Never write `sw_commit` before this.** Making the trial slot permanent
throws away the only free safety net there is, and **never write it from a
script or while a trial is still being evaluated** (a boot loop, a soak):
every trial is a separate `sw_tryactive`. Commit only from the running
trial image, once you are satisfied:

    nv setenv sw_commit <slot>

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
    one (`boot=N-1 slot=... build=... crumb=...`): check that slot and
    build id are the trial's before reading on. It reaches one boot back
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
