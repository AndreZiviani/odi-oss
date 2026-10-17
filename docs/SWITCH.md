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
- **acl clean**: every row of the three ACL tables (96 each: the rule
  pattern, kind 2 rows 128-223; the rule mask, kind 2 rows 0-95; the
  action, kind 3) is read back, written all-zero and read back again,
  before the next step turns ACL ingress on. The table RAM survives a
  reset and a reboot: after a power cut it holds random words, and after
  a warm boot from the stock firmware it holds that firmware's rules (a
  stick back on this image after a stock boot showed a stock rule 3,
  matching 192.168.0.1, next to the module-load replay's rule 1). A rule
  is valid when bit 144 of its pattern row is set (bit 16 of word 0 in
  `odi_switch_table_write()` order); the module-load replay writes rule 1
  with it set. The step logs what it found and what it left:

      odi_switch: ACL rows before the clean: 2/96 rules valid (first 1), 4/96 rows not empty
      odi_switch: ACL rows clean after 1 pass(es): 0/96 rules valid, 0/96 rows not empty

  A row still not empty after a second pass, or a read that timed out,
  gives `ACL rows NOT clean after 2 passes` with the counts; the boot
  goes on. The module-load replay then writes rule 1 (pattern, mask and
  action), the only ACL row this image uses.

  The L2 table is not cleared here. Whether an L2 row is in use is the
  engine's HIT answer to a read, not a bit of the row, so nothing shows
  that writing a row all-zero by number removes its entry; removing one
  is a delete by key through the hash, and this driver deletes only
  multicast keys. Learned rows age out; what an earlier image left there
  is listed by `diag l2-table get all`.
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
- **host pause**: `PORT_FORCE_ABILITY(0).TX_PAUSE = 0`, then
  `PORT_FORCE_SELECT(0).TX_PAUSE = 1`: the host port never sends PAUSE
  frames to the host. Ours, not the stock image's; see
  [Host port PAUSE](#host-port-pause).

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
  unicast still passed. Right after the replay `odi_switch_init_modload()`
  puts the unknown-multicast mask back to `0x7`: the CPU needs broadcast
  (ARP) but no unknown multicast (no IPv6 in the kernel, no group joined on
  the stick, the reserved 01:80:C2 groups have their own actions), and with
  IGMP snooping off every IPTV group is unknown, so a stream would be copied
  to the CPU at stream rate. The unknown-unicast mask is cmd 64's.

The families stay in the records for review; nothing selects by them.

The table-engine writes of the capture (`0x012000`-`0x01202c`) are not in
the file: they are the handshake of the rows the capture also records as
table ops, which `odi_switch_table_write()` performs itself. Records are
not grouped by table because the stock sweep interleaves four CF tables per
row, and the order is kept as the hardware saw it.

The downstream default-forward rule is in this file: `CLS_RULE_B[255]`,
`CLS_MASK_B[255]` and `CLS_DS_ACTION[255]`, a wildcard downstream rule
whose action is force-forward (`UNI_ACT` 1).

## GPON boot init

`gpon_init.bin` is the GPON MAC bring-up the PON steps `gpondrv` to
`gponact` made in the capture, replayed once per boot by the first
`gponact` (`odi_gpon_init.c`), after `gponsn` has given it the serial
number. Early in it the table pulses `PONMAC_RESET.RESET_REQ` (0x70000c
bit 0). The replay stops after the record that releases it and, before
it goes on:

1. **waits for the reset.** It reads `PONMAC_RESET` until `RESET_COMPLETE`
   (bit 8) is set, at most 20000 reads 1 us apart, and logs the number of
   reads (`odi_gpon: GPON MAC reset complete after N reads`), or an error
   with the register value when the bound runs out. The stock driver
   waits for the same bit; a register capture of its bring-up shows about
   a hundred reads.
2. **cleans the GPON tables.** Every `DSF_GEM_FLOW_TYPE` word (128) is
   written to 0, and every row of the GEM-port CAM (128 rows,
   `DSF_GEM_CAM_CTL` 0x701100), of the Alloc-ID CAM (32 rows,
   `DSF_ALLOC_CAM_CTL` 0x7010c0) and of the downstream multicast MAC
   filter (256 rows, `DS_MC_FILTER_CTL` 0x704084, the same control word
   layout) is cleared with a request of `OP` 3, each polled for `DONE`
   with the bound of every CAM poll.
3. **reads every row back.** A request of `OP` 2 per row sets
   `ROW_VALID` (bit 13) when the row still holds an entry, with the entry
   in `DSF_GEM_CAM_RDATA` / `DSF_ALLOC_CAM_RDATA` / `DS_MC_FILTER_RDATA`
   (0x704090); the flow type words are read directly. The same read runs
   before the clean, so the log shows what the boot found and what it
   left:

       odi_gpon: GPON tables before the clean: GEM-port CAM 120/128 valid, Alloc-ID CAM 32/32 valid, flow type 119/128 set
       odi_gpon: GPON multicast filter before the clean: 0/256 rows valid (row -1 0x00000000); self-test words 00008000 00008000 00008000 00008000 00008000 00008000 00008000 00008000
       odi_gpon: GPON tables clean after 1 pass(es): GEM-port CAM 0/128, Alloc-ID CAM 0/32, flow type 0/128, multicast filter 0/256

   The multicast filter line also carries the eight words of
   `DS_MC_FILTER_BIST` (0x70428c), read and never written. They are not
   filter content: their fields are the built-in self-test status of the
   filter RAM, the same layout as the words beside them for the other
   downstream GEM RAMs, and 0x8000 is `ALL_BIST_DONE` alone, the test run
   and passed. A word with `BIST_FAIL` (bit 7) or `DRF_BIST_FAIL` (bit 9)
   set, or `ALL_BIST_DONE` clear, adds "(a self-test failed or never
   ran)". The register list carries no reset values, so what a word
   reads straight after a power-up is not known yet. The other fields
   start a test, which is why nothing writes them.

   A row still set after the clean is logged as an error, with the first
   such row and its entry, and the whole clean runs once more. If rows
   survive that too (or a CAM request never finishes), the error line
   says so and the activation goes ahead: the ONU is no worse off than
   without the clean, and the line is in dmesg and the ramlog.

