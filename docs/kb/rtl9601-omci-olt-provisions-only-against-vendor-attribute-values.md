# An ONU that reports plausible attribute values gets fully provisioned; one reporting zeros does not

Against one observed OLT, an OMCI responder answering all-zero values for its autonomously-created managed entities got management-only provisioning, while answering with realistic, stock-firmware-like attribute values unlocked full service provisioning.

*Last verified: 2026-09-21*

---

## What

With every autonomously-created managed entity (ONU-level entity, physical UNI entities, queues, schedulers, and so on) reported as all-zero attributes, one observed OLT completed registration, ran its test sequence, and then created no unicast GEM port, no GEM interworking termination point, and no 802.1p mapper — management-only provisioning. Loading a captured set of realistic attribute values (around 1,800 attributes captured from the stock firmware's own MIB) and answering reads and MIB uploads from those instead, the same OLT went on to provision six GEM ports, five GEM interworking termination points, five mappers, eight bridge ports, VLAN filters, and dozens of queue sets — matching the stock firmware's own provisioned shape.

## Why it matters

Two earlier hypotheses for the OLT stopping short — that it just stops after the test sequence, or that a particular loop-back/identifier managed entity is a hard authentication gate — were both ruled out (the test-sequence answer was necessary but not sufficient, and the identifier check was not the blocker). **This is inference from a single OLT's behavior across a limited number of observed boot cycles, not a confirmed general mechanism** for how every OLT decides what to provision. The operational takeaway, scoped to that one observed OLT: it appears to decide what to provision based on what capabilities the ONU claims to have, so an ONU implementation needs to report plausible capability values for its autonomous entities rather than defaults, to get full service provisioning rather than management-only.

## Evidence

- A sequence of boots before a capability-defaults mechanism was added showed management-only provisioning; after it was added, the next boot showed full service provisioning.
- A large captured reference dump of the stock firmware's full MIB state, used as the source of realistic attribute values.

## See also

- [OMCI MIB model](rtl9601-omci-mib-model.md)
- [OMCI MIB readback](rtl9601-omci-mib-readback.md)
