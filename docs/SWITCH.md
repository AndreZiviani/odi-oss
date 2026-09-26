# The switch core

Evidence behind the switch-core code in `kernel/extra/drivers/net/ethernet/odi/`:
what each register write was checked against, and what was left out and
why. The code comments say what a write does; this file says how it was
established. Register names are those of `odi_switch_hw.h`.

## Register window

The switch-core register file is at physical 0x1B000000. The stock
driver never ioremaps it: it dereferences the KSEG1 alias 0xBB000000
(0xBB000000 & 0x1FFFFFFF is 0x1B000000), and every register offset in a
capture (`CLASSIFY_SETUP`, `ACL_PORT_ENABLE`, `TABLE_CMD`, the GPON and
PON queue ranges) is a byte offset from it. Single reads on a live stick
confirmed it: `CLASSIFY_SETUP` at 0x1b01600c, `ACL_PORT_ENABLE` at
0x1b015040, `FLOOD_BCAST_PORTS` at 0x1b01c020 and `TABLE_CMD` at
0x1b012000 all read plausible values. `odi_switch.c` ioremaps the
physical base and maps only the part in use (`ODI_SWITCH_MMIO_SIZE`, up to
the PON queue block at +0xF00000).

0xB8000000 is a real window on this chip too, the SoC chip-control
registers, and much smaller. A switch-core offset added to it lands in
address space no device claims, and the first such write
(`CLASSIFY_SETUP`) stalled the whole SoC with no timeout and no clean
reset. The CPU-port NIC at 0xB8012000 (`odi_nic.c`) is a separate window
with its own mapping and never goes through the switch-core accessors.

## Register replays

Four captured write sequences are replayed rather than derived, all
through one loop, `odi_replay_run()` (`odi_replay.c`, with the record type
in `odi_replay.h`):

| sequence | source | register record | also |
|---|---|---|---|
| board init | `odi_board_data.c`, compiled in | write | SoC |
| sdkinit, one verb at a time | `sdkinit.bin` | read, then write | SoC, table rows |
| module load | `modload.bin` | read, then write | table rows |
| GPON boot init | `gpon_init.bin` | write, serial-number words substituted | table rows |

The read before a register write is the full-mask read-modify-write the
capture shows (`reg = (reg & 0) | value`). It is kept as a real read: it
changes nothing a register ends up holding, but without it the bus
traffic differs, and a clear-on-read register in a replay would see one
read fewer. Dropping it would need its own trial.

A SoC record carries the KSEG1 address of a SoC-window register and goes
through `odi_soc_write()` (`odi_soc.c`), which accepts only the offsets on
its one allowlist and logs any other. The replays write four of them
(clock/reset enable, IP enable, two GPIO registers).

### Board init

The stock board code sets up the switch core, the LED controller and the
I2C master before the first `/proc/odi_init` verb. `odi_board_data.c` is
that window of a clean ISP1 stock boot capture: 88 writes, write-only,
none dropped by the tracer, transcribed in capture order, with the three
SoC writes (0xb8000044, then 0xb8003324 and 0xb8003328) in position
between the switch-core writes around them. It is compiled in because it
runs at board init, before any filesystem, and is small and fixed enough
not to need a generator. `test/odi_board_test.sh` diffs the replay, write
for write, against the same window in
`test/fixtures/isp1-260923-g4-board-init-filtered.txt`.

## Platform init

`odi_switch_init_platform()` (`odi_switch_platform.c`) makes the switch-core
settings the stock OMCI kernel modules made when they loaded. rcS runs it
once through `/proc/odi_omci`, after the odi_init main loop (whose `switch`
verb writes the baseline these steps narrow) and before `omcid` starts,
which is where the stock image ran it. Each step is a read-modify-write of
the fields it owns. Board ports: UNI 0, PON 2, CPU 3; port 1 is unused.

- **classify**: `CLASSIFY_SETUP.US_NO_MATCH_ACTION = 1` (0 normal, 1 no PON
  match required, 2 drop). A running stock stick reads `0x1c09`: the field
  is already 1 there, `PON_SELECT` is 1 and `PATTERN1_COUNT` is `0xe0`. A
  fresh boot of ours reads `PATTERN1_COUNT = 0x80`, the value the `switch`
  verb writes. `PATTERN1_COUNT` is the boundary between the two CF pattern
  partitions and grows with the services the stock stack has provisioned,
  so it is allocator state, not a boot constant, and is not written.
