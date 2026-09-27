# Settings

What you can configure on a stick running this image, what each setting
actually does here, and what applying it costs. The config store this image
shares with the stock firmware holds 184 keys; this image reads 21 of them,
and the web UI (`confd`) offers only those for editing. This page says which
is which, and why each one costs what it does.

The programs named below are described in [`docs/TOOLS.md`](TOOLS.md).

## Read this first

**Where settings live.** In two XML files on the jffs2 `config` partition,
`/var/config/lastgood.xml` (CS: the service settings) and
`/var/config/lastgood_hs.xml` (HS: the hardware identity), reached as
`/etc/config/`. The web UI writes them through `/etc/scripts/flash set`,
which changes the file and nothing else; applying is a separate step (below).
A few more settings are plain files on the same partition ("Switch files").

**The partition is shared with the other slot.** Reflashing never touches
it, and whichever slot boots reads it. On a stick that keeps the stock
firmware as its committed slot, every key here -- including the 163 this
image ignores -- is still what the stock firmware reads when the stick
falls back to it. Do not delete keys this image does not use, and keep
backups whole: the UI's backup and restore carry every key.

**Apply classes.** Every setting and control is marked with one of four
classes, in this document and beside it in the UI:

| class | meaning | how it is applied |
|---|---|---|
| **LIVE** | takes effect at once, no interruption | `apply.sh network`; the UI runs it straight after the save |
| **SERVICE RESTART** | a daemon restarts; the fibre service stays up | none of the keys needs this today |
| **INTERRUPTS INTERNET** | applied without a reboot, but the fibre service drops while it is | `apply.sh omci`; the UI offers "Apply now" behind a confirmation |
| **REBOOT** | read only at boot | a reboot; the UI offers "Reboot now" behind a confirmation |

**What INTERRUPTS INTERNET costs.** `apply.sh omci` deactivates the ONU,
restarts omcid, and re-activates it: the OLT sees the ONU range again,
resets its MIB and provisions every service from scratch, and the new omcid
builds them with the settings as they now are. Measured on ISP1 with the
6.18 kernel: the ONU was back at O5, with its six OMCI services provisioned
again and one omcid running, about 13 s after `apply.sh omci` started. The
provisioning after the re-activation takes as long as the OLT takes, so
another OLT may be slower.
Without the re-activation a restart would change nothing the OLT sees:
omcid reads the store once, at start, and builds connections only when the
OLT provisions them.

**Every REBOOT also interrupts internet.** A reboot takes the ONU off the
line: on ISP1, 75 to 81 s from `reboot` until userland confirms (the
management address answers), then the PON steps range the ONU back to O5. And on a stick running
a **trial slot**, a reboot does not come back to this image: it boots the
committed slot, which is normally the stock firmware (`docs/FLASHING.md`).
The UI's reboot confirmation says which slot it comes back on.

## Settings this image reads