Then the rest of the table runs: it programs this ONU and activates it.
Every row of ours (the ONU-ID row, the OMCC GEM port, the T-CONTs and
flows) is written later, as the OLT assigns it, so all of them land in
clean tables.

Why: these tables are on-chip RAM. A warm reset keeps whatever the
last session left in them, which is this ONU's own, still valid entries.
A power-up leaves whatever the RAM came up with: read back on a stick after
a power cut, 120 of the 128 GEM-port rows and all 32 Alloc-ID rows were
valid with random entries, and about 120 flow type words were not zero.
With 30 foreign Alloc-IDs in its CAM the ONU answers bandwidth grants meant
for other ONUs; that stick accepted about nine times the bandwidth map
entries of a warm boot, ranged to O5 and was deactivated by the OLT 21
times in 150 s, and OMCI never started. Before this clean the replay
carried its own clean of the same rows, issued right after the reset
release with no wait: on that stick the clears of GEM-port rows 0 to 119
and of every Alloc-ID row were lost, and only rows 120 to 127 came out
clear, so a block still in reset drops what reaches it. A warm reboot of
the same image kept the garbage; one boot of the stock firmware, which
waits for the reset, cleaned it.

The replay's own clean records (and its clear of the 256-entry multicast
MAC filter table that follows them, issued back to back with no wait for
`DONE`) still run; after the wait they land in a block that is out of
reset. The boot table sets the filter up to be consulted for downstream
multicast (`GPON_GEM_DS_MC_CFG` 0x704080 ends at 0x59: no IPv4 or IPv6
force pass or drop), so a row it left behind could change which groups
get through; the clean above now reads it back and clears it as well, but
what a stick holds there after a power cut has not been seen yet.
`test/odi_gpon_init_clean_test.sh` runs
the clean against a model of the two CAMs whose reset drops writes for a
while after its release, and checks that the plain replay leaves the
garbage and this one does not.

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
| 10 | getTransceiverStatus | `odi_ddm_get` | `odi_ddm.c` |
| 23 | setPriQueue | `odi_sw_ponmac_queue_add` (downstream), `odi_sw_ponq_queue_add` (upstream) | `odi_switch_qos.c` |
| 24 | clearPriQueue | `odi_sw_ponq_queue_del` | `odi_switch_qos.c` |
| 25 | cfgGemFlow | `odi_sw_gpon_usflow_set` (downstream), `odi_sw_ponq_us_gem_set`, `_sidvalid_set`, `_glb_th_set`, `_sid2qid_set` (upstream) | `odi_switch_ds_gem.c`, `odi_switch_qos.c` |
| 11 | setSignalParameter | `odi_gpon_signal_threshold_set` (the SF/SD thresholds) | `odi_gpon.c`, `odi_gpon_ctr.c` |
| 30 | setPortAutoNegoAbility | `odi_sw_port_autoneg_get`, `_set` | `odi_switch_port.c` |
| 32 | setPortState | `odi_sw_port_admin_set` | `odi_switch_port.c` |
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
- **cmd 32**: the admin state of an Ethernet UNI, `{port, state}` (ME 11
  AdministrativeState; omcid sends state 0 for a lock). It is the stock
  SDK's admin enable: a lock writes LINK (bit 4) of `PORT_FORCE_ABILITY` to
  0 and then sets LINK of `PORT_FORCE_SELECT`, so the port's link is forced
  down; an unlock clears that select bit. Bit 4 only, both ways, so the
  `TX_PAUSE` force on port 0 (the word is 0x40 unless
  `odi_switch.host_pause` is set, [Host port PAUSE](#host-port-pause)) is
  kept. The capture only ever shows the unlock, a read and a write of 0 to
  `PORT_FORCE_SELECT(0)`: both ISPs leave the UNI unlocked. On this SFP the
  UNI is the host port, so a lock takes the host link down, the stick's own
  management path included, until the OLT unlocks; OMCI does not cross that
  port. A lock of the PON or CPU port is refused (it would cut OMCI itself).
  The host tests compare the unlock bracket with the capture under the
  stock policy (`test/odi_host_pause_test.c` covers the lock).
- **cmd 64**: `{sel, enable, portMask}`, a lookup-miss flood mask, as the
  stock OMCI layer does it: `sel` picks unknown unicast
  (`FLOOD_UNKN_UCAST_PORTS`, 0x01c028), broadcast (0x01c020) or unknown
  multicast (0x01c024); `enable` 1 adds portMask's ports, 0 removes them,
  every other port keeps its bit; 2 (trap) is refused. omcid names one
  port per class 47 create or delete, so a delete or ME 45
  `DiscardUnknown` = 1 takes that port out, not every port. One exception
  is kept from the old fixed write: cmd 64 on the unknown-unicast mask
  clears the CPU bit, which the module-load replay sets (0xf, above) and
  the stock image never has; both ISPs run with it clear after the first
  provisioning. The capture's call (the PON port added) gives 0x7, written
  four times: the SDK writes the word once per port, and the host replay
  compares brackets write for write.
- **cmd 23** has two shapes, and the direction in its argument picks one.
  A downstream queue writes `PORT_QUEUE_MAP` (0x01c0c0; the word at
  0x01c0a0 is never written) with 0xd4, identical in the eight instances
  ISP1 sends, all before any T-CONT exists. The register holds a 2-bit
  group index per port, four ports: 0xd4 is the UNI on group 0, the other
  ports as the init left them, the result of the stock read-modify-write of
  port 0 for a queue with no drop precedence marking. An upstream queue,
  and cmd 21, 24 and the upstream side of cmd 25, are the upstream PON
  queues, below.
- **cmd 25** downstream is the DS GEM port CAM row and its
  `DSF_GEM_FLOW_TYPE` word (FLAGS 3 for the OMCI/broadcast port, 2 for
  data), plus the slot record the AES path reads (below). `ena` 0 (a
  delete) is refused downstream: omcid never sends one and the driver has
  no downstream delete. The upstream side is below.

- **cmd 10**: one SFF-8472 DDM word, for the ANI-G Test result. omcid
  sends `{type, data[32]}` with the type (2 temperature, 3 supply voltage,
  4 bias current, 5 Tx power, 6 Rx power, the `odi_ddm.h` selectors) as a
  big-endian word at bytes 0-3, and reads the A2h word back, big-endian,
  at `data[0..1]`. The command is a DDM read through `odi_ddm_get()`,
  both bytes under `odi_i2c_lock`. Its writes are the capture's: the
  A2h device select (0x234413a in `I2C_MASTER_SETUP`, port 1), the byte
  address in `I2C_BYTE_ADDR` (0xbc) and START in `I2C_CMD` (0xc4), once
  per byte; the five captured instances are A2h 98, 104, 102, 100 and 96,
  the order omcid asks for voltage, Rx, Tx, bias and temperature.

  The driver used to replay those writes with no completion poll and no
  read of `I2C_READ_DATA`, taking 0xbc and 0xc4 for GPIO pin selects: the
  command returned 0 with nothing in `data`, so every ANI-G Test result
  reported 0 V, no light, 0 mA and 0 C, and the transaction it started
  was still running when it released `odi_i2c_lock`.

### Upstream PON queues

cmd 21, 23 (upstream), 24 and 25 (upstream) program the upstream scheduler
from their arguments. Nothing in them is replayed. The register meanings
come from the stock driver's queue add, queue delete and upstream flow add
and delete (the RTL9602C SDK: `dal_rtl9602c_ponmac.c`,
`dal_rtl9602c_gpon.c`, the GPON module's `gpon_res.c`, and the OMCI
platform layer's `pf_rtl96xx_SetPriQueue`, read as a reference), with the
register addresses and fields of the chip's register map. The proof that
the reading is right: given the arguments omcid sends on ISP1 (five
T-CONTs, one queue each, weights 1, 30, 24, 4, 4), the code writes every
word of the 16 cmd 23 and cmd 25 brackets of the stock capture
(`test/odi_switch_cmd_test.sh`), which were replayed word for word before.

The window `PONQ_COUNT_MASK` (0xf020a8 + 4 n) holds these registers:

| n | register | what |
|---|---|---|
| 15 | `DRN_CMD` | drain one queue: queue << 3, QUEUE_MODE (bit 2), start (bit 1); bit 0 reads 1 while it runs |
| 20.. | `PON_SID2QID` | the queue of each upstream flow, 7 bits, 4 flows a word |
| 37.. | `PON_SIDVALID` | one bit per upstream flow |
| 60+q | `PON_QID_CIR_RATE` | CIR of queue q, 18 bits |
| 125+q | `PON_QID_PIR_RATE` | PIR of queue q, 18 bits, 0x3ffff no limit |
| 190+t | `PON_SCH_QMAP` | the queue mask of T-CONT t |
| 207 | `PON_TCONT_EN` | one bit per T-CONT |
| 208.. | `PON_WFQ_TYPE` | one bit per queue: 0 strict, 1 WFQ |
| 212.. | `PON_WFQ_WEIGHT` | 10 bits per queue, 3 a word |
| 235 | `PON_SID_GLB_TH` | global PBO thresholds, ON 28:16, OFF 12:0 |
| 236.. | `PON_SID_RPV_TH` | per-flow PBO thresholds, same fields |

There are 65 queues. T-CONTs 0-7 share queues 0-31 and T-CONTs 8-15
share 32-63; the queue mask holds the queue's number within its group of
32 (logical), so physical = 32 * (t / 8) + logical. T-CONT 16 is the OMCC,
on the default Alloc-ID, with queue 64: the module-load replay sets its
mask (+206 = 1), its enable bit (+207 = 0x10000) and its flow (64, valid,
on queue 64) before any OMCI.

- **cmd 21** answers with the T-CONT index of the Alloc-ID it is given,
  which is the row of the Alloc-ID CAM that holds it: the hardware's T-CONT
  n is the scheduler that serves the grants of the Alloc-ID in CAM row n.
  The row is looked up, never written: an Alloc-ID that no Assign_Alloc-ID
  (type 1) of the current activation placed is refused, and omcid binds the
  T-CONT once the assignment shows in /proc/odi_gpon/status. Only PLOAM
  arms the CAM, because a row decides which grants the ONU answers (G.984.3
  5.5.4: a T-CONT maps to an Alloc-ID "previously assigned to the given ONU
  via the PLOAM messaging channel"). The stock driver writes a row here,
  but only on a T-CONT Set; omcid also sends cmd 21 after every change of
  the PLOAM list, where a write would bring back an Alloc-ID the OLT had
  just taken away. Rows 0-15 are data T-CONTs; the OMCC's row 16 and rows
  17-31 (no scheduler) are refused. Word 0 goes in as the number of queues
  omcid will put on the T-CONT, and the driver reserves a block of that
  many consecutive logical queues in the T-CONT's group, first fit (the
  stock driver also first-fits; it would move the T-CONT to the other
  group when its own is full, where this refuses). Asking again for the
  same or fewer keeps the block, so the same MIB provisioned again lands
  on the same queues.
- **cmd 23**, an upstream queue, is ordinal `index` of T-CONT `owner`:
  logical queue = block start + ordinal. The first queue of a T-CONT sets
  its `PON_TCONT_EN` bit; a queue not yet in the mask is drained (a bounded
  poll of `DRN_CMD` bit 0; the driver warns and goes on if it never
  clears) and added to `PON_SCH_QMAP`. A strict queue gets type 0, then
  weight 1; a WFQ queue (`wrr`) its weight, then type 1. That order is the
  "+208 before or after +212" order the replay used to track. Then
  `PON_QID_CIR_RATE` and `PON_QID_PIR_RATE` from `cir` and `pir`, written
  one less except 0, 1 and the maximum, and a PIR of 0 with a CIR of 0 as
  no limit. Each write is a read-modify-write of the queue's field. The
  flows already mapped to the queue are made valid again.
- **cmd 24** deletes the queue: its flows out of `PON_SIDVALID`, a drain,
  its bit out of the mask, and the T-CONT disabled with its last queue,
  whose block is then free for cmd 21 to reserve again.
- **cmd 25** upstream with `ena` 1 maps flow `flow_id` to its GEM port
  (`US_GEM_PORT_MAP`), sets its `PON_SIDVALID` bit, recomputes the global
  PBO thresholds when the flow is new, and puts it on its queue
  (`PON_SID2QID`): ordinal `queue` of T-CONT `tcont`, refused when that
  T-CONT has no such queue. With `ena` 0 it is the stock delete: GEM port
  0 in the slot, the valid bit cleared, the thresholds recomputed.
  The thresholds for n upstream data flows are ON = 7904 - (n - 1) * s
  and OFF = 7744 - (n - 1) * s, with s the ON threshold of flow 0
  (`PON_SID_RPV_TH[0]`, 150 after the sdkinit replay) and ON no lower than
  2000 (OFF 1840). The five ISP1 pairs are its first five values.
- **cmd 0** (the stock resetMib, the first of the stock command enum)
  empties the upstream state: every upstream flow deleted as cmd 25 `ena`
  0 does, every queue of a data T-CONT deleted as cmd 24 does, every
  T-CONT block freed. The Alloc-ID CAM (PLOAM's), the OMCC (T-CONT 16,
  flow 64), the downstream GEM rows and the bridge connections are left.
  omcid sends it on a MIB reset (the OLT's, the CLI's, a re-registration)
  and when it starts again in the same boot without resuming, so the next
  provisioning never meets the last one's queue blocks or flows.

omcid changes only what differs from what it programmed: a T-CONT whose
Alloc-ID and queue at every ordinal are unchanged keeps its index and its
queues, and a queue whose weight, type, marking or rates moved is sent again
in place (cmd 23 on a queue already in the mask writes no drain); a flow is
sent only when its GEM port, T-CONT or queue changed. A T-CONT that changed
is torn down (its flows, then its queues) and built again alone. Only a
change of the PLOAM Alloc-ID list tears everything down, since the CAM
rows, and so the T-CONT indexes, may have moved.

The driver used to replay ISP1's words for these registers by T-CONT and
flow ordinal, which gave every other OLT ISP1's weights, drained T-CONT
4's queue when it set up T-CONTs 5 and up, put T-CONTs 8-15 on queues
their scheduler does not serve, refused a second queue on a T-CONT, took
omcid's flow delete for a create of GEM port 0, and numbered T-CONTs in
call order rather than by CAM row. Before that, the flow word was replayed
by flow id modulo 5, which unmapped flows 1-3 on an ISP with six upstream
flows (issue #42).

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
extension port mask. The stock default row words (for example
0x0003f8ff) are not decoded beyond the member and untag nibbles our code
uses, and a later chip of the family uses a different, wider layout. A service VLAN is 0x15 when the UNI sees it untagged
(members UNI and PON, untagged on the UNI: ISP1 VID 11, ISP2 stock VID
10) and 0x5 when the UNI sees it tagged (ISP1 VIDs 10, 12 to 14). With
VLAN filtering on, a frame whose VID row does not list the ingress port is
dropped at ingress: the likely reason ISP2 discarded every downstream
frame at the PON port while this table was replayed from the ISP1
capture, where VID 10 had no row until the ninth call.

A rule that passes tagged frames of any VID -- no VID in its filter, not
untagged-only, both tag actions transparent: forward-all, tagged-only or a
priority filter, all from the class 84 forward operation -- has no VID for a
row of its own, so while it is active every row 2..4094 carries its members
(UNI and PON, tagged, 0x5), ORed into the service rows; releasing it
rewrites every row without it (`odi_switch_bdgconn.c`,
`rule_passes_any_vid()`, pinned by `test/odi_switch_bdgconn_vlan_test.c`).
No capture has such a rule; whether the hardware drops a tagged frame on a
0 row with the final `VLAN_SETUP` of cmd 51 (`FILTER_ON` 0) is not
measured, and this keeps such a frame from depending on it.

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
`test/odi_reg_test.c` pins the order. The two words are read high, low,
high, and again (three tries at most) when the two highs differ: nothing
shows the hardware latching one half when the other is read, and a
low-then-high read across a wrap of the low word, about every 34 s at
line rate for the octet counters, came back 4 GiB too high. Should all
three tries see a wrap, the top bit of the low word tells which high it
goes with. Left unmapped, and refused: the
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
one bit with a read-modify-write. It also keeps the flag per GEM port, and
cmd 25 applies it when it writes the slot: one OLT sends Encrypted_Port-ID
right after O5, before any OMCI has created the port, and a flow cmd 25
writes again (every QoS or bridge rebuild, an OMCI MIB reset) gets no new
PLOAM. cmd 25 writes FLAGS once with the bit already in it, as the stock
flow add does; written bare and set after, the row would decrypt nothing in
between. The kept flags are forgotten when the ONU-ID is discarded; the next
activation configures the ports again. The table covers DS slots 0..63,
every flow cmd 25 accepts. Not reproduced from the capture: the CAM
rewrite around the bit (it writes the same GEM id back), and the interrupt
mask save and restore the stock handler makes around it.

The key itself is new for every Request_Key: the driver draws 16 bytes
from the kernel RNG (`hw_new_key()` in `odi_gpon_hw.c`), as G.984.3 clause
12.3 asks ("cryptographically unpredictable"). A request that arrives before
the RNG is initialised still gets a key, with a one-time warning, because
no key would make the OLT ask again and then deactivate the ONU.
`test/odi_gpon_aes_key_test.sh` runs it against the register model.

The OMCC port may be encrypted too (G.984.3 9.2.3.8 names any Port-ID): an
Encrypted_Port-ID for the OMCI port sets the bit on its row (FLAGS `0x14`,
`odi_gpon_hw_gem_port_encrypted()`), as the stock GPON module does, and a
Configure_Port-ID writes the row back unencrypted (`0x04`). Without it, an
OLT that encrypts the OMCC would leave every downstream OMCI frame
unreadable. No OLT seen so far encrypts it, so this path is host-tested
only. Like the stock module, the message's `b` bit is not read.

Setting the bit on every data port at creation was rejected: a port the
OLT does not encrypt would then be decrypted into garbage, and nothing here
could tell. Every OLT seen so far encrypts every data port.

## Host port PAUSE

Port 0 is the host port: the SerDes link to the host, in Fiber 1G
(1000BASE-X) with auto-negotiation on (`network.sh` sets the mode). The
stock image leaves the port's abilities to that negotiation, and the host
NICs seen so far advertise PAUSE both ways, so the port ends up able to
send PAUSE frames to the host. A live register dump of a stick on the stock
modules reads `PORT_ABILITY(0) = 0xfe`: 1000 Mb/s, full duplex, fibre, link
up, PAUSE received and sent, negotiated. This image forces the sending side
off and leaves the rest alone.

Why: when the upstream stops draining (no grants from the OLT, or garbage
in the Alloc-ID CAM, see [GPON boot init](#gpon-boot-init)), frames from
the host pile up in the switch, and the port sends XOFF after XOFF for as
long as the stall lasts. A host NIC that honours them stalls its transmit
queue; Linux declares a transmit timeout after 5 s and resets the adapter,
over and over (seen on an Intel 10G NIC behind both test sticks: about
100,000 and 5.9 million XOFF frames received, and adapter resets for as
long as the stick stayed stuck). An ONU drops at its own queue when its
upstream cannot drain, and TCP adapts; that is what the switch does when
the port does not send PAUSE.

The registers, one word per port, with the bit layout of the stock
register table:

| register | offset | role |
|---|---|---|
| `PORT_FORCE_ABILITY` | 0x000180 | the value a port runs with for each field forced |
| `PORT_ABILITY` | 0x000198 | read-only: the ability the port runs with |
| `PORT_FORCE_SELECT` | 0x0001b4 | one bit per field: 1 forced value, 0 negotiated |

In all three, bit 6 is `TX_PAUSE` (the port sends PAUSE), bit 5 `RX_PAUSE`
(it obeys PAUSE it receives), bit 4 the link, bits 1:0 the speed. The
select works field by field: the stock image forces the CPU port whole
(`PORT_FORCE_SELECT(3) = 0xfff`, `PORT_FORCE_ABILITY(3) = 0x16`: 1000 Mb/s,
full duplex, link, no PAUSE), and the host port not at all.

The platform settings write `PORT_FORCE_ABILITY(0).TX_PAUSE = 0`, then
`PORT_FORCE_SELECT(0).TX_PAUSE = 1`, value before select so the port never
runs on a stale forced value, each a read-modify-write of the one bit, and
log the ability the port then reads (read right after the write; the
line below is the one expected on a stick, not yet seen):

    odi_switch: host port PAUSE toward the host off: ability 0x0be (link 1, tx pause 0, rx pause 1)

cmd 32 writes `PORT_FORCE_SELECT(0)` at every provisioning; its leaf changes only the LINK bit
([Command leaves](#command-leaves)). `test/odi_host_pause_test.c` checks
both words after the platform settings, after the whole ISP1 provisioning
twice over, and through the parameter below.

What is left alone:

- **Receiving PAUSE.** `RX_PAUSE` stays negotiated, so a host that sends
  PAUSE is still obeyed. In practice the host sends none.
- **The advertisement.** A 1000BASE-X link negotiates PAUSE with two bits
  each way, and against a partner that advertises both, no advertisement
  of ours resolves to "receive only": clearing ours would turn PAUSE off in
  both directions and needs a renegotiation, a link flap. So the host still
  resolves PAUSE as on in both directions (`ethtool -a`); it is the port's
  own transmitter that never sends.
- **The buffer thresholds.** The switch has a second set of ingress
  thresholds for ports that do not send PAUSE, where it drops instead of
  pausing. On a live stick they hold the same values as the PAUSE ones
  (0x0230dc and 0x0230e4, the global pair, both on 0x200 / off 0x1f4
  pages; 0x0230f4 and 0x0230ec, the per-port pair, both on 0xf0 / off 0xea),
  so the host port now drops frames where it used to pause the host. No
  drop policy of our own is added.

`odi_switch.host_pause` is the escape hatch, a kernel parameter (default
`N`). Set to `Y` it clears the select bit and the port goes back to the
negotiated PAUSE, as the stock image runs; on the command line it takes
effect with the platform settings, and written to
`/sys/module/odi_switch/parameters/host_pause` on a running stick it is
applied at once (both directions, idempotent).

To see it on a stick: `register get 0x198 1` in `diag` reads
`PORT_ABILITY(0)` (bit 6 clear: the port does not send PAUSE); the
exporter's `gpon_port_pause_frames_total{port="0",direction="transmit"}`
(`dot3OutPauseFrames`) counts PAUSE frames the port sent, and stays flat;
the host's `ethtool -S` `rx_flow_control_xoff` stays flat. The meaning of
bit 6 as the sending side (bit 5 receiving) is the one thing here not yet
seen on hardware: that is what these three readings check.


## Host port auto-negotiation

The host port's SerDes runs in Fiber 1G (1000BASE-X), and its PCS runs the
clause-37 auto-negotiation with the host: `HOST_FIBER_CTRL` (0x021c00) reads
`0x1140` on every stick seen, stock or ours, and nothing in this image
writes its `AUTONEG` bit unless asked. Most hosts negotiate. A host whose SFP
port has auto-negotiation off (some routers offer it per port) sees no link
from a negotiating stick, so `HOST_AUTONEG=off` forces the PCS to 1000BASE-X
full duplex instead (`docs/SETTINGS.md`, "Host port auto-negotiation").
OMCI does not reach this word: the PPTP auto-detection an OLT sets (class 11)
is a UNI PHY ability for cmd 30, and the host SerDes has no such PHY.

`HOST_FIBER_CTRL` is laid out like a clause-22 control register:

| bit | field | 0x1140 |
|---|---|---|
| 15 | `RESET` | 0 |
| 14 | `LOOPBACK` | 0 |
| 13 | `SPEED_LSB` | 0 |
| 12 | `AUTONEG` | 1 |
| 11 | `POWER_DOWN` (the one bit `network.sh` writes, to 0) | 0 |
| 10 | `ISOLATE` | 0 |
| 9 | `AUTONEG_RESTART` (self-clearing) | 0 |
| 8 | `FULL_DUPLEX` | 1 |
| 6 | `SPEED_MSB` (with bit 13: 10 = 1000 Mb/s) | 1 |

The bit layout is the one of the stock register table for this block. The
same table has a second candidate, a two-bit "force auto-negotiation" field
(bits 9:8) of another host SerDes word at 0x021808, never read on a stick.
This image uses bit 12 of the PCS control word, whose meaning is the
standard clause-22 one, and touches 0x021808 in no path. On a stick it
gives a working forced link: with the bit cleared (0x1140 becomes 0x0140)
the port keeps reporting the link of a forced PCS, and against a host whose
SFP port has auto-negotiation off a forced stick carries traffic where a
negotiating one gets a link and none. The other candidate was never needed.

`odi_sw_host_autoneg_apply()` (`odi_switch_port.c`) is one read-modify-write
of bit 12: cleared for off (`0x1140` becomes `0x0140`: no negotiation, 1000
Mb/s full duplex, the speed and duplex bits as the mode set left them);
set, with `AUTONEG_RESTART`, for on (`0x0140` is written `0x1340` and reads
back `0x1140`), so the PCS negotiates at once. Nothing is written when the
bit already holds the wanted value, since a restart flaps a working link. It
logs the word before and after:

    odi_switch: host port auto-negotiation off (forced 1000BASE-X full duplex): control 0x1140 -> 0x0140, ability 0x0fe

The port side (`PORT_FORCE_SELECT(0)`, [Host port PAUSE](#host-port-pause))
is left as it is: the port keeps taking its link, speed and duplex from the
PCS. Whether `PORT_ABILITY(0)` reports the link of a forced PCS the way it
reports a negotiated one is the first thing a stick trial checks; if it does
not, the port side needs forcing too (link, 1000, full duplex in
`PORT_FORCE_ABILITY(0)` and their select bits, as the stock image forces the
CPU port).

Two kernel parameters, under `/sys/module/odi_switch/parameters/`:

- `host_autoneg` (bool, default `Y`, 0644): written, it applies at once, one
  register write, under `odi_switch_lock`. On the command line it is stored
  and the platform settings apply it (a step that reads and writes nothing
  while it is `Y`, so the boot of a stick that never sets it is unchanged:
  the boot goldens do not move). Not persistent: userland decides what each
  boot runs, from `HOST_AUTONEG` (`host-autoneg.sh`, `docs/TOOLS.md`).
- `host_link` (read-only): `1` while `PORT_ABILITY(0).LINK_UP` is set, `0`
  otherwise, read when asked. What `host-autoneg.sh` and the web UI read to
  decide whether a mode works.

`test/odi_host_autoneg_test.c` checks the writes in both directions, that a
second apply writes nothing, that the other bits survive, that the default
platform settings neither read nor write the word, and the link read.

To see it on a stick: `register get 0x21c00 1` in `diag` (bit 12), `cat
/sys/module/odi_switch/parameters/host_link`, and on the host `ethtool
<if>` (`Auto-negotiation: on|off`, `Link detected`).