- **acl**: `ACL_PORT_ENABLE`, one bit per port. The `switch` verb clears
  every port; the stock stick reads `0x5` (UNI and PON). Two writes, one per
  port, as the stock module load makes them.
- **vlan**: `VLAN_SETUP.FILTER_ON = 0`, `PORT_VLAN_INDEX` 0 for ports 0 and
  1, the port-1 bit of `VLAN_INGRESS_CHECK` off, every port an SVLAN uplink,
  `SVLAN_SETUP.FILTER_ON = 0` and `UNTAGGED_ACTION = 2` (port-based SVID).
  Before this step a fresh boot reads `VLAN_SETUP 0x1`, `SVLAN_SETUP 0`,
  `PORT_VLAN_INDEX 0x1001`; after it `0x18`, `0x09` and `0`, the values of
  the stock stick. `VLAN_INGRESS_CHECK` reads `0x0f` on a fresh boot and
  `0x0d` on the stock stick; no capture shows the write, but port 1 carries
  no traffic on this board, so turning its check off cannot change
  forwarding and matches the stock state.
  Not written, because the register strides are not decoded:
  `PORT_VLAN_INDEX` of ports 2 and 3 (base + 4n lands port 2 on the
  separate register at `0x013014`) and the per-port SVID (base + 4n lands
  port 2 of `PORT_SVLAN_INDEX` on `SVLAN_SETUP`).
- **l2**: `L2_LOOKUP_SETUP.CAM_OFF = 0` (a disable bit). The ageing fields
  of the same register belong to cmd 62. No stock reading exists.
- **cf sweep**: every `CLS_RULE_B`, `CLS_DS_ACTION` and `CLS_US_ACTION` row
  (256 each) written all-zero. Bit 16 of word 0 is the valid flag in every
  CF row decoded, so an all-zero row is an invalid one. `CLS_MASK_B` is left
  alone, as a delete leaves it. The downstream default-forward entry at row
  255 is not written here: its action word is not decoded (the module-load
  replay carries the captured row).
- **reserved vid**: `VLAN_SETUP.VID0_MODE = VID4095_MODE = 1` (tag), as the
  stock stick reads, and `SVLAN_SETUP.PRIO_SOURCE = 1` (C-TAG priority).

Left out altogether:

- ACL template index 1 as the stock module load shapes it (DMAC, SMAC,
  C-TAG, GEM index). `SW_0x015008` is addressed by a template index and a
  field slot, and how the two combine into an offset is not decoded. The
  `switch` verb leaves index 1 with a different field set, so an ACL rule
  that assumed the module-load shape of index 1 would match the wrong
  fields.
- The number of ACL rows the stock stack reserves for its own entries. It
  is read from state there is no way to read, and nothing here allocates
  ACL rows yet.

## Module-load replay

`modload.bin` is every switch-core write the stock OMCI kernel modules
made while loading, in capture order: 2587 register writes and 1032 table
rows. `tools/regtrace/mkmodload.py` generates it from that capture and
tags each record with its family (`category`: registers, the CF sweep, the
CF rules written over the sweep, the VLAN row, the L2 unicast row, the ACL
rows; `reg_group` for a register: its register family). The families were
the selection keys of a boot-time mask while the replay was bisected. The
production selection is now applied to the file itself, with
`tools/regtrace/replayblob.py filter` and the mask that every boot used
(`PRODUCTION_MODLOAD_MASK`), and the kernel replays every record:

- every register and every table row as captured, except
- the three LUT flood masks (`FLOOD_BCAST_PORTS`, `FLOOD_UNKN_MCAST_PORTS`,
  `FLOOD_UNKN_UCAST_PORTS`), which carry `0xf`, the CPU port included,
  where the capture has `0x7` (three ports, CPU excluded). Replayed with the
  captured value, broadcast from the LAN stopped reaching the CPU while
  unicast still passed.

The families stay in the records for review; nothing selects by them.

The table-engine writes of the capture (`0x012000`-`0x01202c`) are not in
the file: they are the handshake of the rows the capture also records as
table ops, which `odi_switch_table_write()` performs itself. Records are
not grouped by table because the stock sweep interleaves four CF tables per
row, and the order is kept as the hardware saw it.

The downstream default-forward rule is in this file: `CLS_RULE_B[255]`,
`CLS_MASK_B[255]` and `CLS_DS_ACTION[255]`, a wildcard downstream rule
whose action is force-forward (`UNI_ACT` 1).

