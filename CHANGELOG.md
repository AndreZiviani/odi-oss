# Changelog

Releases of the flashable image. Trial builds between releases are not
listed here.

## Unreleased

- gpon: `/proc/odi_gpon` shows the PLOAM content, for debugging an OLT that
  deactivates the ONU right after O5 (issue 29). Each `ploam_ring` entry now
  carries the ONU-ID and the 10 content bytes in hex; two new lines show the
  last Upstream_Overhead decoded (guard, preambles, pattern, delimiter, the
  pre-assigned delay flag and value in 32-byte units and in bits, power level)
  and the last Ranging_Time (EqD bits, protection path bit, the
  USF_MIN_RESP_DELAY value read and the MULTFRAME/INFRAME written). The kernel
  log gets one `event=ranging` line per O4 to O5 transition, on the same
  ratelimit as `event=onu_state`. No behaviour change; the ring grows by
  128 bytes of RAM and the decoded copies take about 50.
  The `ploam_ring` timestamps and `last_los_ms` are now milliseconds since
  boot; they were raw jiffies, which start near 2^32 and wrapped minutes
  after boot.

**Automated dependency bump.**
- linux 6.18.54 -> 6.18.55


- docs: repair text merged into CHANGELOG.md by union-resolved rebases: the
  v1.1.0 section had lost the entries of five changes merged the same
  morning (the ramlog reset reason and `flash get` of an unset key, `nv
  commit` and `slot-state.sh`, the autonomous entities in the MIB dumps, the
  link and provisioning events, `omcli provision`), restored below from their
  commits; a stray blank line inside the v1.2.0 list is removed, and
  test/qemu/run-qemu.sh has its blank line back before the diagnostics scenario.

## v1.2.1 — 2026-09-29

- Pins odi-sfp-exporter v1.2.1 (was v1.2.0): its alert rules no longer count
  the port-3 undersize frames (one per upstream OMCI reply, harmless).
- AGENTS.md has a release checklist: every change since the last tag has an
  entry, no merge debris, `Unreleased` moved into the tag section, and the
  published notes checked (odi-oss also checks its confd/metricsd pins).
- Fixes the NIC sending PAUSE frames toward the switch CPU port (port 3) with
  its receive ring empty: 1-6 per second on both test lines, about 12 a
  minute plus bursts of up to 50 a second, where the stock firmware sends
  none. The register next to the flow-control thresholds is the index of
  the last receive descriptor the CPU handed back, which the hardware
  compares against its own; `odi_nic` wrote it once at init and never
  again, so once per lap of the ring the hardware saw itself within the
  threshold of it. The driver now writes it after every poll, starts it at
  depth - 1, and uses the stock thresholds (assert at 16 descriptors left,
  release at 48) as a margin in descriptors. This replaces the v1.0.2
  rescale of those thresholds, which only shrank the window. The ring
  indices are also reset when the rings are reallocated, to match the
  hardware after its reset. The index is written after every descriptor,
  as the stock does, not once per poll: a poll that took the whole NAPI
  budget (the ring depth) wrote the same byte as the poll before.
  `test/odi_nic_hw_test.c` checks the index against a model of the
  comparator, idle and under a flood.
- Fixes a flood of frames to the stick (`ping -f` at 1400 bytes, ISP1)
  leaving the management path dead until a power cycle: the `odi_nic`
  interrupt-storm guard counted every interrupt, and with NAPI a CPU that
  keeps up takes one interrupt per frame, so 2500 frames a second for 2 s
  masked the NIC and disabled its interrupt for good. It now counts only
  interrupts that schedule no RX work (a status bit that fires again, or
  the line firing while the poll is already scheduled); a line with
  nothing set for us is left to the kernel spurious-interrupt detector.
- New watchdog rule, CPU-port RX: `odi_wdt` resets the board when the switch
  keeps offering frames to the CPU port (port 3 delivered, discarded or
  held back by our PAUSE, from the switch MIB) for 30 s while the NIC takes
  none, logging one line of NIC state first; the ramlog reason is
  `wdt_cpu_rx`. An idle CPU port never counts. A trial that wedges the
  management path now reverts by itself instead of needing a power cycle.
  Host-tested in `test/odi_wdt_test.c`; `docs/SETTINGS.md`, "Watchdog
  rules".

## v1.2.0 — 2026-09-29

- Pins odi-ui confd v1.2.0 (was v1.1.1): light/dark toggle, equal readout
  sizes, the VLAN keys as LIVE, rows for ONU_HW_VERSION, OLT_SW_DOWNLOAD and
  OMCI_UNKNOWN_ME_OK, and every setting and risky option value saying whether
  it disrupts the link.
- Upstream OMCI and upstream data survive a second provisioning in one boot.
  The kernel placed each upstream priority queue (driver command 23) by a
  count of the calls since boot and ignored the direction and T-CONT in its
  argument, so the second ISP1 provisioning (an omcid identity reload, a
  respawned omcid, any re-registration or QoS rebuild) wrote T-CONTs 5 to
  17: it overwrote the scheduler word of the OMCC T-CONT and cleared the
  OMCC and the five data T-CONTs from the T-CONT enable word. The ONU still
  ranged to O5, but the OLT got no OMCI answer and deactivated it every
  30 s until a reboot (seen on ISP1 at the second identity reload). A
  downstream queue now writes only its port queue map, an upstream one the
  words of the T-CONT it names, T-CONTs 0 to 15 and one queue per T-CONT
  (the captured shape; anything else is refused and logged instead of
  landing on another T-CONT). `test/odi_reregister_test.sh` provisions ISP1
  three times in one boot, with two deactivations and an OLT deallocation,
  and requires the OMCC path and every upstream queue word to be what the
  first provisioning left.
- CHANGELOG.md: three stray merge-conflict marker lines removed.
- `apply.sh omci` no longer stops omcid and starts its own beside the one
  init respawns. That was the bug: every respawn of `svc-omcid.sh` found the
  stray omcid holding redirect type 1, exited 1, and its re-provisioning path
  ran `gpondeact`/`gponact` -- 44 exits in about two minutes on hardware, the
  ONU cycling O1 to O3 and never reaching O5 until the stray was killed. The
  script now sends SIGHUP to the running omcid (`pidof`), waits, bounded, for
  the outcome in `/var/run/omcid-reload` and reports it (`apply.sh omci
  [seconds]`, 20 s by default, inside the web UI's 25 s); it exits 1 when no
  omcid runs or none answers, and never starts a process. Init owns omcid; a
  genuine crash still respawns and re-provisions as before.
- omcid reloads on SIGHUP (the Unix convention for rereading configuration)
  instead of being restarted: it rereads the store (`lastgood*.xml`,
  `odi.conf`, `omci-identity.on`) and compares it with what it runs. Nothing
  changed: logged, nothing done. Only `VLAN_CFG_TYPE`, `VLAN_MANU_MODE`,
  `VLAN_MANU_TAG_VID` or `VLAN_MANU_TAG_PRI`: the bridge connections and
  VLAN rules are torn down and built again in place from the MIB it holds,
  by the same code as the first build, with the ONU staying in O5; the rules
  are byte for byte a fresh omcid's (`src/omci/reload-test.sh` compares both
  ISP sessions, with a new tag and with transparent mode). `GPON_SN`,
  `GPON_PLOAM_PASSWD`, the LOID keys, `OMCI_SW_VER1/2`, `GPON_ONU_MODEL`,
  `OMCC_VER`, `OMCI_VENDOR_PRODUCT_CODE`, `ONU_HW_VERSION`, the identity
  switch or `OMCI_UNKNOWN_ME_OK`: omcid itself writes `gpondeact`, clears its
  MIB as a MIB reset does, holds three seconds, gives the driver the new
  serial number and password (`gponsn`, `gponpw`), writes `gponact` and lets
  the OLT provision it again, in the same process (no respawn, no snapshot
  resume). The re-registration is a state machine in the main loop: nothing
  sleeps, the watchdog ping and the CLI keep being served, a second SIGHUP
  during it folds into one more reload, and it gives up after 150 s. New
  lines `event=reload`, `event=reload_step` and `event=reload_done` (never
  rate limited; key names, never values) and `/var/run/omcid-reload` say
  what happened. `omcid -i file -j seconds` are test flags.
