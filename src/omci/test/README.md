# Fixture config stores

Two pairs of `lastgood.xml` / `lastgood_hs.xml`, in xmlconfig's format, for the
`ident` tests in `qemu-test.sh`.

**Every value here is invented.** Real serials, PLOAM passwords and LOIDs are
kept in a separate private repository, not in this one, which is public.

- `cfg_agree*` -- `LOID` and `LOID_OLD` agree, so the current value is used.
- `cfg_differ*` -- `LOID_OLD` is empty and `LOID` is not, which is the case
  /etc/runomci.sh resolves in favour of the OLD one. The result is that nothing
  is sent at all.

Note where each key sits: `LOID`, `LOID_OLD` and `LOID_PASSWD` are in the cs
file, `GPON_SN`, `GPON_PLOAM_PASSWD` and `LOID_PASSWD_OLD` in the hs one. That
is xmlconfig's own split, recovered from its descriptor table.

- `cfg_esc*` -- every XML entity xmlconfig escapes, in one value:
  `a&b<c>d"e'f`. The reader must hand back the decoded string, not the raw
  attribute. This is the case the first version of `cfgstore.c` got wrong, and
  it is not academic: a PLOAM or LOID password containing `&` would have
  authenticated with the wrong credential, silently.

Added 2026-09-24, also invented:

- `cfg_csonly*` -- `GPON_PLOAM_PASSWD` and `LOID_PASSWD_OLD` in the cs file,
  the layout ISP1 actually has. omcid reads each key from whichever file
  carries it.
- `cfg_vlan_type0.xml`, `cfg_vlan_nopri.xml` -- the manual VLAN gated off by
  `VLAN_CFG_TYPE` 0, and by an empty priority.
- `cfg_report*` -- the five OLT identity keys set to test values, and
  `cfg_report_empty*` with all five present and empty.
