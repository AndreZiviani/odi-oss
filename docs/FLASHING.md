# Flashing and trial boot

The full procedure for putting an image built here onto a stick, and for
getting back out if it does not come up. Read "Safety model" in the top-level
README first if you have not already.

## Before anything

1. **Find out which slot is running**, and flash the *other* one:

       cat /proc/cmdline

   `root=31:5` is slot 0, `root=31:7` is slot 1.

2. **Record the bootloader environment**, so there is something to compare
   against afterwards, and so you know what to restore if something goes
   wrong with the environment itself rather than the image:

       nv getenv

   Keep `sw_tryactive`, `sw_commit`, `sw_active`, and the version strings.
   **`sw_commit` must already equal the running slot.** That is what the
   revert path (`boot_by_commit`) reads on the next boot; `fwu.sh` itself
   refuses to write the slot named by `sw_commit`, but confirm it yourself —
   the failure this protects against is a trial reverting *into* the
   unproven image instead of away from it.

3. **Copy the tarball to the stick.** `/tmp` is ramfs on a device with a few
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

**What this catches, and what it does not.** The watchdog is kicked by a
kernel thread that needs nothing from userland (`docs/KERNEL.md`), so a
kernel that is alive keeps kicking it even if the boot scripts above it are
completely stuck — no network, no ssh, nothing answering. That state does
**not** self-revert on its own from the stuck side: it reverts on the *next*
boot, because `sw_tryactive` was already cleared, so a power cycle brings
the previous (committed) image straight back with nothing lost — it just
needs your hand on the power. A kernel that panics, or never mounts a root
filesystem, does trigger the watchdog itself and reverts without any
intervention at all.

**Never write `sw_commit` before this.** Making the trial slot permanent
throws away the only free safety net there is. Commit only from the running
trial image, once you are satisfied:

    nv setenv sw_commit <slot>

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
- **Boot the other (working) image** and read the DRAM ramlog back with
  `tools/memprobe` (`tools/memprobe/README.md`, `docs/KERNEL.md`): it
  survives the watchdog reset and holds the last console output the failed
  trial produced, including a stamp confirming whether U-Boot handed off to
  the new kernel at all. When the working image is ours too, it saved the
  failed trial's pages before overwriting them: `cat /proc/odi_ramlog_prev`
  (the boot counter one below the running one, and the trial's slot and
  build id, confirm it is the trial).
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

`docs/ACCESS.md` covers getting in over ssh or the web UI, and moving
files on and off the stick.