- The kernel takes a new serial number after boot: `gponsn` while the ONU is
  deactivated now rewrites the PLOAM slot the boot activation armed (the
  serial-number slot of `gpon_init.bin`, found by its `sn_word` records)
  instead of only the driver's copy, which the OLT would then have failed to
  match on the next Assign_ONU-ID. In any other state it answers `-EBUSY` and
  changes nothing. Before, a new `GPON_SN` needed a reboot.
- The four VLAN keys are LIVE now: they apply through `apply.sh omci` without
  dropping the fibre service (odi-ui schema moves with this).
- omcid takes the T-CONT Alloc-IDs from the OLT instead of a table captured
  on one ISP1 session. A T-CONT the OLT sets over OMCI keeps what it set
  (G.988 9.2.2, as ISP2 does); one it never sets (ISP1 sets none) is bound to
  the next Alloc-ID the OLT assigned by PLOAM, in assignment order, from
  `alloc_ids` in `/proc/odi_gpon`, and reads 0x00FF (unassigned, not
  programmed) when there is none. A MIB upload and a Get of the AllocID
  answer the same. A change in the PLOAM list reprograms the T-CONTs bound
  to it, and logs `event=alloc_ids`. Before, an OLT assigning any other
  Alloc-IDs got ISP1's reported and sent to the driver. `omcli tcont` shows
  each T-CONT's Alloc-ID and its source; `omcid -g` reads a file in place
  of `/proc/odi_gpon` for the tests.
- omcid no longer treats a deleted or MIB-reset row as live: the store walk
  behind the upstream QoS and bridge-connection rebuilds handed out free
  slots, which keep the class and data of their last row. After one OLT
  session and a MIB reset, the next rebuild programmed every GEM port and
  T-CONT of the old session again (seen under qemu: the ISP1 session, then
  the ISP2 one, into one omcid).
- The kernel releases an Alloc-ID the OLT deallocates (Assign_Alloc-ID type
  255, G.984.3 9.2.3.9): its CAM row is deleted and freed for the next
  assignment, and it leaves `alloc_ids`. Before, a deallocation was stored as
  one more assignment.
- omcid logs what it does not model, once per boot: `event=unknown_me
  class=<n> op=<create|set|get|delete|get_next|test>` for a managed entity
  class it has no model for and `event=unknown_msg type=<n> class=<n>` for a
  message type it does not handle, each first sighting one syslog line (the
  usual 20-a-minute event limit applies), every sighting counted in
  `/var/log/omcid-unknown.txt`, which `diag-bundle.sh` collects as
  `odi-diag/log/omcid-unknown.txt`. A respawned omcid reads the file back
  and does not log the same thing twice in one boot. The ISP1 OLT session
  already shows three: class 351, and message types 17 and 1 sent to ONU-G.
- New odi-only key `OMCI_UNKNOWN_ME_OK` (`/etc/config/odi.conf`, default
  off): `1` answers a Create, Set or Get of a class omcid does not model with
  success instead of "unknown entity", the counterpart of the stock
  `OMCI_FAKE_OK`, for an OLT that stalls on the error. Nothing is stored, a
  Get answers no attributes, MIB data sync counts the faked writes. Read at
  omcid start: INTERRUPTS INTERNET (`apply.sh omci`). `docs/SETTINGS.md` has
  the risk.
- omcid no longer counts a refused Set (an unknown class) in MIB data sync:
  the OLT counts only the writes it was told succeeded.
- omcid accepts a software download from the OLT instead of refusing it, and
  never installs it: Start, Download section (acknowledged per G.988 window,
  a window with a missing section refused so the OLT resends it), End (the
  image CRC-32 and size checked), Activate and Commit are answered with
  success, the image is counted and discarded, and the software image entity
  reports the flags the OLT expects (`is_valid`, `is_active`,
  `is_committed`) for the rest of the boot. Nothing is written to flash or the
  U-Boot environment and the stick never reboots. A new odi-only key,
  `OLT_SW_DOWNLOAD` in `/etc/config/odi.conf`: `accept` (the default) or
  `reject` (the old "not supported" answers), read at every download. Every
  step is an `event=sw_image` line with the sections and `crc=ok|bad`. ISP2's
  End software download to ONU-G at every session start is no longer logged
  as a software image step. `make test-omci` runs both modes, under
  `qemu -strace` for accept, and asserts no flash, exec or reboot.
- Transparent VLAN handling, documented as a choice (`docs/SETTINGS.md`,
  "VLAN handling"): `VLAN_MANU_MODE` 0 is "router tags" -- the stick adds
  and removes no tag and every frame passes with its own tags, so one port
  can carry internet, IPTV and voice as router subinterfaces; 1 is "stick
  tags", the manual `VLAN_MANU_TAG_VID`. No new key: 0 is the stock
  firmware's own "no manual tag", so both slots agree. On a line without
  class 84 (ISP2) transparent builds forward-all rules, which now also put
  the UNI and PON on every VLAN row; on a class 84 line (ISP1, FwdOp 0x10)
  the listed VIDs pass tagged and untagged frames are discarded, as the OLT
  provisions. `omcli vlan` prints the mode (`handling` line) and omcid says
  `manual vlan off (transparent)` at start. `src/omci/vlan-test.sh` (in
  `make test-omci`) builds both ISP sessions in both modes.
- omcid reads the class 84 forward operation (FwdOp, G.988 table 9.3.11-1)
  and every entry of its VLAN filter list, up to twelve. Before, it built one
  VID filter from the first entry whatever the code, which is only right for
  0x10 (the mandatory code; ISP1 sends it with one entry per port, so
  nothing changes there) and dropped every VLAN after the first on a
  multi-VLAN port. Now: 0x00 forwards everything, 0x01 untagged only,
  0x02/0x15 tagged frames of any VID, and the positive filters -- by VID
  (0x03/0x04/0x0f/0x10/0x1c/0x1d), priority (0x07/0x08/0x11/0x12/0x1e/0x1f) or
  both (0x0b/0x0c/0x13/0x14/0x20/0x21) -- one rule per entry, plus an
  untagged rule for the codes that bridge untagged frames (the manual
  add-tag rule when the manual tag is on). Negative filtering (0x05/0x06/
  0x09/0x0a/0x0d/0x0e), filtering by TCI and MAC address (0x16-0x1b) and
  codes past the table are built as 0x10 and logged once per code as
  `event=vlan_fwdop code=0x.. inst=.. result=unsupported built_as=0x10`.
  Every code has a check in `make test-omci`; the ISP1 and ISP2 driver-call
  goldens are unchanged.
