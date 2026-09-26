# A new MIPS cache implementation must generate clear_page/copy_page itself

Porting a MIPS cache implementation forward: two generator calls are easy to
miss, and skipping them hangs the boot silently with no error.

*Last verified: 2026-09-23*

---

## What

Mainline MIPS builds `clear_page()` and `copy_page()` at run time from a
short generated instruction sequence, but only when the CPU's cache-init
path explicitly calls the two builder functions for them; the existing
cache implementations for other MIPS cores do this as their last step. A
new cache implementation that wires up every cache-flush hook but skips
these two calls leaves both routines as empty, unpopulated code buffers —
and the very first page-zeroing operation during boot jumps straight into
one of them.

## Why it matters

This is a silent hang, not a crash: the boot log looks completely normal
right up to the point memory management starts allocating pages, and then
simply stops with no fault, no panic, and no further output.

## Evidence

On a from-scratch MIPS cache implementation for this CPU (Linux 6.18), the
kernel reached its process-table initialization message and then hung
silently in three trials in a row, even after every individual cache
operation had been checked against known-good behavior. Adding the two
generator calls, a whole-cache flush right after them, and one additional
default-cacheability setting fixed it, and the kernel booted cleanly.

That fix combined three changes in a single trial and was not bisected
afterward, so crediting the fix entirely to the two generator calls is not
fully isolated — treat the whole-cache-flush and cacheability pieces as part
of the same fix rather than confirmed-unnecessary.