## Command leaves

`odi_switch_dal.h` declares one leaf per group of switch-core writes an
OMCI driver command makes; `odi_switch_cmd.c` calls them. Each leaf is the
register sequence of the stock image for that command, captured on ISP1
during provisioning (`test/fixtures/isp1-260922-boot5.txt`), with the
fields that vary between instances as parameters.
`test/odi_switch_dal_test.sh` replays each leaf into a bracket and
compares it with the matching `test/fixtures/dal-cmd*.txt`, whose header
names the capture lines it came from. The stock driver names of the
commands, from the command table of the shipped binary:

| cmd | stock name | leaf | file |
|---|---|---|---|
| 10 | getTransceiverStatus | `odi_sw_ponmac_transceiver_get` | `odi_switch_port.c` |
| 23 | setPriQueue | `odi_sw_ponmac_queue_add`, `_add_ext` | `odi_switch_qos.c` |
| 25 | cfgGemFlow | `odi_sw_gpon_usflow_set` (downstream), `odi_sw_ponmac_flow_queue_set` (upstream) | `odi_switch_ds_gem.c`, `odi_switch_qos.c` |
| 30 | setPortAutoNegoAbility | `odi_sw_port_autoneg_get`, `_set` | `odi_switch_port.c` |
| 32 | setPortState | `odi_sw_port_force_get`, `_set` | `odi_switch_port.c` |
| 51 | activeBdgConn | `odi_sw_cf_add` | `odi_switch_cf.c` |
| 62 | setAgeingTime | `odi_sw_l2_aging_set` | `odi_switch_port.c` |
| 64 | setFloodingPortMask | `odi_sw_l2_flood_mask_set` | `odi_switch_port.c` |

- **cmd 62**: one write of `L2_LOOKUP_SETUP`, AGE_TICKS 3000 and
  AGE_ON_LINK_DOWN 1. The leaf is a read-modify-write, because the
  register also holds CAM_FULL_ACTION, ARP_AS_KNOWN, the two multicast
  hash-mode bits and CAM_OFF; the capture shows one write because the
  register reads 0 at that point.
- **cmd 30**: the UNI PHY registers at indirect PHY addresses 0xa400,
  0xa408 and 0xa412 through `PHY_ACCESS_CMD`/`PHY_ACCESS_DATA`: the read
  commands, each address twice, then the three values written back, with
  one more read command on 0xa400 before its write. The fields are not
  decoded, so the parameters are named by address.
- **cmd 32**: `PORT_FORCE_SELECT(0) = 0`, every FORCE bit clear, so speed
  and duplex come from the PHY. The admin state the command carries has no
  register write in any capture; the leaf that took it did nothing and is
  gone.
- **cmd 64**: the capture writes `FLOOD_UNKN_UCAST_PORTS` (0x01c028, not
  `FLOOD_BCAST_PORTS` at 0x01c020, which no capture writes) four times with
  the same word, 0x7. The word holds the bit of every port, so the four
  writes are one value repeated, not one per port; the repeat is kept
  because the host replay compares brackets write for write.
- **cmd 23** has two shapes. Before any T-CONT exists it writes
  `PORT_QUEUE_MAP` (0x01c0c0; the word at 0x01c0a0 is never written) with
  0xd4, identical in the first eight instances. The register holds a 2-bit
  index per port, which 0xd4 does not fit, so it is written raw. Once
  T-CONTs are being created (instances 9 to 13, interleaved with cmd 21)
  it writes seven `PONQ_COUNT_MASK` words: +207 (one bit per queue,
  cumulative), +15 (8 more per instance), +190+n (the scheduling slot of
  queue n, value 0), +208 and +212 or +213, +60+n (0) and +125+n (0x3ffff).
  The order of +208 and +212/+213 depends on n: the first queue (n 0, the
  only instance where +208 is 0) writes +208 first, every later queue
  writes it second, in all five instances. +212 is used for instances 9 to
  11 and +213 for 12 and 13.
- **cmd 25** programs both directions. The downstream side is the DS GEM
  port CAM row and its `DSF_GEM_FLOW_TYPE` word (FLAGS 3 for the
  OMCI/broadcast port, 2 for data), plus the slot record the AES path
  reads (below). The upstream side writes `US_GEM_PORT_MAP(slot)`, then
  `PONQ_COUNT_MASK` +37 (cumulative), +235 twice (two sub-fields, one
  write each) and +20, or +21 for the last of the five ISP1 instances; the
  fields of these words are not decoded.