| key | UI label | what it does here | read by | apply |
|---|---|---|---|---|
| `LAN_IP_ADDR` | Management IP | the `br0` address ssh, the UI and the exporter answer on | `network.sh` | LIVE |
| `LAN_SUBNET` | Management netmask | its netmask | `network.sh` | LIVE |
| `LAN_ENABLE_IP2` | Second management address | 1 adds a second address on `br0:2`, 0 removes it | `network.sh` | LIVE |
| `LAN_IP_ADDR2` | Second management IP | that address (both sticks store 192.168.100.1) | `network.sh` | LIVE |
| `LAN_SUBNET2` | Second management netmask | its netmask | `network.sh` | LIVE |
| `VLAN_CFG_TYPE` | VLAN config mode | 1 lets the manual tag through; anything else is no manual tag | omcid | INTERRUPTS INTERNET |
| `VLAN_MANU_MODE` | Manual VLAN mode | 1 tags with the VID and priority below; anything else is no manual tag | omcid | INTERRUPTS INTERNET |
| `VLAN_MANU_TAG_VID` | Service VLAN ID | the C-VLAN the ONU adds upstream and strips downstream on the services the OLT provisions | omcid | INTERRUPTS INTERNET |
| `VLAN_MANU_TAG_PRI` | VLAN priority | the 802.1p bits of that tag | omcid | INTERRUPTS INTERNET |
| `GPON_PLOAM_PASSWD` | PLOAM password (identity) | the PLOAM password, stored as hex; empty means none is sent | rcS `gponpw auto`; `apply.sh omci` re-sends it | INTERRUPTS INTERNET |
| `LOID`, `LOID_PASSWD` | LOID, LOID password | answered to CTC-profile OLTs in the LOID authentication entity | omcid | INTERRUPTS INTERNET |
| `LOID_OLD`, `LOID_PASSWD_OLD` | LOID (in force) | the value actually answered when it differs from the one above | omcid | INTERRUPTS INTERNET |
| `OMCI_SW_VER1`, `OMCI_SW_VER2` | Software version, image 0/1 | the version reported for each software image (ME 7 attribute 1, 14 characters) | omcid, with `omci-identity.on` | INTERRUPTS INTERNET |
| `GPON_ONU_MODEL` | ONU model (identity) | the equipment id in ONU2-G (attribute 1, 20 characters) | omcid, with `omci-identity.on` | INTERRUPTS INTERNET |
| `OMCC_VER` | OMCC version | ONU2-G attribute 2, decimal | omcid, with `omci-identity.on` | INTERRUPTS INTERNET |
| `OMCI_VENDOR_PRODUCT_CODE` | Vendor product code | ONU2-G attribute 3, decimal | omcid, with `omci-identity.on` | INTERRUPTS INTERNET |
| `ELAN_MAC_ADDR` | UNI MAC address (identity) | the MAC of `eth0`, `eth0.2` and `br0` | `network.sh` at boot | REBOOT |
| `GPON_SN` | ONU serial number (identity) | the serial the OLT authenticates; a wrong value means no service | rcS `gponsn auto` | REBOOT |

Why the two REBOOT keys cannot be applied live:

- `ELAN_MAC_ADDR`: Linux refuses a new MAC on an interface that is up, so
  `network.sh` sets it on `eth0` and its host port with the links down,
  before `br0` exists. Doing that on a running stick takes down every way in
  in the middle of the change, with nothing to put it back if it fails.
- `GPON_SN`: the kernel writes the serial into the PON MAC once, at the
  first activation of a boot (the `gpon_init` replay); a later `gponsn` verb
  updates only its own copy, not what goes upstream. Rewriting it live needs
  a kernel change.

Details that bite:

- **`/etc/config/lan-ip`, when it exists, overrides `LAN_IP_ADDR`**, and the
  netmask then falls back to 255.255.255.0 whatever `LAN_SUBNET` says. With
  neither, the address is 192.168.1.1/24. The UI says the key is ignored
  while the file exists. `apply.sh network` follows the same precedence.
- **Changing `LAN_IP_ADDR` moves every way in, at once.** The UI asks first,
  then links to the new address. Turning on the second address beforehand
  gives a way back that does not move.
- **The manual VLAN tag is gated the way the stock firmware gates it**
  (`/etc/runomci.sh`): applied only with `VLAN_CFG_TYPE` 1, `VLAN_MANU_MODE` 1,
  and both a VID and a priority present. With it absent or gated off,
  services carry no manual tag. Both lines run 1/1 (ISP1: VID 11, priority 0,
  read 2026-09-24; ISP2: VID 10 in the stored backup), so neither changes.
  `omcli vlan` prints whether the tag is applied.
- **The OLD LOID wins.** When `LOID` and `LOID_OLD` differ, the OLD value is
  answered -- including an empty OLD beside a set LOID, which answers
  nothing; the same for the passwords. That is the stock rule, kept so both
  slots agree. The UI writes `LOID_OLD` together with `LOID` (and the
  password pair), and shows the `_OLD` keys read-only. Untested: neither
  ISP1 nor ISP2 uses a LOID, and neither runs against a CTC OLT.
