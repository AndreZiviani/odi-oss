# Reserve only the queues a GEM CTP actually references, not every queue the OLT sets

*This note describes behavior of an intermediate, now-superseded vendor-based driver generation for this chip variant — one that reported a smaller T-CONT/queue count than the original vendor driver. The specific counts below belong to that driver generation only.*

Reserving every priority-queue row the OLT provisions (rather than only the ones an upstream GEM port actually references) exhausted this driver generation's small queue pool and produced an out-of-range queue index returned with a success code.

*Last verified: 2026-09-21*

---

## What

On this intermediate driver generation, an observed OLT provisioned 72 priority-queue rows, of which only five were actually referenced by upstream GEM port entities. Reserving a queue for every provisioned row (rather than only the five actually referenced) exhausted the driver's 32-queue pool: it produced sparse, non-contiguous physical queue assignments and, for the fifth T-CONT, returned a queue index of 8 — one past the eight scheduler slots this driver generation actually has — with a success return code. That flow then sat on a nonexistent scheduler and forwarded nothing. Reserving only the actually-referenced queues instead gave dense, valid mappings, and the connection forwarded correctly.

## Why it matters

The durable lesson, independent of the specific counts above: **range-check any index a queue create/reserve operation hands back against the device's actual queue-pool size** (available from the capability query — see the [device capabilities note](rtl9601-omci-device-capabilities.md)) rather than trusting the return code alone — this driver generation was observed to silently return an out-of-range index with a success code rather than an error. And don't reserve every theoretically possible queue up front just because the OLT provisioned a row for it; reserve only what an actual upstream GEM CTP references. It's also worth rebuilding the queue-assignment graph once per idle period rather than once per individual provisioning message, since a single provisioning burst can otherwise re-run the same delete-and-recreate sequence dozens of times.

## Evidence

- One boot: reserving every provisioned queue row produced sparse physical queue assignments and an out-of-range scheduler/queue assignment for the fifth T-CONT.
- A later boot: reserving only the actually-referenced queues produced dense, valid scheduler/queue assignments, full connectivity, and one queue-rebuild transaction per boot instead of dozens.

## See also

- [Upstream queue ordinal is dense per T-CONT](rtl9602c-omci-upstream-queue-ordinal-is-dense-per-tcont.md)
- [OMCI device capabilities](rtl9601-omci-device-capabilities.md)
