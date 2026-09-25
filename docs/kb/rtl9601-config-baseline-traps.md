# There is no read-only dump of factory defaults, and a backup's key count drifts harmlessly

The stock config tool's "dump defaults" option actually prints the current
configuration, not the factory defaults; and comparing key counts against
an older backup is not a reliable way to detect data loss.

*Last verified: 2026-09-15*

---

## What

**There is no read-only way to dump the built-in configuration defaults.**
The stock config tool's option that looks like it should print factory
defaults instead prints the **current configuration**. A small built-in
default file on the device is the only authoritative default set, and it
covers only a handful of keys; for everything else the only usable
reference is a baseline captured from a unit you already trust, which
answers "what has changed" rather than "what did it ship as".

The main config file is a delta computed against that small default file,
recomputed on every write — so its key count naturally drifts from an
older backup with nothing actually lost.

## Why it matters

A "compare against defaults" feature built on the "dump defaults" option
ends up labelling every already-provisioned value as a factory default. And
**do not treat a key-count difference against a backup as data loss on its
own** — diff the specific values you care about instead.

## Evidence

The "dump defaults" behaviour was caught only because a "compare against
defaults" feature reported zero differences on a unit that was carrying
line-specific identity and provisioning values it clearly should not have
matched.

One config write on one unit reduced its key count noticeably, converging
on exactly the same smaller key set a second, otherwise-identical unit
already had — verified key for key identical between the two. The keys
that disappeared read as "not set" on **both** units afterward, which is
their normal state; a second, previously-untouched unit matched its own
earlier backup exactly, which is what established the reduction was
normalisation rather than loss.

## See also

- [Config store](rtl9601-config-store.md)
