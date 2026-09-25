# Only the switch MIB counters show whether the ONU is actually forwarding

An ONU can sit fully registered with every alarm clear and still forward
nothing — the Linux network interface counters never see the forwarded
traffic at all, because forwarding happens entirely in switch hardware.

*Last verified: 2026-09-21*

---

## What

The host-side Linux network interface counters cannot tell you whether the
stick is passing subscriber traffic. Forwarding happens in **switch
hardware and never reaches the CPU** — the PON network interface reads zero
on every field, and the host/bridge interfaces only count the stick's *own*
management traffic. In one measured window, the host interface moved a few
kilobytes while the switch ports each moved tens of megabytes.

The counters that actually answer the question are the switch/PON MAC MIB
counters, read through the stock CLI:

    diag mib dump counter port all

**Port 2 is the PON side, port 0 is the host side**, and forwarding shows up
as a **mirror** between them — whatever port 2 receives, port 0 transmits,
and vice versa. Nothing moving on port 2 while port 0 transmits means the
unit is registered on the PON but the data path itself is dead.

**There is a third port that the default view hides: port 3 is the CPU
port** — the stick's own IP stack, not a physical interface. It has to be
asked for explicitly by number; the default "all ports" view resolves to a
fixed port class read from a small on-device structure, and the CPU port
isn't part of that default class even though reading it directly works
fine. It's the only place that shows the management plane's own traffic,
which the host-side interface counters otherwise undercount. See
[the port-class note](rtl9601-diag-port-all-semantics.md) for the mechanism.

These counters are free-running (not read-and-clear), wider than 32 bits,
and non-destructive to read — resetting them requires an explicit separate
command. **One exception: an OMCI performance-monitoring cycle clears the
host-side port's counters** on a fixed interval, because there's an active
G.988 performance-monitoring instance watching that port and none watching
the PON-side port — worth knowing if you're treating these as a
monotonically-increasing counter for rate calculation, since a
rate-over-time calculation handles a reset correctly but the absolute value
will still dip.

## Why it matters

**A fully registered ONU with every alarm clear tells you nothing about
whether traffic actually flows.** An observed real state on this hardware:
ONU fully registered, every alarm clear, every provisioning operation
returning success, and nothing forwarding in either direction — with no
standard interface counter showing anything wrong. Registration and
forwarding are separate questions, and only the switch MIB counters tell
them apart.

Also: avoid a different counter family exposed by the GPON-specific
counter command — those registers *are* read-and-clear, so reading them is
destructive and steals counts from anything else reading them, including
the stock web UI. They are also not monotonic, which makes them the wrong
shape for a simple rate-based metric.

A dead next-hop gateway that never answers a ping is not necessarily a
forwarding problem — some gateways simply don't answer ICMP as a matter of
policy. Test end-to-end reachability against a well-known public address
instead of assuming a silent gateway means a broken data path.

## Evidence

Port identity was established by correlating traffic deltas over a
measurement window on two independently operated PON lines, rather than
inferred from port numbering alone — both agreed on which port mirrored
which. The device also names its own ports through a safe capability-query
command (see
[device capabilities](rtl9601-omci-device-capabilities.md)), which agrees
with the correlation-based mapping despite the two methods sharing no
common input.

Non-destructiveness and counter width were both confirmed by direct
measurement: a counter read twice a few seconds apart only grew, and one
observed counter value was well past what a 32-bit counter can hold.

## See also

- [Fork the CLI rather than link the vendor library](rtl9601-cli-over-vendor-lib.md)
- [Config is jffs2 files, not what the `flash` command implies](rtl9601-config-store.md)
- [The `all` port keyword is a device port class](rtl9601-diag-port-all-semantics.md)
- [Device capabilities via a safe `get`](rtl9601-omci-device-capabilities.md)
