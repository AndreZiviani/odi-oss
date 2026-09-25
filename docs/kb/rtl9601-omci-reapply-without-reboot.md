# An OMCI config change applies without a reboot by restarting the daemon

Killing and re-running the stock daemon's startup script reapplies every OMCI-affecting config key in about six seconds, without a reboot — and forwarding and any established PPPoE session survive the gap, because the data path lives in switch hardware.

*Last verified: 2026-09-15*

---

## What

**A config change can often be applied without rebooting.** The stock firmware's OMCI daemon startup script rebuilds every daemon argument from the current config store and ends in a single backgrounded launch. Killing the daemon and re-running that script (after fixing up its PATH assumptions — see the [config store note](rtl9601-config-store.md)) applies the whole set of OMCI-affecting config keys — device type, the manual-VLAN family, the feature-bitmask family, PLOAM settings, the GPON serial number, and the dual-management-mode setting — in about 6 seconds, with a byte-identical command line rebuilt from the current config.

Forwarding even continues *while* the daemon is dead, because the data path is programmed into switch hardware and the control plane's absence does not tear it down.

## Why it matters

Stored config is otherwise read only at boot (see the [config store note](rtl9601-config-store.md)), so this is the way to apply a change without taking the ONU through a full reboot. Do not lean on forwarding surviving for long gaps, though: an OLT will eventually deactivate an ONU that stops answering OMCI for too long.

## Evidence

Measured: the ONU returned to full operational state (O5), port counters kept advancing, and an established PPPoE session **survived untouched** (verified by pushing TCP traffic through it, not merely by checking interface state). Around 2 MB crossed the PON port during a two-minute gap with the daemon dead.

## See also

- [Config store](rtl9601-config-store.md)
- [OMCI app process tree](rtl9601-omci-app-process-tree.md) — how to tell one daemon instance from two after a restart
- [Forwarding visibility](rtl9601-forwarding-visibility.md)
