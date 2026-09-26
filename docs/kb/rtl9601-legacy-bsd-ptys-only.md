# The stock (OEM) kernel has no devpts, only legacy BSD pseudo-terminals

The stock kernel's pseudo-terminal support predates the modern Unix98 API
entirely — a modern libc's `openpty()` fails on it, and the stock SSH
daemon is old enough that current SSH clients refuse to talk to it at all.

*Last verified: 2026-09-16*

---

## What

The stock (OEM) kernel has **no devpts filesystem** — its list of supported
filesystems has no entry for it at all. It has **legacy BSD-style
pseudo-terminals** instead.

**This is a kernel configuration choice, not a property of the hardware.**
Unix98 PTY support is normally on by default upstream, and the stock
firmware's kernel configuration turns it off; enabling it brings in the
pseudo-terminal filesystem along with it, giving a replacement kernel a
proper `/dev/pts` tree. It matters because a modern busybox `telnetd` build
cannot work at all without it — see
[the telnetd/PTY note](busybox-telnetd-needs-unix98-ptys.md).

## Why it matters

**Anything that uses pseudo-terminals on the stock kernel must not rely on
the modern `openpty()` API.** A typical embedded libc's `openpty()`
implementation only knows about the modern `/dev/ptmx` device, which this
kernel doesn't have, so it fails right after what looked like a successful
login. An SSH daemon built for this kernel needs to be built to use its own
legacy BSD-style scanner instead — and that kind of build-flag change is
easy to silently no-op if the build system caches the earlier detection
result, so it's worth diffing the resulting binary after changing a flag
like this rather than trusting that the flag took effect.

Building your own SSH daemon is necessary at all because the stock one is
old enough (a 2007-era release) that it only offers key exchange and cipher
algorithms current SSH clients refuse by default — making it unusable from
a modern SSH client without explicitly re-enabling legacy algorithms on the
client side.

## See also

- [A modern busybox telnetd needs Unix98 PTY support](busybox-telnetd-needs-unix98-ptys.md)
- [A file missing from the image may be a symlink into RAM](rtl9601-rootfs-symlinks-into-var.md)
- [The stock userland's networking gaps](rtl9601-userland.md)
