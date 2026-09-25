# Nothing feeds the random pool early on this device — dropbear can generate host keys from ~27 bits of entropy

At boot, nothing seeds the kernel's entropy pool ahead of the SSH daemon, so
the very first host-key generation can draw from as little as ~27 bits of
entropy; a seed file on the persistent config partition fixes it, but only
once a seed has actually been saved after the pool initialised.

*Last verified: 2026-09-21*

---

## What

On an early boot, the kernel logged an SSH host-key generation reading
`/dev/urandom` with only 27 bits of entropy available. `seedrng`-style
tooling (crediting a saved seed at boot, then writing a fresh one) fixes
this — but with a catch: run at early boot time the pool is still empty, so
the seed it saves that run is marked "do not credit," and the *next* boot
still starts from nothing. Only a run that happens after the pool has had a
chance to initialise (in testing, about 90 seconds after boot) saves a
creditable seed; the boot after *that* one starts with several times more
usable entropy than the very first boot did.

## Why it matters

Host keys and any session keys generated on first boot are weak without
this. The seeding tool used here needs a monotonic boot-time clock (added to
Linux in 2.6.39), so it only works on a 3.x or newer kernel — running it
against an older kernel line will build fine but fail at runtime.

## Evidence

Observed across five consecutive boots on a 3.x-kernel build: entropy
available climbed from a very low double-digit figure at the first boot
after a fresh image to roughly 1,400+ a minute past boot once a credited
seed was in place, consistent with the "only a post-init run credits"
mechanism above.