- **omcid looks for each key in both files**, the one xmlconfig assigns it
  to first. ISP1 keeps `GPON_PLOAM_PASSWD` and `LOID_PASSWD_OLD` in the CS
  file, and until 2026-09-24 omcid looked in HS only and reported both as
  missing (display only; rcS reads the right file, so ranging was fine).
- **The identity keys (`GPON_SN`, `GPON_PLOAM_PASSWD`, `ELAN_MAC_ADDR`,
  `GPON_ONU_MODEL`) need the UI's confirmation tick.** They cannot be
  regenerated: take a backup first.
- **A key cannot be cleared from the UI**: `flash set` refuses an empty value.

### The OLT identity keys, and their switch

`OMCI_SW_VER1`, `OMCI_SW_VER2`, `GPON_ONU_MODEL`, `OMCC_VER` and
`OMCI_VENDOR_PRODUCT_CODE` are reported to the OLT **only while
`/etc/config/omci-identity.on` exists** (the UI's "OLT identity" switch on the
Config tab). Off, or with a key empty or absent, omcid answers what it always
has:

| entity, attribute | switch off, or key empty | switch on, key set |
|---|---|---|
| software image 0 and 1, Version | `0.0.0` | `OMCI_SW_VER1`, `OMCI_SW_VER2` |
| ONU2-G, Equipment id | the device id (`RTL9602C`) | `GPON_ONU_MODEL` |
| ONU2-G, OMCC version | 0x80 | `OMCC_VER` |
| ONU2-G, Vendor product code | 15, the captured stock value | `OMCI_VENDOR_PRODUCT_CODE` |

The switch exists because the plain rule -- an empty key keeps today's value
-- does not keep today's value on our sticks: **both already store all five,
written by the stock firmware**. ISP1 (read 2026-09-24): `OMCI_SW_VER1` and
`OMCI_SW_VER2` `V1.0-220923`, `OMCI_VENDOR_PRODUCT_CODE` 15, `OMCC_VER` 128,
`GPON_ONU_MODEL` `IGD`; ISP2's backup holds the same model, product code and
OMCC version and other version strings. Honouring them unconditionally would
change the software version both OLTs see from `0.0.0` to a stock string and
the equipment id from `RTL9602C` to `IGD`. With the switch off, nothing
changes. The stock firmware may also rewrite `OMCI_SW_VER1/2` itself when it
boots, so a value set here does not necessarily survive a fall-back to the
stock slot. `omcli ident` shows what is stored and whether it is reported.

## Web UI controls

| control (tab) | on this image | apply |
|---|---|---|
| Save (Config) | writes the keys, reads each back, then applies LIVE ones and offers the rest | per key, above |
| Apply now (save bar, OLT identity) | `apply.sh omci`, after a confirmation | INTERRUPTS INTERNET |
| Reboot now (save bar, Firmware) | reboots, after a confirmation naming the slot it comes back on | REBOOT |
| OLT identity switch (Config) | creates or removes `/etc/config/omci-identity.on` | INTERRUPTS INTERNET (with Apply now) |
| Stock keys (tab) | the 163 keys only the stock firmware reads, read-only | -- |
| Config UI credentials (Admin) | `/etc/config/confd.auth` | LIVE (sign in again) |
| SSH keys (Admin) | `/etc/config/dropbear.d/authorized_keys`, read at every login | LIVE |
| Download a full config backup (Admin) | both stores, every key, identity included | -- |
| Restore (Admin) | writes the differences, every key, through the same checks as Save | per key; restored keys are not applied |
| Reset the service config (Admin) | `flash default cs`: merges `/etc/config_default.xml` into CS (below) | LOID keys: INTERRUPTS INTERNET; the rest are stock-only |
| Ping (Tools) | IPv4 literals, three packets | LIVE |
| Kernel log (Tools) | the ring buffer, read without consuming it | -- |
| Status: optics, ONU state, alarms, counters | `diag` batch | -- |
| Read the MAC table (Status) | works: confd runs `diag l2-table get entry address valid` (the stock spelling of `diag l2-table get all`), which walks the switch L2 lookup table through `/dev/odi_sw`; read-only | -- |
| Services, MIB | `omcicli` answered by omcid | -- |
| Firmware: Upload | into `/tmp/img.tar`, which is RAM (8 MB cap) | -- |
| Firmware: Write | `fwu_starter.sh`: the inactive slot only, in the background; the page follows its log | -- (then Try) |
| Firmware: Try a partition | sets `sw_tryactive` with our `nv` | REBOOT |
| Firmware: Keep | sets `sw_commit`: removes the fallback to the other slot | at the next boot |

