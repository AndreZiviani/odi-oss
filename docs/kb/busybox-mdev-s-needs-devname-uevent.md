# A modern busybox `mdev -s` names every device by major:minor on a pre-2.6.32 kernel

Pairing a modern busybox with an older (pre-2.6.32) kernel line silently
breaks device-node naming: `/dev` fills with numeric names like `4:64`
instead of `ttyS0`, `null`, or `urandom`.

*Last verified: 2026-09-14*

---

## What

busybox's `mdev -s` populates `/dev` by walking `/sys/dev`. Every entry
there is a symlink named `MAJOR:MINOR`, not by device name; a modern `mdev`
recovers the real device name from a `DEVNAME=` field in that device's
kernel event data, and falls back to the raw `MAJOR:MINOR` string when that
field is absent.

**Kernels before Linux 2.6.32 do not emit `DEVNAME`.** So on such a kernel,
`mdev -s` creates `/dev/4:64`, `/dev/5:1`, `/dev/1:3` — and no `console`, no
`null`, no `ttyS0`, no `urandom`. Every node has the right major and minor
number, wrapped in an unusable name.

An older busybox build does not have this problem: it walked a different
part of `sysfs` where the directory name already *is* the device name. A
newer busybox build drops that older code path entirely.

## Why it matters

This is silent, and it only shows up when a **newer** busybox is paired with
an **older** kernel — exactly what happens when you modernise an embedded
root filesystem's userland package versions without also changing the
kernel line underneath it. The usual pattern is a boot script that mounts a
tmpfs over `/dev` (hiding whatever static nodes shipped with the image) and
then runs `mdev -s` to repopulate it. Before the userland upgrade this
worked fine; after it, `/dev` fills with junk.

The consequences are all indirect and easy to misattribute:

- `init` can't open the console device named in its configuration and
  respawn-loops trying.
- anything reading from the hardware random number source fails — an SSH
  daemon generating its first host key, for instance, often with its error
  output discarded.
- redirecting output to the null device fails everywhere it's used in the
  boot path.

Hotplug (devices appearing after boot) is **not** affected — only the
initial `-s` sweep at boot is broken, which is why this can pass a casual
test on an already-running system.

## How to tell

List `/dev` right after an `mdev -s` sweep. If it's full of `MAJOR:MINOR`
style names instead of real device names, this is it.

## What to do

Prefer static device nodes. An embedded image that doesn't need hotplug
support at all needs no `mdev -s` sweep in the first place: ship the nodes
in the filesystem image and don't mount anything over `/dev`. That also
covers nodes that have no corresponding entry to walk in the first place,
such as the hardware random number device.

Static rename rules that map a fixed major:minor to a name still work, since
that mechanism doesn't depend on `DEVNAME` — but they hardcode major/minor
numbers and are a worse fit than just shipping real nodes.

## See also

- [The stock userland's networking gaps](rtl9601-userland.md)
