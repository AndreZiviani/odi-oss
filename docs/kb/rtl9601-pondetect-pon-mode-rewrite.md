# The stock firmware silently rewrites its own PON mode and reboots after ~24s of unlocked light

If a unit sees optical signal but cannot achieve PON sync for about 24
seconds, the stock firmware rewrites its own PON mode setting in flash and
force-reboots, coming back configured as the other PON type — which then
forwards nothing and looks exactly like a dead unit.

*Last verified: 2026-09-13*

---

## What

The stock firmware runs a background PON-mode auto-detect process that
polls PON link state roughly every two seconds. On the twelfth consecutive
sample where optical signal is detected but sync is not achieved (about 24
seconds of sustained "light but no lock"), it decides the configured PON
type is wrong, rewrites the PON mode setting in flash, and force-reboots.
The unit then comes back running the *other* PON type (GPON vs EPON) —
which forwards nothing on the correct network and looks exactly like a
bricked unit, with no log surviving the reboot to explain why (the device
has no persistent system log).

The strike counter resets to zero on "no light at all" and on "signal
locked", so only *sustained* light-without-lock triggers this — a brief
flicker during a fibre reconnection is not enough.

## Details that matter

- **A factory-default, never-serialised unit is exempt.** If the unit's
  serial number is still the factory default placeholder (`RTKG11111111`)
  rather than a real assigned one, this auto-detect is disabled entirely —
  a unit that was never serialised never auto-detects.
- **Disabling the feature does not stop the underlying process.** Even with
  auto-detect turned off, the same background process keeps running: on
  every "signal but no sync" sample it still issues a link-recovery command
  (a `setsockopt` call, option `0x2c3d`). Only the flash-write-and-reboot
  step is skipped.
- The command used to check current PON mode also has a cosmetic bug: on
  one specific log line it prints an uninitialised value before the real
  mode is computed, and the correct value follows one line later.

## Why it matters

This is a silent, self-inflicted reconfiguration with no log that survives
a reboot. Any activity that keeps the fibre lit while the PON MAC cannot
achieve sync — bench testing, a fibre fault, an experiment that
interrupts the OMCI/management stack — can end with the unit rebooted
*and* switched to the wrong PON type. **Check the current PON mode setting
before concluding a unit is dead** (`flash get PON_MODE`, and
`flash set PON_MODE <value>` to put it back).

## See also

- [Config store](rtl9601-config-store.md)
