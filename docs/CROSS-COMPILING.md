# Cross-compiling for the stick

This CPU (a Lexra RLX5281) is not a stock MIPS32 core: it implements the
MIPS instruction set in pieces, and a normal MIPS toolchain's default output
contains instructions it does not have. Every binary that reaches the image
— kernel, upstream packages, our own tools — is built to avoid them, and
every one is checked afterward rather than trusted. This document covers
what you need to compile your own C for this board, in either of the two
styles this repo uses.

## The instruction set: what to target, and why

Measured by executing instructions on the device itself, not read out of a
datasheet:

    confirmed to work:   the full MIPS-I set, plus movz/movn, ll/sc, sync,
                          bltzl, madd
    confirmed ILLEGAL:   mul, clz (and the rest of the SPECIAL2/mips32
                          encoding class), teq/tne/tge/tlt and their
                          immediate and unsigned forms, beql/bnel (and their
                          beqzl/bnezl assembler aliases)

A generic `mips32`-targeting toolchain emits several of the illegal ones by
default, in its own runtime libraries as well as in your code. So every
build in this repo targets:

    -march=mips2 -mno-branch-likely -mdivide-breaks

- `-march=mips2` keeps codegen to MIPS-I plus the MIPS-II additions this
  core does have, and away from the SPECIAL2 encodings (`mul`, `clz`) that
  only exist from mips32 onward.
- `-mno-branch-likely` removes `beql`/`bnel`, which mips2 and later use for
  branch-delay-slot optimization by default.
- `-mdivide-breaks` emits a `break` instead of `teq` to trap divide-by-zero.
  `-march=mips2` alone already avoids the *branch* conditions that reach
  `teq`, but this flag stops the compiler reaching for it directly too.

This is not a one-off compiler flag: it has to reach every object that ends
up in the image, including target libraries built by the toolchain itself
(`toolchain/README.md`), and it is verified rather than assumed — see "the
ISA audit" below.

## Two build styles

Everything in `src/` is **freestanding**: no libc, no C runtime, one
`_start`, and syscalls made directly. Everything from `packages/` (busybox,
dropbear, iproute2) is **linked against uClibc-ng**, our own build of it for
this target. Pick whichever fits what you are building; both are built with
the same instruction-set constraints above.

### Freestanding: `-nostdlib`, direct syscalls

`src/nv` is a small, complete example. Its Makefile (`src/nv/Makefile`)
compiles with:

    CFLAGS := -std=c11 -Os -Wall -Wextra \
              -march=mips1 -mabi=32 -EB -msoft-float -G0 \
              -fno-pic -mno-abicalls -ffreestanding -fno-builtin -fno-stack-protector \
              -ffunction-sections -fdata-sections
    LDFLAGS := -nostdlib -nostartfiles -static -Wl,-e,_start -Wl,--build-id=none \
               -Wl,--gc-sections

