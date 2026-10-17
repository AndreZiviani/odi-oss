# Tools and daemons

Every program this image ships that you would run, restart or read the log
of: what it is, how to invoke it, what it reads, where it writes, and who
starts it. Settings -- the config-store keys, the web UI's controls and the
switch files on the config partition -- are in
[`docs/SETTINGS.md`](SETTINGS.md).

Everything in `src/` is ours: freestanding, statically linked MIPS binaries
with no libc (see `docs/CROSS-COMPILING.md` for what that means and why).
`confd` (the web UI) and `metricsd` (the exporter) are separate projects,
fetched as pinned releases by `src/fetch-releases.sh` rather than built from
source here; `/etc/odi-build` on the stick records which releases went in
(`exporter=`, `confd=`).

## At a glance

| program | kind | started by | talks on | logs to | if it dies |
|---|---|---|---|---|---|
| `omcid` | daemon, the OMCI stack | inittab `respawn` (`/etc/scripts/svc-omcid.sh`) | odi_omci netlink (redirect type 1), SysV queues for `omcli`/`omcicli` | `/var/log/omcid.log` | respawned |
| `confd` | daemon, the web UI | inittab `respawn` (`/etc/scripts/svc-confd.sh`) | TCP 80 | `/var/log/services.log` | respawned |
| `metricsd` | daemon, the Prometheus exporter | inittab `respawn` (`/etc/scripts/svc-metricsd.sh`) | TCP 9100 | `/var/log/services.log` | respawned |
| `dropbear` | daemon, ssh and scp | inittab `respawn` (`/etc/scripts/svc-dropbear.sh`) | TCP 22 | syslog (every login) | respawned |
| `syslogd` | daemon, the system log | inittab `respawn` (`/etc/scripts/svc-syslogd.sh`) | UDP 514, if `SYSLOG_SERVER` is set | its own circular buffer (`logread`) | respawned |
| `klogd` | daemon, kernel log to syslog | inittab `respawn` (`/etc/scripts/svc-klogd.sh`) | -- | syslogd | respawned |
| `ntpd` | daemon, NTP client (opt-in) | inittab `respawn` (`/etc/scripts/svc-ntpd.sh`), only while `NTP_SERVER` is set | UDP 123 to `NTP_SERVER` | syslog | respawned |
| `igmpd` | daemon, IGMP snooping | nothing (shipped, not started; see its section) | odi_omci netlink (redirect type 4), `/dev/odi_sw` | stdout | -- |
| `login` | serial console login | inittab `askfirst` (press Enter on the console) | ttyS0 | -- | respawned |
| `diag` | CLI: optics, GPON state, counters, registers | you, rcS, network.sh, metricsd, confd | `/dev/odi_sw`, `/proc/odi_gpon/status`, netlink | stdout | -- |
| `i2cget` / `i2cdump` / `i2cset` | CLI: the optics module I2C devices (busybox) | you | `/dev/i2c-0` | stdout | -- |
| `omcli` / `omcicli` | CLI for omcid | you, metricsd, confd | omcid's queues | stdout | -- |
| `omciprobe` | CLI: one raw driver command | you | odi_omci netlink | stdout | -- |
| `omcicap` | CLI: capture the OMCI channel | you | odi_omci netlink (takes type 1) | stdout | -- |
| `nv` | CLI: the U-Boot environment | you, confd | mtd `env`, `env2` | stdout | -- |
| `flash` | CLI: the config store | confd, you | `/var/config/lastgood*.xml` | stderr | -- |
| `fwu.sh` | CLI: flash one slot | you, `fwu_starter.sh` (from the image tarball) | mtd `k0`/`r0` or `k1`/`r1` | stdout | -- |
| `fwu_starter.sh` | CLI: write an uploaded tarball to the inactive slot | confd, you | runs `fwu.sh` from the tarball | `/tmp/fwu.log`, `/tmp/fwu.state` | -- |
| `apply.sh` | CLI: apply saved settings without a reboot | confd, you | `network.sh addr`; SIGHUP to omcid | stdout | -- |
| `diag-bundle.sh` | CLI: the diagnostics bundle, secrets redacted, identity masked | confd (`GET /api/diag`), you | reads logs, `/proc`, `nv`, the exporter, the optics module (`/dev/i2c-0`) | the tar.gz it writes | -- |

Every daemon -- `omcid`, `confd`, `metricsd`, `dropbear`, `syslogd`, `klogd`
and `ntpd` -- and the serial `login` are all `respawn` entries in
`/etc/inittab`: busybox init restarts whichever one exits, forever, the
instant it does (no restart budget, no rate limit -- `docs/SETTINGS.md`,
"native over hand-rolled"). There is no telnet server (busybox is built
without telnetd).

## How the image starts things

`/etc/inittab` runs `/etc/init.d/rcS` as `sysinit`, and busybox init starts
no `once` or `respawn` entry until it returns -- so rcS never blocks, and
backgrounds everything slow. In order, rcS:

1. mounts `/proc`, `/sys`, `/var` (tmpfs, capped at 6 MB) and `/var/tmp`
   (its own tmpfs, capped at 8 MB -- `/tmp` is a symlink to it), makes the
   `/var` directories, raises `vm.min_free_kbytes`, and mounts the jffs2
   `config` partition on `/var/config`
   (`/etc/scripts/mount-config.sh`; `/etc/config` is a symlink to it);
2. seeds entropy (`seedrng`, seed kept in `/etc/config/seedrng`), sets the
   hostname and `lo`;
3. runs the switch SDK init verbs through `/proc/odi_init` (`intr` ...
   `ponmac`), one crumb before and after each; the kernel loads the
   register replay behind each verb from `/lib/firmware/odi/sdkinit.bin`
   and releases it again (`docs/KERNEL.md`);
4. brings up management networking with `/etc/scripts/network.sh` (host
   SerDes check, MAC, `br0` over `eth0.2`, the address, and the second
   address `br0:2` when `LAN_ENABLE_IP2` is 1);
5. registers omcid as a watchdog client (`/proc/odi_wdt/register`, 60 s
   deadline): omcid pings its own deadline from its own main loop once it
   starts, and the kernel -- the only owner of the hardware watchdog --
   stops kicking if an armed client's deadline is missed,
   `docs/SETTINGS.md` ("Watchdog rules").

rcS returns there. Busybox init then fires `/etc/init.d/rcS.pon` (a `once`
entry -- started, not waited for) and starts the four daemon `respawn`
entries in the same breath: `metricsd`, `confd` and `dropbear`
(`/etc/scripts/svc-metricsd.sh`, `svc-confd.sh`, `svc-dropbear.sh`;
`dropbear` generates the ed25519 host key on first boot) and `omcid`
(`/etc/scripts/svc-omcid.sh`, `-a -d`). Each can be turned off with a
`.off` file, see `docs/SETTINGS.md`; each script sets its own
`oom_score_adj` right before it execs the daemon in place
(`/etc/scripts/respawn.sh`), and busybox init restarts whichever one exits,
forever, on its own -- no `supervise()`, no restart budget any more.

Concurrently with those four, `rcS.pon`:

6. sets the optics pins, the `optics` verb of `/proc/odi_init`
   (`PIN_GPIO_SELECT` only, outside the step list);
7. runs the PON steps: `i2c 1`, `i2cen 1`, `laser`, `gpon`, `rxsd`, `gpondrv`,
   `gpondev`, then the switch init (the platform settings and module-load
   replay, through `/proc/odi_omci`) and the watchdog confirmation
   (`/proc/odi_wdt/userland_ok`; without it the board resets at 120 s of
   uptime -- the network is not a condition, unless `/etc/config/confirm-arp`
   exists, development only, then only after an ARP reply from the `.2` of
   the `br0` subnet), then `gponsn` and `gponpw` (serial number and PLOAM
   password from the config store), then `gponact` -- which first polls
   `/proc/odi_omci` for the omcid registration itself, bounded at 10 s,
   since omcid now starts as its own respawn entry rather than being forked
   inline here. `/etc/config/pon-steps` replaces the whole list
   (development, `docs/BOOT.md`).

   The `laser` step reads the id byte of the laser driver chip (A2h page 2,
   byte 0xa0) and drives the TX-enable (SoC GPIO 13) only for the Semtech
   GN25L95 (id 0x6a). On any other chip, or when the id cannot be read, the
   laser stays off and `gponact` is refused (`-EPERM`; `dmesg` says why).
   `ODI_LASER_UNVERIFIED=1` in `/etc/config/odi.conf` makes the boot run
   `laser unverified` instead, which switches the laser on regardless
   (`docs/SETTINGS.md`); `/etc/config/optics.off` skips the step.
   `/proc/odi_gpon/status` shows the outcome: `laser on <0|1> ldd_id <id> unverified
   <0|1>`.

A second `once` entry, `/etc/scripts/slot-state.sh`, starts with them: it
writes the slot state file and the login notice (below, "Slot state") and
exits.

`docs/BOOT.md` has why each stage is where it is, and the development
aids of `/etc/init.d/rcS.dev` (`breadcrumbs.on`, `confirm-arp`,
`pon-steps`), shared by rcS and rcS.pon. A background job in rcS also
trims `/var/log/omcid.log` and
`/var/log/services.log` once a minute: past 256 KB each is cut back to its
last 128 KB. `/var` is RAM, so every log is gone at reboot; the only
records that survive one are the DRAM ramlog (`/proc/odi_ramlog_prev` on
the next boot of this image, `tools/memprobe` from the stock one) and, when
`/etc/config/breadcrumbs.on` is set, `/etc/config/breadcrumbs` and
`/etc/config/trial-diag.txt` on flash.

## Daemons

### `omcid` -- the OMCI stack