**Reset** merges the image defaults instead of erasing: every key
`/etc/config_default.xml` names (the four LOID keys, emptied; `DEVICE_TYPE`,
`DUAL_MGMT_MODE` and the three `OMCI_CUSTOM` masks, which only the stock
firmware reads) takes its default, a default key absent from both files is
added, and every other key -- the management addresses, the VLAN, the PLOAM
password, the identity -- is left alone. The HS file is never touched; a
default key that lives there (`LOID_PASSWD_OLD` on a stick that keeps it
where xmlconfig puts it) stays as it is. The reset keys are reset for the
stock slot too, because the store is shared. The UI downloads a backup
before it starts.

The Firmware tab shows, for the running partition, the image it runs
(`image=` in `/etc/odi-build`, or `/etc/version`), and the U-Boot record
`sw_version0/1` beside it when they differ: `fwu.sh` does not update that
record unless asked, so it often still names the firmware the slot held
before.

## Keys this image stores but does not use

Writing any of these changes the file and nothing else on this image; the UI
shows them read-only on its Stock keys tab. They still steer the stock
firmware if the stick falls back to it, so leave them as they are unless you
mean to change the stock slot -- over ssh with `flash set`, or through a
restore.

| keys | why |
|---|---|
| `DEVICE_TYPE` | this image is a bridge (SFU) only |
| `OMCI_CUSTOM_BDP`, `OMCI_CUSTOM_RDP`, `OMCI_CUSTOM_MCAST`, `OMCI_CUSTOM_ME`, `DUAL_MGMT_MODE` | select stock OMCI plugins; omcid only displays them (`omcicli get cflag`, `get dmmode`) |
| `OMCI_FAKE_OK`, `OMCI_OLT_MODE`, `OMCI_VEIP_SLOT_ID`, `OMCI_PORT_TYPE`, `OMCI_TM_OPT`, `OMCI_WAN_QOS_QUEUE_NUM` | stock OMCI stack options with no counterpart in omcid |
| `OMCI_LOGFILE`, `OMCI_LOGFILE_MASK`, `OMCI_DBGLVL` | omcid always logs every frame to `/var/log/omcid.log` |
| `PON_MODE`, `PON_DETECT_ENABLE`, `EPON_*` | this image is GPON only and never rewrites the PON type |
| `PON_VENDOR_ID`, `MAC_KEY`, `GPON_PLOAM_FORMAT` | the vendor id comes from `GPON_SN`; the other two are read by the stock firmware only. Keep them in backups |
| `FIBER_MODE`, `LAN_SDS_MODE` | `network.sh` always sets the host SerDes to Fiber 1G; the UI never writes them |
| `LASER_POLARITY_TYPE`, `PON_LED_SPEC` | rcS drives the laser enable itself; there is no PON LED |
| `SUSER_NAME`, `SUSER_PASSWORD`, `USER_*`, `E8BDUSER_*`, `SUPER_*` | stock firmware accounts; this image logs in as root with SSH keys or the per-build password |
| `SNOOPING_ENABLED`, `RTK_IGMP_*` | `igmpd` can program the switch now, but it never receives a frame: the switch forwards IGMP instead of trapping it, nothing delivers a trapped frame to it, and it cannot send one on, so turning snooping on would cut the OLT off from the reports. With one UNI the only gain would be keeping unknown multicast off the CPU port. `docs/TOOLS.md` has the details |
| `DNS_MODE`, `DNS1`-`DNS3` | a bridge resolves nothing |
| `SW_PORT_TBL[*]`, `AUTO_PVC_SEARCH_TBL[*]` | omcid programs the switch from the OLT's MIB |
| `WAN_*`, `LAN_RIP`, `LAN_AUTOSEARCH`, `UPNP*`, `SYSLOG*`, `NTP_EXT_ITF`, `TFTP_SERVER_ADDR`, `POSIX_TZ_STRING`, `REBOOT_TIME`, `DHCP_PORT_FILTER`, `BR_*`, `SNMP_SYS_NAME`, `DEVICE_NAME`, `RTK_DEVID_*`, `RTK_DEVINFO_*`, `HW_CWMP_*`, `INIT_*`, `MP_*`, `SPC_*`, `MIB_*_MAC_CTRL`, and the other stock gateway keys (`BYTE`, `WORD`, `OUI`, `PORT_REMAPPING`, ...) | home-gateway and TR-069 features of the stock firmware |
| `ADSL*`, `HW_WLAN*`, `WLAN_MAC_ADDR`, `HW_RF_TYPE` and the other radio calibration keys | hardware this board does not have |

