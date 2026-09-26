# The U-Boot environment is a redundant pair, and its CRC covers everything but the flags byte

U-Boot keeps a redundant pair of environment blocks with a big-endian CRC32
that covers everything after the flags byte — not the obvious "whole
block minus the CRC" — and the higher flags value is the active copy.

*Last verified: 2026-09-21*

---

## What

`nv getenv` / `nv setenv` is how `sw_tryactive` and `sw_commit` (see
[trial boot](rtl9601-uboot-trial-boot.md)) get set, which makes this
load-bearing for every safe flash on this device. The stock tool talks to a
proprietary vendor library to do this, so a from-scratch replacement needs
to work directly against the raw flash layout and checksum scheme below,
with no vendor code involved.

Everything needed is the raw MTD device and a CRC32. The block, read off a
device:

    mtd "env"    8 KB, 4 KB erase blocks
    mtd "env2"   a REDUNDANT pair, same size

    offset 0..3     CRC32, BIG-ENDIAN
    offset 4        flags byte
    offset 5..end   NUL-separated "key=value", a double NUL ends the list
    the CRC covers offsets 5..end -- NOT the flags byte

**That flags byte is the whole trap.** It is what makes this a *redundant*
environment rather than a plain one, and it is why the obvious "CRC32 of
everything after the first four bytes" does not verify. Both readings were
tried; only `crc32(buf[5:])`, compared big-endian, matches.

**The higher flags byte wins.** On the unit examined, one copy had flags
`0x00` and the other `0x01`, both CRC-valid, and the stock tool reported
using the second. Two observations cannot tell a sequence counter from a
simple active/obsolete marker, so a replacement writer should either settle
that first, or rewrite only the copy it read and leave the flags byte
alone — correct under either reading, though it gives up the ping-pong the
redundancy exists for.

## The CLI contract matters because boot scripts parse it

Reimplementing from the visible command output alone gets two behaviours
wrong; they only show up when a replacement is run side by side with the
original:

- **A key that does not exist prints nothing and exits successfully.** Not
  an error, not a non-zero status. Boot scripts read a value and test the
  result for emptiness, so a replacement that prints an error line instead
  would still "pass" — by accident, not by design.
- **Reading the whole environment with no argument prints a header line
  first** (naming which of the two copies is in use), then the key/value
  pairs, then a trailing blank line.

Writing behaviour was not characterised to the same depth, since checking
it means actually writing flash. The stock tool's own error strings say an
overflow evicts a key rather than failing outright — that much is known,
no more.

## Why it matters

A from-scratch reimplementation of this tool is small, self-contained, and
needs no vendor library, kernel module or driver header. The one piece
that is not obvious is the flash-erase step, which carries a MIPS ABI trap
of its own: the direction bits used to build the erase ioctl request differ
from what an x86-derived header would compute — see
[the MIPS o32 syscall ABI note](mips-o32-syscall-abi-traps.md).

Read-side verification is possible entirely off the device: capture a
block, check the CRC, and compare a round-tripped read against a reference
implementation byte for byte. The write path can be exercised the same way
using an emulated flash device carrying a copy of the real partition
layout — which covers the erase and write handling in general, though not
this specific NOR part's own driver quirks.

**A caution the redundancy makes necessary.** If a replacement writer
rewrites only the copy it read (as suggested above), the two copies can
diverge and stay diverged: on one unit, the "current" copy read one value
for a boot-select variable while the other, stale copy still read the
previous value — and the stale copy is the one a lost mid-write update
would fall back to. Read both copies before trusting either, and after any
write.

## Evidence

Read directly off a live device: 67 key/value pairs, both copies valid,
`crc32(buf[5:])` matching the stored big-endian value where neither a
little-endian reading nor a CRC starting at the flags byte does. A
from-scratch replacement's output was byte-identical to the stock tool's
across all 67 pairs.

A later trial found the two copies disagreeing after an interrupted write —
one held the pre-trial value for the trial-boot slot selector, the other the
post-trial value — and a copy-targeted write command restored agreement
without touching the value that was actually correct. Checking both copies
is worth doing as a matter of routine, not only when something looks wrong.

## See also

- [Trial boot](rtl9601-uboot-trial-boot.md) — what `sw_tryactive` and
  `sw_commit` actually do, and why an automatic rollback would be wrong
- [Config store](rtl9601-config-store.md) — the *other* key-value store on
  this device, unrelated
- [MIPS o32 syscall ABI traps](mips-o32-syscall-abi-traps.md)
