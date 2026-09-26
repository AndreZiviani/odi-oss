# With multi-LAN mode enabled, host-port frames arrive on a VLAN subinterface, not the raw interface

A stock kernel configuration option present on this board's driver splits
incoming frames across per-switch-port sub-interfaces; bridging only the
"main" network interface silently drops all of the host's own traffic.

*Last verified: 2026-09-16*

---

## What

With multi-LAN device mode enabled (on by default in the vendor's reference
kernel configuration for this board), the network driver creates one
Ethernet interface per switch port, demultiplexing incoming frames by which
switch port they arrived on: a base interface, a `.2` sub-interface carrying
the host-side switch port's traffic, a `.3` sub-interface for another LAN
port, plus the PON-facing interface. A kernel built with this option
disabled instead has a single interface that sees everything, and a bridge
built from just that one interface is enough.

Bridging only the base interface reads as "nothing arrives": in testing, a
bridge containing only the base interface saw a handful of frames while the
host-port sub-interface — outside the bridge and administratively down —
received every ARP request from the gateway. The host-port sub-interface
also keeps a fixed driver-default MAC address unless one is set explicitly,
and a bridge takes the lowest MAC address among its member interfaces — so
getting this wrong can also produce a bridge with a MAC address nobody
intended.

The fix is to bring the host-port sub-interface up, give it its own
intended MAC address, and add it to the bridge alongside the base
interface.

## Why it matters

Watching counters on the bridge or on the base interface alone reads as "no
traffic is arriving" exactly when the traffic is one interface over. The
alternative — building a kernel with multi-LAN device mode disabled, for a
single combined interface more like a typical router image — has not been
tested against this hardware.

**On a from-source replacement image, this same behaviour is implemented by
our own network interface driver** rather than the vendor's, but the
underlying shape — host-port traffic landing on its own sub-interface,
needing its own bridge membership and MAC address — still holds.

## Evidence

Observed via interface counters (base interface: a handful of received
frames; host-port sub-interface: dozens, matching every gateway ARP
request) and confirmed via bridge membership listing after the fix.
