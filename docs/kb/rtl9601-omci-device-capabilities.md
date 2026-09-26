# The device's capability tables come from one read-only driver query

GEM/T-CONT/queue counts, port numbers, and the UNI-to-switch-port table are all returned by a single safe (read-only) capability query rather than needing to be hardcoded or guessed.

*Last verified: 2026-09-13*

---

## What

At startup, the stock OMCI daemon issues one read-only driver command that returns a single fixed-size (120-byte) capability block. Every device-capability field the daemon later relies on is a field in that one block, and because the command is a `get`, it can be issued alongside a running daemon without disturbing it.

The block accounts for all 120 bytes. On one observed ODI DFP-34X-2C2 (with the OMCI driver generation shipped in this device's stock firmware):

| offset | field | observed value |
|---|---|---|
| 0..63 | per-port type/index table, 32 entries | one live slot |
| +64 | fast-Ethernet port count | 0 |
| +68 | Gigabit-Ethernet port count | 1 |
| +72 | **CPU port number** | **3** |
| +76 | **PON port number** | **2** |
| +80 | RGMII port number | -1 |
| +84 | POTS port count | 0 |
| +88 | **T-CONT count** | 16 |
| +92 | GEM port count | 64 |
| +96 | **priority queue count** | 128 |
| +100 | queues per UNI | 8 |
| +104 | per-T-CONT / per-UNI queue depth (2 bytes, 2 pad) | 0, 0 |
| +108 | meter count | 16 |
| +112 | reserved meter id | 15 |
| +116 | L2 table size | 1088 |

**Important: these T-CONT, GEM, and queue counts are what this particular vendor OMCI driver generation reports, not a fixed hardware constant of the chip family.** Different driver generations on the same chip family have been observed to report different T-CONT and queue counts (see the [superseded-driver notes](rtl9602c-omci-active-queues-are-gem-referenced-only.md) for an example on a related chip variant). Any code path that needs these numbers should query the capability block at runtime rather than hardcoding the values in this table.

**The CPU port and PON port numbers are in this same block**, which independently confirms the switch port map used elsewhere (see [forwarding visibility](rtl9601-forwarding-visibility.md)) — a map that could otherwise only be established by correlating switch counters. The device reports its own port numbers for the price of one read.

**The switch port a UNI entity id maps to is the position of its slot in the per-port table**, not a value stored in that slot. The lookup searches for a slot whose second byte matches the low byte of `(managed-entity id − 1)` and whose first byte matches an expected slot type — and the type pairing for the two UNI entity kinds (physical Ethernet UNI vs. VEIP) is not the pairing a naive reading of the values suggests. The lookup returns success whether or not it found anything, leaving the caller's output variable untouched on a miss.

On an ODI DFP-34X-2C2 the table holds one live slot (an Ethernet UNI entity mapping to switch port 0), and everything else marked empty.

## Why it matters

Three things that would otherwise need a guess or a hardware probe are one read: how many GEM flows exist to allocate from, how many priority queues, and which switch port a UNI is. A replacement stack should ask the driver for these instead of carrying a hardcoded table that is wrong on the next firmware or driver revision.

It also settles a port assignment that had been guessed two different ways from indirect signals (an entity-id arithmetic guess, and a frame-size default that turned out to belong to the PON port, not something the OLT set through a UNI). Three independent signals agree: this capability table, the port map from switch counters, and the vendor's own performance-monitoring history, which all point at the same switch port for the observed UNI.

## Evidence

Capability block read at daemon start; also independently re-read live beside a running stock daemon without disturbing it. A from-scratch responder using the reported table wrote the UNI admin state to the reported port and the driver returned success.

## See also

- [OMCI driver interface](rtl9601-omci-driver-interface.md)
- [Forwarding visibility](rtl9601-forwarding-visibility.md)
