# Decision: keep every device-side tool freestanding, not linked against a libc

Every tool this project writes for this device is built without a libc at
all, and that stays true even now that a full from-source libc toolchain
exists for this target.

*Last verified: 2026-09-15*

---

## The decision

Every device-side tool this project writes — the diagnostics CLI, the OMCI
daemon and its helper tools, the multicast daemon, the environment tool,
the metrics exporter, the web/config daemon — is built `-nostdlib
-nostartfiles -static -ffreestanding` and links no libc at all. Keep it that
way, including now that a full uClibc-ng toolchain exists for this target
(see [rtl9601-build-own-toolchain](rtl9601-build-own-toolchain.md)).

Measured across every tool this project ships: fully static, with zero libc
symbols of any kind.

## What the freestanding runtime actually costs

A representative tool's freestanding support code (syscall wrappers, a
minimal printf, a handful of string functions) comes to under 1% of that
tool's own binary size — a few thousand bytes total: about twenty syscall
wrappers, one printf implementation, five string functions. A statically
linked libc would add on the order of 100–200 KB to a tool that is
otherwise a few kilobytes — twenty times larger, to replace a few kilobytes
of code that already works.

## The reasons, strongest first

**1. Freestanding binaries run on the stock OEM firmware unmodified.** They
depend only on the kernel's syscall interface and nothing else, so the same
binary can be dropped onto a stock, unmodified stick and just work,
whatever libc that stock image happens to ship — no reflashing required.
This is actively used: a config-directory override lets a tool win over the
stock image's own copy of a similarly-named binary, specifically because
the replacement is self-contained.

A statically linked libc binary would also be droppable this way, so this
argument alone would only rule out *dynamic* linking — but freestanding
goes further: it makes no assumption about libc behavior at all, which
matters because a modern libc can emit syscalls an old kernel line simply
does not have.

**2. The recovery tools must not share a point of failure.** The
environment tool is how a trial boot slot gets selected; the diagnostics
CLI is how you see anything on the device at all. Freestanding, the
environment tool keeps working even when everything else on the image is
broken. Dynamically linked, one broken shared libc would take out every
tool at once — on a device whose only other fallback is physical access to
the board.

**3. It has no dependency on any vendor toolchain.** These tools build with
an ordinary distro-packaged cross-compiler. Linking any libc at all would
require that libc's own compiled code to be safe for this CPU's actual
instruction set, which historically meant depending on a vendor-supplied
toolchain (see
[rtl9601-vendor-prebuilt-toolchain](rtl9601-vendor-prebuilt-toolchain.md) for that history —
now superseded by this project's own from-source toolchain for the rare
case something really does need libc-scale functionality). Freestanding
avoids that dependency entirely, for every tool that doesn't need it.

**4. The instruction-set audit covers everything.** This core traps on
several instructions a generic compiler target would otherwise emit (see
[rtl9601-isa-map](rtl9601-isa-map.md)). Freestanding, every instruction in
the binary comes from this project's own build, so an instruction-set audit
is complete rather than having to also trust an inherited libc binary.

## What this gives up, honestly

**A libc-grade, battle-tested printf.** This project's own minimal printf
had a real bug in the field for months: a fixed-size internal buffer sized
for numeric formatting silently truncated any string argument longer than
about 70 characters. It surfaced only when a new tool happened to print a
long bootloader variable next to the stock firmware's own output. A libc
printf is exercised by a huge number of users; a hand-written one is
exercised only by this project.

That argues for better tests of the hand-written formatter, not for 100+ KB
of libc — the formatter's self-test suite grew directly out of finding this
bug, and a property-based test over random format/argument pairs is the
obvious next step, at zero runtime cost.

Also intentionally absent: a general-purpose allocator, a generic sort
routine, command-line option parsing, and string-to-number parsing beyond
what's hand-written. None of these are needed by anything shipped today.

## The exception worth naming

A component that genuinely needs libc-scale functionality — TLS, DNS
resolution, locale handling, real threads — should link a libc as *that one
binary*, not by changing every other tool. A hypothetical HTTPS-serving
web/config daemon is the realistic case where this could apply, and it is
worth noting the tension: that is also the binary whose ability to run on
an unmodified stock image matters most, so the better answer there is
likely a second, separately-built variant rather than changing this
decision project-wide.

The multicast daemon was the one open question this note originally left
unresolved, and it has since been built freestanding with no issues — its
work (sockets and timers) never actually needed libc-scale functionality.

## See also

- [rtl9601-vendor-prebuilt-toolchain](rtl9601-vendor-prebuilt-toolchain.md) — why anything that *does* need a full libc historically needed a vendor toolchain
- [rtl9601-isa-map](rtl9601-isa-map.md)
- [mips-o32-syscall-abi-traps](mips-o32-syscall-abi-traps.md) — what the syscall wrappers had to get right
