# odi-oss

Open firmware for the **ODI DFP-34X-2C2** GPON SFP ONU stick (Realtek
RTL9602C SoC, Lexra RLX5281 CPU, 32 MB RAM, 8 MB NOR flash), built from
source: a current mainline Linux kernel, our own userland, and our own
toolchain.

> [!WARNING]
> **For the ODI DFP-34X-2C2 only** (see "Supported hardware").
>
> **This repository was written with AI assistance ("vibecoded") and
> validated on real hardware, with no warranty of any kind.** Flashing a
> stick with the wrong image, or interrupting a flash, can brick the stick
> or take down the fiber (GPON) service it is carrying. Read "Safety model"
> below and "Quick start" before you flash anything, and never flash a
> stick that is your only connection to the Internet without a way back.

## Supported hardware

**Only the ODI DFP-34X-2C2.** It is the one device this firmware was written
for and the only one it has been tested on. The image drives the board
directly (GPIO numbers, I2C wiring of the optics, flash layout, switch and
PON registers), so on any other stick it will at best not boot and at worst
leave it unreachable.

- **DFP-34X-2C3:** as far as we know it differs from the 2C2 only in the
  polish of the fiber connector: the 2C2 is SC/UPC (blue), the 2C3 SC/APC
  (green). The electronics should be the same, so the image should
  work, but it has **never been tried on a 2C3**. If you do, you are the
  first; please report the result.
- **Anything else** (other ODI models, other RTL9601/RTL9602C sticks,
  rebrands with a different board): not supported, even with the same SoC.

## Safety model

This firmware is meant to be flashed **into the slot the stick is not
currently running from**, and booted **once, on trial**, before it is ever
made permanent:

- The stick has **two boot slots** (kernel + rootfs pairs). You always flash
  the slot you are *not* running, so the working image — normally the stock
  (OEM) firmware — stays untouched in the other slot the whole time.
- A newly flashed slot is **inert** until you tell the bootloader to try it:
  `nv setenv sw_tryactive <slot>` boots that slot **exactly once**, with the
  hardware watchdog armed. If the new image does not come up — kernel panic,
  no root filesystem, `init` never starting — the watchdog resets the board
  and the bootloader falls back to the slot it already trusts. Nothing is
  lost, and nothing needs to be done by hand.
- **Never commit a trial.** Making a slot permanent (`sw_commit`) throws that
  safety net away; do it only after you have booted the trial image, watched
  it work, and are willing to lose the fallback if you are wrong. The full
  procedure, including recovery if something still goes wrong, is in
  [`docs/FLASHING.md`](docs/FLASHING.md).

## What this is

The device shipped with a closed firmware image running Linux 2.6.30. This
repository is not a repack of that image: it builds a complete replacement
firmware from source — kernel, userland tools, daemons and toolchain — for
the same hardware.