Answers the OLT's OMCI, holds the MIB the OLT provisions, and turns it into
switch programming: T-CONTs, GEM flows, queues, bridge connections and
their VLAN rules, over the odi_omci netlink command path into the
`odi_switch` driver built into the kernel (`docs/KERNEL.md`). This is the
daemon that decides whether the OLT sees a working ONU.

    omcid [-a] [-r] [-d] [-f] [-w units] [-c caps-hex] [-s state] [-g file]
          [-l file] [-i file] [-j seconds]

    -a          program the switch (apply mode); without it, dry run
    -r          restart: first remove every bridge connection a previous
                omcid left in the switch (with -a)
    -d          daemon: no frame or idle limit
    -w units    exit after this many idle 5 s units (default 24)
    -c hex      use this capability blob instead of the driver one
    -s state    use this ONU state for the resume decision instead of
                asking the driver (test only; see docs/BOOT.md, "Resume
                without re-registration")
    -g file     read the Alloc-IDs and the serial from this file instead
                of /proc/odi_gpon/status (test only)
    -l file     read the UNI link bit (0 or 1) from this file instead of
                the odi_switch host_link parameter (test only)
    -i file     append the driver verbs of a reload to this file instead of
                writing /proc/odi_init (test only)
    -j seconds  how long a reload waits for O5 (default 150; test only)
    -f          start even if a live process holds redirect type 1
    -h          this text; starts nothing

At startup, before opening the OLT-facing socket, a respawned `omcid` also
decides whether to resume its MIB from `/var/run/omcid-mib.snap` instead of
starting empty -- see docs/BOOT.md, "Resume without re-registration". That
snapshot is written after every Create, Set or Delete, and deleted on a
MIB reset; none of it is a command-line concern beyond `-s`.

init runs `/etc/scripts/svc-omcid.sh`, a `respawn` entry that execs
`/bin/omcid -a -d` into `/var/log/omcid.log`, unless
`/etc/config/modules.off` is set; nothing else starts an omcid. Arguments are matched whole;
anything unrecognised exits before a socket is opened, and a second omcid
refuses to start while a live one holds redirect type 1 (`-f` overrides).

It reads, from the config store, at start and again on SIGHUP (below): the manual VLAN
(`VLAN_MANU_TAG_VID` and `VLAN_MANU_TAG_PRI`, applied only with
`VLAN_CFG_TYPE` 1 and `VLAN_MANU_MODE` 1, as the stock firmware gates them;
with `VLAN_CFG_TYPE` other than 1 it applies the OLT's own class 171 rule when
it has the one supported shape, otherwise the service is transparent,
`docs/SETTINGS.md`, "VLAN handling"),
the serial number until the kernel reports one (`GPON_SN`), the LOID keys
(answered in the CTC LOID-authentication entity), the six OLT identity keys
(the ONU-G hardware version, `ONU_HW_VERSION`, from `/etc/config/odi.conf`;
all six reported only while `/etc/config/omci-identity.on` exists), and
`DUAL_MGMT_MODE` and the `OMCI_CUSTOM_*` masks for display only; and, from
`/etc/config/odi.conf`, `OMCI_UNKNOWN_ME_OK` (its start-up line `unknown
entities: ...` says which way) `OMCI_PRIVATE_ME` and `OMCI_VEIP`; the VEIP slot (`OMCI_VEIP_SLOT_ID`,
with bit 256 of `OMCI_CUSTOM_ME`) and the private-entity switch are on the start-up line `VEIP in slot ...`. At every
Start software download, Activate and Commit from the OLT it reads
`OLT_SW_DOWNLOAD` from `/etc/config/odi.conf`: accept (the default) answers the
download and discards the image, reject refuses it; it never flashes and never
reboots (docs/SETTINGS.md, "Software download from the OLT"). On a Reboot
of the ONU-G it reads `OMCI_ACCEPT_REBOOT` the same way: off (the default) is
answered "command not supported", `1` is answered success and followed by a
SIGTERM to init a second later, `2` is answered success and nothing follows. Each key is
looked up in both store files, the one xmlconfig assigns it to first. Its
start-up line in the log says what it found (`store: loid ..., manual vlan
..., identity report ...`). A change takes effect with `apply.sh omci`, which
sends the SIGHUP; `docs/SETTINGS.md` has what each one does.

The T-CONT Alloc-IDs are the OLT's own. A T-CONT (ME 262) the OLT set over
OMCI keeps the Alloc-ID it was set to (G.988 9.2.2). One it never set -- ISP1
sets none -- takes the next Alloc-ID the OLT assigned by PLOAM that no set
T-CONT claims, in assignment order, from the kernel's list (`alloc_ids` in
`/proc/odi_gpon/status`, read once a second); with none left it reads 0x00FF,
unassigned, and is not programmed. That is what a MIB upload and a Get of the
AllocID answer, and what goes to the driver. When the kernel list changes (an
assignment, a deallocation, a re-ranging) the T-CONTs bound to it are
reprogrammed a quiet second later; `omcli tcont` shows each binding and where
it came from.

It logs every OMCI frame in and out (`<-` / `->` lines) and every driver
call to `/var/log/omcid.log`, about 1.7 MB a day on a busy OLT before the
trim. `omcli` and `omcicli` are its clients; `cat /proc/odi_omci` shows the
kernel side (who holds each redirect type, frames delivered and dropped).

On SIGINT or SIGTERM it deregisters from the OMCI channel and exits, and init
respawns it (see "Restarting a daemon" below). While it is not running the
OLT's OMCI goes unanswered; after a respawn it resumes from its snapshot or
starts with an empty MIB and keeps the switch programming already in place
until the OLT provisions it again.

**SIGHUP rereads the config store**, the Unix convention for a reload, and is
what `apply.sh omci` sends. The handler sets a flag and the main loop does the
work, so a second SIGHUP during a reload is folded into one more reload and the
watchdog ping never stops. omcid compares what it reads with what it is
running (only what the OLT or a connection would see: with the identity
switch off the report keys do not count) and does the least that applies:

| what changed | what omcid does | the ONU |
|---|---|---|
| nothing | logs `event=reload changed=none` | untouched |
| only `VLAN_CFG_TYPE`, `VLAN_MANU_MODE`, `VLAN_MANU_TAG_VID`, `VLAN_MANU_TAG_PRI` | tears the bridge connections down and builds them again from the MIB it holds, by the same `bdgconn_rebuild()` the first build uses | stays in O5, nothing re-ranged |
| `GPON_SN`, `GPON_PLOAM_PASSWD`, the LOID keys, `OMCI_SW_VER1/2`, `GPON_ONU_MODEL`, `OMCC_VER`, `OMCI_VENDOR_PRODUCT_CODE`, `ONU_HW_VERSION`, the identity switch, `OMCI_UNKNOWN_ME_OK`, `OMCI_FIXED_ME`, `OMCI_PRIVATE_ME`, `OMCI_VEIP`, `OMCI_VEIP_SLOT_ID` (with `OMCI_CUSTOM_ME`), bit 64 of `OMCI_CUSTOM_ME` (with or without VLAN keys) | writes `gpondeact` to `/proc/odi_init`, clears its MIB as a MIB reset does (`event=mib_reset side=local`), holds three seconds, hands the driver the serial number (`gponsn`, only if it changed) and the password (`gponpw`), writes `gponact`, and waits for O5 and the services | deactivated and ranged again; the process is never restarted |

The re-registration is a state machine advanced by the main loop, so no step
sleeps: the CLI, the queues and the watchdog ping are served while the ONU is
down. It ends when `/proc/odi_gpon/status` says O5 and at least one service exists,
at `result=timeout` (O5 not reached) or `result=no_services` after 150 s, or
at `result=failed` when the driver refused `gpondeact` (nothing was touched,
and omcid goes back to the values it was running with, so the next SIGHUP
tries again) or `gponact` (the ONU is left off the line). A refused `gponsn`
does not stop the activation: the ONU comes back on the old serial number and
the result is `sn_not_applied`. The serial number is rewritten into the
PLOAM slot the boot activation armed, and only while the ONU is deactivated.

Three event lines tell the story (below): `event=reload changed=... action=...
keys=...` (key names, never values), `event=reload_step verb=... rc=...` for
each driver verb, and `event=reload_done ... result=... duration_s=...
[o5_s=...] services=N`. They are never rate limited. `/var/run/omcid-reload`
holds the same outcome for `apply.sh` (`id` pid.sequence, `state`, `changed`,
`action`, `result`, `keys`, `duration_ms`, `o5_ms`, `services`), replaced whole
by a rename; omcid removes a stale one at start.

### `confd` -- the web UI

The separate odi-ui project, fetched as a release. Serves the UI and its
JSON API on the port given as its only argument (`confd 80` here), HTTP
Basic with the credential in `/etc/config/confd.auth` (`user:password`;
default `admin` / `admin` while that file is absent). It reads its pages and
five `.tsv` tables (`keys`, `meta`, `consumers`, `features`, `settings`)
from `/etc/confd/`, and any file of the same name in
`/etc/config/confd/` overrides the shipped one; `settings.tsv` among them
is the list of keys this image reads and what applying each costs. It runs
`/etc/scripts/flash`, `/etc/scripts/apply.sh`, `/etc/scripts/fwu_starter.sh`,
`/bin/diag`, `/bin/omcicli`, `/bin/nv`, `/bin/ping` and `/sbin/reboot` for
its pages, is single-threaded (a ping or an `apply.sh omci` blocks every
other request for its duration; a firmware write no longer does), and logs
nothing of its own to speak of.
`docs/ACCESS.md` has the ports and the SSH-key API; `docs/SETTINGS.md` has
what each control does on this image, including the ones that cannot work
here.

### `metricsd` -- the Prometheus exporter

The separate odi-sfp-exporter project, fetched as a release. `metricsd [port]`,
default 9100, any path answers. One scrape runs one `diag` batch (optics,
ONU state, alarms, per-port MIB counters) and one `omcicli dump srvflow`,
and reads `/proc/meminfo`, `/proc/net/dev`, `/proc/loadavg`,
`/proc/uptime`, `/etc/odi-build` and, once per boot,
`/proc/odi_ramlog_prev`. `gpon_omci_services` -- how many
services the OLT provisioned -- is what tells "reachable but unconfigured"
apart from a working ONU; `gpon_image_info` says which image is running.
Its `path` label is `argv[0]`, so a hand-started copy is visible as such.
`gpon_boot_count` is this boot's ramlog boot counter (boots of this image
since the last power cycle), and `gpon_last_reset_reason{reason="..."} 1`
is why the previous boot ended, the `reason=` of `/proc/odi_ramlog_prev`
(`docs/HACKING.md`, "Reading a boot you could not see"): `wdt_client`
with a `client` label (e.g. `client="omcid"`), `wdt_mem`, `wdt_cpu_rx`, `wdt_userland`,
`reboot`, `halt`, `poweroff`, `panic`, `oops`, `power` or `unknown`. An
alert on `gpon_last_reset_reason{reason=~"wdt_.*"}` fires after a watchdog
reset. Both need an exporter newer than v1.1.2.

From the first odi-sfp-exporter release after v1.1.2 on, the
`gpon_provision_*` families say what the ISP provisioned, so a plan change --
a speed tier, a moved VLAN, another T-CONT -- shows up in Grafana:

| metric | from |
|---|---|
| `gpon_provision_tconts`, `gpon_provision_tcont_info{alloc_id}` | `/proc/odi_gpon/status`, `alloc_ids`: the Alloc-IDs the OLT assigned by PLOAM |
| `gpon_provision_gem_ports`, `gpon_provision_gem_port_info{gem_port,direction}` | `omcicli provision`: the GEM port network CTPs the OLT created |
| `gpon_provision_vlan_info{vlan,source}` | the same: each VID, once per place (`vlan_filter`, `ext_vlan_filter`, `ext_vlan_treatment`) |
| `gpon_provision_traffic_descriptors`, `gpon_provision_traffic_descriptor_{cir,pir}_bytes_per_second{descriptor}` | the same: the OLT's traffic descriptors (ME 280) and their rates |
| `gpon_provision_mib_entities`, `gpon_provision_mib_data_sync` | the same: what omcid holds, and the MIB data sync counter |

