# A "TPType 3" bridge port can point at either the 802.1p mapper or the GEM interworking termination point, depending on the OLT

G.988 says MAC bridge port termination-point type 3 is an 802.1p mapper service profile, but different OLTs have been observed provisioning that same type code to point at different kinds of managed entities — a responder that only follows one shape reaches registered (O5) status with no unicast traffic passing.

*Last verified: 2026-10-27*

---

## What

G.988 says MAC bridge port termination-point type 3 is an 802.1p mapper service profile. The ISP2 OLT does exactly that: the bridge port's pointer targets the 802.1p mapper entity, whose eight priority-bit slots all name the same GEM interworking termination point. The ISP1 OLT instead points the same termination-point-type-3 pointer directly at the GEM interworking termination point, and hangs the mapper off that entity's own service-profile pointer instead. The stock firmware's daemon handles both shapes; a reimplementation that only follows the ISP2 OLT's shape reaches full registration (O5) on an ISP1-OLT-style connection with the multicast connection built and no unicast service.

The two shapes can also overlap. The ISP3 OLT points the pointer at the 802.1p mapper, as G.988 says, but its mapper sends each priority bit to a GEM interworking termination point of its own (eight GEM ports per service), and the termination point of priority bit 0 has the same instance id as the mapper. Entity instance ids are only unique within a class, so the pointer resolves against both classes. Taking the termination point, the ISP1 reading, builds the connection of the priority-0 GEM port alone, and every frame the OLT sends on the other seven is dropped by the switch.

## Why it matters

Everything else about the connection looks healthy in that failure mode: registered (O5), MIB in sync, T-CONT and GEM flow programmed, one bridge connection installed. The tell is that the active-connections dump has no unicast row while the bridge-port table shows a termination-point-3 port whose pointer does not resolve to a bridge-connection instance. On ISP3 the failure is narrower and harder to see: unicast data the OLT sends at priority 0 works both ways, while what it sends at other priorities never reaches the UNI. There, DHCP offers came down at priority 5 and ARP replies at priority 7, so the line looked like an OLT that ignores DHCP and ARP: no lease, no gateway, while a statically configured host with the gateway's MAC address could reach the internet. The tell is the per-flow downstream counters: the GEM ports of the other priorities count frames received and forwarded to the switch, and no bridge connection names them.

The rule that covers all three shapes: when a mapper of the pointer's instance id exists and its priority-bit slots name any termination point other than the pointer itself, the pointer is the mapper. Otherwise, resolve the termination-point pointer against bridge-connection instances first; if none matches, resolve it against 802.1p mapper instances and take the distinct interworking termination points its priority-bit slots name (one connection per distinct termination point — carrying the priority bit when exactly one slot names it, VID-only otherwise); and pair a GEM only with the UNIs/VEIPs sharing the same bridge id.

## Evidence

- On the ISP2 OLT: a termination-point-3 bridge port pointing at an 802.1p mapper instance, whose priority-bit slots all named the same GEM interworking termination point, with only the multicast connection built.
- On a different, ISP1-OLT-facing boot of the same device: both a unicast and a multicast connection built, matching the two service-flow rows the stock firmware itself shows for that provisioning.

- On the ISP3 OLT (2026-10-27): two termination-point-3 bridge ports, each pointing at an 802.1p mapper whose eight priority-bit slots name eight different GEM interworking termination points, the first of which has the mapper's instance id. With the termination point taken, the bridge connections were built for the priority-0 GEM port of each service only (7 connections); three DHCP discovers drew three frames of 334 bytes on the priority-5 GEM port and ARP requests drew frames on the priority-7 one, all counted as received and forwarded by the GPON block and none delivered to the UNI. With the mapper taken, 49 connections over all sixteen GEM ports, and DHCP and ARP answered on the first try; DHCPv6 and router solicitations were answered too.

## See also

- [OMCI bridge connection](rtl9601-omci-bridge-connection.md)