| area | stock (OEM) firmware | this firmware |
|---|---|---|
| source | closed | fully open: mainline Linux + our own drivers and userland, all built from source, no proprietary code shipped or linked |
| kernel | Linux 2.6.30 | Linux 6.18.53 (current LTS), carried as 19 changed lines in 6 mainline files plus our own overlay, so the next LTS port is cheap |
| switch / GPON MAC driver | closed | our own GPL driver, an independent implementation, built straight into the kernel |
| GPON/OMCI daemon | closed | our own `omcid`, with `omcli`/`omcicli`, `omciprobe` and `omcicap` tooling around it |
| switch/optics CLI (`diag`) | closed; some commands crash or hang the CLI | our own CLI over our kernel's interfaces: optics, GPON state, alarms and flows, port MIB counters, the MAC table, register access; batches commands and always exits cleanly; the exporter's commands are byte-compatible with the stock CLI |
| optics (DDM) readout | closed | our own SFF-8472 reader, exposed through `diag` |
| multicast | closed IGMP handling | IGMP snooping is off (not used: one UNI port leaves little to prune). `igmpd` ships but is not started (see [`docs/TOOLS.md`](docs/TOOLS.md)) |
| web UI | closed, minimal | `confd` (a separate project): same port; offers only the 21 settings this image reads, each marked LIVE, SERVICE RESTART, INTERRUPTS INTERNET or REBOOT, and applies them without a reboot where it can; the switch MAC table; firmware upload and write to the inactive slot; SSH-key management; build/version info |
| metrics | none | a Prometheus exporter, `metricsd` (a separate project), including whether the OLT actually provisioned service, not just link state |
| SSH | an old dropbear needing legacy algorithms re-enabled on the client | a current dropbear, ed25519 host key, `scp` in both directions |
| login security | telnet enabled by default, and a fixed, well-known default login (`admin`/`admin`) | no telnet; ssh only, with SSH keys or a root password generated per build and stored as a SHA-512 crypt hash |
| memory | about 1.1 MB free after a cache drop, with the stock services running | about 15 MB available: our drivers load the switch and GPON register tables from firmware files, apply them and free them |
| kernel size | fits its partition with little to spare | about 83 KB spare (1,274,429 of 1,359,872 bytes), after cutting kernel features nothing on the stick uses and the driver bring-up scaffolding |
| reboot | not measured | `reboot` resets at once through a watchdog restart handler; back in the stock image about 73 s later |
| toolchain | a decade-old vendor cross-compiler | our own gcc / binutils / uClibc-ng, built from source, targeting the CPU's actual instruction set; published as prebuilt container images pinned by digest, so a build pulls the toolchain instead of compiling it for an hour: a clean image build takes about 3.5 minutes on a native 8-core host (building the toolchain from source is still one command) |
| boot safety | undocumented | one-shot trial boot (`sw_tryactive`) plus a watchdog that self-reverts a hang or a boot whose userland never comes up; boot confirmation does not depend on the network |
| boot reliability | not a concern of the stock image | two bootloader leftovers found and fixed: NIC DMA still running into memory the kernel reuses (stopped first thing at boot), and an instruction cache that does not see new code (invalidated whenever a page can run); release images boot 15 of 15 in a row |
| boot debugging | none (no serial console) | every console line mirrored into DRAM that survives a watchdog reset; the next boot of this image shows the failed one in `/proc/odi_ramlog_prev`, with fatal-signal register dumps and early-boot crumbs |
| testing | none published | host tests of the drivers against a register model, the OMCI daemon replayed under qemu against captures from two ISPs, golden tests of the exporter-facing CLI output, of the whole boot register stream, of every command and `/proc` write the boot script makes, and of the driver calls of an OMCI session per ISP, CI on every change |
| documentation | none | build, flashing and recovery, every setting and toggle, the boot stage by stage (`docs/BOOT.md`), the kernel port, a contributor guide (`docs/HACKING.md`) |

Every row above is backed by something you can read or run in this repo:
[`docs/IMPROVEMENTS.md`](docs/IMPROVEMENTS.md) has the detail and the
verification for each one.

## Quick start

**Build** (needs Docker; see [`docs/BUILDING.md`](docs/BUILDING.md) for the
full breakdown and remote-build option):

    make image-all      # pulls the toolchain; kernel, packages, our tools, the image
    make image          # just the tarball, out/image/<version>.tar, once the pieces above exist

**Flash**, into the slot you are not running, and trial-boot it — never
commit on the first boot. Full procedure, including how to read a boot you
could not otherwise see: [`docs/FLASHING.md`](docs/FLASHING.md).

**First login**, once the trial image is up: ssh as `root`, with
the password `image/build.sh` generated for that build
(`out/image/root-password-<version>.txt`), or the web UI on port 80
(default `admin`/`admin`). Add your own SSH key from there; the root
password stays valid beside it (it is baked into the read-only rootfs, and
`ROOT_PW=` at build time chooses it). Full detail, including every port and how to move files on
and off the stick: [`docs/ACCESS.md`](docs/ACCESS.md).

## Status and limitations

**Tested on real sticks.** The current release,
`odi-oss-260925-618z1` ([`CHANGELOG.md`](CHANGELOG.md)), runs on two
DFP-34X-2C2 sticks on two different ISPs' GPON networks:

- **ISP1**: 15 of 15 consecutive trial boots reached a confirmed userland
  (75-81 s from `reboot`), then a 3-hour soak with a sample every 10
  minutes came out clean: ONU state O5 throughout, every data round trip
  through the stick's own NIC checksummed correctly, no receive or transmit
  errors, no kernel errors, flat free memory.