- **cmd 10**: the port-1 I2C master device select (`I2C_MASTER_SETUP(1)`,
  0x234413a), then GPIO 29 and GPIO 31, twice: GPIO 29 takes the two
  values of the instance (0x62, then 0x63 in the first), GPIO 31 is always
  1. It shares the I2C master with the DDM read, so it takes
  `odi_i2c_lock`.

### Bridge connections

`odi_sw_cf_add()` is the whole sequence of one cmd 51: the CF rows it is
handed, a VLAN table sweep and a register template. Which rows, and in
which order, is decided by the command layer (`odi_switch_cmd.c`), which
derives them from the OMCI bridge rule; the leaf writes what it is given.
The plain-register part is the same, bit for bit, in all twelve ISP1
instances and in the tail of the ISP2 stock bracket
(`test/fixtures/isp2-260921-cmd51-tail.txt`), so it is replayed as a
fixed template: it is idempotent.

- Each CF row is written after the `CLASSIFY_PATTERN_SEL` word that holds
  its template bit (32 rows per word, row 254 in word 7); every row is on
  template 0, so the word is 0.
- The VLAN table is written twice. The first pass sets all 4096 rows to
  0xf0. Rows 1 and 0 are then rewritten (row 1 0x3f8ff then 0x3f800, row
  0 0xff), and the second pass writes rows 2 to 4094 row by row, 0 except
  the service rows. Row 4095 is not rewritten in any of the twelve
  instances.
- Around them: `VLAN_SETUP` stepped through a fixed series of values,
  four `VLAN_ACCEPT_FRAMES`/`VLAN_INGRESS_CHECK`/`PORT_VLAN_INDEX`
  triplets, the egress tag group, two passes of the reserved-multicast
  (`LINK_MCAST`) block with the DSCP remark and IPMC leak writes between
  them, `PROTO_VLAN_GROUP(0..3)` twice each and the 16 `PORT_PROTO_VLAN`
  slots. The capture addresses 0x013008 through the macro for
  `VLAN_ACCEPT_FRAMES(2)`; the register there is `VLAN_SETUP`, whose four
  fields decode the values written.

### CF rows

A CF (classification) row is three tables at one index: `CLS_RULE_B` and
`CLS_MASK_B` (two words each) hold the match, `CLS_US_ACTION` or
`CLS_DS_ACTION` (three words) the treatment, and the direction bit of the
match selects the action table. Word 0 is the most significant word, the
order `odi_switch_table_write()` takes and a capture lists. Row 255 is the
downstream catch-all of the module-load replay (force forward, no tag
change). The captured allocation puts the more specific rows at lower
indexes, which implies the lowest matching index wins; that is inferred
from the ordering, not measured.

The match is a TCAM data/care pair, not value and mask: for every bit,
RULE = data AND care and MASK = care AND NOT data, so a bit set in either
word is cared about and the two never overlap. Every service row of both
captures has this property, and the stock diag CLI prints the rows back
as databit/carebit with care = RULE | MASK. Bits over the 64-bit pair:

| bits | field |
|---|---|
| 48 | VALID (RULE only) |
| 47..32 | ethertype / inner-tag word (never cared in any capture) |
| 31 | direction: 1 downstream (from the PON), 0 upstream |
| 30..23 | TOS / GEM index (never cared: downstream rows match the tag only) |
| 22..11 | VID of the outer tag |
| 10..8 | priority of the outer tag |
| 7..5 | internal priority (never cared) |
| 4, 3 | the frame carries an S-tag, a C-tag |
| 2..0 | source port (upstream rows: the UNI) |

Confirmed by value across the twelve ISP1 instances: direction, VID (10
to 14), priority (4, 5 and 0 cared, 8 not), both tag flags and the UNI
change in step with the OMCI input; the rest is layout only and never
seen non-zero. Bit 16 of `mask_w0` is not a match field: the stock driver
writes it 0 on the row an insert adds and 1 on every other row write (the
rows an insert shifts, and a rewrite in place). No effect on matching
shows in either capture; ours writes it the same way so the host replay
stays identical.

The action, 67 bits over three words (word 0 is bits 95..64):

