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
                          madd
    confirmed ILLEGAL:   mul, clz (and the rest of the SPECIAL2/mips32
                          encoding class), teq/tne/tge/tlt and their
                          immediate and unsigned forms, beql/bnel (and their
                          beqzl/bnezl assembler aliases)
    decodes, WRONG:      bltzl. It does not trap, but the delay slot always
                          runs and control always falls through, taken or
                          not. It was once listed as working, from a probe
                          that could only see SIGILL; do not use it, and
                          treat the other branch-likely forms (bgezl, blezl,
                          bgtzl, bltzall, bgezall) as unverified.

A generic `mips32`-targeting toolchain emits several of the illegal ones by
default, in its own runtime libraries as well as in your code. So every
build in this repo targets:

    -march=mips2 -mno-branch-likely -mdivide-breaks

- `-march=mips2` keeps codegen to MIPS-I plus the MIPS-II additions this
  core does have, and away from the SPECIAL2 encodings (`mul`, `clz`) that
  only exist from mips32 onward.
- `-mno-branch-likely` removes `beql`/`bnel` (and the other branch-likely
  forms, `bltzl` included), which mips2 and later use for branch-delay-slot
  optimization by default.
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
              -fno-pic -mno-abicalls -ffreestanding -fno-builtin $(HARDEN_CFLAGS) \
              -ffunction-sections -fdata-sections
    LDFLAGS := -nostdlib -nostartfiles -static -Wl,-e,_start -Wl,--build-id=none \
               -Wl,--gc-sections $(HARDEN_LDFLAGS)

