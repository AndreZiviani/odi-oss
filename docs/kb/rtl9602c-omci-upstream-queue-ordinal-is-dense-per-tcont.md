# A GEM flow's upstream queue field is a dense per-T-CONT ordinal, not a queue's own id

*This note describes behavior of an intermediate, now-superseded vendor-based driver generation for this chip variant.*

On this intermediate driver, a GEM flow's upstream queue field must be a dense ordinal within its T-CONT (0, 1, 2, ...), not the OMCI managed-entity id of the queue; using the managed-entity id there produced an invalid scheduler/queue assignment and an ONU that registered fully but passed no upstream traffic.

*Last verified: 2026-09-21*

---

## What

On this intermediate driver generation, one command reserves a given number of queues on a T-CONT the GPON application already holds, and returns the driver's own index for that reservation. A separate command then programs those queues by **ordinal** (0 through n-1) within that same T-CONT — and a GEM flow's upstream queue field must name that ordinal, with the kernel internally adding the T-CONT's own dynamically assigned queue-id base on top. This driver orders queues by scheduling type first (round-robin queues, then strict-priority queues by descending OMCI priority), breaking ties by ascending managed-entity id.

## Why it matters

Every earlier hypothesis chased during this investigation (a driver descriptor mismatch, a classifier mask issue, a security/encryption-related theory) turned out to be a real but unrelated defect, not the cause of the actual outage — the queue-ordinal mapping was. With a GEM flow's queue field left as a raw managed-entity id instead of the correct ordinal, every flow landed on the same invalid scheduler/queue assignment; the ONU still reached full registration, fully provisioned, but passed nothing upstream. A small diagnostic that prints the resolved scheduler/queue mapping per flow is the fastest way to catch this class of bug early.

## Evidence

- One boot: every flow resolved to the same invalid scheduler/queue assignment, and nothing passed upstream.
- A later boot, using ordinals instead: each flow resolved to its own distinct scheduler/queue pair, and round-trip connectivity worked end to end.
- An earlier boot had already shown the queue-reservation command returning a failure code when asked to reserve zero queues, and success when asked for a full T-CONT's worth — confirming the reservation step itself worked correctly in isolation.

## See also

- [OMCI bridge connection](rtl9601-omci-bridge-connection.md)
- [Active queues are GEM-referenced only](rtl9602c-omci-active-queues-are-gem-referenced-only.md)
