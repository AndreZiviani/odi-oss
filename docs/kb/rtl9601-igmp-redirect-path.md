# IGMP/MLD control frames reach userland over the same redirect mechanism as OMCI, with a one-based port field

Trapped IGMP and MLD control frames don't arrive on a normal network
interface at all — they're delivered to userland over the same
kernel-to-userland redirect mechanism OMCI uses, but on a different
channel, with a source port field that's one-based instead of zero-based.

*Last verified: 2026-09-14 (the stock firmware); the caveat below 2026-09-25*

---

## What

Trapped IGMP and MLD control frames do not arrive on a normal network
interface. They are delivered to userland over the same netlink-style
packet-redirect mechanism used for OMCI traffic, but on a separate channel —
identified by uid 4, versus uid 1 for the OMCI channel. The
underlying delivery primitive is channel-agnostic — it copies the buffer
verbatim — so **every byte of framing is added by the sender on the
kernel-to-userland leg, and the framing differs by channel**: the IGMP
channel (uid 4) prepends 3 bytes where the OMCI channel (uid 1) prepends 4.

    kernel -> userland
      byte 0     source port, ONE-BASED (not zero-based)
      byte 1     VLAN id, high nibble
      byte 2     VLAN id, low byte
      byte 3...  the Ethernet frame

    userland -> kernel, native byte order
      u32  portMask      which ports to transmit on
      u32  sid           stream id, used only upstream on GPON
      u8[] frame

There is a payload size cap of 1600 bytes; the kernel drops anything
longer.

This describes the stock firmware. On this repository's kernel the
redirect transport (`odi_omci.c`) delivers only OMCI frames (RX reason 246,
redirect type 1); nothing delivers IGMP on uid 4, and `igmpd` is shipped
but not started, so IGMP snooping is off on this image.

## Why it matters

Three details each break a reimplementation silently rather than loudly.

**The port field is one-based.** A port mask built naively as `1 <<
port_field` is off by one on every port; the underlying hardware port mask
is `1 << (port_field - 1)`.

**A VLAN-tagged frame can arrive double-tagged.** The frame that reaches
userland can already carry its VLAN tag reinserted into the Ethernet frame
itself, in addition to being given separately in the 3-byte prefix above —
and only when the VLAN id is non-zero. The prefix's own VLAN field should
be treated only as the untagged fallback, not as authoritative when the
frame itself is already tagged.

**The VLAN id in the prefix carries a `+1` encoding.** The 2-byte VLAN
field in the prefix is byte-swapped relative to the raw hardware tag value,
*and* is one greater than it. Everything downstream that reads this field
already accounts for the increment, so it only causes trouble for code
reading the raw hardware tag value directly — it looks exactly like an
off-by-one bug and invites being "fixed" incorrectly.

And one in the transmit direction: **a transmit port mask that names some
but not all Ethernet ports is silently widened by the firmware to every
Ethernet port.** Only an empty mask, or one that already includes the PON
port, passes through unmodified. A mask meant to act as a filter ends up
behaving like a flood.

## See also

- [OMCI over SysV message queue](rtl9601-omci-ipc.md)
- [Only the switch MIB counters show whether the ONU is forwarding](rtl9601-forwarding-visibility.md)