The list the UI uses is odi-ui `schema/settings.tsv`; a key missing from it
is a key the UI will not offer, so the two must change together.

## Switch files on the config partition

Plain files under `/etc/config`, created with `touch` or `echo`, removed
with `rm`. They survive reflashing like the keys do, so a switch left behind
applies to every later image.

| file | effect | apply |
|---|---|---|
| `omci-identity.on` | report the five OLT identity keys (above); the UI toggles it | INTERRUPTS INTERNET (`apply.sh omci`) |
| `dropbear.off`, `confd.off`, `metricsd.off` | that daemon is not started | REBOOT |
| `lan-ip` | the management address, one line; overrides `LAN_IP_ADDR` | LIVE (`apply.sh network`) |
| `pon-steps` | development: a PON step list that replaces the built-in one (`docs/BOOT.md`); a wrong list means no PON | REBOOT |
| `sds.off` | skip the host SerDes check and fix in `network.sh` | REBOOT |
| `optics.off` | skip the optics setup: **the laser stays off, no service** | REBOOT |
| `modules.off` | skip the platform init and omcid: **no OMCI, no service**; `apply.sh omci` refuses | REBOOT |
| `breadcrumbs.on` | development: boot crumbs, `trial-diag.txt` and a heartbeat, on flash | REBOOT |
| `confirm-arp` | development only: confirm the boot to the watchdog only after an ARP reply from the `.2` of the `br0` subnet, so an unreachable trial reverts by itself; **a stick whose host is not on `.2` then resets every 120 s** | REBOOT |
| `confd.auth` | the UI credential, `user:password` | LIVE |
| `dropbear.d/authorized_keys` | root's SSH keys | LIVE |
| `confd/` | replacement web UI files and `.tsv` tables, by name | LIVE |

Every REBOOT row interrupts internet, as every reboot does. The three
development rows are read by `/etc/init.d/rcS.dev` and exist for trial
boots; `docs/BOOT.md` has what each does. Of them only `confirm-arp` can
reboot a stick by itself: left on one whose host is not on `.2`, it resets
every 120 s.

Files of earlier images that this one ignores, and that can be removed:
`bdgconn-probe` (an older image rebooted unconditionally after the seconds
it held), `skip-steps`, `replay/`, `regtrace.*`, `regtrace-mark`,
`regtrace-all`, `modload.mask`, `sdkinit.mask` and `parity.*`.

`docs/HACKING.md` ("Feature toggles") is the developer reference behind this
table: where each file is read, its default, and which ones are development
aids, together with the kernel parameters, build variables and
`/proc` control files.

