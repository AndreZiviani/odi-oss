# Building

Everything that compiles for the target builds inside Docker containers.
Nothing in this repo touches a stick.

## Prerequisites

On the build host: Docker, `bash`, GNU `make`, `git`, `curl`, `xz`, `gpg`
and `shasum` (the kernel fetch checks the tarball both ways), and about
20 GB free: the toolchain volume is 9 GB, the kernel build volume, the
kernel tree and `build/` about 1.7 GB each, the Docker images 2.3 GB.
The containers are `amd64`; on any other host they run under emulation,
which is slow for the toolchain and the kernel
([remote host](#building-on-a-remote-host)).

`confd` (the web UI) and `metricsd` (the exporter) are both public releases;
`make releases` downloads them with `gh` when it is installed, or with
`curl` otherwise. No login or token is required; an optional `GITHUB_TOKEN`
only raises the anonymous rate limit.

## From a clean clone

    git clone git@github.com:AndreZiviani/odi-oss.git && cd odi-oss
    make image-all

That is the whole build. It runs these steps, in this order, each one its
own target that can also run alone, and each skipping work it has already
done (Docker layers, pinned tarballs, `.built-with` stamps, the volumes).
Timings are from a clean clone on an 8-core x86_64 host with 19 GB of
RAM, default `JOBS=4`, measured 2026-09-25:

| step | what it does | first run |
|---|---|---|
| `make kernel-tree` | fetches linux-6.18.53 from cdn.kernel.org, checks its SHA-256 and GPG signature, extracts it to `kernel/618/mainline` and packs it into `build/kernel-618/tree.tar` (`kernel/tree.sh`) | 1.5 min (mostly the 150 MB download) |
| `make toolchain` | pulls the two prebuilt toolchain images, pinned by digest in `toolchain/images.env`: gcc 16.2.0 / binutils 2.47 / uClibc-ng 1.0.59 for this CPU, and the freestanding Debian cross-gcc (see `toolchain/README.md`, and "The toolchain images" below) | under a minute, once the images pull |
| `make toolchain-audit` | re-proves the target libraries in the uclibc image carry no instruction this CPU traps on (the image build already refused any) | seconds |
| `make kernel` | Linux 6.18.53 for the RTL9602C (`VERSION=` stamps the ramlog build id, `CRUMBS_CORE=1` adds the debug crumbs) — `docs/KERNEL.md` | 2 min |
| `make packages` | busybox (`make busybox`), our config fragment, then dropbear (with `scp`) and iproute2, all ISA-audited | 2 min |
| `make src` | our own tools (`diag`, `omcid`, `omcli`, `omciprobe`, `omcicap`, `nv`, `igmpd`), freestanding, into `out/bin` — `docs/CROSS-COMPILING.md` | 10 s |
| `make releases` | fetches `confd` and `metricsd` as pinned, checksummed release assets (confd v1.0.4, metricsd v1.1.1; `CONFD_TAG=`/`METRICSD_TAG=` to override, or `CONFD_TAG= CONFD_BIN=<binary>` for a local `confd` build) | 5 s |
| `make image` | assembles the flashable tarball into `out/image/`: squashfs rootfs (with the register replay tables in `/lib/firmware/odi/`), the uImage, `fwu.sh` and `md5.txt` | 15 s |

Total: about six minutes from `git clone` to `out/image/<version>.tar` once
the toolchain images pull (the kernel fetch and the image pull are the two
dominant first-run costs; "The toolchain images" below has their own
timing). Building the compiler used to add the better part of an hour to
that; now it does not. A second `make image-all` on the same tree, with
everything already pulled and cached, takes about a minute. The root
password the image was built with is in
`out/image/root-password-<version>.txt`.

`make image` alone (without the rest) works once the pieces it needs
already exist; on a fresh tree it names every missing one and stops.

If `make releases` cannot reach GitHub (offline build host, rate limit),
there are two ways round it: run `make releases` on a machine that can, and
copy what it writes -- `out/bin/metricsd`, `out/bin/confd`,
`out/bin/releases.env` and the `out/confd-assets/` directory -- to the same
paths in the build tree before `make image`; or build without the exporter
and the UI, `ALLOW_PARTIAL=1 make image`.

### Several builds on one Docker host

Each Docker volume and image the build uses has an override, so a second
build on the same host shares nothing with the first:

    export VOL=mine-kbuild OSS_IMAGE=mine-uclibc:local \
           DIAG_IMAGE=mine-freestanding:local TOOLS_IMAGE=mine-image
    make image-all

## The toolchain images

Nothing here compiles a compiler. The toolchains are container images built
and published by the odi-toolchain repository
(<https://github.com/AndreZiviani/odi-toolchain>) and pinned by digest in
`toolchain/images.env`; `toolchain/image.sh` pulls each on first use.

The packages are public: an anonymous pull works, no login or token needed.
A failed pull (offline build host, rate limit) prints the fallback: build
the images from the odi-toolchain repository (`make -C odi-toolchain
uclibc freestanding`, about an hour for the first) and point `OSS_IMAGE=` /
`DIAG_IMAGE=` at the local tags. `toolchain/README.md` has the details.

On a native x86_64 host with 8 cores, from a fresh tree and fresh volumes,
the kernel, the packages, our tools and the image take about three and a
half minutes once the images are pulled (measured 2026-09-25; the kernel
fetch and the release downloads come on top). Building the uclibc
toolchain itself used to add about an hour to a first build.

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
    make test-rcs       # the rcS action trace against its goldens; needs out/busybox, not in `make test`

CI (`.github/workflows/ci.yml`) runs `make lint`, `make test-host`,
`make test-diag` and `make test-omci` on every push to `main` and every
pull request, and adds `make src` on release tags and manual runs; the
kernel and image are not built there. It pulls the public freestanding
toolchain image anonymously, no login needed.

Every ELF that reaches the image — kernel, busybox, dropbear, iproute2, our
own tools — passes an instruction audit (`packages/isa-audit.sh`,
`packages/isa-allowlist.sh`) against what the RLX5281 actually implements.
This is a build gate, not a report: a binary carrying a trapping instruction
fails the build rather than shipping.

## The root password and image versioning

`image/build.sh` generates a random 14-character root password per build
(`ROOT_PW=` to choose your own, `ROOT_PW=none` for none), hashes it with
SHA-512 crypt (`$6$`, `openssl passwd -6`, default rounds) and writes the
plaintext to `out/image/root-password-<version>.txt`, because a shared
default baked into a public image is worse than no default. This is the
default for a source build.

`ROOT_PW=locked` ships root with **no password at all** instead: the
account's password field is `!`, a hash no `crypt()` ever produces, and the
image also carries `/etc/odi-keys-only`, which `/etc/init.d/services` reads
to start dropbear with `-s` (refuse password logins outright), so ssh does
not even offer a password prompt. This is what the published releases build
with — a per-build password baked into a squashfs anyone can unpack is not
a secret. First access to a `ROOT_PW=locked` image is through the web UI
(`confd`, port 80 — the built-in `admin`/`admin` until a password is set),
whose SSH-key admin page can add a key for root; `docs/FLASHING.md` has the
exact steps. The three shapes (`locked`, `none`, the default password) are
written by `image/gen-root-account.sh`, which `test/root_pw_test.sh`
(`make test-host`) exercises directly — no kernel or busybox needed for
that one.

`VERSION=` names the build;
`ALLOW_PARTIAL=1` lets you build an image missing an optional package or
`confd`'s assets (useful while iterating); `ALLOW_NO_KCONFIG=1` builds
without shipping `/etc/kernel-config`, the `.config` kept on the device for
reference.

## Building on a remote host

The uclibc toolchain image is `amd64`; on a non-x86_64 host it runs under
emulation, which is slow, especially for the kernel build. `tools/remote-build.sh` runs a build on a native x86_64 Linux host
over ssh instead:

    ODI_REMOTE=user@yourbuildhost tools/remote-build.sh 'make image-all'

It rsyncs the current working tree (tracked and untracked, so uncommitted
changes build too) to the remote host, runs the given command there, and
copies `out/image/*.tar`, the root-password files and any build logs back to
your local `out/image/`. `ODI_REMOTE` is required and names no host of its
own; `ODI_REMOTE_DIR` defaults to `/root/odi/odi-oss`. The remote host needs
the prerequisites above. The kernel tree and the download cache are not
synced (the tree is gigabytes): the first build there fetches them, later
ones reuse them. `make releases` needs no token on the remote host either;
an optional `GITHUB_TOKEN`, if you want the higher rate limit, is handed to
the remote command on stdin, never on a command line:

    GITHUB_TOKEN=$(gh auth token) ODI_REMOTE=user@yourbuildhost tools/remote-build.sh 'make image-all'

## Cleaning up

    make clean       # removes build/ and out/
    make distclean    # also drops the fetched kernel tree and the download cache

The toolchain images and the kernel build volume survive both; remove them
deliberately if you want them gone (`docker volume rm odi-kbuild-618`,
`docker image rm` on the references in `toolchain/images.env`).
