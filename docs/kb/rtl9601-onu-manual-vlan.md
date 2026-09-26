# The service VLAN tag lives in the config store, not in the OMCI extended-VLAN table

Reading the OMCI extended-VLAN table to find a service's VLAN tag gives the wrong answer, silently: the tag actually comes from the device's own config store.

*Last verified: 2026-09-13*

---

## What

On the ODI DFP-34X-2C2, the C-TAG the upstream service adds is a config-store value (`VLAN_MANU_TAG_VID`), not something carried in the OMCI extended-VLAN table (class 171). On a stick whose live connection adds C-TAG VID 10, reading class 171 gives:

    Treatment Outer : PRI 15, VID 4096, TPID 6, RemoveTags 0
    Treatment Inner : PRI 15, VID 4096, TPID 0

`VID 4096` is the "no VID specified" sentinel and `PRI 15` is "no tag". There is no 10 anywhere in it, and no second class 171 instance.

Two sticks, two values, each matching its own live connection dump:

| stick | config-store VLAN | C-TAG the connection adds |
|---|---|---|
| ISP2 stick (PPPoE on VLAN 10) | 10 | 10 |
| ISP1 stick (VLAN 11) | 11 | 11 |

The ONU is in manual VLAN mode and tags locally: it presents a VEIP to the line and does the tagging itself rather than relying on an OMCI-provisioned rule.

## Why it matters

**Reading the extended-VLAN table to find out what tag a service carries gives the wrong answer, and gives it silently** — every field reads "unspecified", which looks like "no tagging" rather than like "look elsewhere".

For anyone reimplementing the OMCI daemon this is the difference between a bridge connection that forwards and a service that works. A rule built from class 171 alone forwards untagged frames, the OLT discards them, and every local signal still looks healthy: driver calls return 0, the ONU is at O5, the switch counters show upstream leaving. Measured over 90 seconds with a connection built entirely by replacement code:

| rule | upstream forwarded | downstream arriving |
|---|---|---|
| no connection at all | 0 | 132,659 |
| transparent | 871,109 | 222,714 |
| C-TAG ADD, VID from the store | 1,687,894 | **2,635,078** |

**Downstream arrival is the signal**, because it cannot rise unless the far end is accepting what you send. Upstream leaving the PON port proves only that the switch forwarded it.

## The rule the store implies

Every field pinned on a working stick:

    rule_gen   EXTENDED VLAN
    filter       S-TAG NO TAG, C-TAG NO TAG, EtherType no filter
    S-TAG Act    TRANSPARENT
    C-TAG Act    ADD, VID ASSIGN, PRI ASSIGN
                 (PRI, TPID, VID) = (config-store priority, copy-from-inner,
                                     config-store VID)
    out     tag_count 1, TPID_8100, same (PRI, VID)

Downstream multicast is the mirror: C-TAG **REMOVE**, `is_mcast 1`, and the out-style back to the ignore sentinels (PRI 8, VID 4096).

**The treatment-TPID enum values are the reverse of their names' apparent order**: **0 is copy-from-inner**, 1 copy-from-outer. There is a third, smaller enum for the out-style TPID field where **0 is TPID_8100**.

## Evidence

Both sticks read live. The traffic figures are switch-counter samples over 30- and 90-second windows with the stock daemon stopped and a replacement responder owning the line, including a torn-down control window that measured a hard zero upstream.

## See also

- [GEM flows do not forward on their own](rtl9601-omci-bridge-connection.md)
- [Config store](rtl9601-config-store.md)