`omcicli provision` is one more fork per scrape, under the same 2 s bound as
`dump srvflow` and skipped when that one timed out. Its answer, one line per
item, all numbers decimal:

    tcont me=32768 alloc_id=282 index=0      a T-CONT omcid programmed
    gem me=2 port=1434 direction=3 tcont_me=32768 us_td=0 ds_td=0
    vlan vid=10 source=vlan_filter           sorted by VID, one per source
    td me=1 cir=12500000 pir=62500000 cbs=0 pbs=0
    summary rows=161 tconts=5 gem_ports=6 vlans=6 traffic_descriptors=0 services=6 mib_data_sync=184

The `tcont` lines are omcid's view (the T-CONTs its GEM ports use, with the
Alloc-ID a Get of the T-CONT returns); the exporter takes the T-CONTs from
`/proc/odi_gpon/status` instead, the OLT's own assignment.

From the first odi-sfp-exporter release after v1.2.1 on, the same read of
`/proc/odi_gpon/` gives the GPON link: every `ctr` total as a `*_total`
counter (`gpon_ds_bip_error_blocks_total`,
`gpon_ds_fec_codewords_total{result}`, `gpon_ds_bwmap_allocations_total`,
`gpon_us_gem_bytes_total`, ...), `gpon_alarm_events_total{alarm}` and
`gpon_alarm_seconds_total{alarm}`, `gpon_onu_state_seconds_total{state}`,
`gpon_onu_state_transitions_total{from,to}`, `gpon_fec_active{direction}`,
and the driver's PLOAM, REI, TO1/TO2 and interrupt counts. The exporter's
`docs/METRICS.md`, "The GPON link", has the list and the PromQL for a weak
fibre and a stuck ONU.

### `dropbear` -- ssh and scp

Started by `svc-dropbear.sh` (through `respawn.sh`) as

    /sbin/dropbear -F -r /etc/config/dropbear.d/ed25519 -D /etc/config/dropbear.d -p 22

(`-s` is added on an image built with `ROOT_PW=locked`, which refuses
password logins.)

No `-E`: dropbear logs through syslog by default (every login is logged),
same as everything else here that can reach it -- `logread` shows it. The
host key and `authorized_keys` live on the config partition, so both
survive a reflash; the keys file is read at every login, so adding a key
needs no restart. `/bin/scp` is the same multi-call binary (legacy scp
protocol: OpenSSH 9+ clients need `scp -O`). See `docs/ACCESS.md`.

### `syslogd`/`klogd` -- the system log

Two respawn entries, `svc-syslogd.sh`/`svc-klogd.sh`: syslogd in the
foreground with a 64 KB circular buffer (`-n -C64`), read with `logread`;
klogd (`-n`) forwards kernel messages into it. Neither exists on the stock
image. With `SYSLOG_SERVER`
set in the config store (docs/SETTINGS.md), syslogd also adds `-L -R
host[:port]`: forwarded remotely, kept in the local circular buffer too.
`apply.sh syslog` restarts it without a reboot.

### `ntpd` -- NTP client (opt-in)

`svc-ntpd.sh` starts busybox ntpd (`-n -p NTP_SERVER`) only while
`NTP_SERVER` is set in the config store (docs/SETTINGS.md); unset, the
respawn entry runs the same off-flag placeholder every other disabled
service uses, and does nothing. Also new versus stock, which has no RTC
and no NTP client at all. `apply.sh ntp` starts or stops it, live, when
the setting changes.

### `igmpd` -- IGMP snooping (shipped, not started)

    igmpd [-w] [-f] [-n COUNT]
    igmpd [-w] -j GROUP -p PORTS [-v VID]
    igmpd [-w] -l GROUP [-v VID]

    -w         WRITE the entries to the switch. Off by default.
    -n COUNT   stop after COUNT frames (0, the default, is forever)
    -f         start even if a live process holds uid 4
    -j GROUP   one-shot: program GROUP with member PORTS (hardware port
               bits, 0x1 the UNI) and exit
    -l GROUP   one-shot: remove GROUP and exit
    -v VID     the VID a one-shot names (default 0); printed only, the
               entry is keyed on filtering id 0 (SVL)
    -h         this text; registers nothing

As a daemon it registers for packet-redirect type 4, describes every IGMP
frame the switch traps to the CPU, keeps the group and router-port state,
and with `-w` programs each change into the switch as a static L2
multicast entry: the group MAC (01:00:5e and the low 23 bits), keyed on
filtering id 0 (SVL), with the joined ports as members. Not on the VID the
report came in on: every VLAN on this image is shared, which the stick shows
in its own table (every learned address is an SVL row on filtering id 0,
the VID-14-tagged one from the PON included), so an entry keyed on a VID
would never match a frame. `docs/KERNEL.md` ("The L2 table") has the row
layout. The writes go
through `/dev/odi_sw` (`ODI_SW_IOC_L2_MC_ADD`/`_DEL`, `odi_switch_l2.c`);
`diag l2-table get all` shows the result.

`-j` and `-l` drive that same switch path by hand, without registering
anything, so an entry can be put in and checked without an IGMP frame:

    igmpd -w -j 239.1.2.3 -p 0x1        # member: the UNI
    diag l2-table get all               # 01:00:5E:01:02:03 ... SVL mc 0x1
    igmpd -w -l 239.1.2.3               # a second -l: no such entry in the switch

Without `-w` both print the entry they would write and touch nothing.

**Nothing starts it, on purpose.** On this kernel the daemon never sees a
frame, for three reasons that each rule out starting it at boot:

- the switch forwards IGMP rather than trapping it: the stock init (which
  rcS replays) leaves every IGMP and MLD action in `IGMP_PORT_ACTION`
  (0x01105c + 4 * port) at forward, so no IGMP frame reaches the CPU;
- odi_omci delivers only OMCI frames (RX reason 246) on its redirect
  channel; nothing hands a trapped IGMP frame to type 4;
- and igmpd has no transmit path. Turning the trap on would take every
  host report away from the OLT, and every OLT query away from the host,
  until igmpd sent each one on itself. The OLT would then let the
  membership lapse and stop sending the stream.

What snooping would buy here is small. The stick has one UNI, so there is
no second LAN port to prune multicast from; the OLT already sends a
multicast GEM only for groups someone joined. The one saving left is the
CPU: the image floods unknown multicast to all four ports, the CPU port
included, and a static entry per joined group would keep that traffic off
the CPU. That is worth a trap and a transmit path only once there is a
multicast source to test against. Until then `SNOOPING_ENABLED` stays out
of the UI (`docs/SETTINGS.md`).

## CLIs

### `diag` -- the switch, GPON and optics CLI

Our own CLI over our kernel's interfaces. `src/diag/README.md` is the full
reference; the commands:

    pon get transceiver vendor-name|part-number|temperature|voltage|
                        bias-current|tx-power|rx-power|all|alarm-status
    gpon get onu-state                GPON state machine state, O1-O7
    gpon get alarm-status             LOS, LOF and LOM, live
    gpon get flows                    GEM flows omcid programmed
    gpon get flow-counters            per-flow GEM counters (/proc/odi_gpon/flows)
    mib dump counter port <ports>     port MIB counters; all, 2, 0-3, 0,2-3
    l2-table get all                  the MAC table: every valid L2 table row
    l2-table get entry address valid  the same, in the stock spelling
    l2-table get index <index>        one row by number, with its raw words
    register get <address> <words>    switch-core register read
    register set <address> <value>    switch-core register write
    help                              the list, with descriptions
    exit

Keywords may be shortened while unambiguous (`reg g 0x1d0 1`), and a
trailing `?` lists what matches. Two ways to run it:

    diag pon get transceiver rx-power     # one command from argv, then exit
    printf 'gpon get onu-state\nexit\n' | diag   # commands on stdin, one per line

On stdin it reads until `exit` or end of input. A caller whose stdin never
closes leaves diag waiting for the next line forever, so scripts feed it a
pipe or a file and wrap it in `timeout` (rcS and network.sh use
`timeout 10`). Over ssh, pipe inside the remote command
(`ssh root@<stick> 'printf "...\n" | diag'`): `ssh root@<stick> diag < file`
reaches diag as an immediate end of input.

`l2-table get all` walks the switch L2 lookup table (1,024 hashed rows,
plus the 64 CAM rows while those are on, as they are on this image: 1,088)
and prints one line
per valid row:

    L2 table: 1088 rows, IPv4 multicast looked up on MAC + VID/FID
    MACAddress        Spa Fid Age Vid  State  Ext Hash Type Ports Index
    02:00:00:00:00:01 0   0   7   0    Auto   0   SVL  uc   -     0x06c
    01:00:5E:01:02:03 -   -   -   0    Static -   SVL  mc   0x1   0x17c
    02:00:00:00:00:02 2   0   7   14   Auto   0   SVL  uc   -     0x270 ctag
    02:00:00:00:00:03 0   0   7   0    Auto   0   SVL  uc   -     0x364
    02:00:00:00:00:04 3   0   7   0    Auto   0   SVL  uc   -     0x390
    5 entries

(a trial stick, with `igmpd -w -j 239.1.2.3 -p 0x1` in place: the host
on the UNI, the OLT side on the PON with its C-tag, the stick own CPU
port, and the static group.)

`Spa` is the port a unicast address was learned on (0 the UNI, 2 the PON,
3 the CPU), `Age` counts down from 7 and stops at 0, `Auto` is learned and
`Static` written. A multicast row has no source port or age and lists its
member ports instead. The header words are the stock listing's, which is
what lets the web UI read either; `l2-table get entry address valid` is
kept as a second spelling because confd sends it, and the same confd runs on
the stock slot. `l2-table get index <n>` reads one row whatever its state
and prints its three raw words, for checking the layout against the
hardware. All three are read-only.

Which rows are listed is the table engine answer, not a bit of the row:
after each row read the engine reports whether the row holds an entry
(TABLE_STATUS.HIT, 0x012004 bit 12). The row own bit 77, which a write sets
to place an entry, reads back set on every row, empty ones included, and an
empty row reads back with its bucket in the low MAC octet -- so a listing
that trusted bit 77 showed all 1,024 hashed rows as `00:00:00:00:00:NN`. `get
index` still prints any row, with `valid yes` or `valid no` from the
engine.

The commands the exporter runs keep the stock CLI's syntax and output byte
for byte, and `register get` keeps its `0x<address> 0x<value>` layout for
rcS and network.sh. `register set` writes the switch core
directly: it is how rcS and network.sh program the optics and the host
SerDes, and a wrong value there can take the host link away.