## Resilience (v1.0.2)

Not config-partition settings -- there is no UI or file toggle for any of
these, they are fixed at boot -- but they answer the same question this
document answers for everything else: what does this image actually do,
and what does applying it cost. Background: a 20 MB `scp` into `/tmp` on
ISP1 (2026-09-27) exhausted RAM (`/tmp` was ramfs, unbounded and
unreclaimable), the OOM killer took dropbear, confd and omcid, and none of
them restarted -- the stick stayed at O5 (the hardware datapath keeps
forwarding on its own) but was unmanageable until a power cycle.

| what | value | why |
|---|---|---|
| `/tmp` (`/var/tmp`) size cap | 8 MB tmpfs | fits a firmware upload (`fwu_starter.sh` stages the tarball there, about 2.6 MB, plus its unpacked squashfs/uImage) and an scp of a few MB with room to spare; past it, a write gets `ENOSPC`, not a system-wide OOM |
| `/var` (log/run/lock/config fallback) size cap | 6 MB tmpfs | `/var/log` is separately trimmed at 256 KB past a 128 KB floor already (rcS); this is the outer bound if that trim ever falls behind |
| `oom_score_adj` | omcid, dropbear: `-1000` (never killed while anything else can be); confd: `-500`; metricsd: `0`, the kernel default | omcid and dropbear are what keeps the ONU provisioned and the box reachable; confd is a convenience next after them; metricsd is the one daemon whose loss costs neither -- first in line if the killer has to take something |
| respawn | omcid, dropbear, confd, metricsd: restarted automatically if they die, rate-limited to 5 restarts per 60 s window, then the supervisor gives up and logs why (`supervise()`, `rootfs/skeleton/etc/scripts/supervise.sh`) | a daemon that cannot stay up for a minute is a problem for the health kicker/watchdog below to escalate, not something a restart loop should spin on forever |
| health kicker period | 30 s (`ODI_WDT_HEALTH_PERIOD_S`, `kernel/extra/drivers/net/ethernet/odi/odi_wdt.h`) | a supervised process (`rootfs/skeleton/etc/scripts/health-kicker.sh`) writes `/proc/odi_wdt/health_kick` every 30 s, but only while omcid is running (or intentionally off, `modules.off`) and `MemAvailable` is at or above the floor below. A kick that arrives late by more than the period resets the board through the watchdog -- independent of, and in addition to, the one-shot boot confirmation `/proc/odi_wdt/userland_ok` already gave |
| health kicker memory floor | 2048 KB `MemAvailable` (`HEALTH_MEM_FLOOR_KB`) | below this the box is judged to be in the same state the 2026-09-27 OOM left it in (0.86 MB free) -- reachable in principle but not usably so, and worth resetting out of automatically rather than waiting for someone to notice |
| `vm.min_free_kbytes` | 1536 (`rcS`, up from the kernel's own default of roughly 128 KB on a box this size) | keeps a slightly larger page-allocator reserve free under pressure, so the OOM killer and a starved `oom_score_adj -1000` daemon get a better chance to make forward progress instead of every allocator racing for the same last few pages |

`make test-qemu` (`docs/HACKING.md`) exercises all of the above except the
watchdog reset itself, which needs `/proc/odi_wdt` -- real hardware, not the
stock kernel the harness boots.

## Not verified yet

- `apply.sh omci` against more than one OLT: it was run end to end once, on
  ISP1 (above).
- `apply.sh network` moving the primary address on a stick. Adding and
  removing the second address live was checked on ISP1 (`br0:2` came and
  went, the primary address and O5 untouched); the host test drives a stub
  `ifconfig` for the rest.
- `fwu_starter.sh` writing a slot from the UI (the guards and the background
  job are host-tested; `fwu.sh` itself is the flasher used by hand).
- The OLT identity keys against an OLT that checks them; LOID against a CTC
  OLT. Untested: neither ISP1 nor ISP2 does either.