- The switch driver gives a bridge rule that passes tagged frames of any VID
  (forward-all, tagged-only, a priority filter) VLAN table membership on
  every row 2..4094 (UNI and PON, tagged), and takes it off every row when
  the rule is released. Those rules have no VID of their own to put a row
  on, and the rows the cmd 51 sweep leaves are 0; a VID filter keeps its own
  row as before. No stock capture has such a rule, so no golden moves;
  `test/odi_switch_bdgconn_vlan_test.sh` pins the rows.

## v1.1.1 — 2026-09-29

- Pins odi-ui confd v1.1.1 (was v1.1.0): the trial banner no longer claims
  what the other partition holds; "Keep" is "Commit" everywhere, with a
  "What committing means" note under the partitions; a smaller receive
  readout; forwarding rates to 2 decimals; the T-CONT card counts only
  assigned Alloc-IDs.
- The README web UI screenshot now links odi-ui (single source) instead of
  a copy in docs/images.

## v1.1.0 — 2026-09-29

- Pins odi-ui confd v1.1.0 (was v1.0.8): the redesigned web UI (Status,
  Config with subtabs, OMCI, System), a trial-boot banner with "Keep this
  image", `GET /api/diag` for the diagnostics bundle. Pins odi-sfp-exporter
  v1.2.0 (was v1.1.2): `gpon_boot_count`, `gpon_last_reset_reason`,
  `gpon_config_info`, `gpon_uncommitted` and the slot series,
  `gpon_provision_*`, and the alert rules in its `prometheus/alerts.yml`.
- The kernel records why each boot ended, and the next boot of this image
  shows it: `/proc/odi_ramlog_prev` now ends its `previous boot:` line with
  `reason=` -- `wdt_client:<name>`, `wdt_mem`, `wdt_userland` (written by
  `odi_wdt` just before it forces the reset), `reboot`, `halt`, `poweroff`
  (a reboot notifier), `panic` (a panic notifier), `oops` (a die notifier,
  kernel mode only), `power` when neither ramlog page survived (DRAM lost: a
  power cycle), and `unknown` when the pages survived but nothing recorded a
  reason (a hang the hardware watchdog caught). The existing fields and
  lines are unchanged. It lives in a new 32-byte reason block in page A
  just before the metadata block, which keeps its offset (format 2), so page
  A now keeps the first 3984 bytes of the log, not 4016.
  `tools/memprobe/ramlog-read.sh` decodes it; host tests cover the block,
  the render and the watchdog rule chosen when several fire at once.
- `flash get` on an odi-only key that holds no value (`SYSLOG_SERVER`,
  `NTP_SERVER`) now prints `KEY=` and exits 0, like a stock key present with
  an empty value, instead of `GET fail.` and 1. A name that is no key at all
  still fails. `svc-syslogd.sh` and `svc-ntpd.sh` read both the same way.
- `METRICSD_BIN=<binary>` puts a local odi-sfp-exporter build in the image
  instead of the pinned release (`src/fetch-releases.sh`), for trials of an
  unreleased exporter.
- docs/TOOLS.md lists the exporter metrics `gpon_boot_count` and
  `gpon_last_reset_reason{reason,client}`, which read the reason above
  (odi-sfp-exporter, after v1.1.2).
- New `nv commit <slot>`, now the documented way to commit a trial by hand
  once you have checked it (`docs/FLASHING.md`, "Committing"): `sw_commit`
  into both U-Boot environment copies, the primary first and read back byte
  for byte, only then the fallback, then both re-read. Refuses a slot that
  is not running (`root=` in `/proc/cmdline`) or not `sw_active`, and an
  environment without two valid copies; never writes `sw_active`.
  Host-tested against a fake flash that fails at every step
  (`src/nv/test/test_commit.c`, now part of `make test-host` with the
  existing `nv` block tests).
- An uncommitted image now says so. `/etc/scripts/slot-state.sh`, a new
  `/etc/inittab` `once` entry, reads the U-Boot environment at boot (never
  writes it) and records the running slot (`root=` in `/proc/cmdline`),
  `sw_active`, `sw_tryactive`, both copies of `sw_commit`, the next boot
  slot and `uncommitted` (1 when either copy names another slot) in
  `/var/run/odi-slot`, a `KEY=value` file for the exporter and the web UI
  (format in `docs/TOOLS.md`, "Slot state"). While uncommitted it puts a
  `TRIAL BOOT` (or `HALF COMMITTED`, naming the copy that disagrees)
  notice with the exact `nv commit` command in `/etc/motd`, which dropbear
  prints at every interactive login, and in syslog. Nothing commits a
  trial automatically: the update procedure ends with a manual `nv commit`
  after checking the trial (`docs/FLASHING.md`, "Committing"). Host-tested
  against a stub nv (`test/slot_state_test.sh`); test-qemu checks the file
  and the login banner under the real busybox and dropbear.
- Fixes `omcicli mib get <class>` answering `0 rows` for every entity the
  ONU creates for itself (ONT data, SWImage, ONT2-G, the T-CONTs, ANI-G, the
  traffic schedulers, ...), found on ISP1: the dump walked only the store
  rows, which exist for an autonomous entity only once the OLT sets one of
  its attributes, while a MIB upload listed all 301. So the web UI showed
  an empty software version and no T-CONTs. `omcicli mib get`, `omcli mib`
  and `omcicli mib getattr` now cover every entity, autonomous and
  OLT-created, sorted by class and instance, with the values a Get returns
  (what the OLT wrote, else the built-in answer), not the zeroes of an
  unwritten row. Every dump now has one shape: the vendor-rendered classes
  (84, 131, 171, 256) print their banner and blocks as before, every other
  class prints omcid's own block, and every answer, empty or not, ends with
  one `N rows` line (the vendor-rendered ones had none, so an empty answer
  printed nothing at all). A name that is no class (`mib get Foo`) is
  refused instead of dumping the whole MIB. test-omci covers an autonomous
  class, the vendor-rendered and the empty shapes, and `getattr`.