- **ISP2**: carrying a household's Internet link (PPPoE through the stick,
  about 52 MB/s on a 20 MB download, the same as the image before it).

Both run it as a one-shot trial over the stock firmware, which stays the
committed slot on each; neither has had this image committed.

This is new, actively developed firmware. Before relying on it:

- It targets exactly one board: the ODI DFP-34X-2C2 / RTL9602C. It is not a
  general MIPS or GPON-ONU firmware.
- `diag` is not the stock CLI: it carries only the commands our kernel can
  answer (`diag help` lists them). The handful the Prometheus exporter uses
  keep the stock syntax and output, so the exporter reads either firmware.
- IGMP snooping is off. `igmpd` ships but nothing starts it, and its switch
  writes fail on the 6.18 kernel; our kernel does not deliver IGMP frames to
  it either (the OMCI transport passes only OMCI frames up). The stick does
  not prune multicast.
- The config store is shared with the stock firmware: of its 184 keys this
  image reads 21, and the UI shows the other 163 read-only.
  [`docs/SETTINGS.md`](docs/SETTINGS.md) has each one and what applying it
  costs.
- The watchdog is armed on every boot: if the kernel hangs or the boot
  scripts never finish within 120 s, the board resets (a trial falls back
  to the committed slot). A stick that boots but is unreachable is not
  reset; power-cycle it. See [`docs/FLASHING.md`](docs/FLASHING.md).
- Builds are not bit-reproducible: the kernel and the image manifest embed
  build time, so two builds of the same tree are equivalent, not identical.
- There is no serial console on this device. If a trial boot goes wrong and
  you cannot reach the stick, `docs/FLASHING.md` covers what to do (and what
  it does not); a hardware UART is the last resort, and the point of the
  trial-boot safety model above is to make that resort as rare as possible.

## Documentation

- [`docs/KERNEL.md`](docs/KERNEL.md) — the 6.18 kernel port: CPU, board, boot,
  the config, our drivers, the ramlog, how to build and check it.
- [`docs/TOOLS.md`](docs/TOOLS.md) — every CLI and daemon: what it does, how
  to run and restart it, what it reads, where it logs, who starts it.
- [`docs/SETTINGS.md`](docs/SETTINGS.md) — every setting the web UI and the
  config partition offer: which ones this image actually uses, and which
  need a reboot or interrupt the internet connection.
- [`docs/BUILDING.md`](docs/BUILDING.md) — building the whole image, piece by
  piece, locally or on a remote build host.
- [`docs/FLASHING.md`](docs/FLASHING.md) — the trial-boot procedure and
  recovery.
- [`docs/ACCESS.md`](docs/ACCESS.md) — ports, the web UI, SSH keys, moving
  files on and off the stick.
- [`docs/CROSS-COMPILING.md`](docs/CROSS-COMPILING.md) — compiling your own C
  for this CPU, freestanding or against our libc.
- [`docs/IMPROVEMENTS.md`](docs/IMPROVEMENTS.md) — the improvements table
  above, expanded, with what backs each claim.
- [`docs/LICENSING.md`](docs/LICENSING.md) — what license covers what in this
  tree.
- [`docs/REFERENCES.md`](docs/REFERENCES.md) — the public specifications the
  GPON code is written against, and how to fetch them.
- [`CHANGELOG.md`](CHANGELOG.md) — what each release changed.
- [`docs/kb/README.md`](docs/kb/README.md) — field notes on the device and the
  stock firmware's behaviour, from reverse-engineering real sticks.
- [`docs/HACKING.md`](docs/HACKING.md) — the contributor guide: layout, the
  build, the recovery mechanism in full, testing, the known traps, and every
  feature toggle, for a developer who wants to change this firmware.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — scope, reporting issues, the gates.
- [`AGENTS.md`](AGENTS.md) — repo layout, build/test commands and house rules,
  for anyone (human or AI) working on this codebase.

## License

Our own code (kernel patches and drivers, `src/`, the rootfs skeleton and the
build scripts) is GPL-2.0-only, the same license as the Linux kernel; see
[`LICENSE`](LICENSE).
The toolchain and every upstream package it builds (busybox, dropbear,
iproute2) keep their own upstream licenses. See
[`docs/LICENSING.md`](docs/LICENSING.md) for the full breakdown.
