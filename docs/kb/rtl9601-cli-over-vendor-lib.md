# Decision: read optical/switch metrics by scripting the stock CLI, not by linking the vendor library directly

To read optical diagnostics (DDM) and switch metrics from the stock
firmware, script the existing `diag` CLI rather than link against the
vendor's own optics/switch library directly.

*Last verified: 2026-09-15*

---

## Decision

Read the values by forking the stock firmware's `diag` command-line tool
and parsing its text output, batching multiple commands into a single
invocation over its stdin, and bounding the child process with a timeout —
rather than linking the vendor's own optics/switch shared library directly
from a monitoring daemon.

## Why

**Linking the vendor library directly would cost the freestanding build.**
That library needs a full libc, so a caller would have to link the
device's own libc and give up the freestanding, dependency-free build this
project uses for its own tools (see
[rtl9601-freestanding-over-libc](rtl9601-freestanding-over-libc.md)).

**The vendor library's calling convention for these functions is
undocumented.** Argument types and ordering would have to be guessed rather
than read from any specification.

**The performance cost of forking is real but small in context.** A full
metrics scrape as several separate CLI invocations measured about 220 ms,
against typical scrape intervals of 15–60 seconds — latency alone does not
justify the added risk of linking an undocumented library.

**Commands can be batched into one process invocation.** The CLI reads
commands from its own standard input and echoes each one back, so a whole
scrape's worth of commands can be sent as one script to one process
invocation rather than one process per command; measured, eight commands as
eight separate process invocations cost roughly 5x what the same eight
commands cost fed to one long-lived invocation. Not every command is safe
to send this way, though — see the operational gotcha below.

**The CLI must be bounded, never trusted to exit on its own.** The stock
`diag` tool does not exit cleanly when its input reaches end-of-file, and
at least one of its commands can hang indefinitely rather than return at
all — see
[rtl9601-vendor-diag-spins-when-stdin-closes](rtl9601-vendor-diag-spins-when-stdin-closes.md)
for the specific failure mode and its symptoms. Any caller that forks this
CLI must not leave the child able to outlive the parent: a single-threaded
server that does is not slow, it is dead, since every later request queues
behind a child process that will never exit. A new command should only be
added to the batched form once it has been shown, empirically, to
terminate correctly when fed on stdin rather than as a command-line
argument — that is a property of the specific command, not of the CLI in
general.

## See also

- [rtl9601-freestanding-over-libc](rtl9601-freestanding-over-libc.md)
- [rtl9601-vendor-diag-spins-when-stdin-closes](rtl9601-vendor-diag-spins-when-stdin-closes.md)
- [rtl9601-userland](rtl9601-userland.md)
