# Changelog

Releases of the flashable image. Trial builds between releases are not
listed here.

## Unreleased

- **Lint no longer chokes on binary files.** The plan/task-id grep in
  `make lint` now skips binary files (`git grep -I`); `docs/images/*.png`
  matched the pattern as raw bytes on the CI runner only, never locally.
- Fixed the odi_nic driver asserting PAUSE toward the switch CPU port at
  ordinary traffic levels. `ODI_NIC_FC_ON_LEVEL`/`ODI_NIC_FC_OFF_LEVEL`
  (the free-descriptor watermarks that gate the NIC's own flow control)
  were derived as a flat quarter/three-quarter of the RX ring depth,
  asserting PAUSE at only 75% ring-used and holding it until the ring
  drained back to 25% used — a band wide enough for ordinary NAPI
  scheduling jitter at a few packets a second to cross and hold, with no
  real congestion behind it. Rescaled to the stock firmware's own
  near-exhaustion trigger proportion (assert near 94% used, deassert near
  81% used) instead, so PAUSE only fires near actual ring exhaustion.

## v1.0.1 — 2026-09-27

`metricsd` (the Prometheus exporter) v1.1.1, up from v1.0.3. The `diag`
commands the exporter sends are unchanged (the exporter contract goldens
in `src/diag/test/` still pass byte for byte), but its metric output
changed: `oversize` is now a frame-size bucket rather than a receive-error
`kind`, there is a new frame-size histogram, and an octet-counter wrap
guard. No odi-oss doc or golden pinned the old `kind="oversize"` label or
`gpon_port_receive_errors_total` by name, so nothing else in this repo
needed updating.

## v1.0.0 — 2026-09-26

The first tagged release, built and published by `.github/workflows/release.yml`
on push of a `v*` tag. `SHA256SUMS` beside the tarball on the
[release page](https://github.com/AndreZiviani/odi-oss/releases) is what to
check the download against.

- **Release images are keys-only.** `ROOT_PW=locked` (the release workflow's
  default; `docs/BUILDING.md`) ships root with no password at all — the
  `/etc/passwd` field is `!` and dropbear runs with `-s`, so ssh does not
  even offer a password prompt. A source build still defaults to a random
  per-build password unless it also passes `ROOT_PW=locked`. First access to
  a keys-only image is through the web UI (`confd`, port 80, `admin`/`admin`
  until you change it), whose SSH-key admin page adds a key for root;
  `docs/FLASHING.md` and `docs/ACCESS.md` have the exact steps. The three
  shapes (`locked`, `none`, the default password) are written by the new
  `image/gen-root-account.sh`, tested on its own by `test/root_pw_test.sh`.
- **confd v1.0.5.** Config page reorder, MIB class picker, text sweep in the
  web UI.
- **Prebuilt toolchain.** The gcc 16.2.0 / binutils 2.47 / uClibc-ng 1.0.59
  toolchain and the freestanding Debian cross-gcc are container images from
  the odi-toolchain repository, pinned by digest in `toolchain/images.env`
  and pulled on first use, instead of an hour of compiling into a local
  Docker volume. The same recipe builds them from source there, and the
  image built with them is identical file for file to one built with the
  old volume, build timestamps aside. The ISA audits are the shared ones
  in those images.
- **The boot script reads top to bottom.** `/etc/init.d/rcS` is the boot
  in order, 233 lines instead of 637, and `docs/BOOT.md` says why each
  stage is where it is. The development aids moved to
  `/etc/init.d/rcS.dev`, which ships in the image: `breadcrumbs.on`,
  `confirm-arp` and `pon-steps` work as before. The files only earlier
  images read (`bdgconn-probe`, `skip-steps`, `replay/`, `regtrace.*`) are
  ignored; `docs/SETTINGS.md` lists them. The switch init and omcid start
  whenever the switch driver is there, so an image built without
  `/etc/kernel-config` no longer comes up without OMCI.
  A failed `devmem` read in the optics setup no longer ends the boot
  before the PON steps.
- **The PLOAM password and the serial number stay out of the logs.** The
  boot logged each PON step with its argument, so `gponpw <password>` and
  `gponsn <serial>` reached the kernel log, the DRAM ramlog and, with
  `breadcrumbs.on`, the breadcrumbs file. The steps are now logged by name.
- **A fast metrics scrape no longer holds service back.** omcid builds the
  bridge connections one quiet second after the OLT stops sending, and it
  counted any CLI request as activity: a client polling `omcicli` every
  few seconds kept the stick in O5 with no service (on ISP1 all 497 OMCI
  frames were in by 44 s and no service was up at 467 s). The quiet second
  now counts OMCI frames only.
- **Quieter NIC.** The NIC state dump after the first open and the ring
  register read-backs now need `odi_nic.debug=1`, like its other traces.
- Internal restructuring with the boot register stream, the OMCI driver
  calls and the boot actions pinned by goldens: one register replay engine
  for the four captured sequences; the switch-core leaves in six files by
  topic and the bridge-connection derivation in its own; omcid's driver
  path and MIB handlers in five files by topic; the comment pass over all
  of them; the capture and replay helpers the 6.18 kernel cannot use
  deleted.
- **The driver bring-up scaffolding is gone.** The switches that only a
  bisection trial ever set: `modload.mask`, `sdkinit.mask`, `parity.table`
  and `parity.mask` on the config partition (each also armed a reboot 600 s
  into every boot, now gone too), the `platform_init_mask` kernel
  parameter, and the `init_parity`, `parity_add`, `sdkinit_mask`,
  `ds_encrypt`, `peek`, `poke`, `ddm` and `i2c` writes of `/proc/odi_omci`,
  whose one write is now `switch_init`. Registers are read and written with
  `diag register get/set`, the optics with `diag pon get transceiver`. A
  normal boot writes the same registers in the same order as before
  (checked on the host against a golden of the whole boot). The kernel
  config has four odi-oss symbols instead of ten. uImage 2.3 KB smaller.
- **`reboot` resets at once.** The kernel registers a restart handler that
  resets the board through the watchdog at its shortest timeout, about a
  third of a second after shutdown, instead of spinning until the
  watchdog window ran out. On ISP1, `reboot` to the stock image answering
  again went from about 115 s to about 75 s. The NIC DMA is stopped first,
  on an emergency restart too. `halt` and `poweroff` hang with the watchdog
  armed and reset about 42 s later.
- **Root password hash: SHA-512 crypt (`$6$`), not MD5 (`$1$`).**
  `image/build.sh` now hashes the per-build root password with
  `openssl passwd -6` at the algorithm's default rounds (5000, no `rounds=`
  tag). Both verifiers on the stick go through the same libc `crypt()`:
  dropbear's password auth calls it directly, and busybox login/su default to
  `USE_BB_CRYPT_SHA=y`. yescrypt (`$y$`) was considered and ruled out:
  uClibc-ng 1.0.59 has no yescrypt code at all, and dropbear's `crypt()`
  would return NULL for a `$y$` hash and lock root out over ssh -- the only
  way in, this device has no serial console. A crypt() microbenchmark cross-
  compiled with our toolchain and timed under a calibrated qemu-mips-static
  proxy lands around 0.2s per login check on this ~300 BogoMIPS core at the
  default rounds, well under the ~0.5s where fewer rounds would be worth
  considering, so the rounds stay at the default.
- **Two names the stock firmware chose, gone.** `/proc/rtk_init` is now
  `/proc/odi_init`, and `/proc/luna_watchdog` is now `/proc/odi_wdt`
  (`userland_ok` and `watchdog_flag` keep their names). `odi_omci`'s
  netlink transport rides its own protocol number instead of
  `NETLINK_USERSOCK`, so it no longer shares a family with unrelated
  users. Every consumer in this repo (rcS, `apply.sh`, tests, goldens,
  docs) moved with them; nothing on the wire or in the MIB path changed.

## odi-oss-260925-618z2 — 2026-09-25

On top of 618z1:

- **The MAC table.** `diag l2-table get all` reads the switch L2 lookup
  table back (a row is in use when the table status HIT bit says so after
  the read), and the web UI's "Read the MAC table" shows it. `igmpd -j`/`-l`
  add and remove a static multicast entry by hand; IGMP snooping itself
  stays off.
- **Boot confirmation without the network.** rcS confirms userland to the
  watchdog once its own steps have run, whatever address the host uses; the
  120 s deadline still resets a boot that hangs and still reverts a trial.
  `/etc/config/confirm-arp` brings back the ARP-on-.2 bar for development.
- `confd` v1.0.4.
- Tried on ISP1: O5, the 4 learned addresses listed, a manual multicast
  join and leave, exporter 8/8.

## odi-oss-260925-618z1 — 2026-09-25

The first release. Everything in `docs/IMPROVEMENTS.md`: Linux 6.18,
fully open — our own switch, GPON MAC, CPU-port NIC and OMCI transport
drivers, an independent implementation built straight into the kernel, no
proprietary kernel module anywhere in the image; our own OMCI daemon
(`omcid`) driving GPON provisioning end to end; watchdog rescue for a hung
kernel or a boot whose userland never comes up; the DRAM ramlog console for
reading a boot with no serial console; devtmpfs; seeded entropy; a current
dropbear with SSH keys and `scp`; per-build root password; the web UI
(`confd`) and the Prometheus exporter (`metricsd`) with
`gpon_omci_services`; one-shot trial boots with `fwu.sh` guards; our own
toolchain (gcc 16 / binutils 2.47 / uClibc-ng), building the kernel and
every userland binary; `make image-all` from a clean clone, and CI for
lint, the host tests, `diag` and the OMCI suite.

Tested: on ISP1, 15 of 15 consecutive trial boots and a clean 3-hour soak;
on ISP2, carrying a household's Internet link. Both as a one-shot trial
over the stock firmware (see "Status and limitations" in `README.md`).

What changed in the last round of trial builds before this release:

**Kernel** (`docs/KERNEL.md`)

- Linux 6.18.53. The patches to mainline files are down to 19 changed
  lines in 6 files (`tools/kernel-footprint.sh`): the RLX5281 probe case,
  the board's System type entry and the Makefile and Kconfig hooks.
  Everything else is in the `kernel/extra` overlay.
- The CPU is built as plain `CPU_R3000`, so mainline's own R3000 exception
  model, TLB and context-switch code run unchanged. Barriers go through the
  board's `__wbflush` (a `sync`), and our DMA ordering points use
  `rtl8686_sync()`, which does not depend on `CPU_HAS_SYNC`.
- The NIC DMA the loader leaves running is stopped in `prom_init()`,
  before the kernel owns any memory. Until then it could write received
  frames into page-cache pages: the cause of the rare boots that died with
  init or `login` killed by SIGBUS.
- The PON packet-buffer (PBO) downstream window is reserved from
  `0x016ff000`, where the hardware has it; it used to start one page
  higher.
- icache coherence: `flush_data_cache_page()` and `flush_cache_page()` on
  an executable mapping invalidate the whole icache, and `free_initmem()`
  flushes both caches before the init sections are freed.
- Register replay tables are firmware files (`/lib/firmware/odi/*.bin`,
  loaded with `request_firmware()` and released after use): about 484 KB
  of RAM and 8.5 KB of kernel image back.
- Config diet: AIO, signalfd, timerfd, eventfd, inotify, fhandle,
  fadvise/madvise, rseq, membarrier, cross-memory attach, the page monitor,
  core dumps, ethtool netlink and swap are off. `POSIX_TIMERS`,
  `FILE_LOCKING` and `VM_EVENT_COUNTERS` are deliberately kept.
- `print-fatal-signals=1` on the command line, so a process killed by a
  signal leaves its registers in the ramlog.
- The ramlog keeps the previous boot: `/proc/odi_ramlog_prev` and
  `/proc/odi_ramlog_prev_raw`, with a boot counter, slot, build id and last
  early crumb in a metadata block at the end of page A. Early crumbs are on
  by default; crumbs in core files are opt-in (`CRUMBS_CORE=1`,
  `kernel/618/debug/`).
- Locking over the shared switch engines: `odi_switch_lock` (mutex),
  `odi_switch_dsf_lock` (IRQ-safe leaf spinlock), `odi_i2c_lock` over the
  whole DDM byte sequence, and a lock for the watchdog enable writers.
- The OMCI netlink socket refuses senders without `CAP_NET_ADMIN`.
- NIC error paths: RX refill never leaves the hardware pointing at a freed
  buffer, a stopped TX queue is always woken, every DMA mapping is checked.
- Build warnings: 22, all GCC 16 "'retain' attribute ignored" in mainline
  networking files; none from our code.

**Userland**

- `diag` is our own CLI (`src/diag/README.md`). The commands
  the exporter runs are byte-compatible with the stock CLI, pinned by the
  golden files in `make test-diag`.
- Settings (`docs/SETTINGS.md`): the web UI offers the 21 keys this image
  reads, each with an apply class (LIVE, SERVICE RESTART, INTERRUPTS
  INTERNET, REBOOT); the 163 stock-only keys are shown read-only.
  `apply.sh` applies the management addresses live and the OMCI settings
  by re-ranging the ONU; `fwu_starter.sh` writes an uploaded image to the
  inactive slot from the UI; a second management address (`br0:2`); the
  OLT identity keys are reported only with `/etc/config/omci-identity.on`.
- `confd` v1.0.4 (the MAC table) and `metricsd` v1.0.3.
- IGMP snooping is off: `igmpd` ships, is not started, and its switch path
  does not work on this kernel.