### `i2cget`, `i2cdump`, `i2cset` -- the optics module over I2C

The module's own I2C devices are on `/dev/i2c-0`, the SoC optics port
(`odi_i2c_adapter.c`): 0x50 is A0h, the identification EEPROM (vendor,
part number, serial), and 0x51 is A2h, the diagnostics, served by the
laser driver chip. The busybox applets read them:

    i2cdump -y 0 0x50               # A0h, all 256 bytes
    i2cdump -y 0 0x51               # A2h: DDM in 96-105, page select at 127,
                                    # the selected page in 128-255
    i2cget -y 0 0x51 0x7f           # which page is selected
    i2cset -y 0 0x51 0x7f 2         # select page 2

The controller does one byte per transaction, behind a device select and a
byte address, so the adapter offers byte-data reads (`i2cget`, `i2cdump`
mode `b`, the default), I2C block reads (`i2cdump` mode `i`) and nothing
else: `i2cdetect` cannot scan it, since there is no transaction without a
byte address. Only 0x50 and 0x51 are answered; any other address fails
with "No such device or address", because the controller does not report
a missing device and a read there would return the previous byte read. Every transaction is bounded in the kernel (5 ms a byte) and
shares one lock with the exporter's DDM reads, so neither needs pausing.

The one write the kernel accepts is the page select, A2h byte 127. Any
other `i2cset` fails with "Operation not permitted" before anything reaches
the bus: the other pages are the laser driver's own tables, and a stray
write there could change how the laser is driven. To look at a page, note
the current one and put it back afterwards:

    p=$(i2cget -y 0 0x51 0x7f); i2cset -y 0 0x51 0x7f 2
    i2cget -y 0 0x51 0xa0           # page 2, byte 0xa0
    i2cset -y 0 0x51 0x7f "$p"

On the ISP1 stick page 2 byte 0xa0 reads 0x6a, which the stock firmware's
start-up takes for a Semtech GN25L95 laser driver. A0h bytes 68-83 are the
module serial number: mask them before posting a dump. `diag-bundle.sh`
collects all of this, masked, under `module/`.

### `omcli` and `omcicli` -- the omcid client

One binary; `/bin/omcicli` is a symlink to `omcli`. When omcid is running,
omcli uses omcid's own queue and commands:

    omcli state                          serial, device, ONU state, MIB sync
    omcli provision                      what the OLT provisioned: T-CONTs,
                                         GEM ports, VLANs, traffic descriptors
                                         (the format is under metricsd)
    omcli conn                           the bridge connections omcid built
    omcli flows                          the GEM flow tables, per direction
    omcli mib [all|classId] [entityId]   every managed entity, the ONU's own
                                         and the OLT's, with the values a Get
                                         returns; ends with an `N rows` line
    omcli caps                           the device capability blob, decoded
    omcli tcont                          T-CONT entity id to driver index, the
                                         PLOAM Alloc-IDs, and each T-CONT's
                                         Alloc-ID and where it came from
    omcli vlan [cs.xml]                  the manual VLAN from the config store,
                                         and whether the tag is applied
    omcli ident [cs.xml hs.xml [odi.conf]]  the identity and the OLT
                                         identity keys, and whether those are
                                         reported (passwords are never printed)
    omcli bridge <ingress|any> <gem> <dir>   build one bridge rule by hand
    omcli cfgset <file> <dir> <key> <v>  write one key into a config store
    omcli help

`bridge` and `cfgset` write; the rest read. The file arguments of `vlan` and
`ident` are for tests: omcid loads what they name in place of the store, and
keeps using it until it restarts. The stock command shapes
(`get sn|log|logfile|devmode|dmmode|cflag|version`, `mib get <class>
[entity]`, `dump conn|srvflow|qmap|tasks`) are answered by omcid too, in the
stock output format, which is what `confd`'s MIB and Services pages and the
exporter's `omcicli dump srvflow` rely on. `omcli` with no arguments prints
the stock command list; the setters in it (`set sn|devmode|dmmode|loid`, `mib
set|create|delete`) need `-f` and omcid does not implement most of them.
`omcicli get tables` is safe against our omcid (it has no lock to deadlock
on), but the stock daemon wedges on it, so the web UI refuses it outright.

Maintenance flags: `--rmq KEY` removes a message queue left by a killed
client (there is no `ipcrm` in the busybox), `--inject HEX` feeds a 48-byte
OMCI frame in as if it came off the line (`--inject-file F` every frame in
F, one per line, back to back), `--vendor` talks to a stock
daemon instead, `--id N [-f]` sends a raw command id.

### `omciprobe` -- one raw driver command

    omciprobe [-f] <cmd|name> [hexbyte ...]
    omciprobe -l            list the known commands
    omciprobe -h            this text

Sends one OMCI driver command to odi_switch over the odi_omci netlink
socket and dumps the reply, bypassing omcid. Never registers for the OMCI
channel. Only read-only commands run without `-f`; a write sent this way
reconfigures the switch behind omcid's back, and omcid has no way to know.

### `omcicap` -- capture the OMCI channel

    omcicap [-f] [-x] [-n frames] [-w ticks]

    -n frames  stop after this many frames (default 100)
    -w ticks   stop after this many idle 5 s ticks (default 12)
    -x         hex-dump the head of every frame
    -f         take the channel even from a live holder (omcid)
    -h         this text; registers nothing

Registers for OMCI redirect type 1 and prints every frame the OLT sends.
Only one process can hold that channel, so it refuses to start while omcid
holds it. The procedure is: stop omcid, capture, start omcid again (see
below). While omcicap runs, and until omcid is back, the OLT's OMCI goes
unanswered and the OLT will eventually re-range the ONU -- which is
usually the conversation worth recording. Never on a stick you need to stay
in service.

### `nv` -- the U-Boot environment

    nv getenv [name]              print one variable, or all of them
    nv setenv [-c 1|2] name value set one (in the copy that was read, or copy 1|2)
    nv fallback [name]            read the OTHER copy, the one U-Boot falls back to
    nv commit slot                sw_commit=slot in BOTH copies, verified

Reads and writes the redundant environment (mtd `env` and `env2`) with a
CRC check and no vendor library. It is what arms and commits a trial boot:
`sw_tryactive`, `sw_commit`, `sw_active`, `sw_version0/1`
(`docs/FLASHING.md`). `setenv` erases and rewrites a partition U-Boot
needs; a power cut in that window leaves that copy invalid and U-Boot on
the other, stale one. **Never write `sw_commit` before you have booted and
verified a trial from the running image.**