- **Link and provisioning events in syslog: after an outage the log says
  who started it.** One greppable `event=` line per event, `key=value`
  after it (docs/TOOLS.md, "Link and provisioning events", has the table
  and how to read an outage with it). From the GPON driver, as printk
  through klogd: every ONU state change (`event=onu_state from=O5 to=O2
  in_state_s=... cause=deactivate_onu_id side=olt`), where `side` is `olt`
  (a PLOAM message: deactivate, disable serial number, ranging), `timer`
  (TO1/TO2), `line` or `local` (gponact/gpondeact), and the downstream LOS
  bit going on and off (`event=los`), sampled at every interrupt and every
  BER interval, so a fibre pull in O5 shows within 10 s. From omcid, to
  `/dev/log` (facility daemon, the syslog(3) datagram; omcid has no libc)
  and to omcid.log: its start (`run=boot|restart`, `mode=resume` or
  `reregister` and why), an OLT or CLI MIB reset, the MIB upload, the first
  and last write of a provisioning burst with its totals (`provision_end
  creates=82 sets=102 ...` for the ISP1 session), and the reboot and
  software-download requests it refuses. Rate limited on both sides (30
  and 20 lines a minute, each saying what it dropped); nothing per
  message. The old rate-limited `odi_gpon: state O4 -> O5 (onu_id 3)`
  line is replaced by `event=onu_state`. test-host covers the cause
  mapping, test-omci (`events-test.sh`) every omcid line against the ISP1
  session, and test-qemu that kernel messages and omcid lines reach
  `logread`.
- New `omcli provision` and `alloc_ids` in `/proc/odi_gpon`, for the
  exporter's `gpon_provision_*` metrics, so an ISP plan change (speed tier,
  VLAN, T-CONT) shows on a graph. `omcli provision` prints one line per T-CONT
  omcid programmed, GEM port network CTP, VLAN (from the VLAN tagging filter
  data and the extended VLAN tagging table, matched or set, sorted by VID) and
  traffic descriptor, and a summary line. `/proc/odi_gpon` gains `alloc_ids`:
  the Alloc-IDs the OLT assigned by Assign_Alloc-ID, the T-CONT list the
  exporter uses. omcid's own T-CONT Alloc-IDs can differ from it: for an OLT
  that never sets them over OMCI (ISP1), a Get answered the value captured from
  the stock stack. `METRICSD_TAG= METRICSD_BIN=<binary>` builds an image with a
  local exporter, as `CONFD_BIN` does for confd. `events-test.sh` checks the
  provision answer for the ISP1 session.
- Adds `/etc/scripts/diag-bundle.sh`, the diagnostics bundle: the previous
  boot ramlog, `/var/log/*`, dmesg, `/etc/odi-build`, `/etc/version`,
  `/proc/odi_wdt/*`, uptime, meminfo, mounts, `ps`, the slot variables from
  both environment copies, an exporter scrape and the config store, in one
  tar.gz. Every password is redacted in the config copy (a fixed key list
  plus any key named like a secret, `docs/TOOLS.md`) and each secret value is
  then scrubbed from every file in the bundle, as text and hex. Busybox only;
  every step under `timeout`, logs capped at 1 MB, the bundle at 2 MB. The web
  UI serves it as `GET /api/diag` from the next odi-ui release; this image
  still pins confd v1.0.8. `make test-qemu` plants secrets and asserts none
  of them reaches the bundle.
- Adds `tools/config-backup.sh`: a host-side (POSIX sh, Linux and macOS)
  backup of the config store through the web UI `GET /api/backup`, keeping a
  dated copy only when a setting changed and the newest `ODI_KEEP` (30), the
  credential from a file or the environment, every download bounded by
  `curl -m`, non-zero exit on failure. `docs/SETTINGS.md`, "Backing up from a
  host", has cron and systemd-timer examples.

## v1.0.8 — 2026-09-28

- Fixes `SYSLOG_SERVER` and `NTP_SERVER` never saving from the web UI on a real
  stick (`did not stick, device holds ''`; `flash get` printed `GET fail.`). Both
  are keys the stock firmware never had, and `flash` only edited keys already in
  `lastgood*.xml`. They now live in `/etc/config/odi.conf`, a plain `KEY=value`
  file written by temp file and rename; `flash set/get`, `flash all cs` and
  `svc-syslogd.sh` / `svc-ntpd.sh` use it for those names, and an empty value
  clears the key. Stock keys and the XML are untouched, and the stock image
  ignores the file. test-qemu now runs the real `flash` against a writable
  config dir (it used to seed a fixture XML) and asserts a UI save of
  `SYSLOG_SERVER` reaches `syslogd -R`.
- Pins odi-ui confd v1.0.8 (was v1.0.7): the Config page can clear
  `SYSLOG_SERVER` and `NTP_SERVER` (an empty value removes the key).

## v1.0.7 — 2026-09-28

- Pins odi-ui confd v1.0.7 (was v1.0.6): `SYSLOG_SERVER` and `NTP_SERVER` are
  now set from the web UI (Config, other). Saving one runs `apply.sh syslog` /
  `apply.sh ntp`, so syslogd or ntpd restarts under init respawn with the new
  value; no reboot, fibre service untouched. confd refuses empty values, so
  clearing either key still needs a shell.

## v1.0.6 — 2026-09-28

Same changes as the v1.0.5 tag, which was never released: its release run
failed at the SBOM attestation because the CycloneDX SBOM had no
`serialNumber`, which the attestation action requires. `tools/generate-sbom.sh`
now emits one, derived from the version and the commit so the SBOM stays
reproducible.

**A respawned omcid resumes instead of getting re-provisioned: zero
outage, not eighteen seconds.** v1.0.4's `omci-respawn-reprovision.sh` made
a respawn recover, but through a visible re-range (O5 -> O1 -> O5).
`omcid` now snapshots its whole MIB -- every managed entity, the MIB data
sync counter, and the bookkeeping that maps an entity to what is actually
programmed in the switch (GEM flow ids, T-CONT map, broadcast flow,
service table) -- to `/var/run/omcid-mib.snap` after every Create, Set or
Delete, and again after the quiet-second `us_qos_rebuild()`/`bdgconn_rebuild()`
that actually fills the flow, T-CONT and service-table bookkeeping (the
message handler alone snapshots too early to catch it), written atomically
with a version header and a CRC32. A respawned
`omcid` whose PON is still O5 and whose snapshot names this same device
loads it by plain memory copy and resumes answering -- no re-registration,
and not one switch-programming driver call, because the datapath was
never touched. `omci-respawn-reprovision.sh` waits briefly on `omcid`'s own
decision (`/var/run/omcid-resume-decision`) and skips its deactivate/
reactivate sequence entirely on a "resumed" decision; anything else (no
snapshot, a mismatched device, not O5, or a MIB the OLT has since reset --
which deletes the snapshot) falls back to the v1.0.4 path unchanged. See
docs/BOOT.md, "Resume without re-registration", and
`src/omci/resume-test.sh`, the driver-call golden replayed across a
`kill -9` and respawn.

**Release CI: no more double-publish on a tag, plus SBOM and attestations.**
A tag has landed on `release.yml` twice before, the second run failing at
`gh release create` because the first already made the release; the workflow
now serialises same-tag runs (`concurrency:`) and its publish step is
idempotent (edits and re-uploads with `--clobber` if the release already
exists, instead of failing). Every release now also ships a CycloneDX SBOM
(`tools/generate-sbom.sh`, built from the pins already in this tree) and
build-provenance + SBOM attestations for the tarball, its checksums and the
SBOM itself; `docs/FLASHING.md` says how to verify them.

