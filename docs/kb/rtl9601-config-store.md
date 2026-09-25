# Config lives in an ordinary jffs2 file, and the stock `flash` wrapper has silent no-ops

Persistent configuration is ordinary files on a small read-write
filesystem, not an opaque flash blob — but the stock `flash` command-line
wrapper around it fails silently in several distinct ways.

*Last verified: 2026-09-15*

---

## What

Persistent config is **ordinary files on a read-write filesystem**, not an
opaque flash blob. The config partition is mounted read-write (jffs2,
around 240 KB, with roughly 200 KB typically free). Firmware updates write
only the kernel and rootfs partitions, so everything here survives a
reflash. Restoring a config is a plain file copy — no special erase step
needed.

Two files matter: one holds service configuration (CS) and the other holds
hardware identity (HS: serial number, MAC-derived key material, vendor id).
Resetting service configuration to defaults leaves identity untouched — with
one important exception, covered in
[the LOID note](rtl9601-loid-old-value-wins.md). Resetting hardware identity to
defaults would wipe identity, and should essentially never be done.

The `flash` command is a shell wrapper around a lower-level config tool, and
it fails silently in four distinct ways — three that do nothing, and one
that writes the wrong key:

1. **Whether an unqualified call to `flash` works at all depends entirely on
   which shell you're in.** Boot-time processes have a PATH that includes
   the script's own directory, so a boot script calling `flash` unqualified
   works. An interactive login or remote-command session generally does
   not have that directory on its PATH by default, so the *same* command
   typed manually fails with a "not found" error — and running a single
   remote command (rather than an interactive login) is worse still,
   because it never picks up the login shell's environment setup at all.
   This has two practical consequences: re-running a boot script by hand
   needs that directory added to PATH first (skipping this makes some
   OMCI-provisioning scripts silently apply empty values and can take the
   unit's GPON MAC configuration offline); and at least one stock boot
   script has an unrelated PATH-handling typo that happens to be harmless
   only because of what follows it on the same line — worth checking
   before appending anything to it.
2. **Clearing a key by setting it to an empty string does not work.** The
   underlying tool refuses to write an empty value and falls through to a
   usage message with a non-zero exit code — but it is easy to miss that a
   "successful-looking" invocation actually failed. To truly clear a key,
   you have to reproduce what the wrapper does directly against the
   underlying tool, bypassing that empty-value guard.
3. **A configuration write is not re-read until the next boot.** The
   process that consumes these settings builds its entire startup command
   line from them once, at boot, so a *running* process can disagree with
   stored configuration indefinitely afterward. Re-running the relevant
   startup script applies a change without a full reboot; see
   [reapplying OMCI config without a reboot](rtl9601-omci-reapply-without-reboot.md).
4. **A table-row key written in bracket notation silently writes a
   *different* key and reports success.** Table rows are addressed with a
   dotted `TABLE.<index>.<field>` syntax. A bracketed form like
   `TABLE[1].field` is not a recognised synonym and is not rejected either —
   it resolves to some unrelated entry, writes that instead, and exits
   successfully while reporting the key it actually hit. This is the only
   trap here that corrupts rather than does nothing, and a naive
   verify-after-write check does **not** catch it, because reading back the
   *wrong* key still "succeeds" — only comparing against the specific key
   you meant to write catches it.

## Why it matters

Every one of these produces a change that appears to have succeeded. Only a
verify-against-the-intended-key check catches the fourth one; the third
means checking the actual running process (not the stored configuration) is
the authority on what is currently in effect, and reading a stale running
process's own command line to infer current configuration produces a
confident, wrong conclusion.

## See also

- [LOID/password precedence](rtl9601-loid-old-value-wins.md) — which keys
  are service config and which are identity, and why a service-config reset
  can leave an old password in force
- [Config baseline traps](rtl9601-config-baseline-traps.md) — no read-only
  dump of defaults, and why a backup's key count drifts
- [Reapplying OMCI config without a reboot](rtl9601-omci-reapply-without-reboot.md)
- [Manual VLAN](rtl9601-onu-manual-vlan.md) — the service's VLAN tag is a
  handful of keys in this store, not in the OMCI extended-VLAN table, which
  is the least obvious thing the store holds
- [Rootfs symlinks into /var](rtl9601-rootfs-symlinks-into-var.md)
- [Userland overview](rtl9601-userland.md)
- [Trial boot](rtl9601-uboot-trial-boot.md)
- [JFFS2 payload sizing](rtl9601-jffs2-payload-sizing.md)
