# A "TPType 3" bridge port can point at either the 802.1p mapper or the GEM interworking termination point, depending on the OLT

G.988 says MAC bridge port termination-point type 3 is an 802.1p mapper service profile, but different OLTs have been observed provisioning that same type code to point at different kinds of managed entities — a responder that only follows one shape reaches registered (O5) status with no unicast traffic passing.

*Last verified: 2026-09-21*

---

## What

G.988 says MAC bridge port termination-point type 3 is an 802.1p mapper service profile. The ISP2 OLT does exactly that: the bridge port's pointer targets the 802.1p mapper entity, whose eight priority-bit slots all name the same GEM interworking termination point. The ISP1 OLT instead points the same termination-point-type-3 pointer directly at the GEM interworking termination point, and hangs the mapper off that entity's own service-profile pointer instead. The stock firmware's daemon handles both shapes; a reimplementation that only follows the ISP2 OLT's shape reaches full registration (O5) on an ISP1-OLT-style connection with the multicast connection built and no unicast service.

## Why it matters

Everything else about the connection looks healthy in that failure mode: registered (O5), MIB in sync, T-CONT and GEM flow programmed, one bridge connection installed. The tell is that the active-connections dump has no unicast row while the bridge-port table shows a termination-point-3 port whose pointer does not resolve to a bridge-connection instance. The rule that covers both shapes: resolve the termination-point pointer against bridge-connection instances first; if none matches, resolve it against 802.1p mapper instances and take the distinct interworking termination points its priority-bit slots name (one connection per distinct termination point — carrying the priority bit when exactly one slot names it, VID-only otherwise); and pair a GEM only with the UNIs/VEIPs sharing the same bridge id.

## Evidence

- On the ISP2 OLT: a termination-point-3 bridge port pointing at an 802.1p mapper instance, whose priority-bit slots all named the same GEM interworking termination point, with only the multicast connection built.
- On a different, ISP1-OLT-facing boot of the same device: both a unicast and a multicast connection built, matching the two service-flow rows the stock firmware itself shows for that provisioning.

## See also

- [OMCI bridge connection](rtl9601-omci-bridge-connection.md)
