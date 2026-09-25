# The toolchain

One cross toolchain, ours, built from source: **binutils 2.47, gcc 16.2.0 and
uClibc-ng 1.0.59**, targeting `mips-linux-uclibc`, big-endian, o32, soft
float. It builds the kernel, busybox, dropbear and iproute2; our own
freestanding tools use a separate, stock compiler (see "Freestanding tools"
below).

    make toolchain          # build-oss-toolchain.sh, into the Docker volume odi-oss-toolchain-318
    make toolchain-audit    # audit-toolchain.sh: the target libraries carry no illegal instruction

The prefix lives in a named volume rather than the working tree because the
build writes hundreds of thousands of small files and a macOS bind mount is
both slow and case-insensitive. The container is `Dockerfile.oss`
(`packages/oss-env.sh` prints its tag), native to the host.

## Why our own, and why the flags

The CPU is a **Lexra RLX5281**. It implements the MIPS ISA in pieces; measured
by executing each instruction on the device:

    ok:       lwl lwr swl swr, movz movn, ll sc sync, bltzl, madd
    ILLEGAL:  mul, clz, teq, beql, bnel

A generic mips32 toolchain emits the illegal five in its own libraries. So the
target is **mips2** with three suppressions, and they reach the target
libraries too, not only our code:

    -march=mips2        no mul, no clz (SPECIAL2 / mips32)
    -mno-branch-likely  no beql / bnel
    -mdivide-breaks     break instead of teq on divide by zero

`--with-arch=mips2` is baked into gcc; the two others are passed as
`CFLAGS_FOR_TARGET` (they are not gcc defaults, so the kernel build passes
all three again explicitly). `audit-toolchain.sh` checks the built libgcc and
libc rather than trusting either mechanism.

## The libc configuration

uClibc-ng starts from its defconfig; `build-oss-toolchain.sh` sets what this
device needs and records why beside each line. Beyond the architecture and
the Lexra flags: SHA-256/512 crypt (root passwords), the resolver (nslookup),
pty support with the BSD fallback, and four features the packages once
shimmed around and now take from the libc: SUSv3/SUSv4 legacy calls (`mktemp`),
libutil (`openpty`), the stack-protector runtime (`__stack_chk_fail` with a
random guard) and `nftw`. Left off on purpose: 64-bit `time_t` (its stat
path needs `statx`, a 4.11 syscall this kernel lacks), utmp/utmpx (a
read-only rootfs has nowhere to write them), IPv6 (the kernel has none),
threads (nothing shipped uses them).

## The Lexra opcodes

The RLX5281 has three instructions no binutils ever had: `mflxc0` and
`mtlxc0` (the LXC0 coprocessor, TLS register, watchpoints, interrupt
vectors) and `cls` (count leading sign).
`patches/binutils/0001-lexra-lxc0-cls.patch` adds them to
`opcodes/mips-opc.c`, encoded exactly as this core's own instruction
decoder expects, so that code for this core can use them and `objdump`
decodes them. The kernel (`docs/KERNEL.md`) emits none of them today:

    mflxc0 rt, rd[, sel]   COP0, rs field 0b00011   0x40600000
    mtlxc0 rt, rd[, sel]   COP0, rs field 0b00111   0x40e00000
    cls    rd, rt          SPECIAL3, function 0x0e  0x7c00000e

Kernel-side ISA overrides (`cache` is MIPS III, `movn` MIPS IV, `mtc0` with a
select field MIPS32) are `.set` directives in our own kernel sources
(`kernel/extra/`), not assembler changes.

## Freestanding tools

`diag`, `omcid`, `omcli`, `omciprobe`, `omcicap`, `igmpd` and `nv` are
`-nostdlib -nostartfiles -static` and link no libc at all: they depend on the
kernel syscall ABI and nothing else, which lets them be dropped onto a
running stick (the stock image included) without reflashing, and keeps the
recovery tools working when everything else is broken. They do not use the
toolchain above: `src/build.sh` builds them with Debian's
`gcc-mips-linux-gnu` at `-march=mips1`, in the `odi-diag-toolchain`
container (`src/diag/Dockerfile`; `Dockerfile.freestanding` here is the same
file), locally and in CI alike. `docs/CROSS-COMPILING.md` has the worked
example.

## Verifying, rather than trusting

Every binary that reaches the image is audited (`packages/isa-audit.sh`) for
the instructions this CPU traps on, disassembled at mips32 so the SPECIAL2
encodings decode as `mul` and `clz` rather than as unknown words:

    mul clz clo teq tne tge tgeu tlt tltu teqi tnei tgei tgeiu tlti tltiu beql bnel beqzl bnezl

The count must be 0. It is a list of what has been proved illegal plus the
rest of its encoding class (the conditional traps), not a guess at what might
be fine; a check of the opposite form once passed a dropbear carrying 147
`teq`.

## History

The project started on the vendor prebuilt toolchain (gcc 4.4.5, uClibc 0.9.30.3, 32-bit x86
binaries in an emulated container), first for everything, then for the kernel
only. Its libc cost busybox five applets, dropbear its pty support, and root
passwords SHA-512; its gcc could not compile C11. The kernel followed the
userland onto this toolchain on 2026-09-21 and the vendor toolchain is no longer fetched.
