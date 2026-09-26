# The toolchain

This tree builds with two prebuilt toolchains, published as container images
by the **odi-toolchain** repository
(<https://github.com/AndreZiviani/odi-toolchain>), which is where their
Dockerfiles, build scripts, patches and the ISA audit live now:

| image | contents | builds |
|---|---|---|
| `odi-toolchain-uclibc` | our own binutils 2.47 (with the three Lexra opcodes), gcc 16.2.0 and uClibc-ng 1.0.59, `mips-linux-uclibc`, big-endian, o32, soft float, at `/opt/oss`; the Linux 6.18 UAPI headers; the host tools a kernel build needs | the kernel, busybox, dropbear, iproute2 |
| `odi-toolchain-freestanding` | Debian `gcc-mips-linux-gnu`, binutils and `qemu-mips-static` | everything in `src/`, `test-diag`, `test-omci` |

Both carry `isa-audit` and `isa-allowlist`, which `packages/isa-audit.sh`
and `packages/isa-allowlist.sh` run over every binary that reaches the image.

## Which versions

`images.env` here pins each image **by digest**, the one place that names
them. `toolchain/image.sh oss|diag` prints the reference and pulls it on
first use; every build script and Makefile goes through it, so nothing else
needs changing when a pin moves.

    make toolchain          # pull both
    make toolchain-audit    # re-audit libc.a and libgcc.a in the uclibc image

The published uclibc image is `linux/amd64` only; on an arm64 host Docker
runs it under emulation, correctly and slower (or build it natively, below).

**While the packages are private**, a pull needs a one-time login with a
GitHub token that has `read:packages`:

    echo "$TOKEN" | docker login ghcr.io -u <github user> --password-stdin

Once they are public, no login is needed. `toolchain/image.sh` says so when a
pull fails.

## Building the toolchain from source instead

The images are reproducible from the odi-toolchain repository alone, and a
local build is a drop-in replacement:

    git clone https://github.com/AndreZiviani/odi-toolchain
    make -C odi-toolchain uclibc          # about an hour; JOBS=8 for more cores
    make -C odi-toolchain freestanding    # about two minutes

    export OSS_IMAGE=odi-toolchain-uclibc:local
    export DIAG_IMAGE=odi-toolchain-freestanding:local
    make image-all

A local image is never pulled or rebuilt by this tree; if the variable names
one that does not exist, the build stops and says how to make it.

## Why these toolchains, in short

The CPU is a Lexra **RLX5281**, which implements the MIPS ISA in pieces
(`mul`, `clz`, `teq`, `beql` and `bnel` trap; `ll`/`sc`, `movz`/`movn`,
`madd` and `bltzl` run). The uClibc-ng toolchain targets
`-march=mips2 -mno-branch-likely -mdivide-breaks`, baked into gcc and its
target libraries, and its image build fails on any trapping instruction in
`libc.a` or `libgcc.a`. Our own tools are freestanding (`-nostdlib`, no libc
at all) and target `-march=mips1`, so they also run on the stock image. The
odi-toolchain `flags.mk` has the full argument for the two baselines, and
`docs/CROSS-COMPILING.md` the worked example.