(the freestanding tools build against Debian's `gcc-mips-linux-gnu` at
`-march=mips1`, in the freestanding toolchain image, which is what CI uses and is a stricter subset than
`-march=mips2`; they build equally well with this repo's own toolchain.)
`-EB` is big-endian, `-mabi=32 -msoft-float` is the o32 soft-float ABI this
kernel expects, `-G0` disables MIPS small-data (`gp`-relative) addressing —
there is no runtime here to set `$gp` up — and `-fno-pic -mno-abicalls`
keeps the code absolute rather than position-independent, again because
there is no dynamic linker to relocate it. `-nostdlib -nostartfiles` means
no libc and no crt0 at all; `-Wl,-e,_start` tells the linker your own entry
point is called `_start`. `HARDEN_CFLAGS` and `HARDEN_LDFLAGS` come from
`src/harden.mk`, shared by every Makefile in `src/`; "Hardening the
freestanding binaries" below says what they are and what the usual set
leaves out here.

**The entry point and the syscall layer**, shared by every freestanding tool
(`src/diag/src/start.S`, reused by `src/nv`'s Makefile):

```asm
_start:
	lw	$a0, 0($sp)		/* argc */
	addiu	$a1, $sp, 4		/* argv */
	jal	ssp_seed		/* the stack protector guard, see "Hardening" */
	nop
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

## Hardening the freestanding binaries

`omcid` parses OMCI from the OLT and `igmpd` parses IGMP and MLD from the
host and the PON side, both as root. The usual hardening set of a Linux
distribution (FORTIFY, PIE, RELRO, BIND_NOW) assumes a libc and a dynamic
linker, and these binaries have neither, so each mitigation was weighed for
this target on its own. Every tool in `src/` gets the same flags, from one
place, `src/harden.mk`, because they share one runtime (`start.S`, `io.c`).
The compiler is the one in the freestanding toolchain image (Debian gcc
12.2, binutils 2.40; `toolchain/images.env`). The busybox, dropbear and
iproute2 builds use the uClibc-ng toolchain and are not covered here.

| mitigation | state | why |
|---|---|---|
| `-fstack-protector-strong` | **on** | gcc on MIPS checks against a global `__stack_chk_guard`, not a TLS slot, so a freestanding program can provide it. `start.S` does: `_start` seeds the guard from the 16 random bytes the kernel passes in the aux vector (`AT_RANDOM`) before `main` runs, keeping the first byte in memory zero as glibc does (24 random bits on a 32-bit word), and `__stack_chk_fail` writes `stack-protector: stack smashing detected, aborting` to stderr (the daemon's own log, through `respawn.sh`) and to `/dev/kmsg` (`logread`, `dmesg`, the ramlog), then kills the process with SIGABRT, its default action restored first (so no handler runs), and `exit_group(127)` if SIGABRT is blocked. No wait anywhere: the process ends at once, `print-fatal-signals=1` logs its pid and registers, and init respawns it. Both are assembly because both must not be protected themselves: the seed runs before the guard holds its value, the failure path on a frame known to be overwritten. |
| `-fstack-clash-protection` | **on** | gcc 12 implements it on MIPS as one probe store in every prologue, plus a probe loop for a frame over 4 KB. Nothing needs it today: the largest frame in any of these binaries is 8 KB (`nv`'s `env_set`; omcid's largest is 4.5 KB), none is sized at run time (no VLA, `-Wvla` keeps it so, no `alloca`), and the guard gap below a Linux stack is 1 MB. It is there so that a future large or dynamic frame cannot step over that gap. |
| `-ftrivial-auto-var-init=zero` | **on** | a local read before it is written reads zero, not what an earlier call left on the stack. |
| `-fno-strict-overflow` | **on** | signed and pointer arithmetic wrap, so a length check written as an overflow test is never optimised away. Costs nothing measurable. |
| non-executable stack (`PT_GNU_STACK`) | **on**, declared | `-z noexecstack`, and a `.note.GNU-stack` section in `start.S`: one object without that note makes the linker mark the whole program as needing an executable stack (`GNU_STACK RWE`). The RLX5281 has no execute-inhibit bit (no RI/XI, an R3000-class TLB), so the hardware executes any readable page: the flag changes what the kernel records for the mapping, not what the CPU allows. It is kept because it is the correct declaration and costs nothing. |
| no writable and executable segment | **on**, checked | text and rodata are one `R E` segment, data and bss one `RW` segment. Same caveat as the stack: nothing enforces it in hardware. |
| `_FORTIFY_SOURCE` | not applicable | it swaps libc calls (`memcpy`, `strcpy`, `sprintf`, `read`) for checked versions through glibc headers. There are no libc headers here and no such calls: copies go through our own bounded helpers (`str_copy` takes the destination size), `out_fmt` writes through the bounded output buffer, and `memcpy`/`memset` in `io.c` exist only for the copies the compiler emits for struct assignments, whose sizes are constants. A home-made `__builtin_object_size` wrapper would have nothing to check. |
| format checking (`-Wformat-security`) | not applicable | `out_fmt` is not printf: by design it takes every integer as a `long` (`io.c`), so printf's checker flags its correct calls (the attribute was tried: every `%d` call site warns). Every format string is a literal. |
| PIE, static-pie, ASLR of the program | off | the binaries are static `ET_EXEC` at 0x400000: text, data and bss sit at fixed addresses, while the kernel still randomises the stack and the mmap base for them (mainline `fs/binfmt_elf.c`). static-pie on MIPS o32 needs `-mabicalls` code, every call and global access through `$t9` and the GOT, and a self-relocator in `_start` that does what the dynamic linker does for the MIPS GOT (local entries, global entries, the `DT_MIPS_*` tags): a loader of our own. Measured on omcid: +7.3% text and +18 KB of GOT and relocated data. Not worth it on a CPU where data is executable anyway, below. |
| RELRO, BIND_NOW | not applicable | no dynamic linker, no PLT, no GOT (`-mno-abicalls`), no `.init_array`: nothing is written by relocation at start-up that could then be made read-only. Constant data, function-pointer tables declared `const` included, is in `.rodata`, in the read-only segment already. |
| `-fcf-protection`, shadow stack, `-mbranch-protection` | not available | x86 and AArch64 only (gcc refuses `-fcf-protection` for this target). |
| `-fzero-call-used-regs` | off | gcc accepts it on MIPS, but it is aimed at return-oriented gadgets on x86, and costs instructions on every return. |

What this does not cover, so it is not mistaken for more: the stack
protector catches a linear overrun of a local array that reaches the
return path; it does not catch an indexed write that skips the guard, and
it covers no static buffer. Most of omcid's state, its receive buffers
included, is static (`.bss`, about 500 KB), and no compiler mitigation here
protects it; with no execute-inhibit bit and fixed addresses, a corrupted
function pointer in `.bss` is still a way to run code. The defence there is
the code: bounded parsers, and the host tests under ASan and UBSan
(`make test-host-asan`).

**What it costs**, text bytes of the stripped binaries, before and after:

| binary | before | after | growth |
|---|---|---|---|
| omcid | 211328 | 224980 | +13652 (+6.5%) |
| igmpd | 65232 | 69612 | +4380 (+6.7%) |
| omcicap | 37340 | 38140 | +800 (+2.1%) |
| diag | 29804 | 33700 | +3896 (+13.1%) |
| omcli | 15036 | 16024 | +988 (+6.6%) |
| nv | 12172 | 13708 | +1536 (+12.6%) |
| omciprobe | 8488 | 9196 | +708 (+8.3%) |

Of omcid's growth, the stack protector is about 9.1 KB, zero-initialised
locals 3.3 KB, the stack clash probes 1.2 KB and the runtime in `start.S`
0.4 KB; `-fno-strict-overflow` changes nothing measurable.

**Checked on every build.** Every Makefile in `src/` runs
`tools/check-hardening.sh` on the binary it ships, after the link and before
the strip (the checks need the symbol table), so `make src`, `make
test-omci-build` and every CI job that builds a binary fail when a
mitigation is lost: `PT_GNU_STACK` missing or executable, a segment both
writable and executable, the guard or `__stack_chk_fail` undefined, `_start`
not seeding the guard, or no function calling `__stack_chk_fail` at all.
`make -C src/diag ssp-test` (part of `make test-diag`) runs the protector
for real under qemu-user: two runs get two different guards, both with the
zero first byte, an overrun of a local array dies of SIGABRT with the
message before the function returns, the same call without the overrun
returns, and the check refuses the same program built with
`-fno-stack-protector`.

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
