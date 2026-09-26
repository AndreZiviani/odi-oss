# Decision: build a current gcc + uClibc-ng toolchain targeting mips2, not mips1

A current, from-source toolchain builds everything for this core — kernel
included — and correctly targets mips2 rather than the more conservative
mips1 a vendor toolchain defaults to.

*Last verified: 2026-09-21*

---

## What

gcc **16.2.0**, binutils **2.47** and uClibc-ng **1.0.59** build a working
cross toolchain for this CPU, replacing a vendor-supplied toolchain built
around 2010. Verified by running the output, not just inspecting it: a
static MIPS-II binary exercising float formatting, `libm`, and string
handling produces correct output under emulation, and all eighteen target
libraries this project builds contain zero instructions this core traps on.

This toolchain now builds the kernel too (as of the current from-source
kernel port), once binutils is taught the handful of extra opcode encodings
this CPU family needs beyond stock MIPS binutils.

## Five libc options that packages otherwise work around with their own shims

uClibc-ng's default configuration leaves off several features that busybox,
dropbear and iproute2 then work around in their own source trees, each
workaround a small correctness risk of its own. Enabling them in the libc
instead removes the workarounds: a legacy libc call busybox needs directly,
tree-walk support iproute2 needs, `openpty` support (so dropbear needs no
special flag to fall back to its own pty handling), and stack-protector
support with a real random guard value (so `-fstack-protector-strong`
links against genuine libc support rather than a constant placeholder).

On the *current* kernel line this toolchain targets, 64-bit-safe time
handling (the Y2038 problem) and its underlying kernel support are both
available — so leaving 64-bit `time_t` off is now an active choice to make
deliberately (for a smaller or simpler build), not something forced by an
old kernel's syscall surface. That was not true earlier in this project,
when the kernel line in use predated that kernel support entirely; anyone
reusing this note's toolchain settings against an older kernel should
re-check this rather than assume it still applies.

## The target is mips2, and that is the key decision

Direct execution on real hardware confirms `ll`, `sc` and `sync` are legal
on this core (see [rtl9601-isa-map](rtl9601-isa-map.md)). That is what makes
targeting mips2 tractable: uClibc-ng gets native atomics instead of needing
a fallback. The MIPS-II instructions that *are* illegal on this core — a
trap-on-condition instruction and two branch-likely forms — are exactly the
ones a pair of GCC flags suppress:

```
--with-arch=mips2 --with-abi=32 --with-float=soft --with-endian=big
CFLAGS_FOR_TARGET="-march=mips2 -mno-branch-likely -mdivide-breaks -Os"
```

A hardware bit-scan instruction and a single-step multiply-with-result
instruction cannot appear at mips2 at all, being from a newer instruction
family this core also lacks.

**A borrowed off-the-shelf cross-compiler does not work.** A generic
distro-packaged MIPS cross-GCC may accept the mips2 architecture flag
happily, but its own prebuilt runtime library is typically built for a
newer MIPS baseline regardless of that flag, and carries the trapping
instructions anyway. **The runtime library sets the ISA floor, not the
compiler flag** — the toolchain's own runtime and libc must be *rebuilt*
targeting this ISA, not merely invoked with a narrower `-march`.

## Audit the LIBRARIES, not a test program

An earlier from-source toolchain attempt looked fine because its test
application binaries were clean, while its own compiler-runtime and libc
static libraries still carried dozens of the trap instruction each. The
flags have to be baked in twice: into the toolchain's own build
configuration, and again as the flags used to build target code. The
practical audit step is to check the actual machine code of every static
library the toolchain will link against, not just a sample application
binary.

That audit earned its keep on the first real run, finding one instance of
the trap-on-condition instruction inside the compiler's own runtime
library — traced to a hardened-control-flow-redundancy check GCC can pull
in under a specific hardening flag. It was excluded by name with the
reasoning recorded, rather than by weakening the general check.

## What had to be worked around, all "old kernel meets new software"

Building against a kernel line from 2009 forced several tradeoffs that no
longer apply once building against the current kernel line, but are worth
recording because they will resurface for anyone targeting the older
kernel branch:

1. A modern libc's default 64-bit `time_t` path on this 32-bit ABI needs a
   syscall not present before Linux 4.11; disabling it means 32-bit
   `time_t` (Y2038-limited), which was a hard floor set by the old kernel
   ABI, not a choice, at the time.
2. A libc network-time function falls through to a legacy syscall path on
   pre-2.6.39 kernels that does not compile cleanly under a current
   compiler — an upstream libc code path that is simply unexercised by
   anyone still targeting that old a kernel, worth reporting upstream.
3. A very old kernel export set is occasionally inconsistent with itself
   (a header is present but not listed as exported), fixed upstream later;
   the practical fix is to copy the missing header into the build sysroot
   rather than patch the old kernel's export list.
4. uClibc-ng's default configuration can leave C99 math support off while
   floating point support is on, which fails to link on MIPS specifically,
   because MIPS (unlike some other architectures) has no inline resolution
   path for one specific math primitive that floating-point printf
   formatting needs.

Threads are off, because nothing in this image needs them and the libc
configuration used here selects a no-threads build by default.

## What it bought

Recovered as genuinely working, verified by running the output rather than
merely by a build that stopped failing: DNS name resolution support,
`posix_fallocate`, and `unshare`, letting hand-written workaround code in
downstream packages be deleted outright. A downstream network-tools
package moved forward several major versions with no patches needed against
its upstream source, because an old-GCC limitation on designated
initializers with anonymous struct/union members is gone.

**Not recovered, for a kernel-version reason rather than a toolchain one**
(when targeting the older kernel line): two Linux namespace-related
utilities stay unavailable, because the libc only builds those wrappers
where the kernel headers it's compiled against declare the corresponding
syscall numbers, and the old kernel line predates both syscalls. Shimming
them would ship command-line tools that always fail at runtime — worse
than not shipping them.

**That is the general pattern for anything still missing when targeting
the old kernel line: the kernel is the floor, not the toolchain.** Targeting
the current kernel line removes this particular class of limitation
entirely.

## Evidence

Built in a Linux container natively (a kernel-headers build step for the
older kernel line cannot run on macOS, and a macOS-hosted cross compiler is
not reproducible); the toolchain build itself writes large numbers of small
files, so its working prefix lives on a proper Linux volume rather than a
macOS bind mount, for both speed and case-sensitivity reasons.

## See also

- [rtl9601-isa-map](rtl9601-isa-map.md)
- [rtl9601-freestanding-over-libc](rtl9601-freestanding-over-libc.md)
- [rtl9601-vendor-prebuilt-toolchain](rtl9601-vendor-prebuilt-toolchain.md) — the toolchain this one replaced, kept for its measured ceilings
