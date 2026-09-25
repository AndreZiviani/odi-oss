# A kernel port for this CPU must take the R3000 exception model, not R4000's

Upstream MIPS Linux keys several core exception-handling primitives on the
older R3000 CPU class; giving this CPU family a fresh Kconfig identity
without also selecting that class silently gets the wrong model and dies on
the very first exception return.

*Last verified: 2026-09-23*

---

## What

This CPU is R3000-class: its status register keeps a three-deep interrupt
enable/kernel-mode stack (no separate "exception level" bit), it returns
from an exception with the R3000-family return-from-exception instruction
rather than R4000's, and its TLB entry-high register holds a 6-bit address
space ID rather than R4000's wider field. Upstream Linux's MIPS port
selects all of that behavior — exception entry/exit code, register save and
restore layout, the fast-path interrupt handler, new-thread status
register setup, and the default address-space-ID width — behind a single
build-time CPU-class flag for the R3000 family. A new CPU identity added to
the kernel for this specific chip, if not also marked as belonging to that
same class, silently inherits the R4000-family version of every one of
those sites instead.

## Why it matters

A kernel built this way boots, prints its entire startup log, and takes its
first timer interrupt — and then dies at the very first return-from-exception
instruction, with the fault handling itself also running under the wrong
privilege-level model. On one affected build this surfaced as a crash deep
in an unrelated-looking kernel subsystem, triggered from the idle task —
a symptom that gives almost no hint that the actual problem is the exception
model.

## Fix

Introduce a combined class flag selected by both "is R3000" and "is this
specific CPU", and key the R3000-specific code sites on that combined flag
rather than only on the older, generic R3000 selector. Leave
architecture-tuning flags and the CPU-detection code on the plain R3000
selector unchanged. A useful sanity check after the fix: the kernel's
compiled code should contain zero uses of the R4000-family
return-from-exception instruction.

The simpler alternative, which needs no core-file edit at all, is to
build the CPU as the plain R3000 selector. Its cost is the barrier model:
the R3000 selector assumes no `sync` instruction, so the ordinary write
and read barriers compile to nothing, and full barriers go through a
board-supplied write-buffer flush hook. On this core, which does
implement `sync`, the board has to point that hook at a `sync` and use
its own `sync` wherever DMA ordering depends on it (docs/KERNEL.md,
"The CPU"). That route has passed the static gates but has not yet had
a hardware trial.

## Evidence

Before the fix: the kernel took its first timer tick, then hit a TLB
exception with an unexpected null pointer inside exception-return handling
almost immediately. After keying the exception-handling sites on the
combined class flag: the kernel ran nearly three thousand timer ticks and
reached a normal root-filesystem mount.

## See also

- [rtl9601-isa-map](rtl9601-isa-map.md)
