# Two different stored LAN SerDes mode values produced the same runtime mode

Two sticks stored different values for the config key that reportedly
controls the host-side SerDes mode, and both still came up in the same
runtime mode — this key has a reputation for bricking a stick, so don't
change it speculatively.

*Last verified: 2026-09-15*

---

## What

Two sticks stored different values for the config key that reportedly
selects the host-side SerDes mode — and **both reported the same runtime
mode (Fiber 1G)** regardless. Why the two stored values produced the same
result is not established.

What is established is where the runtime mode actually lives, and it is not
the register this config key was assumed to map onto. The hardware mode is
controlled by a specific bitfield in a different register entirely, set by
a documented multi-step write sequence (see
[the SerDes mode encoding note](rtl9602c-serdes-mode-encoding.md)); the
register the config key was thought to control is never actually touched
by the code path that applies this setting at boot.

## Why it matters

This key carries a reputation as one of the more dangerous ones to touch on
this platform, and the evidence here supports treating it as dangerous, not
as safely inert: two observed values happened to produce the same result on
these two units, which is not the same as proving the setting has no
effect. Leave it alone.

## See also

- [SerDes mode is a register bitfield, not the config key many assume](rtl9602c-serdes-mode-encoding.md)
- [Config is jffs2 files, not what the `flash` command implies](rtl9601-config-store.md)
- [The OLD LOID value wins](rtl9601-loid-old-value-wins.md) — this key is one of a small set of identity/hardware keys stored the same special way
