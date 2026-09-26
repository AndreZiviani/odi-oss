# Register access on the stock firmware is getsockopt/setsockopt on a raw socket, not ioctl

Every hardware register read and write in the stock firmware's switch/optics
tooling goes through a `getsockopt`/`setsockopt` call on a raw socket with a
custom option number — there is no ioctl interface and no memory-mapped
device file involved.

*Last verified: 2026-09-13*

---

## What

Register access on this device is a raw-socket sockopt call, not ioctl and
not a memory-mapped device file:

    fd = socket(AF_INET, SOCK_RAW, 0xff);
    getsockopt(fd, 0, <opt>, buf, &len);   /* read  */
    setsockopt(fd, 0, <opt>, buf,  len);   /* write */
    close(fd);

A fresh socket is opened and closed for every single access — there is no
persistent handle.

The observed option numbers and their payload shapes:

| operation | option | payload |
|---|---|---|
| register read/write by index | `0x3211` | `{register, bit, value}` (12 bytes) |
| register read by address | `0x3216` | `{address, value}` (8 bytes) |
| register write by address | `0x3213` | `{address, value}` (8 bytes) |
| SoC address read (absolute) | `0x3218` | `{address, value}` (8 bytes) |
| optical transceiver read | `0x2c39` | 100-byte buffer |

Raw sockets need root, which is not a practical constraint on this device
since everything already runs as root — but it is a constraint for anything
you build that runs with reduced privileges.

## The address you pass is not the address that gets read

Switch-core register addresses are **relative to a base address**, not
absolute physical addresses. This device's base is the uncached memory
window at `0xbb000000`. Addresses at or above `0x2000000` are rejected, as
is anything not 4-byte aligned.

**Handing this interface an untranslated address is not a failed read — it
wedges a kernel thread.** Observed directly: the calling process went into
an uninterruptible-sleep state with zero memory footprint, survived being
killed with an unblockable signal, and stayed that way until reboot.

This is easy to confuse with plain CPU overload, because both raise the
load average — but they are not the same failure. An uninterruptible-sleep
task inflates the load average **without consuming any CPU**, so the device
stays completely responsive otherwise: other tools, SSH, and any metrics
collection keep working fine, and memory is unaffected. **Check the actual
process state before concluding a stick is overloaded** — a stuck register
read looks very different from a busy one once you look past the load
average number.

## Why it matters

It means a small freestanding program can read and write switch registers
and the optical diagnostics with a handful of syscalls and no supporting
libraries at all. It also explains part of the cost of the stock tooling: a
socket create, syscall, and close for every single access, rather than a
persistent handle.

Note separately that on MIPS, `SOCK_STREAM` and `SOCK_DGRAM` are swapped
relative to x86 — see
[MIPS o32 ABI traps](mips-o32-syscall-abi-traps.md) — so copying socket type
constants from another architecture silently produces the wrong socket
type; `SOCK_RAW` itself happens to have the same value on both.

## See also

- [MIPS o32 ABI traps](mips-o32-syscall-abi-traps.md)
