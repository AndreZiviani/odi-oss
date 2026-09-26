# On the stock firmware, the OLD LOID/password value wins when it differs from the new one

The stock provisioning script sends the OLD LOID/password over OMCI whenever
it differs from the current one — and a factory-config reset can leave the
two halves of a credential pair permanently out of sync.

*Last verified: 2026-09-15*

---

## What

The stock firmware keeps two families of provisioning identity keys, split
across two config files with different reset behaviour — one file survives
a factory config reset, the other does not. Of the LOID-related keys, one
(`LOID_PASSWD_OLD`) lives in the file that survives a reset, while the rest
of the LOID family lives in the file that gets cleared — which is not
obvious from the key names alone.

The stock provisioning script does not send the current `LOID`/`LOID_PASSWD`
values to the OMCI stack directly. Instead it compares each value against
its "OLD" counterpart:

    if current == old:  use current
    else:                use old

Read as a rule: **when the current and OLD values disagree, the OLD one is
what actually goes out over OMCI.**

## Why it matters

Two consequences:

- **Setting a new LOID while its OLD counterpart is empty is silently
  ignored.** The provisioning script only passes a LOID argument at all when
  the two already match; an empty OLD value isn't treated the same as an
  equal one — the argument is omitted entirely rather than passed as empty.
- **A factory config reset clears the current LOID password but cannot
  touch its OLD counterpart**, because they live in different files with
  different reset behaviour. After such a reset the two disagree, so the
  stick keeps authenticating with the pre-reset password. This means
  "identity survives a factory reset" is true in a worse way than it
  sounds: the serial number survives, and so does half of the LOID
  credential — leaving the two halves inconsistent rather than either
  cleanly surviving or cleanly resetting.

Provisioning values read back from the config store are XML-escaped, so a
tool that hands back the raw attribute text gets exactly the password fields
wrong, silently, unless it unescapes them first.

## See also

- [Config is jffs2 files, not what the `flash` command implies](rtl9601-config-store.md)
- [Reapplying OMCI config without a reboot](rtl9601-omci-reapply-without-reboot.md)
