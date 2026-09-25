# odi-oss

Open firmware for the **ODI DFP-34X-2C2** GPON SFP ONU stick (Realtek
RTL9602C SoC, Lexra RLX5281 CPU, 32 MB RAM, 8 MB NOR flash), built from
source: a current mainline Linux kernel, our own userland, and our own
toolchain.

> [!WARNING]
> **This repository was written with AI assistance ("vibecoded") and
> validated on real hardware, with no warranty of any kind.** Flashing a
> stick with the wrong image, or interrupting a flash, can brick the stick
> or take down the fiber (GPON) service it is carrying. Read "Safety model"
> below and "Quick start" before you flash anything, and never flash a
> stick that is your only connection to the Internet without a way back.

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
| kernel | Linux 2.6.30 | Linux 6.18 (current LTS) |
| switch / GPON MAC driver | closed | our own GPL driver, an independent implementation, built straight into the kernel |
| GPON/OMCI daemon | closed | our own `omcid`, with `omcli`/`omcicli`, `omciprobe` and `omcicap` tooling around it |
| switch/optics CLI (`diag`) | closed; some commands crash or hang the CLI | our own small CLI over our kernel's interfaces: optics, GPON state, alarms and flows, port MIB counters, register access; batches commands and always exits cleanly |
| optics (DDM) readout | closed | our own SFF-8472 reader, exposed through `diag` |
| multicast | closed IGMP handling | `igmpd`, an IGMP snooping daemon, observe-only and not started by default (see [`docs/TOOLS.md`](docs/TOOLS.md)) |
| web UI | closed, minimal | `confd` (a separate project): same port, SSH-key management, build/version info |
| metrics | none | a Prometheus exporter, `metricsd` (a separate project), including whether the OLT actually provisioned service, not just link state |
| SSH | an old dropbear needing legacy algorithms re-enabled on the client | a current dropbear, ed25519 host key, `scp` in both directions |
| toolchain | a decade-old vendor cross-compiler | our own gcc / binutils / uClibc-ng, built from source, targeting the CPU's actual instruction set |
| boot safety | undocumented | one-shot trial boot (`sw_tryactive`) plus a watchdog that self-reverts a hang |

Every row above is backed by something you can read or run in this repo:
[`docs/IMPROVEMENTS.md`](docs/IMPROVEMENTS.md) has the detail and the
verification for each one.

## Quick start

**Build** (needs Docker; see [`docs/BUILDING.md`](docs/BUILDING.md) for the
full breakdown and remote-build option):

    make image-all      # toolchain, kernel, packages, our tools, the image
    make image          # just the tarball, out/image/<version>.tar, once the pieces above exist

**Flash**, into the slot you are not running, and trial-boot it — never
commit on the first boot. Full procedure, including how to read a boot you
could not otherwise see: [`docs/FLASHING.md`](docs/FLASHING.md).

**First login**, once the trial image is up: ssh as `root`, with
the password `image/build.sh` generated for that build
(`out/image/root-password-<version>.txt`), or the web UI on port 80
(default `admin`/`admin`). Add your own SSH key and turn the password login
off from there. Full detail, including every port and how to move files on
and off the stick: [`docs/ACCESS.md`](docs/ACCESS.md).

## Status and limitations

This is new, actively developed firmware. Before relying on it:

- It targets exactly one board: the ODI DFP-34X-2C2 / RTL9602C. It is not a
  general MIPS or GPON-ONU firmware.
- `diag` is not the stock CLI: it carries only the commands our kernel can
  answer (`diag help` lists them). The handful the Prometheus exporter uses
  keep the stock syntax and output, so the exporter reads either firmware.
- `igmpd` ships but nothing starts it, and its switch writes fail on the
  6.18 kernel. Treat multicast/IGMP snooping as not implemented.
- The web UI edits the stock firmware's config store, and most of its keys do
  nothing on this image; a few of its buttons (Apply, Reset, firmware Write)
  cannot work here. [`docs/SETTINGS.md`](docs/SETTINGS.md) says which.
- Builds are not bit-reproducible: the kernel and the image manifest embed
  build time, so two builds of the same tree are equivalent, not identical.
- There is no serial console on this device. If a trial boot goes wrong and
  you cannot reach the stick, `docs/FLASHING.md` covers what to do (and what
  it does not); a hardware UART is the last resort, and the point of the
  trial-boot safety model above is to make that resort as rare as possible.

## Documentation

- [`docs/KERNEL.md`](docs/KERNEL.md) — the 6.18 kernel port: CPU, board, boot,
  our drivers, how to build it.
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
- [`docs/kb/README.md`](docs/kb/README.md) — field notes on the device and the
  stock firmware's behaviour, from reverse-engineering real sticks.
- [`AGENTS.md`](AGENTS.md) — repo layout, build/test commands and house rules,
  for anyone (human or AI) working on this codebase.

## License

The kernel is Linux, GPL-2.0, including our patches and drivers on top of it.
The toolchain and every upstream package it builds (busybox, dropbear,
iproute2) keep their own upstream licenses. See
[`docs/LICENSING.md`](docs/LICENSING.md) for the full breakdown, including
where our own original tools (`src/`) currently stand.