(the freestanding tools build against Debian's `gcc-mips-linux-gnu` at
`-march=mips1`, in the freestanding toolchain image, which is what CI uses and is a stricter subset than
`-march=mips2`; they build equally well with this repo's own toolchain.)
`-EB` is big-endian, `-mabi=32 -msoft-float` is the o32 soft-float ABI this
kernel expects, `-G0` disables MIPS small-data (`gp`-relative) addressing —
there is no runtime here to set `$gp` up — and `-fno-pic -mno-abicalls`
keeps the code absolute rather than position-independent, again because
there is no dynamic linker to relocate it. `-nostdlib -nostartfiles` means
no libc and no crt0 at all; `-Wl,-e,_start` tells the linker your own entry
point is called `_start`.

**The entry point and the syscall layer**, shared by every freestanding tool
(`src/diag/src/start.S`, reused by `src/nv`'s Makefile):

```asm
_start:
	lw	$a0, 0($sp)		/* argc */
	addiu	$a1, $sp, 4		/* argv */
	addiu	$sp, $sp, -24		/* 16B arg save area, 8B-aligned */
	move	$fp, $zero		/* terminate the frame chain */
	jal	main
	nop
	move	$a0, $v0		/* main()'s return value is the status */
	li	$v0, 4001		/* __NR_exit: o32 syscall base 4000 + 1 */
	syscall
```

The kernel jumps straight here with the stack already holding `argc`
followed by `argv`, so `main(argc, argv)` costs two instructions to set up.
o32 also owes the callee 16 bytes of argument save area on the stack, which
is why `_start` adjusts `$sp` before calling `main`. There is no libc `exit`,
so the fall-through after `main` returns is a raw `exit` syscall — number
`4001`, because o32 syscalls are numbered from a base of 4000, not from the
usual small integers you may know from x86 or ARM. `src/diag/src/sys.h`
holds the rest of the numbers this code needs (`open`, `read`, `write`,
`ioctl`, socket calls, `clock_gettime`, ...), each looked up in the actual
target `<asm/unistd.h>`, never assumed from another architecture — o32's
socket-call numbers and constants in particular do **not** match the generic
Linux ones (`getsockopt` is a different number here, `SOCK_STREAM` and
`SOCK_DGRAM` are swapped, `O_SYNC`'s bit pattern differs), and a wrong
constant is a syscall that "succeeds" at doing the wrong thing rather than
failing loudly. The same applies in the other direction: to find out whether
a binary can reach a syscall (the kernel config diet did this for every
ELF in the rootfs, `docs/KERNEL.md`), look for the o32 number, 4000 + N,
loaded right before a `syscall` instruction. A scan for the bare table
number finds nothing and "proves" every syscall unused.

A three-argument syscall is a single inline `syscall` instruction (`$v0` =
number, `$a0`-`$a2` = args, `$a3` != 0 on return means error); anything
needing a fourth or more (like `mmap2`) goes through `__syscall6`, declared
in `sys.h` and written in `start.S`, which reshuffles the o32 calling convention's register/stack split
into what the kernel expects.

### uClibc-ng linked: cross-compiling a normal autotools/kbuild project

`packages/busybox/build.sh` and `packages/dropbear/build.sh` are the two
worked examples. Both run inside the uclibc toolchain image
(`packages/oss-env.sh` prints it), with our own cross-compiler on `PATH`:

    export PATH=/opt/oss/bin:$PATH
    export CROSS_COMPILE=mips-linux-uclibc-
    export ARCH=mips
    make defconfig
    # apply the package's own config fragment, then:
    make oldconfig

dropbear additionally passes explicit `CFLAGS` for the same reason the
kernel build does — the project's own build system does not know to avoid
`teq`/`mul`/`clz`/`beql`/`bnel` on its own:

    LEXRA_CFLAGS="-march=mips2 -mno-branch-likely -mdivide-breaks -Os"
    make ... CFLAGS="$LEXRA_CFLAGS" ...

This is the same pattern for any autotools- or kbuild-style project: set
`CROSS_COMPILE`/`CC` to the `mips-linux-uclibc-` prefix from the toolchain
image (`/opt/oss/bin`), set `ARCH=mips` if the project's build system asks for it, and make
sure your own `CFLAGS` carry the three flags above — many build systems will
not add them for you even when they otherwise get the target right.

## Checking a binary before it goes anywhere near a stick

    packages/isa-audit.sh <binary> [<binary>...]

Disassembles every executable section of each binary at `mips32` (so the
instructions above decode as themselves rather than as unknown words) and
fails if it finds any of them. This is the same gate `image/build.sh` runs
over every ELF in the staged rootfs — it is a build gate, not a report: a
trapping instruction is refused, not merely flagged.

    packages/isa-allowlist.sh <binary> [<binary>...]

The complementary check, in the other direction: instead of a list of known
bad instructions, this keeps a list of mnemonics *confirmed present* on the
device and fails on anything else — because a deny-list can only catch
instructions someone already thought to add to it. (This is not a
theoretical concern here: an earlier audit of only the first form once
passed a binary carrying 147 `teq` instructions, because nobody had put
`teq` on that list yet.) Run both before you trust a binary you built
yourself; either one failing means go back to your `CFLAGS`, not force the
binary onto the device to see what happens.

## Getting a binary onto the stick

Once it passes both checks, copy it over (`docs/ACCESS.md` has the full
detail):

    scp -O yourbinary root@<stick>:/tmp/      # OpenSSH 9+ needs -O: legacy scp protocol only

or, with no ssh available at all, over a plain `nc` pipe — start a listener
on the stick and send from the host, or the reverse, whichever direction
your access allows; `docs/ACCESS.md` and `docs/FLASHING.md` cover what is
and is not available on a stock (OEM) image versus this one.
