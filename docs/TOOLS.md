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
| `login` | serial console login | inittab `respawn` | ttyS0 | -- | respawned |
| `diag` | CLI: optics, GPON state, counters, registers | you, rcS, network.sh, metricsd, confd | `/dev/odi_sw`, `/proc/odi_gpon`, netlink | stdout | -- |
| `omcli` / `omcicli` | CLI for omcid | you, metricsd, confd | omcid's queues | stdout | -- |
| `omciprobe` | CLI: one raw driver command | you | odi_omci netlink | stdout | -- |
| `omcicap` | CLI: capture the OMCI channel | you | odi_omci netlink (takes type 1) | stdout | -- |
| `nv` | CLI: the U-Boot environment | you, confd | mtd `env`, `env2` | stdout | -- |
| `flash` | CLI: the config store | confd, you | `/var/config/lastgood*.xml` | stderr | -- |
| `fwu.sh` | CLI: flash one slot | you, `fwu_starter.sh` (from the image tarball) | mtd `k0`/`r0` or `k1`/`r1` | stdout | -- |
| `fwu_starter.sh` | CLI: write an uploaded tarball to the inactive slot | confd, you | runs `fwu.sh` from the tarball | `/tmp/fwu.log`, `/tmp/fwu.state` | -- |
| `apply.sh` | CLI: apply saved settings without a reboot | confd, you | `network.sh addr`; `/proc/odi_init`, omcid | stdout | -- |

Every daemon -- `omcid`, `confd`, `metricsd`, `dropbear` -- and the serial
`login` are all `respawn` entries in `/etc/inittab`: busybox init restarts
whichever one exits, forever, the instant it does (no restart budget, no
rate limit -- `docs/SETTINGS.md`, "native over hand-rolled"). There is no
telnet server (busybox is built without telnetd) and no syslog daemon
running: every daemon's own output is the log.

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

6. drives the optics, the `optics` verb of `/proc/odi_init`
   (`PIN_GPIO_SELECT`, laser TX-enable on GPIO 13);
