# Historical: a prebuilt vendor toolchain's measured limits, kept for the record

This device's SoC vendor publishes a prebuilt cross-compilation toolchain
for this CPU family. This project no longer uses it — a from-source
toolchain now covers everything, kernel included — but its measured limits
and a sharp `-march` trap are recorded here because they're still useful if
anyone reaches for that vendor toolchain themselves.

*Last verified: 2026-09-21*

---

## Superseded

As of this project's current toolchain work, a from-source gcc/binutils/
uClibc-ng toolchain targeting mips2 builds everything this project ships —
userland, kernel, and drivers — and no vendor toolchain is fetched or used
any more. See
[rtl9601-build-own-toolchain](rtl9601-build-own-toolchain.md). What follows
is kept only for its measured facts about the vendor toolchain's behavior.

## Why a generic prebuilt toolchain doesn't work either

Widely available prebuilt MIPS toolchains (distro cross-toolchains, generic
embedded-Linux build systems) commonly target a MIPS32 baseline at minimum,
which emits instructions this specific core does not implement (see
[rtl9601-isa-map](rtl9601-isa-map.md)). Some popular embedded build systems
cannot even be configured below that baseline at all — it's their
architecture floor, not a configurable option.

**Building a from-scratch toolchain for the right ISA target is a subtle
trap**, not a quick fix: a general-purpose toolchain builder can be told to
target the right instruction-set architecture level, but that level alone
still emits a trap-on-condition instruction and branch-likely forms this
core lacks — those need to be suppressed with compiler flags applied not
just to your own code, but to the *toolchain's own runtime libraries*,
requiring a full rebuild of those libraries specifically. Every step in
that process is easy to skip silently, and skipping just one is enough to
ship a broken binary that looks fine until it runs.

## A vendor-toolchain-specific `-march` trap

The compiler flags that are correct for a *generic* MIPS toolchain
targeting this core are actively wrong for this vendor's own prebuilt
toolchain: passing the generic-toolchain ISA flag to the vendor toolchain
aborts the build outright with a hard-coded architecture mismatch error —
the vendor toolchain only accepts its own specific target flag.

This makes for a nasty silent failure mode: a build script that passes
compiler flags through an intermediate variable that never actually reaches
the compile step will have those flags silently dropped rather than
rejected, and the resulting binary looks identical to a correctly-flagged
one until you check for the trap instructions inside it. One real package
build was affected exactly this way and produced an incorrect binary; once
rebuilt with the flags genuinely reaching the compiler, the output was
byte-for-byte different (and correct).

Separately: this vendor toolchain reports at least one relevant safety flag
as "disabled" while its actual generated code is safe anyway, because the
vendor has patched the compiler backend directly rather than gating that
behavior behind the flag the reporting tool checks — so trusting the
toolchain's own self-reported flag state here would be misleading. **The
vendor toolchain needs no additional ISA safety flags at all**, and its
defaults are exactly why binaries built with it pass an instruction-set
audit cleanly.

## What the vendor toolchain costs, measured

Its age is a functional ceiling as well as a stylistic one:

- Its compiler (roughly a 2010-era GCC) accepts anonymous struct/union
  members but cannot use one inside a C99 designated initializer — plain
  member access through the anonymous member compiles fine, so the
  limitation is narrower than "no anonymous member support," and it lands
  in exactly the wrong place for at least one real upstream package,
  capping it at an older release.
- Its bundled libc is missing several now-common library functions
  (resolver query support, `posix_fallocate`, `setns`, `unshare`,
  `syncfs`), which forces a downstream busybox build to drop the
  corresponding applets (`nslookup`, `fallocate`, `nsenter`, `unshare`,
  `sync -F`).
- Its `openpty` support only knows the modern pty-cloning device, while the
  stock kernel line has no modern pty filesystem support — so an SSH
  session using it authenticates and then dies. The SSH daemon needs to be
  told to use its own legacy pty-scanning fallback instead. A from-source
  replacement kernel can enable modern pty filesystem support (the
  upstream kernel default, which the stock kernel ships without), which makes this a choice rather than a strict necessity —
  see
  [busybox-telnetd-needs-unix98-ptys](busybox-telnetd-needs-unix98-ptys.md).
- No SHA-512 password hashing support, so root password hashes are stuck
  on the much weaker MD5-based scheme.

**A modern toolchain should target mips2, not the more conservative mips1**
the vendor toolchain defaults to — see
[rtl9601-isa-map](rtl9601-isa-map.md) for why `ll`/`sc`/`sync` being legal
on this core makes that safe, once the two trap-avoiding compiler flags are
applied consistently.

## What it got right

The vendor toolchain, precisely because it targets this exact CPU core by
default, is the only toolchain in this project's history whose output
passed a strict instruction-set audit with zero unverified instructions on
the first try. Its output is also noticeably smaller, since its bundled
libc is leaner than a modern one — one representative static binary came in
at roughly a third the size of the same source built with a
from-scratch-targeted mips2 toolchain, before that mips2 toolchain's own
runtime libraries were properly rebuilt for this ISA.

## Evidence

An SSH daemon built with the vendor toolchain, including modern
cryptographic algorithm support, built and ran correctly on the device,
authenticating and serving an interactive session. An early attempt at a
from-scratch mips2 toolchain building the same source crashed with an
illegal instruction until its runtime libraries were rebuilt with the
correct flags.

## See also

- [rtl9601-isa-map](rtl9601-isa-map.md)
- [rtl9601-build-own-toolchain](rtl9601-build-own-toolchain.md) — the current toolchain
- [busybox-telnetd-needs-unix98-ptys](busybox-telnetd-needs-unix98-ptys.md)
