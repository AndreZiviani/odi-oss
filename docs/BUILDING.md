# Building

Everything here builds inside Docker containers; the host only needs Docker
itself, `bash`, and (for the kernel fetch) `curl` and `gpg`. Nothing in this
repo touches a stick.

    make image-all

runs every step below in order, from a clean clone, and skips work it has
already done (Docker layers, pinned tarballs, `.built-with` stamps):

| step | what it does |
|---|---|
| `make toolchain` | builds gcc 16.2.0 / binutils 2.47 / uClibc-ng 1.0.59 for this CPU, into the Docker volume `odi-oss-toolchain-318` (see `toolchain/README.md`) |
| `make toolchain-audit` | proves the built target libraries carry no instruction this CPU traps on |
| `make kernel` | Linux 6.18.53 for the RTL9602C (`VERSION=` stamps the ramlog build id, `CRUMBS_CORE=1` adds the debug crumbs) — `docs/KERNEL.md` |
| `make busybox` | upstream busybox, our config fragment, ISA-audited |
| `make packages` | busybox, then dropbear (with `scp`) and iproute2 |
| `make src` | our own tools (`diag`, `omcid`, `omcli`, `omciprobe`, `omcicap`, `nv`, `igmpd`), freestanding, into `out/bin` — `docs/CROSS-COMPILING.md` |
| `make releases` | fetches `confd` and `metricsd` as pinned, checksummed release assets (confd v1.0.4, metricsd v1.0.3; `CONFD_TAG=`/`METRICSD_TAG=` to override, or `CONFD_BIN=` for a local `confd` build) |
| `make image` | assembles the flashable tarball into `out/image/`: squashfs rootfs (with the register replay tables in `/lib/firmware/odi/`), the uImage, `fwu.sh` and `md5.txt` |

`make image` alone (without the rest) works once the pieces it needs already
exist from a previous build.

## bridge-utils

Deliberately **not built**. Everything on this image that used to need
`brctl` is already covered twice over: busybox's own `brctl` applet (built
with the fancy/show features) is a superset of what anything here calls, and
`ip`/`bridge` from iproute2 cover the same ground over netlink, which is the
interface the kernel prefers anyway. `packages/bridge-utils/README.md` has
the accounting; if something ever needs a `brctl` verb busybox does not
implement, adding a `build.sh` there is a small, self-contained change.

## Verifying a build

    make test          # lint + test-host + test-diag + test-omci, about two minutes
    make lint           # shellcheck every script, plus a couple of repo-specific checks
    make test-host      # host-side unit tests for driver logic that has no kernel dependency
    make test-diag      # diag parser and conversions, and the exporter contract under qemu
    make test-omci      # omcid under qemu-user, about 130 checks, no stick needed

CI (`.github/workflows/ci.yml`) runs `make lint`, `make test-host`,
`make src`, `make test-diag` and `make test-omci` on every push to `main`
and every pull request; the toolchain, kernel and image are not built
there.

Every ELF that reaches the image — kernel, busybox, dropbear, iproute2, our
own tools — passes an instruction audit (`packages/isa-audit.sh`,
`packages/isa-allowlist.sh`) against what the RLX5281 actually implements.
This is a build gate, not a report: a binary carrying a trapping instruction
fails the build rather than shipping.

## The root password and image versioning

`image/build.sh` generates a random 14-character root password per build
(`ROOT_PW=` to choose your own, `ROOT_PW=none` for none) and writes it to
`out/image/root-password-<version>.txt`, because a shared default baked into
a public image is worse than no default. `VERSION=` names the build;
`ALLOW_PARTIAL=1` lets you build an image missing an optional package or
`confd`'s assets (useful while iterating); `ALLOW_NO_KCONFIG=1` builds
without shipping `/etc/kernel-config`, at the cost of `rcS` skipping
platform init and `omcid`.

## Building on a remote host

The Docker images here are `amd64`; on a non-x86_64 host, every one of them
runs under emulation, which is slow, especially for the kernel and toolchain
builds. `tools/remote-build.sh` runs a build on a native x86_64 Linux host
over ssh instead:

    ODI_REMOTE=user@yourbuildhost tools/remote-build.sh 'make image-all'

It rsyncs the current working tree (tracked and untracked, so uncommitted
changes build too) to the remote host, runs the given command there, and
copies `out/image/*.tar`, the root-password files and any build logs back to
your local `out/image/`. `ODI_REMOTE` is required and names no host of its
own; `ODI_REMOTE_DIR` defaults to `/root/odi/odi-oss`. The remote host needs
the same prerequisites `make image-all` does — Docker — and, since the
kernel tree is gigabytes and does not change on every build, a
`kernel/618/fetch.sh` run of its own under `$ODI_REMOTE_DIR/kernel/618/`
once, ahead of time.

## Cleaning up

    make clean       # removes build/ and out/
    make distclean    # also drops the fetched kernel tree and the download cache

The toolchain volume survives both — it costs about 30 minutes to rebuild
and nothing in the tree depends on its exact contents — and is removed
deliberately, if you want it gone: `docker volume rm odi-oss-toolchain-318`.