7. runs the PON steps: `i2c 1`, `i2cen 1`, `gpon`, `rxsd`, `gpondrv`,
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

    omcid [-a] [-r] [-d] [-f] [-w units] [-c caps-hex] [-s state]

    -a          program the switch (apply mode); without it, dry run
    -r          restart: first remove every bridge connection a previous
                omcid left in the switch (with -a; apply.sh passes it)
    -d          daemon: no frame or idle limit
    -w units    exit after this many idle 5 s units (default 24)
    -c hex      use this capability blob instead of the driver one
    -s state    use this ONU state for the resume decision instead of
                asking the driver (test only; see docs/BOOT.md, "Resume
                without re-registration")
    -f          start even if a live process holds redirect type 1
    -h          this text; starts nothing

At startup, before opening the OLT-facing socket, a respawned `omcid` also
decides whether to resume its MIB from `/var/run/omcid-mib.snap` instead of
starting empty -- see docs/BOOT.md, "Resume without re-registration". That
snapshot is written after every Create, Set or Delete, and deleted on a
MIB reset; none of it is a command-line concern beyond `-s`.

rcS runs `/bin/omcid -a -d >> /var/log/omcid.log 2>&1 &` when
`/proc/odi_omci` is writable (the switch driver is there) and
`/etc/config/modules.off` is absent. Arguments are matched whole;
anything unrecognised exits before a socket is opened, and a second omcid
refuses to start while a live one holds redirect type 1 (`-f` overrides).

It reads, from the config store, once at start: the manual VLAN
(`VLAN_MANU_TAG_VID` and `VLAN_MANU_TAG_PRI`, applied only with
`VLAN_CFG_TYPE` 1 and `VLAN_MANU_MODE` 1, as the stock firmware gates them),
the serial number until the kernel reports one (`GPON_SN`), the LOID keys
(answered in the CTC LOID-authentication entity), the five OLT identity keys
(reported only while `/etc/config/omci-identity.on` exists), and
`DUAL_MGMT_MODE` and the `OMCI_CUSTOM_*` masks for display only. Each key is
looked up in both store files, the one xmlconfig assigns it to first. Its
start-up line in the log says what it found (`store: loid ..., manual vlan
..., identity report ...`). A change takes effect with `apply.sh omci`;
`docs/SETTINGS.md` has what each one does.

It logs every OMCI frame in and out (`<-` / `->` lines) and every driver
call to `/var/log/omcid.log`, about 1.7 MB a day on a busy OLT before the
trim. `omcli` and `omcicli` are its clients; `cat /proc/odi_omci` shows the
kernel side (who holds each redirect type, frames delivered and dropped).

On SIGINT, SIGTERM or **SIGHUP** it deregisters from the OMCI channel and
exits, so start it detached from your ssh session (see "Restarting a
daemon" below). While it is not running the OLT's OMCI goes unanswered;
after a restart it starts with an empty MIB and keeps the switch
programming already in place until the OLT provisions it again.

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
with a `client` label (e.g. `client="omcid"`), `wdt_mem`, `wdt_userland`,
`reboot`, `halt`, `poweroff`, `panic`, `oops`, `power` or `unknown`. An
alert on `gpon_last_reset_reason{reason=~"wdt_.*"}` fires after a watchdog
reset. Both need an exporter newer than v1.1.2.

### `dropbear` -- ssh and scp

Started by services as

    /sbin/dropbear -r /etc/config/dropbear.d/ed25519 -D /etc/config/dropbear.d -p 22

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
    BC:24:11:05:B3:24 0   0   7   0    Auto   0   SVL  uc   -     0x06c
    01:00:5E:01:02:03 -   -   -   0    Static -   SVL  mc   0x1   0x17c
    00:E4:06:4C:C6:F4 2   0   7   14   Auto   0   SVL  uc   -     0x270 ctag
    04:9F:CA:78:72:82 0   0   7   0    Auto   0   SVL  uc   -     0x364
    38:3A:21:28:27:C8 3   0   7   0    Auto   0   SVL  uc   -     0x390
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

### `omcli` and `omcicli` -- the omcid client

One binary; `/bin/omcicli` is a symlink to `omcli`. When omcid is running,
omcli uses omcid's own queue and commands:

    omcli state                          serial, device, ONU state, MIB sync
    omcli conn                           the bridge connections omcid built
    omcli flows                          the GEM flow tables, per direction
    omcli mib [all|classId] [entityId]   every managed entity, the ONU's own
                                         and the OLT's, with the values a Get
                                         returns; ends with an `N rows` line
    omcli caps                           the device capability blob, decoded
    omcli tcont                          T-CONT entity id to driver index
    omcli vlan [cs.xml]                  the manual VLAN from the config store,
                                         and whether the tag is applied
    omcli ident [cs.xml hs.xml]          the identity and the OLT identity
                                         keys, and whether those are reported
                                         (passwords are never printed)
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
booted from (`root=` in `/proc/cmdline`) or than `sw_active` in the
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
`once` entry), and again whenever it is run by hand. It only reads the
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
quote, `<` or `&`, and replaces the file by rename so a power cut leaves the
old one. It changes the file and nothing else: whatever reads the key reads
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
partition. `docs/FLASHING.md` is the procedure.

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
connection that asked for it, so the answer leaves first. `omci` writes
`gpondeact` to `/proc/odi_init`, stops omcid (SIGTERM, `kill -9` after 10 s),
starts `setsid /bin/omcid -a -d -r` and waits for it to register, re-sends the
PLOAM password the way rcS does (`gponpw`, from the CS file, skipped when
empty), and after at least three seconds deactivated writes `gponact`. The
OLT then ranges the ONU and provisions it again. Refuses with
`/etc/config/modules.off`. Exits non-zero when omcid did not register (the
ONU is re-activated anyway). Serial number changes are not applied: those
need a reboot.

## Boot scripts you can run again

| script | what | safe to re-run |
|---|---|---|
| `/etc/scripts/network.sh` | host SerDes check (and fix), MAC, `br0` over `eth0.2`, the addresses | yes: it checks before it writes, and re-running it is how a management-path problem is debugged |
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
| `/proc/odi_gpon` | ONU state, ONU id, PLOAM counters, the serial number | -- |
| `/proc/odi_omci` | redirect registrations, frame and command counters | `switch_init`: the platform settings and the module-load replay (rcS does this once) |
| `/proc/odi_init` | the last verb's return code | one SDK init or PON verb (rcS does these once) |
| `/proc/odi_wdt/userland_ok` | -- | `1`: userland is up, stop the 120 s reset (one-shot, boot only) |
| `/proc/odi_wdt/watchdog_flag` | -- | `1`: keep kicking the hardware watchdog |
| `/proc/odi_wdt/register` | -- | `"<name> <deadline_s>"`, e.g. `omcid 60`: register (or update) a watchdog client; idempotent, does not arm anything (`docs/SETTINGS.md`, "Watchdog rules") |
| `/proc/odi_wdt/ping` | -- | `"<name>"`: a registered client's own ping; arms its deadline on the first call, refused (`-EINVAL`) for an unregistered name |
| `/proc/odi_wdt/clients` | one line per registered client: name, deadline, armed, last-ping age | -- |
| `/proc/odi_ramlog_prev` | the previous boot's DRAM ramlog, decoded: this boot's counter and slot, the previous boot's counter, slot, build id, last early crumb and reset reason (`reason=`, `docs/HACKING.md`), then its first 3984 bytes and its last 4080 (root only) | -- |
| `/proc/odi_ramlog_prev_raw` | the same two pages as 8192 raw bytes, page A then page B, for `ramlog-read.sh`-style decoding off the stick (root only) | -- |

Writing a verb or an init command by hand re-programs the switch or the
PON MAC under a running omcid; do it on a trial stick only.

## Logs

| where | what | survives |
|---|---|---|
| `/var/log/omcid.log` | every OMCI frame and driver call, omcid's startup | nothing (RAM, trimmed to 128-256 KB) |
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
message: a whole provisioning session is six lines.

The kernel's lines are printk (`kernel: odi_gpon: event=...`, facility kern),
which klogd hands to syslogd; the ONU state machine lives in the driver, so
that is where they come from. omcid's are `omcid[pid]: event=...`, facility
daemon, sent to `/dev/log` the way syslog(3) does (omcid has no libc), and
the same line goes to `/var/log/omcid.log` among the frames around it.

| line | level | when |
|---|---|---|
| `odi_gpon: event=onu_state from=O5 to=O2 in_state_s=3605.120 cause=deactivate_onu_id side=olt onu_id=0` | notice when leaving O5 or entering O7, else info | every ONU state change: the state left, how long it lasted, and why |
| `odi_gpon: event=los state=on onu_state=O5` | notice | the downstream LOS bit set; sampled at every GPON interrupt and every BER interval (10 s on both ISPs), which keeps running in O5 when the light goes |
| `odi_gpon: event=los state=off lasted_s=42.310 onu_state=O5` | info | the LOS bit clear again |
| `omcid: event=start run=boot mode=reregister reason=not_o5 onu_state=O2 rows=0` | notice on `run=restart`, else info | omcid starting: the first start of the boot or a later one (a respawn, `apply.sh omci`), and whether it resumed from its snapshot (`mode=resume`) or starts empty for the OLT to re-provision; `reason` is `not_o5`, `no_snapshot`, `bad_snapshot` or `other_device` |
| `omcid: event=mib_reset side=olt rows=161 mib_data_sync=184 services=6` | notice | a MIB reset: `side=olt` from the OLT, `side=local` from `omcicli mib reset`; what it threw away |
| `omcid: event=mib_upload_begin entities=301` / `event=mib_upload_end entities=301 duration_s=0.009` | info | the OLT reading the MIB back; an upload it abandons has no end line |
| `omcid: event=provision_begin op=set class=256 inst=0 after_mib_reset=1` | info | the first Create, Set or Delete of a burst; `after_mib_reset=1` is a full re-provisioning |
| `omcid: event=provision_end creates=82 sets=102 deletes=0 duration_s=0.827 rows=161 services=6 after_mib_reset=1` | info | 10 s after the burst's last write: what it added up to (Gets and Tests do not count) |
| `omcid: event=olt_reboot class=256 inst=0 result=not_supported` | notice | the OLT asked for a reboot; omcid does not do it |
| `omcid: event=sw_image op=download_start inst=1 result=not_supported` | notice | a software download (`op=download_start`, `download_end` with `sections=N`, `activate`, `commit`); omcid refuses each |
| `omcid: event=suppressed count=12 window_s=60` | notice | omcid's rate limit dropped that many lines in the last minute |

`cause` and `side` on `event=onu_state`:

| cause | side | meaning |
|---|---|---|
| `upstream_overhead`, `assign_onu_id`, `ranging_time` | olt | the OLT ranging this ONU: O2 to O3, O3 to O4, O4 to O5 |
| `deactivate_onu_id` | olt | the OLT deactivated this ONU (Deactivate_ONU-ID): back to O2 |
| `disable_serial_number` | olt | the OLT disabled this serial number (rogue ONU handling, or by hand at the OLT): O7, laser off |
| `enable_serial_number`, `enable_all_serial_numbers` | olt | re-enabled: O7 to O2 |
| `popup` | olt | POPUP: O6 back to O4 or O5 |
| `to1_expired` | timer | O3 or O4 to O2: the OLT did not finish ranging within TO1 (10 s) |
| `to2_expired` | timer | O6 to O1: no POPUP within TO2 |
| `los`, `los_cleared` | line | loss of signal; not wired to the state machine today (`event=los` reports the bit instead), listed for when it is |
| `gponact`, `gpondeact` | local | this side: the boot activation, a respawn re-provisioning (`omci-respawn-reprovision.sh`), `apply.sh omci`, or a write to `/proc/odi_init` by hand |
| `ploam_other`, `ploam_unknown` | olt | a transition after any other downstream message; not expected |

Reading an outage:

- `event=los state=on`, then `event=onu_state from=O5 ... side=olt` once the
  light is back: the fibre, or the OLT side of it.
- `event=onu_state from=O5 ... cause=deactivate_onu_id side=olt` with no LOS:
  the OLT dropped this ONU. A `mib_reset side=olt` and a provisioning burst
  follow if it re-provisions.
- `event=start run=restart mode=reregister` from omcid, then
  `from=O5 ... cause=gpondeact side=local`: omcid died and this side forced
  the re-registration. `mode=resume` and no state change: omcid died and
  nothing on the line noticed.
- `event=olt_reboot` followed by a deactivation: the OLT gave up on a
  reboot this image refuses.
- A board reset by the watchdog leaves no line here (the syslog buffer is in
  RAM, and forwarding stops with the board); the ramlog of the next boot
  records it (docs/KERNEL.md).

Rate limits: the kernel lines have their own printk ratelimit, 30 a minute
(the kernel prints how many it dropped); omcid's, 20 a minute, with an
`event=suppressed` line for what it dropped. A flapping ONU (O2 to O3 to O2
every 10 s) stays well under either.

## Restarting a daemon

Since v1.0.4, all four (metricsd, confd, dropbear, omcid) are `respawn`
entries in `/etc/inittab` (`docs/SETTINGS.md`, "native over hand-rolled"):
busybox init, which forked each one itself, restarts it the instant it
exits -- there is no session to SIGHUP any more, since the daemon is a
child of init (pid 1), never of your ssh session. So the plain way to
restart one is just to kill it:

    kill $(pidof metricsd)
    kill $(pidof confd)
    kill $(pidof omcid)

each comes back with a new pid within a second or two, no `setsid` needed.
For dropbear, kill the listening process (the one without `-2` in `ps`);
open sessions are separate processes and stay up.

After an omcid restart, check `cat /proc/odi_omci` shows `registered:
type=1` with the new pid, and `omcli state`. The new omcid has an empty
MIB until the OLT provisions again, so settings it reads from the MIB or
the config store are not re-applied by a restart alone.
`/etc/scripts/apply.sh omci` is the restart that does re-apply them: it
re-ranges the ONU around the restart, and interrupts internet for it.

`/etc/init.d/services {stop|start} <name>` (metricsd, confd, dropbear or
omcid) is for turning one off across restarts: `stop` writes
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
- Killing omcid, or letting an ssh session hang it up, leaves the OLT
  unanswered until it is started again.
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