`commit` is the commit, as one operation: `sw_commit=<slot>` into the
primary copy (the one U-Boot boots from), read back and compared byte for
byte, then into the fallback copy the same way, then both read again. It
refuses, before writing anything, a slot other than the one the kernel was
booted from (the last `root=` in `/proc/cmdline`, the one the kernel
mounted: the built-in slot-0 line comes before U-Boot's) or than `sw_active` in the
primary, and an environment whose two copies are not both valid; it never
writes `sw_active`. A copy that already says so is not rewritten, so a
second run writes nothing. Exit 0 means both copies now commit the slot;
anything else says which step failed and whether the fallback copy was
touched (it never is before the primary has verified). Interrupting it is
safe at every step: until the primary is verified the fallback still holds
the old environment, and after that the primary wins with the new one.
It is how a trial is committed, by hand, once you have checked it
(`docs/FLASHING.md`, "Committing"); run `slot-state.sh` after it to refresh
the login notice and the exporter.

### Slot state -- `/var/run/odi-slot`

`/etc/scripts/slot-state.sh` writes it at every boot (an `/etc/inittab`
`once` entry), and again whenever it is run by hand or after a commit from the web UI (confd runs it with no environment, so it sets its own `PATH`; a run after the first of a boot does not wait for syslogd). It only reads the
environment (`nv getenv`, `nv fallback`) and `/proc/cmdline`, never writes
either, and replaces the file by rename. One `KEY=value` per line, always
all nine keys, a value empty when it cannot be told:

| key | value |
|---|---|
| `running` | the slot the kernel was booted from, `0` or `1` (`root=31:N` naming `r0` or `r1`) |
| `sw_active` | `sw_active` in the primary copy, `0` or `1` |
| `sw_tryactive` | `sw_tryactive` in the primary copy, as stored (`2` when no trial is pending) |
| `primary_copy` | `1` or `2`: the copy U-Boot boots from (`env` or `env2`); empty when neither is valid |
| `primary_sw_commit` | `sw_commit` in that copy, `0` or `1` |
| `fallback_copy` | the other copy, `1` or `2`; empty when it is not valid |
| `fallback_sw_commit` | `sw_commit` in that copy, `0` or `1` |
| `next_boot` | the slot the next reset boots: `sw_tryactive` when it is `0` or `1`, else `primary_sw_commit` |
| `uncommitted` | `1` when a valid copy's `sw_commit` is not `running`; `0` when every valid copy names it; empty when the running slot or the environment cannot be read |

The exporter (metricsd) turns it into `gpon_boot_slot`,
`gpon_committed_slot` and `gpon_uncommitted`; the web UI reads it for its
trial banner. When `uncommitted` is not `0` the same script puts a one-line
notice in `/var/run/motd` (`/etc/motd`, which dropbear prints at every
interactive login) and in syslog (tag `slot-state`) -- `TRIAL BOOT`,
`HALF COMMITTED` (the fallback copy still names the other slot) or `SLOT
STATE UNKNOWN`, each naming the copies and the command that clears it; when
it is `0` the motd is empty.

### `flash` -- the config store

`/etc/scripts/flash` is ours: a shell script over the two XML files on the
config partition, `/var/config/lastgood.xml` (CS, the service keys) and
`/var/config/lastgood_hs.xml` (HS, the hardware identity).

    flash all cs|hs          print one store
    flash get KEY            KEY=VALUE, from whichever store has it
    flash set KEY VALUE      change an existing key, print KEY=VALUE back
    flash default cs         merge /etc/config_default.xml into CS

`set` only changes a key that already exists, refuses values carrying a
quote, `<` or `&`, and installs the file by rename (the new copy is written
beside the target, compared with the source, then renamed over it) so a power
cut leaves the old one whole. It changes the file and nothing else: whatever reads the key reads
it at its own time, or when `apply.sh` makes it. confd writes every setting
through it. `default cs` sets every key the image defaults file names to its
default (adding one that is in neither file) and leaves every other key, and
the HS file, alone; `docs/SETTINGS.md` has what that resets.
The partition is shared with the other slot, so a key written here is also
what the stock image reads if the stick falls back to it.

### `fwu.sh` -- flash one slot

Travels inside the image tarball, not in the rootfs:

    tar xf odi-oss-<version>.tar fwu.sh md5.txt
    ./fwu.sh <slot> odi-oss-<version>.tar

Refuses the running slot, checks both members' md5 and sizes before erasing
anything, streams them out of the tarball, and never touches the config
partition. It needs only `tar`, `sed`, `cksum`, `md5sum` and `cmp` (no `cut`,
which some stock images lack, so the first flash can run on stock firmware)
and names a missing one before touching anything. It writes `sw_version<slot>`
to the U-Boot environment only with `FWU_RECORD_VERSION=1`. `docs/FLASHING.md` is the procedure.

### `fwu_starter.sh` -- the same, from the web UI

    /etc/scripts/fwu_starter.sh <slot> <image.tar>                background
    /etc/scripts/fwu_starter.sh --foreground <slot> <image.tar>   and wait

What the Firmware tab's Write runs. Refuses a second write while one is
running, and any slot but the inactive one (read the way `fwu.sh` reads it:
`root=31:N` in `/proc/cmdline`, then `sw_active`); extracts `fwu.sh` and
`md5.txt` from the tarball into `/tmp/fwu.d` and checks `fwu.sh` against its
md5 line; then runs `fwu.sh <slot> <tar>` there, detached with `setsid`, its
output in `/tmp/fwu.log` and its outcome in `/tmp/fwu.state` (`running`,
`ok` or `failed <rc>`), which confd reports and the page polls. Never writes
`sw_commit` or `sw_tryactive`; trying the slot is a separate step.

### `apply.sh` -- apply saved settings without a reboot

    /etc/scripts/apply.sh network      LIVE
    /etc/scripts/apply.sh omci         INTERRUPTS INTERNET

`network` runs `network.sh addr -n` to print the plan, then `network.sh addr`
one second later, detached -- moving the primary address drops the
connection that asked for it, so the answer leaves first. `omci [seconds]`
sends SIGHUP to the running omcid (`pidof omcid`) and waits for its answer in
`/var/run/omcid-reload`: first that omcid took the signal (5 s), then the
outcome (20 s, `APPLY_WAIT` or the argument; the web UI gives the script 25).
omcid decides what the change needs (the table under `omcid`): nothing, a
VLAN rebuild in place, or a re-registration. The report says which:

    apply: omcid 412 told to reload its configuration
    apply: VLAN handling changed (VLAN_MANU_TAG_VID): connections rebuilt in place in 0.4 s, the ONU stayed in O5

    apply: identity changed (GPON_SN): the ONU re-registered and is back in O5 with 6 service(s) after 41.3 s (omcid 44700 ms in all)

A re-registration still running at the bound is reported as still working and
is not a failure. Exit 1 when no omcid runs (or `/etc/config/modules.off` is
set), when omcid does not answer the signal within 5 s (an older build treats
SIGHUP as stop), or when the reload's result is `failed`, `timeout`,
`no_services` or `sn_not_applied`. It never starts an omcid and writes
nothing to the driver: init owns the process, and a second omcid started here
used to make the ONU cycle through O1 to O3 while init's respawn entry
re-provisioned around it.

### `host-autoneg.sh` -- the host-port auto-negotiation

    /etc/scripts/host-autoneg.sh try on|off [SECONDS]   a trial, 120 s by default (10 to 600)
    /etc/scripts/host-autoneg.sh confirm                keep it: stores HOST_AUTONEG
    /etc/scripts/host-autoneg.sh revert                 end the trial now
    /etc/scripts/host-autoneg.sh status                 the state, key=value
    /etc/scripts/host-autoneg.sh boot                   rcS only

Applies `HOST_AUTONEG` (`docs/SETTINGS.md`, "Host port auto-negotiation")
through the kernel parameter `odi_switch.host_autoneg`, and reads the host
link from `odi_switch.host_link` (`docs/SWITCH.md`). `try` answers at once
and writes the mode one second later, detached, so the answer leaves before
the link changes; the trial reverts by itself at the end of its window
unless `confirm` (over ssh or the web UI, so over the link being tried)
stores the key first. `confirm` reads the key back and fails, leaving the
trial to revert, when it did not stick. `try` refuses while a trial or the
boot watch runs, and says so when the mode asked for is already the one
running and stored. Exit 1 on every refusal; the reason is on stderr. Every
state change takes a lock (`/var/run/host-autoneg.lock`, a directory; a
holder that died is found by its pid) and replaces the state file in one
rename.

The state file, `/var/run/host-autoneg`, and `status` (the same fields,
plus the live link, the uptime and whether this is a safe-mode boot), one `key=value`
per line; the web UI reads the file, and `status` is in the diagnostics
bundle (`host-link.txt`):

| key | value |
|---|---|
| `setting` | `on` or `off`: `HOST_AUTONEG` as this boot read it, or as a confirm stored it |
| `running` | `on` or `off`: what the host port runs now |
| `phase` | `steady`, `watch` (the boot watch is deciding) or `trial` |
| `fallback` | `1` while the port runs auto-negotiation with the key off because the forced link did not come up this boot |
| `trial_mode`, `trial_deadline` | the mode on trial and the uptime, in seconds, at which it reverts |
| `watch_until` | the uptime by which the boot watch ends at the latest |
| `event`, `event_uptime`, `message` | the last event, below |
| `link` (`status` only) | `1`, `0`, or `unknown` without the kernel parameter |
| `safe_mode_boot` (`status` only) | `1` on a safe-mode boot (`safe-mode.sh`) |

Every event is one line to the kernel log (`host-autoneg: event=<name>
setting=... running=... <text>`, so dmesg, the ramlog and syslog through
klogd), at notice:

| event | when |
|---|---|
| `applied` | a boot with the key off forced the mode and started the boot watch |
| `forced_link` | the boot watch saw the host link come up forced; no fallback |
| `watch_switch` | no link for 30 s; the watch switches mode |
| `fallback` | the link came up only with auto-negotiation; the key stays off |
| `fallback_nolink` | no link in either mode for 10 minutes; stays on auto-negotiation |
| `safe_mode_boot` | a safe-mode boot: nothing applied, auto-negotiation on, trials refused |
| `trial_started`, `trial_expired`, `trial_reverted`, `trial_confirmed`, `trial_failed` | a trial and how it ended |
| `bad_value`, `no_kernel`, `apply_failed` | a key that is neither on nor off, a kernel without the parameter, a write that failed: the port runs auto-negotiation |

A boot with the key unset logs nothing and writes nothing.

### `safe-mode.sh` -- the boot counter and the recovery boot

    /etc/scripts/safe-mode.sh status     this boot and the record, key=value
    /etc/scripts/safe-mode.sh ack        acknowledge a safe-mode boot: removes its record
    /etc/scripts/safe-mode.sh active     exit 0 on a safe-mode boot
    /etc/scripts/safe-mode.sh count      rcS only, right after the config partition is mounted

`count` adds one to `/etc/config/boot-count` and spawns `boot-ok`, which
removes it once the uptime reaches 60 s. Uptime starts with the kernel,
about 54 s after a cold power-on on the sticks measured (the count lands at
about 5 s of uptime), so a power-up counts as short when it is cut roughly
60 to 110 s after power-on: "plug in, wait about 80 s, pull" (docs/SETTINGS.md). A boot that finds five short boots
before it (three with `HOST_AUTONEG=off`) is a safe-mode boot: it writes
`/var/run/safe-mode` and the record `/etc/config/safe-mode`, removes
`HOST_AUTONEG` when that was off, and bind-mounts a RAM copy of
`/etc/passwd` with root's password `admin` over the read-only one
(`docs/SETTINGS.md`, "Recovery: the safe mode"). The flag in `/var/run` is
what the rest reads: `host-autoneg.sh` applies nothing, the PON steps
leave the laser off and skip `gponact` (`rcs-lib.sh`), `svc-dropbear.sh`
offers passwords, and confd accepts `admin:admin`. `status` prints
`active=0|1` with the flag's fields as `now_*`, `record=0|1` with the
record's as `record_*`, and `boot_count` while there is one.

Its events, to the kernel log at notice: `safe-mode: event=boot_count
boot=N short_before=N limit=5|3 uptime=S` on every boot (the uptime is
how early the count lands on a given stick), `event=safe_mode short_boots=N
cleared=HOST_AUTONEG|none|failed root_password=default|failed`, and
`event=safe_mode_ack`.

### `diag-bundle.sh` -- the diagnostics bundle

    /etc/scripts/diag-bundle.sh [--full] [OUT]   default OUT: /tmp/odi-diag.tar.gz

Collects what a bug report needs into one tar.gz, `odi-diag/` inside, and
prints OUT: the previous boot ramlog (`/proc/odi_ramlog_prev`), the tail of
every `/var/log` file (256 KB each, 1 MB in all), `dmesg`, `/etc/odi-build`,
`/etc/version`, `/proc/odi_wdt/{watchdog_flag,userland_ok,clients}`,
`uptime`, `/proc/meminfo`, `/proc/mounts`, `/proc/cmdline`, `ps`,
`sw_active`, `sw_commit`, `sw_tryactive` and `sw_version0/1` from both
environment copies (`nv getenv` and `nv fallback`), one scrape of the
exporter, and the config store. `MANIFEST.txt` lists every step with its
exit status and size, which keys were redacted, and whether the identity
was masked. The web UI serves the same file as "Download a diagnostics
bundle" on the Admin tab (odi-ui `GET /api/diag`, which only runs this and
streams the result).

Under `pon/`, the GPON and OMCI state a PON problem needs:

| file | from |
|---|---|
| `diag.txt` | one `diag` run: `gpon get onu-state`, `gpon get alarm-status`, `gpon get flows`, `mib dump counter port all`, and `register get 0xf020a8 240`, the PON queue window that cmd 23 and 25 write (`docs/SWITCH.md`) |
| `odi_gpon-status.txt`, `odi_gpon-counters.txt`, `odi_gpon-flows.txt`, `odi_gpon-ploam.txt` | `/proc/odi_gpon/status`: ONU state, Alloc-IDs, alarm and state times; `counters`: the link counters; `flows`: the per-flow GEM counters; `ploam`: PLOAM totals and the ring (bodies withheld, below) |
| `odi_omci.txt` | `/proc/odi_omci`: the OMCI channel counters |
| `omcli_{state,provision,flows,tcont,conn}.txt` | `omcli`: what omcid holds and programmed |
| `omcli_mib_<class>.txt` | `omcli mib <class>` for ONU-G, ONU2-G, PPTP Ethernet UNI, VEIP, the bridge and mapper classes, VLAN filtering and extended VLAN tagging, T-CONT, ANI-G, GEM ports and interworking points, priority queues, traffic schedulers and descriptors, multicast |

The MIB classes are a fixed list, not the whole MIB: an OLT can push
credentials through OMCI (SIP user data, authentication methods, large
strings, the TR-069 server), and none of those classes is collected. When
omcid does not answer `omcli state`, the other `omcli` steps are skipped.