**Reproducibility check, and a real kernel non-determinism it found.** `ci.yml`
gained a `reproducible` job (tags and `workflow_dispatch` only -- a full image
build twice is too slow to run on every PR) that builds the same version in
two clean checkouts, each with its own docker build-cache volume, and diffs
the tarballs member by member (`tools/compare-images.sh`), tolerating only the
documented build-time stamps (`etc/version`, `etc/odi-build`, busybox and
every applet hardlinked to it, and the `/etc/passwd` that `ROOT_PW=locked`
writes). Its first real run caught a genuine bug: `kernel/build.sh` set no
`SOURCE_DATE_EPOCH`/`KBUILD_BUILD_TIMESTAMP`/`KBUILD_BUILD_USER`/`KBUILD_BUILD_HOST`,
so every kernel build embedded its own real build time, host and account
(`scripts/mkcompile_h`) and no two builds of the same commit ever produced
the same `uImage`. Now pinned: the timestamp from the commit being built,
fixed strings for the user and host.

**Weekly dependency check.** A new `dependency-bump.yml` (`tools/bump-deps.sh`)
checks the Linux point release, busybox, dropbear, iproute2, the three
odi-toolchain image digests and the odi-sfp-exporter/odi-ui release tags
against upstream, verifies whatever it finds the way the existing fetch
scripts always have, runs the test suite against the result, and opens a PR
only if that passes. Its first real run already found and verified a kernel
point-release bump, linux 6.18.53 -> 6.18.54, included in this change.
**An opt-in NTP client.** The stock image has no RTC and no NTP client at
all. `svc-ntpd.sh`, a new
respawn entry, starts busybox `ntpd` in the foreground against
`NTP_SERVER` (docs/SETTINGS.md) only while that key is set; unset, it runs
the same off-flag placeholder every other disabled service uses, so the
static inittab entry does nothing rather than needing to be commented out.
`apply.sh ntp` (SERVICE RESTART) starts or stops it live, no reboot,
whenever the setting changes. Covered end to end in `test-qemu`: the
harness points `NTP_SERVER` at the qemu user-net gateway address and
checks the guest clock is actually corrected against a host-side NTP
responder (skipped, not failed, when the build host has none).

**syslogd and klogd, with a circular buffer `logread` reads, plus optional
remote forwarding.** The stock image has neither a syslog daemon nor
anywhere central `logread` can read from.
Two new respawn entries,
`svc-syslogd.sh`/`svc-klogd.sh`, start busybox syslogd/klogd in the
foreground with a 64 KB circular buffer (`-C64`), the same off-flag and
config-store-read idiom every other `svc-*.sh` here uses. Setting
`SYSLOG_SERVER` in the config store (docs/SETTINGS.md) adds `-R host:port
-L`: forwarded remotely, kept locally too. `dropbear` now logs through
syslog like everything else here, instead of straight to
`/var/log/services.log` (dropped its own `-E`, which is what was
redirecting it away from syslog); `confd`, `metricsd` and `omcid` have no
syslog option of their own to switch on, so their existing `/var/log/*.log`
files are unchanged. `apply.sh syslog` (SERVICE RESTART) restarts syslogd
without a reboot or an OMCI interruption.

**The optics model gets alarm/warning flags and an optical LOS status, plus
a scriptable host-side transceiver behind the same modelled I2C controller
the driver tests already use.** `pon get transceiver alarm-status` reads
SFF-8472 A2h's alarm and warning flags (bytes 112/113, 116/117) and the
RX_LOS status bit (byte 110) in one contiguous 8-byte transaction
(`odi_ddm.h`'s new `ODI_DDM_ALARM_STATUS` selector, `hw_transceiver_alarms_get()`
on the diag side). `test/odi_optics_model.h` adds a full scriptable SFF-8472
device (A0h/A2h pages, an "absent module" mode, and the IO_GPIO_EN routing
gate from `docs/kb/dfp34x-optics-on-i2c-port1-gated-by-io-gpio-en.md`) behind
`odi_switch_mock.h`'s existing register write-hook, so a host test can drive
a real I2C transaction through `odi_i2c_read_bytes()`/`odi_ddm_get()` into
NACK, low-rx-power alarm/warning, or LOS scenarios rather than a fixed byte
table. The exporter contract (`src/diag/test/exporter.txt`) gains the new
command and three scripted-scenario goldens (rx power drifting to about
-28 dBm, LOS asserted, module absent). See `docs/HACKING.md`, "Optics
model", for how to script a scenario.

## v1.0.4 — 2026-09-28

**A respawned omcid gets provisioned again, not just restarted.** Hardware
finding (rc5, claro, 2026-09-28): after `kill -9 omcid`, init respawns it
within 8 s and it pings and serves `omcicli`, but `gpon_omci_services`
stays 0 for 200+ s -- the OLT had already provisioned this ONU and does not
re-send the MIB to a fresh omcid with an empty one. `svc-omcid.sh` now
tells a respawn apart from the boot's first start with a marker under
`/var/run` (tmpfs, gone at the next reboot) and, on a respawn, backgrounds
`omci-respawn-reprovision.sh`: deactivate, wait for the new omcid to
register, re-apply the PLOAM password, hold, reactivate -- the same
hardware-proven sequence the web UI's `apply.sh omci` already uses, forcing
the OLT to re-range and re-provision. The two callers now share that tail
as `rcs-lib.sh`'s `omci_reactivate` rather than keeping two copies. The
respawned omcid process itself is never touched -- init already supervises
it as a `respawn` entry, so killing or restarting it from here would only
trigger another respawn. `docs/BOOT.md` has the new "Respawn
re-provisioning" section.

**A registered watchdog client is armed at registration, not on its own
first ping.** Hardware trial (rc4, claro, 2026-09-28): omcid answered two
omcicli commands after registering with odi_omci, then stopped -- 0x800
filled to its 64-message cap, `gpon_omci_services` stayed 0 for 5+ minutes,
and `/proc/odi_wdt/clients` showed `armed=0` at 279 s uptime: a client that
never pings at all was never armed, so odi_wdt's per-client deadline never
had anything to check, and a daemon stuck before its first ping was never
reset. Investigated thoroughly (the rc4 diff, three separate
`qemu-mips-static` reproduction attempts combining a CLI flood -- matching
metricsd v1.1.2's/confd v1.0.6's own 2 s-timeout-then-SIGKILL pattern --
with the real mib-reset/mib-upload/mib-upload-next sequence from the
trace, single- and dual-flooder); none reproduced the stall itself
(qemu-user has no netlink, so this needs either real line timing/volume or
a mechanism outside the CLI/MIB-volume angle). Closed the watchdog gap
regardless, since it is a real, independently-justified bug:
`odi_wdt_client_register()`
(`kernel/extra/drivers/net/ethernet/odi/odi_wdt.c`) now takes the
registration uptime and arms immediately, so "never pinged" is caught by
the same deadline as "pinged once and then stalled". This changes the
previously-intentional "registered but unarmed never resets" behaviour (the
modules.off dev case), so `rcS` now only registers "omcid" with odi_wdt
under the same two conditions `svc-omcid.sh` gates the actual exec on
(`/etc/config/modules.off` absent and `/bin/omcid` executable) -- a dev
image that will never start omcid no longer registers it either, so there
is no legitimate registered-but-silent-forever case left. `test/odi_wdt_test.c`
rewritten to match (`test_registered_but_never_pinged_client_still_resets`,
`test_client_reregister_resets_the_clock`).

