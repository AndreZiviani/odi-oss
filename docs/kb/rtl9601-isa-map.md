# RLX5281 instruction set: verify by executing, not by ISA level

The core implements two MIPS ISA levels only in pieces; the safe way to know
what runs is to execute it, never to infer from a family or ISA-level label.

*Last verified: 2026-09-14*

---

## What

Measured by executing each instruction on a real ODI DFP-34X-2C2:

| instruction | family | result |
|---|---|---|
| `lwl` `lwr` `swl` `swr` | MIPS-I unaligned | ok |
| `movz` `movn` | SPECIAL 0x0A/0x0B | ok |
| `ll` `sc` `sync` | MIPS-II | ok |
| `bltzl` | MIPS-II branch-likely, REGIMM | **ok** |
| `madd` | SPECIAL2 funct 0x00 | ok |
| `mul` | SPECIAL2 funct 0x02 | **ILLEGAL** |
| `clz` | SPECIAL2 funct 0x20 | **ILLEGAL** |
| `teq` | MIPS-II trap | **ILLEGAL** |
| `beql` `bnel` | MIPS-II branch-likely, opcode | **ILLEGAL** |

Two ISA levels, both implemented in pieces: SPECIAL2 per-instruction, and
MIPS-II with the REGIMM-encoded branch-likely form present while the
main-opcode branch-likely forms are not. **An ISA level is not a unit on
this core.** Predicting from the family is unreliable in both directions:
"`madd` decodes, so SPECIAL2 exists" and "`ll`/`sc` work, so mips2 is safe"
were both wrong predictions in practice.

This is a Lexra-derived MIPS core: it substitutes a count-leading-*sign*-bits
instruction for the standard count-leading-zeros instruction MIPS32 expects
(so a `clz`-based bit-scan traps even though the core has an equivalent
instruction under a different name), and it does support `ll`/`sc`/`sync`
atomics and unaligned load/store — confirmed both by direct execution and by
instruction counts inside the stock kernel image itself, which contains over
a thousand `ll`/`sc` pairs and dozens of `sync`, in a kernel that boots this
hardware every day. So a full bit-scan routine for this core needs a
substitute instruction, not a fallback to software emulation.

A fully authoritative, exhaustive per-core instruction table for this CPU
family is not known to exist publicly. The table above is everything that has
actually been confirmed by execution so far, not a closed set.

## Why it matters

**Go is out entirely** for anything not hand-restricted: a hello-world binary
from the standard Go compiler emits `mul` hundreds of times and `clz` dozens
of times. Go's compiler has no ISA knob for this; alternate Go toolchains
that claim to target an older MIPS ISA still pull in prebuilt runtime/libc
code that was not built for it, so the instructions still show up. **The
general rule: the runtime library sets the ISA floor, not the compiler
flag.**

**GCC at `-march=mips2` is unsafe** without `-mdivide-breaks
-mno-branch-likely`. `teq` arrives as every divide-by-zero check — 147 of
them in one real build — and branch-likely as another 109. And those flags
must reach the *target libraries* too, not just your own source: a
generic mips2 toolchain's own runtime libraries carried 64 and 95 `teq`
instances respectively until the flags were applied to them specifically.
See [rtl9601-build-own-toolchain](rtl9601-build-own-toolchain.md) for the
toolchain that gets this right end to end.

## Evidence, and the audit lesson

Each instruction was tested in isolation (one process per instruction, so a
SIGILL only ends that one test), with a known-good instruction run first as
a positive control so a broken test harness cannot masquerade as a missing
instruction.

**The audit that missed `teq` is the durable lesson.** An early check listed
the instructions *suspected* of being absent and counted only those, so a
binary full of `teq` passed clean and then crashed with an illegal
instruction on real hardware. A check that only looks for problems you
already know about cannot find a new one. Invert it: keep an allowlist of
instructions *confirmed present by execution*, and treat everything else as
unverified. That form also catches the assembler's zero-register aliases for
the branch-likely instructions, which a plain name-based deny-list does not
name and which under-reported one binary's illegal-instruction count by
dozens.

**Static analysis of the vendor's own binaries cannot substitute for
execution.** The stock firmware's own tools were built for an older,
stricter ISA target than the hardware actually supports, so they simply
never emit some of the instructions this core does implement. Seeing an
instruction absent from vendor-built code proves nothing about whether the
hardware supports it — only executing the instruction does.

## See also

- [rtl9601-build-own-toolchain](rtl9601-build-own-toolchain.md) — the toolchain that targets this ISA correctly, kernel included
- [rtl9601-stock-gcc-cross-compile](rtl9601-stock-gcc-cross-compile.md)
- [mips-o32-syscall-abi-traps](mips-o32-syscall-abi-traps.md)
