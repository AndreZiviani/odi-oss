# What this firmware does differently from the stock image

The stock ODI DFP-34X-2C2 firmware works. This project does not repack it —
it builds a complete replacement from source — and along the way that also
fixes or improves a number of things the stock firmware gets wrong, leaves
out, or does closed. This is the detail behind the table in the top-level
`README.md`; each point below is something you can read or run in this repo
to confirm for yourself.

## Fully open

Nothing in this build is proprietary. The kernel is mainline Linux plus our
own patches and drivers (`docs/KERNEL.md`); the toolchain is built from
source (`toolchain/README.md`); busybox, dropbear and iproute2 are unmodified
upstream releases; everything in `src/` is ours. `docs/LICENSING.md` is the
full accounting, package by package. Contrast this with the stock firmware,
which is closed and not distributable — the reason this project builds
firmware rather than repacking or redistributing anything from the device.

## Kernel

**Linux 6.18**, the current longterm release, instead of the 2.6.30 the
stock firmware runs. Everything the datapath needs — the switch fabric, the
GPON MAC, the OMCI transport, the CPU-port NIC — is our own driver, an
independent implementation built straight into the kernel; there is no proprietary
kernel module in this image at all. `docs/KERNEL.md` has the CPU, board and
driver detail.

**A hung kernel resets itself, and so does a stuck boot script.** The
watchdog kicker (`odi_wdt.c`) tracks a userland-confirmation deadline in
addition to "the kernel is alive," so a boot that hangs before its init
scripts finish still gets the stick back to a known-good image, not just a
kernel that panics outright. See `docs/KERNEL.md` and `docs/FLASHING.md`.

**A boot you cannot see is not a boot you cannot debug.** There is no serial
console on this device; the DRAM ramlog console (`odi_ramlog.c`) mirrors
every kernel console line into DRAM that survives a watchdog reset, so a
failed trial can be read back from the other slot afterward.

**One-shot trial boots by design.** `fwu.sh` refuses to write the slot you
are running from or the slot the bootloader still trusts, and a freshly
flashed image is inert until `nv setenv sw_tryactive <slot>` boots it
exactly once. `docs/FLASHING.md` is the full procedure.

## OMCI and the datapath

**Our own OMCI daemon (`omcid`)** answers the OLT and drives GPON
provisioning end to end: MIB handling, bridge/VLAN setup, T-CONT and GEM
queue programming, over our own in-kernel switch/GPON drivers — no
proprietary daemon or kernel module in the path. `docs/TOOLS.md` covers
`omcid` and the tooling around it (`omcli`/`omcicli`, `omciprobe`,
`omcicap`).

**Optics (DDM) support**: our own SFF-8472 reader (temperature, voltage,
bias current, tx/rx power), exposed through `diag` and the Prometheus
exporter.

**IGMP snooping/proxy (`igmpd`)**, watching group membership and
programming the switch to match — currently shipped in observe-only mode by
default (`docs/TOOLS.md`), so treat multicast as an area still maturing
rather than a finished feature.

## The `diag` CLI

**Our own command set, over our own kernel.** `diag` carries only commands
our kernel can answer: the transceiver's DDM readings, GPON state, alarms
and GEM flows, per-port MIB counters, and switch-core register access
(`diag help` lists them, `src/diag/README.md` explains each). The commands
the Prometheus exporter runs keep the stock CLI's syntax and output byte for
byte, so one exporter build reads both this firmware and the stock one; a
test under qemu pins that output.

**Batched on stdin, always exits.** `diag` reads commands from stdin one per
line and stops cleanly at EOF, rather than requiring a terminal; see
`docs/TOOLS.md` for the one thing to watch — give it a way to see EOF, or
wrap it in `timeout`.

## Access

**A current dropbear** with `scp` in both directions (`scp -O` from a
modern OpenSSH client — the legacy scp protocol is all this dropbear speaks,
having no SFTP subsystem), an ed25519 host key kept on the config partition
across reflashes, and SSH-key management from the web UI or its API.
`docs/ACCESS.md` has the full detail.

**A per-build root password** (`out/image/root-password-<version>.txt`)
instead of a fixed shared default; no telnet (ssh is the only network
login), and every way in fails **open** — a missing config file gives you
the default credential, never a locked door, because the config partition
is exactly what a factory reset erases.

## Web UI and exporter

**`confd`**, our own web UI (a separate project), on the same port the
stock firmware's UI uses, reading the OMCI daemon through the same command
shapes as `omcli`. **`metricsd`**, a Prometheus exporter (also a separate
project), including `gpon_omci_services` — how many services the OLT has
actually provisioned, so a stick that is optically up but not provisioned
is visible as such rather than reading as healthy.

## Build and verification

Every ELF that reaches the image — kernel, packages, our own tools — passes
an instruction audit (`packages/isa-audit.sh`, `packages/isa-allowlist.sh`)
against what the RLX5281 actually implements, as a build gate rather than a
report: a binary carrying an instruction this CPU traps on does not ship.
`make test` runs lint, the host-side unit tests and the OMCI daemon under
qemu (about 130 checks) without touching a stick; `make image-all` builds
the whole image from a clean clone. See `docs/BUILDING.md` and
`docs/CROSS-COMPILING.md`.
