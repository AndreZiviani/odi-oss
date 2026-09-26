# The boot, stage by stage

What `/etc/init.d/rcS` does and why each stage sits where it does. The
script itself says what each block does; this is the reasoning behind the
order, for someone about to change it. `docs/TOOLS.md` ("How the image
starts things") is the short list; `docs/HACKING.md` has the recovery
mechanism the boot confirms to.

## The rules every stage follows

**Nothing may block.** `/etc/inittab` runs rcS as `sysinit`, and busybox
init starts no `respawn` entry until sysinit returns. The serial login is
the one respawn entry, and ssh only starts from `services`, which rcS
launches: anything that blocks in rcS costs every way back into the device
at once. So every slow step runs in the background, and every read that
could stall (diag, arping, anything over the network) runs under `timeout`.

**Nothing may abort.** rcS runs under `set -e`, which is what stops a
typo from going unnoticed, but an aborted sysinit loses the same things a
blocked one does. Every command that can fail is guarded (`|| echo "rcS:
..." >&2`, or an `if`) and the boot carries on with the damage
proportional: a failed `/var` mount costs the logs, not the shells. Beware
of `[ test ] && command` as the last line of a function: when the test is
false the function returns non-zero and `set -e` stops the boot there.

**Every stage is on record.** `crumb` writes one line per stage to the
kernel log (`rcS: ...`), which the DRAM ramlog keeps across a watchdog
reset: a trial that never answered still says how far it got
(`/proc/odi_ramlog_prev` on the next boot of this image). With
`breadcrumbs.on` the same lines go to flash too (below).

## The stages

1. **Mounts.** `/proc`, `/sys` and `/var` (ramfs). `/dev` is devtmpfs,
   mounted by the kernel before init (`CONFIG_DEVTMPFS_MOUNT`), so every
   device node, `/dev/mem` and the pty nodes included, is there by name.
   The five `/var` directories follow, each guarded: if the ramfs mount
   failed, `/var` is the read-only squashfs.
2. **The config partition** (`/etc/scripts/mount-config.sh`, run under
   `sh -x` into `/tmp/mount-config.trace`): the jffs2 partition shared with
   the stock slot, found by its mtd name, mounted on `/var/config`
   (`/etc/config` links there). Host keys, the UI credentials and every
   switch file live on it. A failure is said in the kernel log at once:
   everything that follows reads its switches from here, and a switch file
   that cannot be read is a switch that silently turns itself off.
3. **Entropy.** Nothing on this board feeds the pool early, so without a
   seed every daemon starts on a nearly empty one (dropbear once logged 27
   bits available). `seedrng` credits the seed the previous boot saved
   under `/etc/config/seedrng` and saves a fresh one. At this point the pool
   is not initialised, so that seed cannot be credited; the second run, 90
   s in, saves one the next boot can credit. seedrng reads with
   `getrandom()`; it never reads `/dev/random`.
4. **Hostname and loopback.** Guarded like the rest: `sethostname` needs a
   privilege the chroot test harness does not have.
5. **The SDK-init verbs** through `/proc/odi_init`, `intr` to `ponmac`, in
   the order the stock firmware issues them. Before the network, because
   without them no frame passes between the CPU port and the host port,
   and because `network.sh` needs a working `diag` for its SerDes check.
   Each verb has a crumb before and after it, so one that hangs names
   itself. The kernel loads the replay behind each verb from
   `/lib/firmware/odi/sdkinit.bin` and releases it again.
6. **The management address** (`/etc/scripts/network.sh`), after the
   config partition, which holds the address, and before the services:
   dropbear bound on a box with no routable interface looks exactly like a
   panic from outside.
7. **ptys.** devpts on `/dev/pts`, for the `/dev/ptmx` dropbear opens for
   an interactive session; the BSD pairs (`/dev/ptypN`/`/dev/ttypN`) are a
   fallback it can also scan. devtmpfs normally provides them; rcS makes
   sixteen pairs if `/dev/ptyp0` is missing.
8. **The services** (`/etc/init.d/services`: metricsd, confd, dropbear),
   in the background, with their output appended to
   `/var/log/services.log`. A file rather than the console, because a trial
   has no console, and rather than a pipe, because a pipe the daemons
   inherit is one nothing ever closes.
9. **The log trim**, a background loop: `/var` is ramfs, nothing there is
   evicted, and omcid logs every OMCI frame (about 1.7 MB a day on a busy
   OLT). Past 256 KB each log is cut back to its last 128 KB; the writers
   append, so they carry on at the new end.
10. **The watchdog confirmation**, `echo 1 > /proc/odi_wdt/userland_ok`.
    odi_wdt resets the board at 120 s of uptime unless this is written,
    which is what reverts a trial that never gets here (a hung kernel or a
    wedged rcS). It needs no network: a stick that boots but is unreachable
    is fixed with a power cycle, not a reboot loop. It comes before the PON
    steps, so a problem on the PON side cannot cost the confirmation.
11. **The optics**, the `/proc/odi_init` verb `optics` (`odi_board.c`),
    because the SDK verbs never set them up on this board:
    `PIN_GPIO_SELECT` 0x048 = 0x08082001 (without it the port-1 I2C pins,
    DDM and the module status, stay muxed away and every transaction ends
    in NO_ACK), and SoC GPIO 13, the TX-disable input of the module, as an
    output driven low, the level set before the direction. As an input,
    its reset default, the laser stays off, the module reports
    TX_DISABLE_STATE and the OLT never hears the serial number; with it
    low the ONU went from O3 to O5 within 5 s. It runs before the PON
    steps and outside their list, so a `pon-steps` override keeps it;
    `optics.off` skips it.
12. **The PON steps**, each one `/proc/odi_init` verb: `i2c 1`, `i2cen 1`,
    `gpon`, `rxsd`, `gpondrv`, `gpondev`, then the switch init and omcid,
    then `gponsn`, `gponpw` and `gponact`. The order is fixed by the
    hardware. omcid registers its receive path in a table that the
    `gpondrv`/`gpondev` init clears, so it starts after them (before, the
    OMCI frames were counted and none delivered); the switch init
    deactivates the ONU, so the activation steps come after it (with the
    switch init last, the ONU stayed in O1). The switch init is the
    `switch_init` write of `/proc/odi_omci`: the platform settings and the
    module-load replay the stock OMCI kernel modules made when they loaded
    (`docs/SWITCH.md`). The serial number and the PLOAM password come from
    the config store (`GPON_SN` in the HS file, `GPON_PLOAM_PASSWD` as hex
    in the CS file); an empty password is not sent. The crumbs name the
    verb only, so neither value reaches a log.

## The development aids: rcS.dev

`/etc/init.d/rcS.dev` ships in the image and is sourced by rcS. It
redefines four functions rcS calls at fixed points (`crumb`, `dev_hook`,
`dev_confirm_ok`, `pon_steps`); without the file each is a no-op or the
built-in behaviour. Each aid is off unless its file is on the config
partition, and each is set by hand on the running image before a trial:
the config partition is shared and `fwu.sh` never writes it, so what they
leave there is readable from the image the stick reverts to.

- **`breadcrumbs.on`**: every crumb is also appended, timestamped, to
  `/etc/config/breadcrumbs` (truncated each boot, synced after each line),
  with the host SerDes registers after the network and the PON registers
  after the PON steps. After the services it lowers the soft-lockup
  threshold to 20 s (the default 60 s is later than the watchdog, 42 s
  after the last kick, so it never spoke) and hung tasks to 30 s, starts a
  30 s heartbeat (uptime, free memory, load) to the kernel log, and writes
  `/etc/config/trial-diag.txt`, a network snapshot from the inside: one
  section per write, synced, each under `timeout 5`, and no `diag`, whose
  ioctl can block where `timeout` cannot end it. A write to flash through a
  path the trial is the first test of, so it is off unless asked for.
- The same questions go to the kernel log when `breadcrumbs.on` is set
  **or the config partition did not mount**: then this is the only
  diagnostic the boot gets.
- **`confirm-arp`**: the watchdog is confirmed only after an ARP reply from
  the `.2` of the `br0` subnet, a fixed test host. A trial that comes up
  unreachable then reverts by itself and keeps its ramlog (a watchdog reset
  keeps DRAM; a power cycle does not). Never leave it on a stick whose
  host is not on `.2`: every boot then resets at 120 s.
- **`pon-steps`**: a list that replaces the built-in PON steps, one per
  line: a `/proc/odi_init` verb with its argument, or `omcimods` (the
  switch init and omcid), `gponsn auto` or `gponpw auto` (the values from
  the config store). It is how the order above was found, one step per
  boot. A wrong list means no PON.

`breadcrumbs.on` and `confirm-arp` are documented for users in
`docs/SETTINGS.md`; every switch, with where it is read, is in
`docs/HACKING.md` ("Feature toggles").
