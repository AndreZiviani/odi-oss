# A stock big-endian MIPS GCC builds working freestanding binaries, but only up to -march=mips2

Community wisdom around this chip family says a purchased vendor SDK is
required to compile anything for it at all. That is not true for
freestanding code — but there is a real instruction-set ceiling, and it is
narrower than it looks.

*Last verified: 2026-09-15*

---

## What

An unpatched, off-the-shelf big-endian MIPS GCC (e.g. Debian's
`gcc-mips-linux-gnu`) produces binaries that run correctly on this
hardware — no vendor toolchain, no vendor patch needed, for freestanding
(no-libc) code.

There is a ceiling, though: **`-march=mips2`, and mips2 itself needs two
more flags to be safe** — `-mdivide-breaks -mno-branch-likely`. Above mips2,
GCC emits instructions this core does not implement (a hardware bit-scan
instruction and a multiply-with-result-in-one-step instruction). *At* mips2
with default flags, GCC still emits a trap-on-condition instruction for
every divide-by-zero check, plus branch-likely forms — and this core
implements neither. See [rtl9601-isa-map](rtl9601-isa-map.md) for the full
measured instruction table.

An earlier pass at this note called the mips2 flags "safe" using a check
that only counted the two SPECIAL2 instructions and missed the
divide-by-zero traps and branch-likely forms entirely — precisely the
denylist mistake documented in `rtl9601-isa-map.md`: audit with an
allowlist of what execution actually proved present, never a list of what
you suspected might be missing.

**`-march` governs only code GCC generates from your own source.** Every
prebuilt library linked in must itself target this ISA ceiling or lower —
which is what makes higher-level runtimes needing prebuilt support code
(such as stock Go toolchains) unusable here regardless of compiler flags.

**A stock cross-glibc for this architecture is commonly hard-float only**,
and this core has no FPU, so anything that needs to link a libc needs a
soft-float-capable one built for this target — see
[rtl9601-build-own-toolchain](rtl9601-build-own-toolchain.md) for the
current from-source answer (gcc 16.2.0, binutils 2.47, uClibc-ng 1.0.59,
targeting mips2 with the two flags above).

## Why it matters

The trap people actually hit first is endianness, not the ISA ceiling: this
target needs the big-endian triplet, never the little-endian one.

Measured with a stock GCC:

    -march=mips2   ->  the two SPECIAL2 instructions absent, multiply-only path used
    -march=mips32   ->  a bit-scan instruction appears via a compiler builtin -> TRAPS

## Evidence

Device: this SoC family, the stock firmware's own toolchain reports itself
as a several-generations-old GCC (4.4.x) paired with an equally old uClibc.

Proven freestanding flags (a current Debian GCC targeting MIPS, big-endian,
cross-compiled from an ARM64 host under Docker works fine):

```
-std=c99 -Os -march=mips1 -mabi=32 -EB -msoft-float -G0
-fno-pic -mno-abicalls -ffreestanding -fno-builtin -fno-stack-protector
-nostdlib -nostartfiles -static -Wl,-e,_start
```

Load-bearing in order of how quietly each one fails if dropped: `-EB`
(endianness); then `-G0 -fno-pic -mno-abicalls`, because a hand-written
entry point never sets up the global-pointer register and any
GOT-relative reference then faults; then `-msoft-float`.

Confirmed by running a freestanding binary under 1.2 KB on the device
itself.

**Superseded for anything that links a libc.** This project now builds its
own from-source userland toolchain —
[rtl9601-build-own-toolchain](rtl9601-build-own-toolchain.md). What
survives unchanged from this note: a stock big-endian GCC needs no vendor
toolchain at all for **freestanding** code, which is how every one of this
project's own device-side tools is built — see
[rtl9601-freestanding-over-libc](rtl9601-freestanding-over-libc.md). The
measured ceilings of a prebuilt vendor toolchain, kept for the historical
record, are in
[rtl9601-vendor-prebuilt-toolchain](rtl9601-vendor-prebuilt-toolchain.md).

## See also

- [rtl9601-isa-map](rtl9601-isa-map.md)
- [rtl9601-netcat-file-transfer](rtl9601-netcat-file-transfer.md)