| bits | field |
|---|---|
| 66..61, 60 | DSCP value, DSCP remark enable (0 so far) |
| 59..57, 56 | CF priority, its enable (0 so far) |
| 55..53, 52..50 | C-tag priority source, C-tag VID source (1: the values below, the only value used) |
| 49..47, 46..35 | C-tag priority, C-tag VID |
| 34..33 | C-tag action: 0 none, 1 add, 2 delete, 3 transparent |
| 32..31 | upstream: drop/trap (0 forward); downstream: UNI action (3 forward to the port mask, every service row; 1 the row-255 force forward) |
| 30..24 | upstream: the flow id the frame is queued on (the us_flow of the bridge rule, 0..4 on ISP1) |
| 30..27 | downstream: egress port mask (bit n = switch port n) |
| 23 | upstream: take the flow id (1 on every service row) |
| 22..21, 20..18 | upstream: S-tag VID source, S-tag priority source (1) |
| 23..21, 20..18 | downstream: S-tag VID source, S-tag priority source (1) |
| 17..15, 14..3 | S-tag priority, S-tag VID (0 in every row) |
| 2..0 | S-tag action: 3 delete, 4 transparent (the two seen), 0 none |

A VLAN table row is one word by VID: bits 3..0 the member ports (bit n =
switch port n: 0 UNI, 2 PON, 3 CPU), 7..4 the members that send the frame
untagged, 8 FID/MSTI, 9 S-VLAN check IVL/SVL, 10 IVL/SVL, 17..11 the
extension port mask. A service VLAN is 0x15 when the UNI sees it untagged
(members UNI and PON, untagged on the UNI: ISP1 VID 11, ISP2 stock VID
10) and 0x5 when the UNI sees it tagged (ISP1 VIDs 10, 12 to 14). With
VLAN filtering on, a frame whose VID row does not list the ingress port is
dropped at ingress: the likely reason ISP2 discarded every downstream
frame at the PON port while this table was replayed from the ISP1
capture, where VID 10 had no row until the ninth call.

A CF row delete (`odi_sw_cf_del()`) writes an all-zero rule row, VALID
clear, and an all-zero action row. No capture shows a delete; this is the
encoding the layout implies and the one the platform sweep uses.

### MIB counters

`odi_sw_mib_get()` (`odi_switch_mib.c`) answers a port counter by the
index of `src/diag/src/mib.h` `mib_names[]`, the counter order of the
stock diag, from the `PORT_TX_COUNTERS`, `PORT_RX_COUNTERS` and
`PORT_OAM_COUNTERS` blocks. The two 64-bit counters, ifInOctets and
ifOutOctets, are two consecutive words, the low half at the lower
address: a live ISP1 counter that had not yet passed 32 bits read back
nonzero high and zero low with the halves the other way round, and
`test/odi_reg_test.c` pins the order. Left unmapped, and refused: the
stock duplicates (a direction-specific name such as etherStatsTx... next
to a direction-less etherStats... that no register tells apart:
etherStatsOctets, the broadcast and multicast counts, the undersize and
oversize counts, the frame-size buckets, TxFragments, TxJabbers,
TxCRCAlignErrors, RxUndersizeDropPkts), and the counters no register row
names at all (dot1dPortDelayExceedDiscards, dot1dTpHcPortInDiscards,
dot3StatsAlignmentErrors, dot3StatsFrameTooLongs, dot3OutPauseOnFrames).

## Downstream AES

The OLT encrypts the data GEM ports: downstream frames on them are AES-CTR
ciphertext, and the ONU decrypts a port only with bit 4 of its
`DSF_GEM_FLOW_TYPE` FLAGS set. A full-stream capture of the stock image
shows FLAGS `0x12` on the five ISP1 data ports where ours had `0x02`, and
the bit set right after the OLT sends the G.984.3 Encrypted_Port-ID PLOAM
(which names a GEM port and an enable bit), from the GPON interrupt and
outside any OMCI command. The other FLAGS bits are multicast (0), Ethernet
(1) and OMCI (2).

The GPON core decodes that PLOAM and calls
`odi_switch_gpon_encrypt_port()`, which turns the GEM port id into its DS
slot (recorded by cmd 25 when it creates the port) and sets or clears the
one bit with a read-modify-write. A PLOAM for a port cmd 25 has not
created yet is logged and ignored. Not reproduced from the capture: the CAM
rewrite around the bit (it writes the same GEM id back), and the interrupt
mask save and restore the stock handler makes around it.

Setting the bit on every data port at creation was rejected: a port the
OLT does not encrypt would then be decrypted into garbage, and nothing here
could tell. Every OLT seen so far encrypts every data port.