**`mq_send()` (`src/omci/omci_msgq.h`) is bounded too.** The one SysV IPC
call left in this daemon's whole "bound every wait" sweep that was still a
bare blocking `msgsnd` -- every client (`omcli`/`omcicli`, so confd and
metricsd both) uses it to enqueue a request onto 0x800 or the native queue,
and it had no `IPC_NOWAIT` of its own. metricsd's/confd's own timeouts
SIGKILL the *child*, which does not stop it parking in `msgsnd` first if
the queue omcid owns is momentarily full -- exactly when several such
clients pile in at once. Now `IPC_NOWAIT` plus a bounded retry (100 x 5 ms),
the same shape omcid's own reply paths already use (`vqsrv.c`'s
`vq_reply()`, `clisrv.c`'s `cli_chunk()`).

**Every wait on another process is bounded now, not only the two hit on
hardware.** Audited every `diag`, `omcli`/`omcicli`, `arping`, `nv` and
`/proc` verb write/read in `rootfs/skeleton/etc/` for an unbounded wait.
Added `write_proc_bounded`/`read_proc_bounded` (`scripts/rcs-lib.sh`,
`timeout $PROC_WRITE_TIMEOUT_S sh -c '...'`, 5 s default) for the bare
`echo verb > /proc/odi_init`/`/proc/odi_omci` writes a plain `timeout`
wrapper cannot reach (they are the calling shell's own `write(2)`, not a
forked command) -- used by the `odi_wdt` register write (`rcS`),
`rcs-lib.sh`'s PON steps and `switch_init`, and `apply.sh`'s
`gpondeact`/`gponpw`/`gponact`. `fwu_starter.sh`'s `nv getenv sw_active`
fallback gained a plain `timeout 5`. `docs/SETTINGS.md` gained a "Bounded
waits" section listing the pattern per case, including two documented
exceptions: `rcS.dev`'s dev-hook diag probes (a process parked in an
uninterruptible-sleep kernel wait ignores `timeout`'s signal too), and
`rcS`'s 23-step switch SDK-init loop -- every boot, no exception, and
`write_proc_bounded` would have turned each step's zero-fork builtin
`echo`/`cat` into a `timeout`+`sh` fork pair, close to 90 extra fork/execs
on the path this image has fought hardest to keep fast (rc2 to rc3: 85-90 s
boot down to 31 s), for a step that has never been observed to hang; the
existing before/after crumb already names a stuck one in the ramlog if it
ever does. No change needed in `network.sh` or `rcS.dev`'s confirm-arp
path -- both already wrapped every `diag`/`arping`/`/proc` call in
`timeout`.

