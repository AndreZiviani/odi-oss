# Changelog

Releases of the flashable image. Trial builds between releases are not
listed here.

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
