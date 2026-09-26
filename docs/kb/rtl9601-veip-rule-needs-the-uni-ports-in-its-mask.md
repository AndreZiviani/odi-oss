# A VEIP-only bridge connection needs the UNI ports in its mask too

*This finding was made against an earlier, now-superseded intermediate driver generation — not the original OEM stock firmware, and not verified against a current from-source replacement driver. It is documented here as a gotcha worth checking for anyone implementing a similar OMCI VEIP connection.*

A VEIP-only bridge connection whose port mask holds only the PON-port bit was observed to forward nothing on that intermediate driver, even though a reference dump from the stock firmware showed the same mask value; including the Ethernet UNI ports in the mask fixed it.

*Last verified: 2026-09-21*

---

## What

The VEIP managed entity resolves to the PON port in the device's capability table, so a bridge-connection rule built purely from a VEIP ingress gets a port mask containing only the PON-port bit. On one device, the same rule ended up with an additional UNI-ingress bit merged in, because a separate physical-UNI rule shared the same connection. On a second device whose bridge had only the VEIP (no physical-UNI rule to merge with), the mask held only the PON-port bit; the ONU reached full registration (O5) with both connections installed, but PON port counters stayed at zero in both directions and PPPoE on the far end timed out waiting for its initial discovery response. Adding the Ethernet UNI ports to the VEIP-ingress mask made PPPoE come up within seconds of registration, carrying several hundred Mbit/s in each direction.

## Why it matters

A reference dump from the stock firmware on the second device read the narrower, PON-port-only mask and forwarded fine under the stock firmware's own driver — so copying that mask value byte-for-byte gave a dead data path under this intermediate driver generation. Either that driver generation reads the VEIP mask bit differently from the original stock (2.6-kernel-era) driver, or the stock firmware's userland adds the UNI ports through some other path not captured by the mask alone. The practical rule that held on this intermediate driver: a VEIP conceptually stands in front of the UNIs it serves, so its bridge-connection rule should carry their mask bits too, not just the PON-port bit. Both devices forwarded correctly once built that way.

## Evidence

- One boot with the mask containing only the PON-port bit: PON port octet counters at zero in and out over a 10-second window, and the far-end PPPoE client timing out waiting for its discovery offer.
- A later boot with the UNI ports added to the mask: PPPoE came up, round-trip pings succeeded, and tens of megabytes crossed the PON port in each direction during a short throughput test.
- An earlier, narrower test on the same device (with only a UNI-ingress-derived mask, no VEIP bit) had also forwarded, which is consistent with the UNI ports being the part of the mask that actually mattered.

## See also

- [OMCI bridge connection](rtl9601-omci-bridge-connection.md)
- [OMCI device capabilities](rtl9601-omci-device-capabilities.md)
