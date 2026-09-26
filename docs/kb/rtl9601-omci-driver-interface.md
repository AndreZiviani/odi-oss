# The OMCI southbound is a single socket option with a dense command space

The stock firmware's OMCI application reaches the switch/PON hardware through exactly one socket option, with commands numbered densely from roughly 1 through 73; this note documents that transport and the behavioral traps in it.

*Last verified: 2026-09-21*

---

## Scope

This note describes the OEM stock firmware's OMCI southbound transport — the mechanism the vendor-supplied OMCI application uses to reach the switch and PON hardware. A from-source replacement image does not need to reuse this exact transport: it can implement a different southbound underneath its own OMCI stack, as long as it exposes equivalent command semantics and payload shapes where those are dictated by the hardware itself (T-CONT/GEM/queue programming, bridge rules, and so on).

## What

The hardware is reached through exactly one socket option, taking a small fixed-size request/response buffer (a command id, a length, and up to 256 bytes of payload). The command space is dense, roughly **1 through 73**, and each number has a single, consistent meaning across the firmware. Known meanings include (numbering approximate and firmware-generation dependent): a command that queries ONU state, one that activates GPON, one that creates a T-CONT, one that configures a GEM flow, one that reads per-port switch statistics, one that installs (activates) a bridge connection, and one that sets port bridging mode.

Two payload shapes are used: about 40 commands pass the caller's buffer straight through unmodified, and about 25 pack their own argument structure. The per-port getters and setters share a common two-word shape: the port number echoed back, and the value in the second word.

**There is a simulation mode.** An internal flag can be set to skip the actual hardware call entirely and return the request buffer untouched — useful for testing call sequences without touching the switch.

## Why it matters

A replacement OMCI application needs no vendor library: the whole southbound is this one socket option plus a netlink-delivered packet path for OMCI frames arriving from the OLT. This has been confirmed end to end rather than assumed — a per-port statistics read through this interface matches the equivalent CLI diagnostic counters, counter for counter, via two independent paths into the same driver.

## Three traps worth knowing before programming a data path

**A T-CONT has two names, and the driver wants the internal one, not the OMCI-visible one.** The T-CONT-creation command takes only an allocation id as input and returns the driver's own T-CONT **index** as output. The OLT names a T-CONT by its OMCI managed-entity id. A GEM flow's T-CONT pointer must carry the driver **index** returned by creation, not the managed-entity id — sending the managed-entity id there fails the upstream leg while the downstream leg (which names no T-CONT) succeeds, silently.

**A GEM flow's flow-id field is the caller's to choose, and it is not defaulted.** The flow descriptor's flow-id field indexes the driver's own flow table directly. The stock firmware picks a value deliberately: it reuses whatever id the port already holds in that direction, or takes the lowest free slot, keeping one table per direction. Leaving that field uninitialized makes every flow collide on flow 0, each overwriting the last — the second flow programmed in a given direction is silently refused. A separate field in the same command distinguishes "allocate" from "update": allocating creates a new entry, updating modifies an existing port's flow and fails if there is none.

**The reserved broadcast/multicast GEM port must be reached through its own ordinary downstream flow first — it is not auto-configured.** The broadcast-flow command takes a **flow id**, not a port id, and that flow id has to already exist as a regular downstream flow before the broadcast command can point at it. A lookup that reports "not found" for this does so as the flow-table size, not as an error code, which is easy to mistake for a valid index.

## The queue rate/scheduling command

One command programs a priority-queue descriptor with a queue id, a T-CONT id, committed and peak rates, a weight, a scheduling type (strict priority vs. weighted round robin), a marking value, and a direction. Rates in this descriptor are expressed in units of 8 Kbit/s (the driver divides a byte-rate traffic descriptor by 1024 on the way in); they read as zero on the downstream path, which can look like a reserved field if you are not expecting the unit conversion.

This driver generation has no separate rate-only command: to change a GEM port's rate, the queue is simply re-sent with new rate fields. The queue id in this command is not the queue's own permanent number — it is a rank assigned by sorting a T-CONT's queues by priority (round-robin queues first, then strict-priority queues by descending priority), so adding a queue can renumber its siblings.

## Evidence

Behavior confirmed by comparing command inputs and outputs against the equivalent CLI diagnostics on the stock firmware, and by exercising the same command sequence — identification through full provisioning, with a T-CONT and both directions of a service GEM flow — from an independent, from-scratch implementation on real hardware.

## See also

- [OMCI device capabilities](rtl9601-omci-device-capabilities.md)
- [OMCI bridge connection](rtl9601-omci-bridge-connection.md)
