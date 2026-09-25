# Stock busybox lacks common applets, and a missing one turns a pipeline silently empty

The stock busybox build lacks `head`, `tail`, `tr`, `wc`, `nohup`, `setsid`
and several others — and the failure mode is not an error, it's a silently
empty pipeline that reads as "no data."

*Last verified: 2026-09-16*

---

## What

The stock busybox build lacks **`head`, `tail`, `tr`, `wc`, `pkill`, `seq`,
`basename`, `nohup`, `setsid`** — and `touch`, `sync`, `which` and `dd` are
not there either. The way it bites is worse than the plain absence: the
shell's "not found" message goes to **stderr**, the pipeline yields
**empty** output on stdout, and empty output reads as "no data" if you're
not watching stderr too.

**Never pipe through one of these applets on the device without checking
first; if a pipeline returns nothing, check the tool actually exists before
believing the result.**

Nothing needs installing to work around it — `awk`, `sed`, `grep`, `cut`,
`cmp`, `diff` and `expr` are all present:

| want | use instead |
|---|---|
| `head -N` | `sed -n '1,Np'` |
| `tail -1` | `sed -n '$p'` |
| `wc -l` | `awk 'END{print NR}'` |
| `wc -c` | `awk '{n+=length+1} END{print n}'` |
| `tr a b` | `sed 'y/a/b/'` |
| `touch file` | `: > file` |

With no `setsid` or `nohup` available, a daemon started over an SSH or
telnet session dies when the session ends. Ignoring the hangup signal in the
shell before backgrounding the process fixes it — an **ignored** signal
disposition survives replacing the shell process image, while a handler
function does not.

## Why it matters

This produced a confident wrong conclusion in testing: piping a search
through the missing `head` returned nothing and was read as "the value isn't
set," when it actually was — the pipeline had silently gone empty because
the applet was missing, not because the grep matched nothing.

The hangup case has the same shape: backgrounding a process the naive way
works once (while a session happens to stay open) and kills the daemon the
next time, which looks exactly like a bad binary rather than a signal
handling gap.

**Replacing busybox just to get the missing applets back is the wrong
trade.** It is `/bin/sh`, so every boot script runs through it — and on a
CPU that traps on some instructions a stock compiler will happily emit (see
[the ISA note](rtl9601-isa-map.md)), a miscompiled busybox is not a broken
tool, it's a stick that will not boot. Auditing a full busybox build is a
much bigger undertaking than auditing a small, purpose-built freestanding
tool. Put any extra diagnostics you need into something optional instead of
swapping the shell.

## See also

- [The stock userland's networking gaps](rtl9601-userland.md)
- [The RLX5281 ISA, measured by execution](rtl9601-isa-map.md)
- [A file missing from the image may be a symlink into RAM](rtl9601-rootfs-symlinks-into-var.md)
