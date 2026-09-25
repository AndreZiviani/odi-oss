# GEM flows and T-CONTs alone forward nothing: the bridge-connection rule carries it

Programming GEM flows and T-CONTs gets a service as far as a pipe with no forwarding rule; a separate bridge-connection descriptor (documented here) carries the actual rule and cannot be skipped by a replacement implementation.

*Last verified: 2026-09-21*

---

## What

Programming the GEM flows and the T-CONTs gets a service as far as a pipe with no rule. The forwarding rule is carried by a separate, fixed-size (160-byte) descriptor sent through its own OMCI southbound command: it carries the ingress UNI, the egress GEM port, the rule mode, and the extended-VLAN filter and treatment. Nothing in the much smaller GEM-flow descriptor can express VLAN tagging.

Measured, not inferred. On a provisioned stick — six services, two UNIs, a known C-TAG VID — the bridge connections were torn down one at a time while the stock OMCI daemon kept running, and the switch counters sampled over 30-second windows:

| window | port 2 out (upstream) | port 0 out (downstream) |
|---|---|---|
| connections up | +2,728 | +31,967 |
| **torn down** | **+0** | +5,150 |
| restored | +3,184 | +16,496 |

Upstream stopped dead. Throughout, a service-flow dump still listed all six services with their flow ids intact: **the flows were never removed, the stick just had nothing telling it what to do with them.** The downstream residue is the CPU port's own management traffic.

## What the 160 bytes are

The bridge-connection rule descriptor's fields, as observed on this device family:

    +0   in_use          +20  us_dp_flow      +32  dir (GEM port direction)
    +4   latch         +24  us_dp_mark     +36  vlan_op, 124 bytes, to +160
    +8   service_id          +28  ds_flow
    +12  uni_mask
    +16  us_flow

How it is built: a **VEIP** ingress maps to the PON port, an **IP-host** ingress to the CPU port, and anything else through the UNI-to-switch-port table (see the [device capabilities note](rtl9601-omci-device-capabilities.md), which is where both port numbers live too). `uni_mask` is `1 << uniPort`, or every UNI plus the PON port when the ingress is negative. The flow ids come from a port-to-flow lookup, and **"not found" is reported as the total GEM port count**, not as an error.

Two details that are easy to get wrong and expensive to find:

- **What is sent is the value already stored for that service id in the device's own service table, not a freshly built local copy.** The locally constructed structure is only the input to that store, not the payload that goes out.
- **The stock firmware sleeps a millisecond after every send of this command.** Reproduce that before assuming it is decorative.

## Why it matters

A data path that programs cleanly — every driver call returning 0, the ONU at O5, the service flow table populated — can still forward nothing. That is the same trap as in [forwarding visibility](rtl9601-forwarding-visibility.md), one level down: there, O5 and clear alarms were not evidence of forwarding; here, successful driver calls are not either. Only the switch counters answer.

For anyone replacing the OMCI daemon, it also means this descriptor cannot be skipped, whatever else works.

Three things make that work tractable:

- **The command that tears down a bridge connection takes the connection index.** Internally this indexes an array of 160-byte records directly, so one wrong connection can be removed without disturbing the others.
- **Recovery is a restart of the OMCI daemon**, which replays its saved MIB and reinstalls the connections. No reboot, and it is why a stick forwards after a restart even when its OLT re-provisions nothing — the data path comes from the device's own persisted MIB, not from the line.
- **A CLI command exists that prints the finished descriptor** field by field, which is a reference answer to check a from-scratch constructor against.

## A from-scratch descriptor does forward (2026-09-13)

The measurement above establishes that the stock firmware's connections are what forward. The converse is now measured too: a 160-byte descriptor built entirely by third-party code, sent while the stock OMCI daemon was stopped, carried the service.

The experiment that shows it needs a **torn-down control window in the middle**, because stopping the stock daemon does *not* clear the driver's connections — a replacement stack that appears to forward may simply be riding the stock firmware's own rules. Five windows:

| window | arrived on the host port | forwarded out the PON port |
|---|---|---|
| stock connections up | -- | forwarding |
| ours, stock connections still installed | -- | forwarding (proves nothing) |
| **both torn down** | 553,468 | **0** |
| **ours** | 872,197 | **871,109** |

Half a megabyte arrived and was dropped with no connection; with ours, everything that arrived left. Use the CPU port as a liveness control — it keeps moving throughout, so a dead window is a dead connection rather than a dead stick.

**Forwarding is not the same as the service working**: see [manual VLAN tagging](rtl9601-onu-manual-vlan.md), where the same connection forwarded frames the OLT then discarded because the tag came from the config store rather than from the OMCI extended-VLAN table.

## See also

- [Forwarding visibility](rtl9601-forwarding-visibility.md)
- [OMCI driver interface](rtl9601-omci-driver-interface.md)

## Twelve service connections, reproduced field for field (added 2026-09-21)

A from-scratch implementation of the OMCI daemon can derive the connections from the MIB after a quiet second: one per (ingress bridge port with the physical-UNI or VEIP TP type, GEM), which for a six-GEM, two-ingress service set works out to twelve connections against one OLT — matching the stock count. Rule selection per GEM: the extended-VLAN table entry whose VID equals the config-store manual tag VID gets the extended-VLAN add-tag rule; other VIDs get a single-tag VID filter, carrying the mapper's priority bit when an 802.1p mapper names that GEM interworking termination point exactly once (see [out-style carries the p-bit](rtl9601-vlan-rule-out-style-carries-the-pbit.md)); the multicast GEM gets the downstream-only multicast rule, installed last. Forwarding and multi-hundred-Mbit/s throughput measured through it across many boots.
