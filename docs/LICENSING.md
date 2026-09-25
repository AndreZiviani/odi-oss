# Licensing

What license covers what in this tree, by piece. Written because a public
repository built to replace a closed firmware image deserves a clear answer
to "does this ship or link anything proprietary," not just an implied one.

**Short answer: no.** This build fetches, builds and ships nothing
proprietary. The kernel it produces contains no vendor-supplied object code
and no copied vendor source; every kernel module that used to be a closed
binary blob on this class of device has an open replacement of our own
here instead, built straight into the kernel (`docs/KERNEL.md`).

## The kernel: GPL-2.0

`kernel/618/mainline/` is a pristine, GPG-verified `cdn.kernel.org` release
tarball (`kernel/618/fetch.sh`), never committed. `kernel/618/patches/` are
our own changes to its files, and `kernel/extra/` holds the files we add:
the board and CPU support, the interrupt controller, timer and SPI NOR
drivers, and `drivers/net/ethernet/odi/`, our own driver directory
(switch, GPON MAC, CPU-port NIC, OMCI transport, watchdog, DRAM ramlog
console) — an independent implementation,
our own code written from public specifications (ITU-T G.984.3 and G.988 for
GPON and OMCI, SFF-8472 for optics), hardware behaviour observed on this
board, and the behaviour of the stock firmware observed as a black box. All
GPL-2.0, same as the kernel tree it extends.

### The register replay tables

`rootfs/skeleton/lib/firmware/odi/` holds three binary files
(`sdkinit.bin`, `modload.bin`, `gpon_init.bin`) that the kernel loads with
`request_firmware()`. They are data, not code, and not vendor files: each
is a list of register writes (address, value, table words) recorded with
`tools/regtrace` from this board's own hardware while the stock firmware
initialised it, and packed by our own generators into our own format
(`odi_replay_blob.h`). They carry no strings and no identity: the serial
number words of the GPON table are zero, and the kernel fills them in from
the config store at boot. They are committed rather than generated at build
time because the register listing the generators need is itself read from
the stock firmware binary (below) and is not in this tree.

## The toolchain and upstream packages: their own licenses

Built from source, unmodified except where noted, each keeping its own
upstream license:

| component | version | license |
|---|---|---|
| gcc | 16.2.0 | GPL-3.0 (with the GCC runtime library exception) |
| binutils | 2.47 | GPL-3.0 (a small patch adds three CPU-specific instruction encodings binutils never had; same license) |
| uClibc-ng | 1.0.59 | LGPL-2.1 |
| Linux kernel headers (used to build uClibc-ng) | matches the kernel version they are paired with | GPL-2.0, with the usual "headers do not make your program a derivative work" exception |
| busybox | 1.38.0 | GPL-2.0 |
| dropbear (+ its `scp`) | current | MIT-style (dropbear's own license, not GPL) |
| iproute2 | 7.2.0 | GPL-2.0 |

`bridge-utils` is deliberately not built at all (`packages/bridge-utils/README.md`),
so it does not appear above.

## Our own tools: `src/`, the rootfs skeleton, every build script

`src/diag`, `src/omci`, `src/igmp`, `src/nv`, the rootfs skeleton
(`rootfs/skeleton/`), and every build script in this repo are entirely
ours, written for this project. **They do not currently carry an explicit
license header or a `LICENSE` file of their own.** Before treating any of
this as available for reuse outside this repository, get an explicit
license decision from the project maintainer — silence is not a grant, and
the right choice (matching the GPL-2.0 pieces above, or something more
permissive for code with no kernel/GPL entanglement) is a decision for the
maintainer to make deliberately, not one to infer from this document.

### `src/diag`: what comes from the stock firmware, and what is ours

`diag` itself takes nothing from the stock firmware: its command table,
handlers and help text are written here, and it talks only to our own
kernel. The one stock-firmware dependency is compatibility, not content:
the commands the Prometheus exporter runs keep the stock CLI's syntax and
output format.

`src/diag/tools/regmap-extract.py` reads the switch register table out of
the stock `librtk.so` for the register listings `tools/regtrace` and
`tools/regdump` use, as data, at the time it is run, and never
redistributed as code. The table is numbers only -- each register's
address, width and array/port range, each field's bit position, width and
numeric id -- and those numbers are the whole of what is read.

**The register and field names are ours.** The binary carries none we use;
`src/diag/tools/regnames.txt` names, in our own words, the registers this
project's code and scripts touch, and every other register and field gets a
mechanical name built from its address and bit range (`SW_0x023038`,
`F_15_8`). Nothing here is taken from, or depends on, any other source of
register descriptions. `src/diag/README.md` has the details.

## `confd` and `metricsd`

Separate projects, fetched here as pinned release assets
(`src/fetch-releases.sh`) rather than built from source in this repository.
Their licenses are whatever those projects declare; this repo does not
restate them.

## Public references this code is written against

`docs/REFERENCES.md` — the ITU-T documents the GPON code implements (G.984.3
and its Amendment 1), with their identity and checksums, fetched by
`tools/fetch-refs.sh` rather than stored here (their own copyright terms
forbid redistribution). G.988 (OMCI) and SFF-8472 (optics) are cited by
clause in the code but not fetched. Register names in the drivers and
scripts are ours (`src/diag/tools/regnames.txt`, above); no register
description from any other source is included in this repository.
