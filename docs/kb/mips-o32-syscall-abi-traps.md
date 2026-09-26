# MIPS o32 differs from the generic Linux syscall ABI in several places, and each fails silently

A freestanding (no-libc) MIPS o32 binary cannot reuse syscall constants,
struct layouts, or calling conventions copied from x86/ARM code. Six
differences bite, and every one of them is silent.

*Last verified: 2026-09-14*

---

## What

None of these six differences produce an error: the first is silently
unimplemented, the next three quietly do the wrong thing, and the last two
are constants that are simply different numbers than the ones a port from
another architecture would assume.

1. **`signal(2)` is not implemented on this kernel line.** Its syscall
   number is accepted by the syscall dispatcher but the underlying handler
   is the kernel's generic "not implemented" stub, which returns `-ENOSYS`.
   The trap is in the *calling convention*, not the return value: MIPS o32
   syscalls signal an error through a separate register (the ABI's third
   return-value slot), not by returning a negative number the normal C way.
   A syscall wrapper that only checks "is the return value negative" and
   ignores that separate error flag will read the raw return value (a small
   positive number, the negated errno) as a successful call that did
   nothing — so `signal(SIGPIPE, SIG_IGN)` appears to succeed while the
   signal disposition is actually unchanged. Use `rt_sigaction` instead, and
   always check the ABI's real error indicator, not just the sign of the
   returned value.

2. **MIPS `struct sigaction` puts `sa_flags` FIRST**, then the handler
   pointer, then the signal mask — the reverse of the generic field order,
   where the handler leads. A struct filled in generic order passes the
   flags where the kernel expects a handler pointer. The signal-set size is
   also 16 bytes here (128 signals), not 8.

3. **The socket type constants are BSD-derived, not generic.**
   `SOCK_STREAM` is 2 and `SOCK_DGRAM` is 1 — *swapped* relative to every
   other common Linux architecture — and `SOL_SOCKET` is `0xffff`, not 1.
   `SO_REUSEADDR` is 4; `SO_SNDTIMEO`/`SO_RCVTIMEO` are `0x1005`/`0x1006`,
   not the values a generic-Linux port would assume.

4. **Only four syscall arguments go in registers.** A fifth and sixth
   argument (needed by calls like `setsockopt`) go on the stack at fixed
   offsets, which the o32 calling convention reserves space for even when a
   call takes fewer arguments. Loading those stack arguments has to happen
   *before* adjusting the stack pointer for the call, or the write lands in
   the caller's own stack frame instead.

5. **`EIDRM` is 36 here, not the generic-ABI value.** MIPS kept the original
   System V errno numbering in a range where the generic ABI renumbered a
   handful of values. Code that tests a raw error number for "the System V
   IPC queue was removed" using a value borrowed from another architecture
   never matches on MIPS, silently leaving a stale queue reference in
   place. The values immediately on either side of it are unaffected, which
   is what makes the one wrong comparison easy to miss — everything around
   it keeps working.

6. **`ioctl` request numbers are composed with different bit positions.**
   The "no data", "read" and "write" direction bits, and the size field
   width, sit in different positions in the request number on this
   architecture than on a generic Linux port. A request number copied
   verbatim from another architecture's header is accepted by the kernel
   and means a *different* request — same failure shape as the `EIDRM`
   trap above: the call succeeds and does something else.

## Why it matters

Every one of these is silent, and several are silent in the worst possible
direction: the call returns as if it succeeded, having done nothing.

The `signal` case cost a real debugging session. A no-libc network daemon
died whenever a client cancelled a request or closed a connection early:
writing to a socket whose peer is gone raises `SIGPIPE`, whose default
action is to terminate the process, and with no libc nothing installs a
handler unless the code does it explicitly. The attempted
`signal(SIGPIPE, SIG_IGN)` call appeared to return success and changed
nothing, so the daemon kept dying, and the symptom looked like intermittent
"connection refused" — a server process that was simply no longer running —
which does not look like a signal-handling problem at all.

The swapped socket-type constants are the nastier shape of the same issue:
`socket()` still succeeds either way, just producing a working socket of
the wrong kind.

## Evidence

Verified on the device itself: the SIGPIPE failure was reproduced by
requesting a large response and closing the socket immediately, and the
daemon was gone before the next request could land. Switching to
`rt_sigaction` with the correct field order fixed it, and the same binary
then survived the same test repeatedly.

The socket constants above are load-bearing in binaries that bind and serve
on the hardware today. The five-argument-syscall path (point 4) is
exercised on the device at every boot.

## See also

- [rtl9601-isa-map](rtl9601-isa-map.md) — which *instructions* execute on this core
- [rtl9601-userland](rtl9601-userland.md) — the device this ABI applies to