Under `module/`, the optics module on `/dev/i2c-0`, which tells which laser
driver the stick has and what state its calibration is in:

| file | from |
|---|---|
| `a0.txt` | `i2cdump -y 0 0x50`: A0h, the identification EEPROM, bytes 68-83 (the module serial) blanked as `XX` unless `--full` |
| `a2.txt` | `i2cdump -y 0 0x51`: A2h with the page select as found, untouched: DDM, and the upper half of whatever page was selected |
| `chip.txt` | A2h byte 127 (the page select) as found; for each page selected, what byte 127 read back; page 2 bytes 0xa0 (0x6a: Semtech GN25L95), 0x80 and 0xdc (the UX3320 variant markers); page 3 byte 0xf0 (UX3320 INIT_STATE); and the page select written back and read back |
| `a2-p<N>.txt` | `i2cdump -y -r 0x80-0xff 0 0x51` with page N selected, for N in 2, 3, 0, 4, 5, 6, only when byte 127 read back N |

Selecting a page is the one write the bundle makes anywhere, and the only
one the kernel accepts on this bus. A chip may refuse a page and keep the
one it had (a GN25L95 refuses page 3), which is why each select is read
back and a page is dumped only when it took. The page work is one short
block under a lock (`flock` on `/tmp/odi-diag-i2c.lock`, so two bundles
never interleave their selects): the first failure ends the selects, and
the page found is always written back, also when the bundle is
interrupted. Without the bus (qemu, or a build without the adapter)
every `module/` step is listed in `MANIFEST.txt` with a failing status
and the bundle still succeeds.

**Redaction.** In the copies of `lastgood.xml`, `lastgood_hs.xml` and
`odi.conf`, the value of every key below is replaced by `REDACTED` (an empty
value stays empty, so "not set" still shows):

- by name: `GPON_PLOAM_PASSWD`, `LOID_PASSWD`, `LOID_PASSWD_OLD`,
  `USER_PASSWORD`, `SUSER_PASSWORD`, `E8BDUSER_PASSWORD`, `SUPER_PASSWORD`,
  `MAC_KEY`, `HW_FON_KEYWORD`;
- and any key whose name contains `PASS`, `PWD`, `PSK`, `SECRET`, `TOKEN`,
  `KEY`, `COMMUNITY` or `CRED`.

Then every secret value found that way, plus the web UI password from
`/etc/config/confd.auth`, is scrubbed from every file in the bundle, as text
and as upper- and lowercase hex, run together and space-separated, in case a
log quoted one; values shorter than three characters are redacted in the
config copies only. `confd.auth`, the dropbear keys and `/etc/passwd` are
never collected.

**Masking.** Many OLTs authenticate an ONU by its serial number alone, so a
bundle posted in a public issue would hand out what it takes to clone the
line. By default the serial number (`GPON_SN` and the one in
`/proc/odi_gpon/status`), the MACs (`ELAN_MAC_ADDR` and every interface), the
LOID (`LOID`, `LOID_OLD`) and the module serial (A0h bytes 68-83, also
blanked in `module/a0.txt`, where the dump splits it over two lines) are
replaced in every file, in every spelling
(text, hex, a MAC with or without separators), by `MASKED-` and their last
four characters, which still tells two sticks apart. `--full` keeps them,
for a bundle you send privately; the web UI never passes it.

The PLOAM ring in `/proc/odi_gpon/ploam` prints the first ten bytes of each
message body, which can cut a value in two where the scrub cannot find it.
So the bodies of Password and Encryption_Key are always withheld, and those
that carry the serial number (Serial_Number_ONU, Acknowledge,
Serial_Number_Mask, Assign_ONU-ID, Disable_Serial_Number) unless `--full`.

Every command runs under busybox `timeout` (5 s, 10 s for the exporter
scrape; each `i2cget`, `i2cset` and `i2cdump` on its own), and a bundle over 2 MB is refused rather than written. Exits 1, with
the reason on stderr and no OUT left behind, when it cannot finish.
`make test-qemu` plants known secrets and identifiers in the config,
`confd.auth` and a log, asserts none of them is anywhere in the bundle, and
that `--full` keeps the identifiers but still no secret; and that the
`module/` steps are listed, failing, in a bundle that still succeeds.

## Boot scripts you can run again

| script | what | safe to re-run |
|---|---|---|
| `/etc/scripts/network.sh` | host SerDes check (and fix), MAC, `br0` over `eth0.2`, the addresses | yes: it checks before it writes, and re-running it is how a management-path problem is debugged |
| `/etc/scripts/host-autoneg.sh status` | the host-port auto-negotiation state (above) | yes: read-only |
| `/etc/scripts/safe-mode.sh status` | the safe mode: this boot, the record, the boot count | yes: read-only |
| `/etc/scripts/network.sh addr [-n]` | only the addresses: the primary and the second one (`br0:2`), live; `-n` says what it would change | yes; this is `apply.sh network` |
| `/etc/scripts/mount-config.sh [name] [dir]` | find mtd `config` by name, mount it jffs2 | only if it is not mounted |
| `/etc/scripts/respawn.sh <oom> <log> <cmd...>` | not run directly: exec-ed by each `svc-*.sh` inittab entry, sets `oom_score_adj` then execs the daemon in place | n/a |
| `/etc/init.d/services {stop\|start} <name>` | write/clear `/etc/config/<name>.off` and kill the current process so busybox init restarts it at once (`docs/SETTINGS.md`) | yes |

## Kernel control files

The files userland drives the kernel through. `docs/KERNEL.md` has the
drivers behind them.