**`confd` pinned to v1.0.6.** Same rule applied on the odi-ui side: bounded
every `run_to_buf`/`run_script_to_buf` child wait in the confd daemon
itself (omcicli, diag, flash, apply, ping, fwu, reboot, md5sum, nv --
odi-ui's own CHANGELOG has the full list and the per-command timeouts),
plus an ssh connect/keepalive bound on the maintainer scripts
(`scripts/deploy.sh` and the capture/schema-drift tools) that had none.

**`metricsd` pinned to v1.1.2.** Fixes the matching hardware-trial bug on the
exporter side: `metricsd`'s own child wait (`omcicli dump srvflow`) had no
timeout either, so a stuck omcid (the queue-reclaim bug above) hung the whole
exporter, not just the omcicli queue. See odi-sfp-exporter's CHANGELOG
(v1.1.2) for the fix -- bounded child waits, `gpon_omci_up` reporting a
timeout as a metric instead of silence.

**A respawned omcid reclaims the 0x800 (omcicli) queue instead of waiting
forever for `omci_app`.** Hardware trial (rc3, claro, 2026-09-28): after
`kill -9 omcid` + respawn, `vq_ensure()`'s `IPC_CREAT|IPC_EXCL` failed
`EEXIST` against the queue the previous omcid instance left behind (`kill
-9` skips `on_signal()`'s `mq_remove()`), and the daemon logged "the
omcicli queue is still omci_app's" and never served it again -- 42 queued
requests piled up unanswered. `omci_app` never runs on this image at all
(`src/omci/README.md`), so a stale queue at 0x800 can only be a dead
instance of omcid's own: `vq_ensure()` (`src/omci/respond/vqsrv.c`) now
removes it and recreates on `EEXIST`, same as `mq_open_fresh()` already
does for the omcli queue. `src/omci/qemu-test.sh` gained a `kill -9` +
respawn scenario asserting the reclaim and that `dump srvflow` (the
exporter's own command) answers within 2 s afterwards.

**The omcid watchdog ping is wall-clock-gated now, not loop-iteration-counted.**
Hardware trial (rc2, claro, 2026-09-28): filling `/tmp` to `ENOSPC` left
`last_ping_age` at 27 s against the 15 s the main loop was supposed to
guarantee. Root cause: the loop counted `NL_POLL_US`-spaced iterations and
called `1000000/NL_POLL_US` of them "one second" -- true only if every
iteration takes exactly `NL_POLL_US`; one slow iteration (a full apply on
an OMCI frame, a write that blocks while `/tmp` is nearly full) stretches a
counted second past its real length, so 15 counted seconds can cover well
more than 15 real ones. The `src/omci/respond/main.c` main loop now reads
`CLOCK_MONOTONIC` (`sys_clock_gettime`, already available) and gates both
the 1 s sample tick and the 15 s `wdt_ping()` on elapsed wall-clock time
instead, independent of how long any one iteration takes.

**Daemons are `/etc/inittab` `respawn` entries now, not `supervise.sh`.**
Hardware trial (rc2, claro, 2026-09-28): `kill -9` of omcid was not
respawned, and `odi_wdt` correctly reset the board 61 s later when omcid
missed its ping deadline -- the old shell-loop supervisor
(`rootfs/skeleton/etc/scripts/supervise.sh`, removed) had lost track of
the child. Native over hand-rolled: busybox init forks each daemon itself
and never loses the pid. metricsd, confd, dropbear and omcid are each now
a `respawn` entry running a small per-daemon script
(`rootfs/skeleton/etc/scripts/svc-*.sh`) that checks its own
`/etc/config/<name>.off` flag and execs into `respawn.sh` (sets
`oom_score_adj`, then execs the daemon in the foreground; dropbear already
ran `-F`, and omcid `-d` was never a daemonize flag either -- neither ever
double-forked). `/etc/init.d/services` is now a `stop`/`start` hand tool
(writes/clears the `.off` flag and kills the current process so init
restarts it at once) rather than a boot-time launcher.

**The PON steps split into `rcS` (sysinit) and a new `rcS.pon` (`once`),
so daemon respawn no longer waits on them.** Mounts, the config partition
and the management address stay in `rcS`, which every respawn entry still
waits on (busybox init starts nothing until sysinit returns); the PON
steps (optics, the switch init, `gponsn`/`gponpw`/`gponact`) moved to
`rcS.pon`, a `once` entry started in the same breath as the four daemon
`respawn` entries -- the same real-world concurrency `services start &`
already had with them. Since omcid now starts concurrently rather than
being forked inline mid-PON-steps, `gponact` first polls the live
`/proc/odi_omci` registration table for omcid, bounded at 10 s, rather
than assuming it has already registered (an OLT that activates before
omcid is up would otherwise see its first OMCI frames dropped
unregistered). Shared functions (`config_mounted`, `crumb`,
`confirm_watchdog`, the PON step helpers) live in a new
`rootfs/skeleton/etc/scripts/rcs-lib.sh`, sourced by both scripts. The
`make test-rcs` golden traces were regenerated and reviewed by hand: the
`rcS` trace shrinks to the stages that stayed in it; the PON steps and the
watchdog confirmation no longer appear (they are `rcS.pon` now, untraced
by this harness, which only ever ran `rcS` itself). The `make test-qemu`
`kill -9` resilience scenario now also covers omcid, which starts under
qemu for the first time (previously gated on `/proc/odi_omci`, absent
there; the new `svc-omcid.sh` gates only on `/etc/config/modules.off` and
the binary existing, so it starts, and degrades harmlessly exactly as it
already did on any kernel without `/proc/odi_omci`).

**v1.0.2 and v1.0.3 are both also withdrawn: no management network on real
hardware.** Trialled on claro (2026-09-28), v1.0.3 hung unreachable for 10+
minutes with no ARP reply for the configured management address, and did
not self-revert -- the same symptom the v1.0.3 watchdog redesign was
supposed to have fixed. Root cause: `network.sh` falls back to a guessed
`DEF_IP` (192.168.1.1) whenever the config partition or `lastgood.xml` is
not readable at boot, and `rcS` confirmed the watchdog (`userland_ok`)
unconditionally, before `omci_start`/`switch_init` even ran -- so a stick
that came up with a guessed, unreachable address, or a failed `switch_init`
(`/proc/odi_omci write failed` in the ramlog), still confirmed and never
reverted. `rcS` gained `confirm_watchdog()`, called right after
`omci_start`, which requires the config partition mounted, a real
(non-guessed) management address, and a successful `switch_init` before
writing `userland_ok` -- still with no ARP or reachability dependency. (The
"real, non-guessed address" check was reworked again below, into a live
`network.sh configured` call, superseding the flag file this paragraph
originally described.) `test/rcs_trace_inner.sh`'s `network.sh` stub
updated to match.

**Mounts are declarative now, and "config mounted" is a live check, not a
flag.** `/etc/fstab` (new) lists every mount rcS makes -- `proc`, `sysfs`,
`/var` and `/var/tmp` (tmpfs, capped), `devpts`, and the config partition
as `mtd:config /var/config jffs2` (found by MTD partition NAME, no
`/dev/mtdblockN` node, no `/proc/mtd` lookup, no `mdev` race to lose --
`mount-config.sh` is cut down to the one thing left that a mount option
cannot express, the `/etc/config` symlink check). rcS runs `mount -a`
twice (creating the `/var` subdirectories in between -- `mount` never
makes a missing mountpoint, and nothing under `/var` exists until `/var`
itself has landed). `config_mounted()`, the one place anything asks
whether the config partition is mounted, now greps the live
`/proc/mounts` for a real jffs2 mount at `/var/config` plus the symlink
resolving, instead of trusting a flag file `mount-config.sh` wrote once
and nothing ever rechecked. Caught in the rewrite: the first `mount -a`
must not be traced into `/tmp` -- `/tmp` is a symlink to `/var/tmp`, which
does not exist until that same call has mounted `/var`, and a shell
redirected into a missing directory never runs the command at all
(`make test-qemu` caught this one: ssh never came up).

**Root cause, confirmed on a stick: `kernel/618/config` had neither
`CONFIG_SHMEM` nor `CONFIG_TMPFS`.** Every fix above treated the symptom
(no management network, `/var` read-only) as a boot-order or gating
problem; the actual cause was one line below both mounts in the fstab
rewrite: a `mount -t tmpfs ... size=6m` on a kernel with neither symbol
falls through to `mm/shmem.c`'s `!CONFIG_SHMEM` stub (`ramfs_get_sb`),
which mounts fine but rejects `size=` outright (`Invalid argument`) --
confirmed with the exact error on real hardware. That is why the capped
`/var` and `/var/tmp` mounts silently never landed, `/var` stayed the
read-only squashfs, and nothing downstream of it (the config partition,
`/var/log`) could be created. Fixed by enabling `CONFIG_SHMEM=y` and
`CONFIG_TMPFS=y` in `kernel/618/config` -- uImage grew from about
1,274,6xx to 1,281,554 bytes, comfortably under the 1,359,872-byte
partition cap (78,318 bytes spare). Two more gates against a repeat:
`required_mounts_ok()` (new, in `rcS`) checks `/proc/mounts` for `/var`
and `/var/tmp` both actually being tmpfs right after the second `mount -a`
and logs to kmsg the moment either is missing, and `confirm_watchdog()`
now refuses to confirm without it, on top of the existing config-partition
and `switch_init` checks. The `odi-toolchain-qemu-kernel-malta` kernel
fragment gained explicit `CONFIG_SHMEM`/`CONFIG_MTD`/`CONFIG_JFFS2_FS`
lines (already implied by `malta_defconfig`'s defaults, so no new kernel
tag was needed this time, but the previous mismatch was exactly this kind
of implicit-default drift going unnoticed) and `make test-qemu` gained an
assertion reading `/proc/mounts` for `/var`/`/var/tmp` tmpfs at their
configured sizes -- the check that would have caught this the first time.

**The `/var/run/network-configured` flag file is gone.** `rcS`'s watchdog
confirmation asked a flag written once, at boot, whether `network.sh` had
applied a real (non-guessed) address. Replaced with `network.sh
configured`, a new live-check mode: it recomputes the same IP `network.sh`
would apply at boot and reads it straight back off `br0` (or `eth0`, if
the bridge path failed) via `/proc/mounts`-style live inspection, so a
later re-address or a `network.sh addr` that failed cannot leave a stale
`configured` flag behind.

## v1.0.3 — 2026-09-28

**WITHDRAWN.** Broken on hardware: no management network. Root cause,
confirmed after the fact (v1.0.4 below): `kernel/618/config` had neither
`CONFIG_SHMEM` nor `CONFIG_TMPFS`, so the capped `/var` tmpfs mount failed
and the config partition never mounted. Release images deleted from
GitHub 2026-09-28. Use v1.0.1 or v1.0.4.

**v1.0.2 is withdrawn (marked pre-release) and must not be used.** Trialled
on claro (2026-09-27), it hung for 15 minutes with no ARP or ping despite
the SFP link staying up, and did not self-revert: the ramlog showed the
new userland health-kicker withholding its kick
(`health-kicker: withholding kick (omcid_ok=0 mem_ok=1)`) every 30 s while
`odi_wdt` kept kicking the hardware regardless
(`odi_wdt: alive at 799 s ... userland_ok=1`), because `userland_ok` is a
one-shot flag latched at boot -- the kernel never enforced the health
kicker's periodic confirmation at all. v1.0.1's plain 120 s boot-only
deadline was, ironically, safer: it at least reset a boot that never
confirmed, which is more than v1.0.2's unenforced addition did. The
root cause of `omcid_ok=0` itself was not fully recoverable from that
boot: the DRAM ramlog is a two-page ring buffer and only the later page
survived, so the rcS/PON-step portion of that boot is gone. The redesign
below removes the userland health-kicker and the failure mode it
depended on entirely, rather than patching around one unconfirmed cause.

**Watchdog redesign: the kernel (`odi_wdt`) is now the only owner of the
hardware watchdog** (`docs/SETTINGS.md`, "Watchdog rules"; `docs/BOOT.md`).
Three independent rules, judged and enforced kernel-side, any one of
which stops the kicker:

- the one-shot boot confirmation, unchanged from v1.0.1
  (`/proc/odi_wdt/userland_ok`, 120 s, the `confirm-arp` dev opt-in kept);
- a per-client ping deadline: rcS registers a client by name and deadline
  (`/proc/odi_wdt/register`, e.g. `omcid 60`), the client pings its own
  deadline from its own main loop (`/proc/odi_wdt/ping`) -- omcid does
  this every 15 s (`src/omci/respond/main.c`) -- and the kernel resets the
  board if an armed client's deadline is missed. `/proc/odi_wdt/clients`
  shows each client's state (armed, last-ping age) for debugging;
- a kernel-side memory floor: `MemAvailable` (`si_mem_available()`, no
  userland reader to lose along with the memory it reports on) held below
  2048 KB for 3 consecutive 5 s checks.

The v1.0.2 userland health-kicker (`health-kicker.sh`,
`/proc/odi_wdt/health_kick`) is removed entirely. Every reset now logs
which rule fired, ramlog-visible, before it happens. Host-tested against
a fake clock (`test/odi_wdt_test.c`); the reset itself needs real
hardware to verify (`/proc/odi_wdt`, not exercised by `make test-qemu`).

**Every other v1.0.2 change was reviewed and kept**, unless noted:
size-capped `/tmp`/`/var` tmpfs, `oom_score_adj` biasing, daemon respawn
via `supervise()`, `vm.min_free_kbytes`, `make test-qemu` (the qemu
full-system harness, its health-kicker scenario replaced with a check
that rcS's client registration degrades harmlessly without
`/proc/odi_wdt`), and the `cpu-pause` PAUSE-watermark fix (verified on
hardware, ISP1, 2026-09-27).

**Fixed `make test-rcs`'s golden traces**, stale since v1.0.2 (d7839bd
added the `/var`/`/tmp` tmpfs mounts and wrapped omcid in `supervise()`
but never regenerated `test/fixtures/rcs-trace-*.txt`) -- `test-rcs` had
been failing since v1.0.2 and nobody had run it, since it needs
Docker+ptrace and is not part of `make test` or CI. Regenerated and
reviewed by hand: the only differences are the tmpfs mounts and omcid's
supervise-restart loop against this harness's stub binary.

## v1.0.2 — 2026-09-27

**WITHDRAWN.** Broken on hardware: no management network. Root cause,
confirmed after the fact (v1.0.4 below): `kernel/618/config` had neither
`CONFIG_SHMEM` nor `CONFIG_TMPFS`, so the capped `/var` tmpfs mount failed
and the config partition never mounted. Release images deleted from
GitHub 2026-09-28. Use v1.0.1 or v1.0.4.

**Resilience.** A 20 MB `scp` into `/tmp` on ISP1 exhausted RAM (`/tmp` was
ramfs, unbounded and unreclaimable): the OOM killer took dropbear, confd
and omcid, none of them restarted, and the box stayed unmanageable until a
power cycle even though the hardware datapath kept forwarding on its own.
Fixed together (`docs/SETTINGS.md`, "Resilience"; `docs/IMPROVEMENTS.md`):

- `/tmp` (`/var/tmp`) and `/var` are size-capped tmpfs (8 MB / 6 MB)
  instead of unbounded ramfs, so a full `/tmp` gives `ENOSPC` rather than
  taking the box down with it.
- `oom_score_adj`: `-1000` for omcid and dropbear, `-500` for confd, the
  kernel default (`0`) for metricsd -- the one daemon whose loss costs
  neither management access nor GPON state.
- omcid, dropbear, confd and metricsd now restart automatically if they
  die, rate-limited to 5 restarts per 60 s window (`supervise()`,
  `rootfs/skeleton/etc/scripts/supervise.sh`).
- A periodic health kicker (`rootfs/skeleton/etc/scripts/health-kicker.sh`,
  `/proc/odi_wdt/health_kick`, `kernel/extra/drivers/net/ethernet/odi/odi_wdt.c`)
  resets the board through the watchdog if userland health (omcid alive,
  `MemAvailable` above a floor) stops being reported later in the boot --
  independent of, and in addition to, the one-shot boot confirmation
  (`/proc/odi_wdt/userland_ok`) this image already had, which only ever
  covered a boot that never comes up at all.
- `vm.min_free_kbytes` raised to 1536 (from the kernel default of roughly
  128 KB on a box this size) at boot.

**A qemu full-system test harness** (`make test-qemu`, `docs/HACKING.md`):
boots the real rootfs (busybox, inittab, services, dropbear, confd,
metricsd -- unmodified) under `qemu-system-mips -M malta`, standing in for
the RTL9602C board qemu cannot emulate, and checks ssh (a test-only key),
the web UI and the exporter, plus the resilience scenarios above, end to
end. The kernel it boots is a stock, prebuilt mainline build
(`ghcr.io/andreziviani/odi-toolchain-qemu-kernel-malta`, a new image
published from the odi-toolchain repository, `toolchain/images.env`), not
this repository's own -- so CI never has to build a kernel from source to
run it. Runs in CI on every push and pull request
(`.github/workflows/ci.yml`).

**Fixed the odi_nic driver asserting PAUSE** toward the switch CPU port at
ordinary traffic levels (merged from branch `cpu-pause`).
`ODI_NIC_FC_ON_LEVEL`/`ODI_NIC_FC_OFF_LEVEL` (the free-descriptor
watermarks that gate the NIC's own flow control) were derived as a flat
quarter/three-quarter of the RX ring depth, asserting PAUSE at only 75%
ring-used and holding it until the ring drained back to 25% used — a band
wide enough for ordinary NAPI scheduling jitter at a few packets a second
to cross and hold, with no real congestion behind it. Rescaled to the
stock firmware's own near-exhaustion trigger proportion (assert near 94%
used, deassert near 81% used) instead, so PAUSE only fires near actual
ring exhaustion. Verified on hardware: 3003 → 13 PAUSE frames/min on ISP1.

- **Lint no longer chokes on binary files.** The plan/task-id grep in
  `make lint` now skips binary files (`git grep -I`); `docs/images/*.png`
  matched the pattern as raw bytes on the CI runner only, never locally.

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