| file | read | write |
|---|---|---|
| `/dev/odi_sw` | ioctls: registers, MIB counters, DDM, the L2 table (diag, metricsd, igmpd) | register writes (diag), L2 multicast writes (igmpd -w) |
| `/proc/odi_gpon/status` | ONU state, ONU id, the serial number, EqD, the Alloc-IDs the OLT assigned (`alloc_ids`, in CAM row order: the assignment order, a row freed by a deallocation reused first), the REI count, the TO1/TO2 expiries, the interrupt bookkeeping, `laser on <0|1> ldd_id <id byte, -1 if unread> unverified <0|1>` (the `laser` verb's outcome: `on 0` means the laser was left off and `gponact` is refused, `docs/SETTINGS.md`), `laser_emergency_stop <0|1>` (1 while the OLT holds the ONU in Emergency Stop, O7: the TX-disable line is high and the laser off until the OLT re-enables it, or `gpondeact`/`gponact` or a reboot leaves O7), the last LOS sample, the last Upstream_Overhead and Ranging_Time decoded (`upstream_overhead`, `ranging_time`, with the USF_MIN_RESP_DELAY value and the MULTFRAME/INFRAME written), and the live alarm and FEC bits and the alarm and ONU state times (below) | -- |
| `/proc/odi_gpon/counters` | the sampled GPON link counters (below) | -- |
| `/proc/odi_gpon/flows` | the sampled per-flow GEM counters, one line per flow OMCI created (below) | -- |
| `/proc/odi_gpon/ploam` | PLOAM message totals (`ploam ds_rx <n> us_tx <n>`, `ploam_us_dropped`: replies not sent because the upstream queue stayed full for 1 ms; `ploam_type ds|us 0x<type> <count>`, one line per type seen), and the last 32 PLOAM messages (`ploam_ring`: direction, ONU-ID, type, the 10 content bytes; the eight AES key bytes of an upstream Encryption_Key, type 0x05, print as `xx`) | -- |
| `/proc/odi_omci` | redirect registrations, frame and command counters | `switch_init`: the platform settings and the module-load replay (rcS does this once) |
| `/proc/odi_init` | the last verb's return code | one SDK init or PON verb (rcS does these once) |
| `/proc/odi_wdt/userland_ok` | -- | `1`: userland is up, stop the 120 s reset (one-shot, boot only) |
| `/proc/odi_wdt/watchdog_flag` | -- | `1`: keep kicking the hardware watchdog |
| `/proc/odi_wdt/register` | -- | `"<name> <deadline_s>"`, e.g. `omcid 60`: register (or update) a watchdog client; idempotent, arms the deadline from that moment and resets its clock (`docs/SETTINGS.md`, "Watchdog rules") |
| `/proc/odi_wdt/ping` | -- | `"<name>"`: a registered client's own ping, which restarts its deadline; refused (`-EINVAL`) for an unregistered name |
| `/proc/odi_wdt/clients` | one line per registered client: name, deadline, armed, last-ping age | -- |
| `/sys/module/odi_switch/parameters/host_autoneg` | `Y` or `N`: the host port negotiates, or runs forced 1000BASE-X | `1`/`0`: applied at once (`host-autoneg.sh` does this; by hand it lasts until the next boot) |
| `/sys/module/odi_switch/parameters/host_link` | `1` while the host port has a link | -- |
| `/proc/odi_ramlog_prev` | the previous boot's DRAM ramlog, decoded: this boot's counter and slot, the previous boot's counter, slot, build id, last early crumb and reset reason (`reason=`, `docs/HACKING.md`), then its first 3984 bytes and its last 4080 (root only) | -- |
| `/proc/odi_ramlog_prev_raw` | the same two pages as 8192 raw bytes, page A then page B, for `ramlog-read.sh`-style decoding off the stick (root only) | -- |

Writing a verb or an init command by hand re-programs the switch or the
PON MAC under a running omcid; do it on a trial stick only.

### /proc/odi_gpon/: the link counters

`/proc/odi_gpon` is a directory of four read-only files, one per topic, the
way `/proc/odi_wdt/` is. There is no `/proc/odi_gpon` file: a reader opens
the file it needs, and on a stock image, which has none of them, treats a
missing one as nothing to report.

| file | lines |
|---|---|
| `status` | `state`, `onu_id`, `sn`, `eqd`, `alloc_ids`, `rei_count`, `to1_expiries`, `to2_expiries`, `irq_*`, `laser`, `laser_emergency_stop`, `last_los_ms`, `upstream_overhead`, `ranging_time`, then the sampler's `alarm_now`, `fec_now`, `ds_superframe`, `alarm_events`, `alarm_ms`, `onu_state_ms`, `onu_transitions` |
| `counters` | `ctr_samples`, `ctr`, `ctr_saturated` |
| `flows` | `flow_ack_timeouts`, `flow_saturated`, `ds`, `us` |
| `ploam` | `ploam ds_rx us_tx`, `ploam_us_dropped`, `ploam_type`, `ploam_ring` |

Every line is one fact, a key and then words, all numbers decimal. The GPON MAC counters clear when they are
read, so the driver is their one reader (`docs/KERNEL.md`, "The GPON link
counters"): it samples them once a second from the boot `gponact` on and
prints 64-bit totals since its first sample, taken in that `gponact`. Times are milliseconds: to
the second for the ONU states, to the 100 ms line poll for the alarms while
the ONU is activated.

In `status` (the live bits are absent before the first sample):

    alarm_now los 0                   the live DSF_ALARM_STATE bits at the last
    alarm_now lof 0                   reading: los, lof, lom
    alarm_now lom 0
    fec_now ds 1                      FEC in use: downstream (the OLT sends
    fec_now us 1                      it), upstream (the ONU sends it)
    ds_superframe 305491896           the downstream superframe count
    alarm_events los 1                clear -> set, as read; one per alarm
    alarm_ms los 2000                 time seen set; one per alarm
    onu_state_ms O5 8000              time in each state, O1 to O7
    onu_transitions O4 O5 1           state changes, by state left and
                                      entered; only pairs that happened

In `counters`:

    ctr_samples 11                    samples taken since boot
    ctr ds_bip_err_blocks 4           every counter, always
    ctr_saturated ds_gem_idle 1       samples that read it full; only
                                      counters that ever were

The `ctr` names, by bank: downstream framer `ds_bip_err_blocks`,
`ds_bip_err_bits`, `ds_fec_corrected_bits`, `ds_fec_corrected_bytes`,
`ds_fec_corrected_cw`, `ds_fec_uncorrectable_cw`, `ds_superframe_loss`,
`ds_plend_fail`, `ds_ploam_accepted`, `ds_ploam_crc_err`,
`ds_ploam_overflow`, `ds_bwmap_crc_err`, `ds_bwmap_overflow`,
`ds_bwmap_invalid_0`, `ds_bwmap_invalid_1`, `ds_sn_requests`,
`ds_ranging_requests`, `ds_bwmap_accepted`, `ds_gem_los`,
`ds_gem_hec_corrected`, `ds_gem_idle`, `ds_gem_len_mismatch`,
`ds_gem_port_mismatch`, `ds_gem_non_idle`, `ds_plend_corrected`,
`ds_plend_uncorrectable`, `ds_blen_too_long`; downstream GEM
`ds_eth_multicast`, `ds_eth_unicast`, `ds_eth_multicast_fwd`,
`ds_eth_multicast_leak`, `ds_eth_fcs_err`, `ds_gem_over_interleave`,
`ds_omci`; upstream `us_bursts`, `us_ploam`, `us_dbru`, `us_gem_frames`,
`us_ploam_cpu_normal`, `us_ploam_cpu_urgent`, `us_ploam_no_message`,
`us_ploam_sn`, `us_gem_bytes`. `kernel/extra/drivers/net/ethernet/odi/odi_gpon_ctr.c`
has the register and field of each, and `test/fixtures/odi_gpon_ctr_counters.txt`
a whole file (`odi_gpon_ctr_status.txt` is the sampler lines of `status`). `ds_bwmap_accepted` is the one to watch for a stuck ONU:
on ISP2 it runs about 2,070 a second when served, stops when the OLT has
deregistered the ONU that still says O5, and runs about nine times faster
when the Alloc-ID table holds rows the OLT never assigned.

In `flows`, one line per flow, in index order, downstream then upstream:

    flow_ack_timeouts 0               counts skipped because the register
                                      never answered within its bound
    flow_saturated 0                  counts read as all ones: lost counts
    ds 1 gem 1434 gem_frames 20000 bytes 28000000 eth_rx 20000 eth_fwd 20000
    us 4 gem 1566 gem_frames 330 eth_frames 50 bytes 4294994796

`ds <flow>` and `us <flow>` are the driver flow index, the id omcid sent
with the flow (a row of the downstream GEM port CAM, a slot of the upstream
flow table), not the GEM port id: `gem <port>` is the port the flow was
created with. The OMCI channel is a downstream flow (`gem 4095`) and has no
upstream one. Downstream, `gem_frames` and `bytes` are what the GEM layer
received on the flow, `eth_rx` the Ethernet frames it carried and `eth_fwd`
the ones passed on to the switch; the three frame counts were equal per
flow on a stick. Upstream, `gem_frames` ran about 6.6 times `eth_frames` on
that stick, and `bytes` is a 64-bit count. A flow appears when OMCI creates it
and its totals start from zero then, and again whenever it is created again;
a reset of the flow state empties the file. A line has no flow that was not
created, so an image that never provisions a service has none. The file
exists only on this image: the stock image has no `/proc/odi_gpon` at all,
and a reader treats a missing file as no flows.
`kernel/extra/drivers/net/ethernet/odi/odi_gpon_flow.c` has the registers,
and `test/fixtures/odi_gpon_flows.txt` a whole file. `diag gpon get
flow-counters` prints it.

New lines go in as new keys; a parser skips a key or a counter name it
does not know. Reading one of these registers by hand (`diag register
get`, `memprobe`) takes its count away from the totals and from the REI.
The per-flow registers (`0x701140`-`0x701144`, `0x704040`-`0x704050`,
`0x706048`-`0x70604c` and the byte array at `0x706800`) clear on read as
well and have the driver as their one reader, so nothing else may read
them: not `diag register get`, not `memprobe`, and not the stock firmware
`gpon show counter flow`, which would take its counts away from the totals.

## Logs

| where | what | survives |
|---|---|---|
| `/var/log/omcid.log` | every OMCI frame and driver call, omcid's startup | nothing (RAM, trimmed to 128-256 KB) |
| `/var/log/omcid-unknown.txt` | each managed entity class and message type the OLT sent that omcid does not model, with a count ("Link and provisioning events") | an omcid respawn, not a reboot (RAM) |
| `/var/log/services.log` | stderr of services: dropbear logins, confd, metricsd | nothing (RAM, trimmed) |
| `/var/log/*.err` | the rcS `/proc/odi_omci` writes that failed | nothing |
| `dmesg` | the kernel ring buffer, including `rcS:` progress lines | nothing; the web UI's Tools tab shows it too |
| DRAM ramlog | console output of the last boot | a watchdog reset, not a power cut (`tools/memprobe` from the stock image; `/proc/odi_ramlog_prev` from ours, one boot back) |
| `/etc/config/breadcrumbs`, `trial-diag.txt`, `trial.log` | one line per boot stage, network state, trial rounds | reboots and reflashes (config partition); only with `breadcrumbs.on` or a trial knob |
| `logread` (syslogd) | kernel messages (through klogd), dropbear logins, and the `event=` lines below | nothing locally (64 KB, RAM); everything, forwarded, with `SYSLOG_SERVER` |

## Link and provisioning events

After an outage the question is who started it: the ISP (the fibre, the OLT)
or this stick. Every event that can answer it is one line, `event=<name>`
followed by `key=value` pairs, in syslog -- so `logread | grep event=` on the
stick, or the collector `SYSLOG_SERVER` names (docs/SETTINGS.md), has the
whole story in order, with syslogd's timestamps. Nothing is logged per
message: a whole provisioning session is six lines, plus one per thing the
OLT sent that omcid does not model (nine on ISP1).

The kernel's lines are printk (`kernel: odi_gpon: event=...`, facility kern),
which klogd hands to syslogd; the ONU state machine lives in the driver, so
that is where they come from. omcid's are `omcid[pid]: event=...`, facility
daemon, sent to `/dev/log` the way syslog(3) does (omcid has no libc), and
the same line goes to `/var/log/omcid.log` among the frames around it.

| line | level | when |
|---|---|---|
| `odi_gpon: event=onu_state from=O5 to=O2 in_state_s=3605.120 cause=deactivate_onu_id side=olt onu_id=0` | notice when leaving O5 or entering O7, else info | every ONU state change: the state left, how long it lasted, and why |
| `odi_gpon: event=ranging eqd_bits=... protection=... preassigned=... preassigned_delay=... multframe=... inframe=... onu_id=1` | info | once per O4 to O5 transition: what the Ranging_Time decoded to and the delay written |
| `odi_gpon: event=los state=on onu_state=O5` | notice | the downstream LOS bit set, with the state the loss found; sampled at every GPON interrupt and every 100 ms while the ONU is activated, so a fibre pull shows within 100 ms even with no interrupt. The `event=onu_state` line of what it does to the state follows it (`cause=los`) |
| `odi_gpon: event=los state=off lasted_s=42.310 onu_state=O1` | info | the LOS bit clear again |
| `odi_gpon: event=lof state=on onu_state=O5` / `event=lof state=off lasted_s=...` | notice / info | the same for LOF, loss of downstream framing: light without a GPON frame the framer can lock to |
| `omcid: event=start run=boot mode=reregister reason=not_o5 onu_state=O2 rows=0` | notice on `run=restart`, else info | omcid starting: the first start of the boot or a later one (a respawn), and whether it resumed from its snapshot (`mode=resume`) or starts empty for the OLT to re-provision; `reason` is `not_o5`, `no_snapshot`, `bad_snapshot` or `other_device` |
| `omcid: event=mib_reset side=olt rows=161 mib_data_sync=184 services=6` | notice | a MIB reset: `side=olt` from the OLT, `side=local` from `omcicli mib reset`; what it threw away |
| `omcid: event=mib_upload_begin entities=399` / `event=mib_upload_end entities=399 duration_s=0.009` | info | the OLT reading the MIB back; `entities` is the number of upload records announced (an entity takes one record per 26 bytes of its attributes, as on the stock ONU); an upload it abandons has no end line |
| `omcid: event=provision_begin op=set class=256 inst=0 after_mib_reset=1` | info | the first Create, Set or Delete of a burst; `after_mib_reset=1` is a full re-provisioning |
| `omcid: event=provision_end creates=82 sets=102 deletes=0 duration_s=0.827 rows=161 services=6 after_mib_reset=1` | info | 10 s after the burst's last write: what it added up to (Gets and Tests do not count) |
| `omcid: event=alloc_ids count=5 ids=282,794,1050,1306,538` | info | the Alloc-IDs the OLT assigned by PLOAM (`/proc/odi_gpon/status`), when omcid first reads them and whenever they change: ranging, a deallocation, a re-ranging (`ids=none`); the T-CONTs the OLT does not set are bound to them |
| `omcid: event=olt_reboot class=256 inst=0 result=refused` | notice | the OLT asked for a reboot: `result=refused` (`OMCI_ACCEPT_REBOOT` off, answered "command not supported"), `accepted` (answered success, the stick reboots a second later), `acknowledged` (`OMCI_ACCEPT_REBOOT=2`, answered success, no reboot) or, for any class but ONU-G, `not_supported` (docs/SETTINGS.md) |
| `omcid: event=sw_image op=download_start inst=1 size=2621440 window=32 result=ok` | notice | a software download step from the OLT: `op=download_start` (the image size and sections per window), `download_end` (`sections=N`, the sections accepted, and `crc=ok` or `bad`), `activate`, `commit`. `result` is `ok`; `not_supported` with `OLT_SW_DOWNLOAD=reject`; `refused_active` (a download to the image that is active or committed), `refused_invalid` (activate or commit of an image that is not valid), `crc_error`, `size_mismatch`, `short` or `no_download` otherwise. Accepted or not, the image is discarded and nothing is flashed (docs/SETTINGS.md, "Software download from the OLT") |
| `omcid: event=unknown_me class=351 op=create mt=4` | notice | the first request this boot for a managed entity class omcid has no model for, per class and operation (`op` is `create`, `set`, `get`, `delete`, `get_next` or `test`; `mt` the message type); answered "unknown entity" unless `OMCI_UNKNOWN_ME_OK=1` (docs/SETTINGS.md) |
| `omcid: event=unknown_msg type=17 class=256` | notice | the first frame this boot of a message type omcid does not handle, per type, with the class of that first frame; answered "not supported" |
| `omcid: event=vlan_fwdop code=0x06 inst=18 result=unsupported built_as=0x10` | notice | a class 84 forward operation no bridge rule can express (negative filtering, filtering by TCI and MAC address, a code past G.988 table 9.3.11-1): built as 0x10 instead, once per code per omcid run |
| `omcid: event=reload changed=vlan action=rebuild keys=VLAN_MANU_TAG_VID` | notice | omcid took a SIGHUP (`apply.sh omci`) and reread the store: `changed` is `none`, `vlan` (bridge connections rebuilt in place, the ONU stays in O5) or `identity` (`action=reregister`: the ONU is deactivated and ranged again); `keys` names the store keys that differ, never their values. Never rate limited |
| `omcid: event=reload_step verb=gponsn rc=0` | info, notice when `rc` is not 0 | one driver verb of a re-registration (`gpondeact`, `gponsn`, `gponpw`, `gponact`) and its errno (0 is success) |
| `omcid: event=reload_done changed=identity action=reregister result=ok duration_s=44.700 o5_s=41.310 services=6` | notice | the reload ended. `result` is `ok`; `failed` (the driver refused `gpondeact`, nothing touched, or `gponact`, the ONU left off the line); `timeout` (O5 not reached in 150 s); `no_services` (O5, but none built); `sn_not_applied` (back, on the old serial number). `o5_s` is the time from `gponact` to O5 with services |
| `host-autoneg: event=fallback setting=off running=on ...` | notice | the host-port auto-negotiation (`HOST_AUTONEG`): the boot watch, the fallback and every trial; the events are listed under `host-autoneg.sh` above |
| `safe-mode: event=boot_count boot=1 short_before=0 limit=5 uptime=11` | notice | every boot's count, and a safe-mode boot (`event=safe_mode`); listed under `safe-mode.sh` above |
| `omcid: event=suppressed count=12 window_s=60` | notice | omcid's rate limit dropped that many lines in the last minute |

`cause` and `side` on `event=onu_state`:

| cause | side | meaning |
|---|---|---|
| `upstream_overhead`, `assign_onu_id`, `ranging_time` | olt | the OLT ranging this ONU: O2 to O3, O3 to O4, O4 to O5 |
| `deactivate_onu_id` | olt | the OLT deactivated this ONU (Deactivate_ONU-ID): back to O2, its ONU-ID, Alloc-IDs and OMCC port given up until the OLT ranges it again |
| `disable_serial_number` | olt | the OLT disabled this serial number (rogue ONU handling, or by hand at the OLT): O7, laser off (the module's TX-disable line, GPIO 13, driven high; `laser_emergency_stop 1` in `/proc/odi_gpon/status`) |
| `enable_serial_number`, `enable_all_serial_numbers` | olt | re-enabled: O7 to O2, laser on again |
| `popup` | olt | POPUP: O6 back to O5 (directed), or to O4 (broadcast) to be ranged again from the pre-assigned delay |
| `to1_expired` | timer | O3 or O4 to O2: the OLT did not finish ranging within TO1 (10 s from entering O3) |
| `to2_expired` | timer | O6 to O1: no POPUP within TO2 (100 ms); the ONU-ID and every Alloc-ID and OMCC row go, and it ranges again once the light is back |
| `los`, `lof` | line | loss of signal, or of framing alone (G.984.3 Table 10-1, "ONU detects LOS or LOF"): O5 to O6 (TO2 starts), O2, O3 or O4 to O1 |
| `los_cleared` | line | LOS and LOF both clear: O1 to O2, and the OLT ranges the ONU again; also right after `to2_expired` when the light came back inside the O6 window |
| `gponact`, `gpondeact` | local | this side: the boot activation, a respawn re-provisioning (`omci-respawn-reprovision.sh`), a re-registration omcid makes on SIGHUP (`apply.sh omci`), or a write to `/proc/odi_init` by hand |
| `ploam_other`, `ploam_unknown` | olt | a transition after any other downstream message; not expected |

Reading an outage:

- `event=los state=on`, `event=onu_state from=O5 to=O6 ... cause=los`,
  `from=O6 to=O1 ... cause=to2_expired`, then, once the light is back,
  `event=los state=off`, `from=O1 to=O2 ... cause=los_cleared` and the
  ranging (`upstream_overhead`, `assign_onu_id`, `ranging_time`): the
  fibre, or the OLT side of it, and the ONU coming back by itself. With
  the light back, an ONU that goes O2 to O3 and back on `to1_expired`
  again and again is on an OLT that does not take it: a fibre to another
  ISP, or a serial number this OLT does not know.
- `event=onu_state from=O5 ... cause=deactivate_onu_id side=olt` with no LOS:
  the OLT dropped this ONU. A `mib_reset side=olt` and a provisioning burst
  follow if it re-provisions.
- `event=reload changed=identity ...` from omcid, then `cause=gpondeact
  side=local` and, some seconds later, the OLT ranging the ONU again and
  `event=reload_done result=ok`: `apply.sh omci` re-registered the ONU after an
  identity key changed, and the process never restarted (no `event=start`).
- `event=start run=restart mode=reregister` from omcid, then
  `from=O5 ... cause=gpondeact side=local`: omcid died and this side forced
  the re-registration. `mode=resume` and no state change: omcid died and
  nothing on the line noticed.
- `event=olt_reboot result=refused` followed by a deactivation: the OLT
  gave up on a reboot this image refuses (`OMCI_ACCEPT_REBOOT` is off).
- `event=sw_image op=activate ... result=ok`, then a deactivation or a new
  `download_start`: the OLT expected the ONU to reboot into the image it
  sent; omcid only reports it active (docs/SETTINGS.md).
- A board reset by the watchdog leaves no line here (the syslog buffer is in
  RAM, and forwarding stops with the board); the ramlog of the next boot
  records it (docs/KERNEL.md).

What omcid does not model. Each unknown (class, operation) and each unknown
message type is one line per boot, however often the OLT repeats it, and
`/var/log/omcid-unknown.txt` keeps all of them with a count and the uptime of
the first, even the ones the rate limit dropped:

    unknown_me class=351 op=create mt=4 count=1 first_uptime_s=38
    unknown_msg type=17 class=256 count=9 first_uptime_s=39

A respawned omcid reads the file back first, so "once" holds across a
restart; `/var/log` is RAM, so a reboot starts it empty. It holds 32 distinct
entries and counts the rest on an `overflow` line. `diag-bundle.sh` collects
it with the rest of `/var/log`, as `odi-diag/log/omcid-unknown.txt`. On ISP1
the OLT sends class 351 once and message types 17 and 1 to ONU-G three times
each per provisioning: those three lines are what a session looks like there.

Rate limits: the kernel lines have their own printk ratelimit, 30 a minute
(the kernel prints how many it dropped); omcid's, 20 a minute, with an
`event=suppressed` line for what it dropped. A flapping ONU (O2 to O3 to O2
every 10 s) stays well under either.

## Restarting a daemon

Since v1.0.4, every daemon (metricsd, confd, dropbear, omcid, and later
syslogd, klogd and ntpd) is a `respawn` entry in `/etc/inittab` (`docs/SETTINGS.md`, "native over hand-rolled"):
busybox init, which forked each one itself, restarts it the instant it
exits -- no session hang-up can reach the daemon, since it is a child of
init (pid 1), never of your ssh session. So the plain way to
restart one is just to kill it:

    kill $(pidof metricsd)
    kill $(pidof confd)
    kill $(pidof omcid)

each comes back with a new pid within a second or two, no `setsid` needed.
For dropbear, kill the listening process (the one without `-2` in `ps`);
open sessions are separate processes and stay up.

After an omcid restart, check `cat /proc/odi_omci` shows `registered:
type=1` with the new pid, and `omcli state`. The new omcid resumes from its
snapshot when the ONU is still in O5, and otherwise has an empty MIB until
the OLT provisions again. To make it reread the config store there is no
need to restart it: `kill -HUP $(pidof omcid)` does that, and
`/etc/scripts/apply.sh omci` is the same signal with a report of the outcome
(a VLAN change is rebuilt in place, an identity change re-registers the ONU
and interrupts internet for it). Do not start an omcid by hand next to the
one init runs: the second refuses to start while the first holds the OMCI
channel, and one started with `-f` takes it and leaves init's respawn entry
re-provisioning around a stranger.

`/etc/init.d/services {stop|start} <name>` (syslogd, klogd, ntpd, metricsd,
confd, dropbear or omcid) is for turning one off across restarts: `stop` writes
`/etc/config/<name>.off` (so it stays off across a reboot, until removed)
and kills the current process; `start` clears the flag and kills whatever
is currently running under that entry, so init brings the real daemon back
at once. A plain `kill` alone, with no `.off` flag, always just comes back
-- that is the whole point of `respawn`.

## Dangerous or risky commands

See `AGENTS.md` for the same list aimed at anyone (human or AI) changing
this repository:

- `omcicap` takes the OMCI channel from omcid (it refuses without `-f`), and
  `omciprobe -f` writes behind omcid's back.
- Killing omcid leaves the OLT unanswered until init has respawned it.
- `diag register set` and writes to `/proc/odi_init` or `/proc/odi_omci`
  reprogram the hardware under a running stack.
- `igmpd -w` (the daemon or `-j`/`-l`) writes static L2 multicast entries.
  The driver refuses unicast, broadcast and 01:80:c2:00:00:0x addresses, but
  a wrong member mask on a group that carries traffic misforwards it
  silently. `diag l2-table` itself only reads.
- Reading an undecoded SoC register address with `devmem` can stall the bus
  until the watchdog resets the stick.
- `nv setenv sw_commit` removes the trial-boot fallback; `flash_eraseall` on
  the config partition erases the identity (`GPON_SN`, `ELAN_MAC_ADDR`,
  `MAC_KEY`) the line authenticates on.
- `diag` fed from a stdin that never closes waits forever: give it a pipe or
  a file, and a `timeout`.
