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

  The L2 table is emptied after the platform steps, from
  `odi_switch_boot_init()` (`odi_switch_l2_clean()`): the table, the CAM
  included, survives a warm restart, so the static groups of the stock
  firmware, an earlier pin of a MAC that changed or a row in the CAM would
  still be there. The CAM is turned on first (the **l2** step below),
  because a row in the CAM is out of reach with it off. Every row the
  engine reports in use is deleted by its own words (below), learned and
  static, in up to two passes, and the log says what was found:

      odi_switch: L2 table clean: 3 row(s) (1 static) left by the previous run removed

  Learned rows come back as traffic flows, and the stick's own pins are
  written again at the end of the boot (`odi_switch_pin_sync()`). The flush
  (`L2_TBL_FLUSH_CTRL`, `0x017024`) is not used: it keeps static rows.
  A row left after the second pass is logged as a warning and the boot goes
  on.

  A delete is written with the words the row itself reads back, VALID
  cleared, not with the key alone. Another open implementation measured
  that a hashed delete removes a row only when the rest of the word agrees,
  and that a key-only delete left a row behind the CPU port in place; the
  stick's pin is such a row. Multicast deletes (igmpd) still send the key
  alone, which was checked on a stick.
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
- **l3 off**: `L3_GLOBAL_CTRL` (0x01106c) bit 0, the L3 engine's enable,
  cleared when set. It reads `0x4` (off) after a power-up and on a stick
  running this image; it survives a warm restart, so a firmware that
  enabled the engine would leave it routing. Nothing is written when the
  bit is clear. The L3 tables are not touched: with the engine off they are
  not read.
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
- **pon isolate**: the CPU port and the extension ports cleared from the
  PON port's isolation mask (`PORT_ISOLATION`, port 2), so a frame from
  the fibre is never forwarded to the CPU. Ours, not the stock image's;
  see [PON port isolation](#pon-port-isolation).

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
| 53 | setMacLearnLimit | `odi_sw_l2_learn_limit_set` | `odi_switch_port.c` |
| 62 | setAgeingTime | `odi_sw_l2_aging_set` | `odi_switch_port.c` |
| 67 | setUniPortRate | `odi_sw_uni_police_set`, `odi_sw_uni_shape_set` | `odi_switch_port.c` |
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
  port 0 for a queue with no drop precedence marking. That word is all the
  stock driver writes for a downstream queue; the schedule of the queue
  (strict or weighted, the weight) is ours, on top of it:
  [Downstream queues](#downstream-queues). An upstream queue,
  and cmd 21, 24 and the upstream side of cmd 25, are the upstream PON
  queues, below.
- **cmd 25** downstream is the DS GEM port CAM row and its
  `DSF_GEM_FLOW_TYPE` word (FLAGS 2, Ethernet, plus bit 0, multicast, when
  omcid sets `mcast_filter`: the GEM port of a multicast GEM interworking TP,
  as the stock OMCI layer flags it; ISP1's is 0xfff, ISP2's 0xffe), plus the slot record the AES path reads (below). `ena` 0 (a
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

### UNI limits

Commands 53 and 67, and what became of the rest of the commands that used
to be refused. omcid sends them only when the OLT provisions the matching
attribute, so a port nobody set keeps the boot replay's state (every port
may learn 1088 addresses, no rate). Registers read on a stick and
checked against another open implementation's measurements of the same
chip:

| what | register | boot state read |
|---|---|---|
| learning limit of port p | `0x017008 + 4 * (p / 2)`, 11 bits at `11 * (p % 2)` | `0x00220440`: 1088 for ports 0 and 1 |
| addresses port p has learned | `0x017010 + 4 * (p / 2)`, the same packing | `0x2` and `0x1` with two on the UNI, one on the PON port |
| action past the limit | `0x01c00c`, 2 bits a port | 0, forward the frame without learning it |
| ingress meter of port p | `0x020010 + 0x400 * p`, bits 20..2, 8 kbit/s units | `0x001ffffe`: 0x7ffff, none; bit 1 is flow control |
| egress shaper of port p | `0x02d804 + 4 * p`, bits 19..1, 8 kbit/s units | `0x000ffffe`: 0x7ffff, none |
| second shaper register, port 0 only | `0x02d8b8`, bits 18..0 half the rate in 16 kbit/s units, bit 19 the 2.5G override | `0x0003ffff` |

- **cmd 53**, `{port, limit}`: the limit field of the port, a
  read-modify-write (the other port of the word is kept). A limit at or past
  the table size is "none". 0 is refused, not taken as "learn nothing": it
  is learning off, which nobody asked for. The action register is never
  written, so a frame from an address past the limit is still forwarded,
  only not learned; a drop there would make the limit an outage. The PON and
  CPU ports are refused: a limit on either would cut OMCI or the stick's
  management.
- **cmd 67**, `{port, dir, kbit/s}`: direction 1 (the ME 47 inbound
  descriptor, traffic entering the bridge from the UNI) is the ingress
  meter, direction 2 the egress shaper. The shaper of port 0 only enforces
  its rate when `0x02d8b8` holds half of it (six rates from 8 kbit/s to
  20 Mbit/s all ran at line rate with the first register alone, on the other
  implementation's board), so both are written, the override bit left as it
  is. "No limit" is exactly the reset words. A rate below 1 Mbit/s is
  refused and the port keeps its previous one: such a descriptor is not a
  service plan, and a limit that low would cut the host and the stick's own
  management off, so a refusal fails open. The rate unit of the driver
  (8 kbit/s) is the register's, so the kbit/s are divided by 8 and a rate
  that is not a multiple is rounded down.
- A MIB reset (cmd 0) puts back, on the ports whose limits were set, the boot
  state; the stock stack puts the learning limit back to a global default on
  a bridge port delete, which omcid does not recover, so the reset is where
  the limits end.
- **cmd 61 and ME 50.** `getBridgeTableByPort` returns the number of unicast
  rows the L2 table holds for a port. The stock driver hands the rows
  themselves through a mapped buffer of its character device, which a
  netlink command cannot do, so they come through a command of ours,
  `OMCI_BRIDGE_TBL_CMD` (0x101, `uapi/omci_bridgeport.h`): a page of 30
  entries at a time, each the 8 bytes ME 50 carries (an information word of
  age in seconds, learned or static, blocking; then the MAC). The age is the
  row's 3-bit counter times the ageing time over 7, as the stock driver
  scales it. omcid reads it on every Get of ME 50's table attribute, so the
  OLT sees what is learned then; which port is asked follows the bridge
  port's TP: the UNI, the CPU port for an IP host, the PON port for anything
  else. The OLT thereby learns the MAC addresses behind the stick, which is
  what ME 50 is for.
- **cmd 63** is the stock driver's "the UNIs bridge to one another": it
  rewrites the isolation masks of the UNIs, the very masks
  [PON port isolation](#pon-port-isolation) owns. 1, which is what is
  already true, succeeds without a write; 0 is refused.
- **cmd 40** with a pause time of 0 (flow control off) succeeds, the host
  port already never sends PAUSE; anything else is refused, that is the
  operator's switch (`odi_switch.host_pause`), not the OLT's.
- **Still refused**, each with its reason in the log line of the command:
  34 (max frame size: the switch has two frame-length profiles, profile 1
  is the PON port's at 2031 bytes and the host, UNI 1 and the CPU port
  share profile 0 at 16368, so a per-UNI limit would move the CPU's), 52
  (DSCP remap; the path from the priority to the P-bit of an upstream frame
  is not understood), 54 and 55 (MAC filters, ACL rules; omcid does not send
  them), 56 (S-VLAN TPID, not sent), 59 and 60 (the dot1 rate limiters, an
  ACL rule and a meter per GEM port, not built; omcid sends them for class
  298) and 71 (UNI QoS, not sent).

### Downstream queues

The queues of the UNI port, toward the host: ME 277 rows whose RelatedPort
names the UNI (entities 0..7 on a stick, priority 7 down to 0; the next
eight, the VEIP's, are not scheduled). The OLT provisions them with Weight,
and omcid sends each row as cmd 23, direction DS, when the OLT sets it.
The stock driver writes `PORT_QUEUE_MAP` for it and nothing else (the same
word for ISP1's eight rows, whatever their weights), so before this the UNI
had all eight queues strict, weight 1, whatever the OLT asked. Registers,
read on a stick and checked against another open implementation's
measurements of the same chip, for port p and queue q:

| what | register | boot state read |
|---|---|---|
| priority to queue | `0x01c0b0 + 4p`, eight 3-bit entries, priority i at bits 3i+2..3i | port 0 `0x00fac688`, the identity (the stock init writes it; the chip's reset is 0, every priority to queue 0) |
| queue type | `0x02d85c + 4p`, bit q set = weighted | 0, all strict |
| weight of queue 0 | `0x02d824 + 4(p/2)`, 16 bits, an odd port in bits 31..16 | 1 |
| weight of queues 1..7 | `0x02d82c + 12p + 4((q-1)/3)`, 10 bits, three to a word | 1 |
| weight unit | `0x02d800` bit 17 clear: bytes (set: packets) | `0x00003fff`, bytes |

The scheduler serves every strict queue before any weighted one, the
highest queue number first among the strict ones, and shares what is left
among the weighted queues in the ratio of their weights. Two weighted queues
split the bytes as their weights, with a granularity (fields 1 and 3 gave
29/71 %, 4 and 12 gave 25/75 %), which is why the driver writes four times
the OMCI weight: 255, the largest, is 1020, inside the 10 bits.

What cmd 23 does with a downstream queue, after the stock write:

- the queue is number `index`, the low 16 bits of RelatedPort, of the UNI
  port `owner`. `index` of 8 or more, an owner that is the PON or CPU port
  (omcid names the PON port for a VEIP queue, as its flow does, so that it
  cannot overwrite a PPTP queue of the same priority) or a port that does
  not exist: nothing is scheduled, and the call succeeds as before.
- Weight 2 or more is weighted, at four times the weight; Weight 0 or 1 is
  strict at the reset weight. That is the stock driver's own reading
  (`wrr = Weight >= 2` in the descriptor it builds) and the only one the
  hardware can express: it has no weighted queue above a strict one.
- a register is written only when it changes, so rows at the default write
  nothing and an OLT that provisions nothing leaves the boot state alone.
- the port's priority map is made the identity if it is not, once, so that
  a queue's number is the priority of the frames that reach it.
- a MIB reset (cmd 0) puts every queue it scheduled back to strict at weight
  1. The priority map stays as it is.

Which number is served first is read from the OLT, not from a spec: the
OLT of ISP1 (and the one on the stick this was read on) gives Weight 0 to entities 0 and 1 (priorities 7 and 6) and
30, 24, 19, 14, 9, 4 to entities 2 to 7 (priorities 5 down to 0). Read with
the queue number as the priority, that is two strict queues on top and then
weighted queues whose share falls with the number, which is what such a
template is for. The other reading (OMCI priority 0 first, so queue 7 - p)
puts the two strict queues below the weighted ones, and the hardware serves
strict first, so it would starve the queues the OLT gave the weights. The
flow descriptor's `ds_prio` (cmd 25) is that other reading, the stock
number, and the switch driver does not use it.

**What this does not do: choose the queue of a frame.** The queue a frame
takes is the one its internal priority maps to, and the switch gives a frame
from the PON port its priority from its source weights (`0x01c0f4`): on a
stick the port default (`0x01c0f0`, 0) is the only source with a weight for
that port, so every downstream frame has priority 0 and takes queue 0. The
queue pointer of a GEM port (ME 268, DsPriQPtr) is not looked at by
anything between the GEM port and the queue; a read of the per-queue page
counters (`0x02d038 + 4(8p + q)`, peak in bits 25..16) on a stick after
traffic shows port 0's queue 0 and no other queue of that port used. Every
OLT seen points all its GEM ports at entity 0, so no frame is meant for any
other queue yet, and the schedule is in place for when one is. Steering
(a downstream CF row per GEM port assigning the priority, or the PCP as
a source) is not built: it changes what a frame's priority is on the path of
all service traffic, and the weights of the sources that decide it are
shared with the upstream direction.

`odi_switch.ds_sched=N` turns the schedule off (the stock driver's
behaviour); it is read at each cmd 23, so a write to
`/sys/module/odi_switch/parameters/ds_sched` covers the next provisioning
and does not undo what is programmed. `test/odi_switch_dsq_test.c` covers
the registers and the boot golden shows the stream.

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

### Extended VLAN rules

The extended VLAN tagging operation of class 171 reaches the switch as
cmd 51 descriptors of `rule_gen` `OMCI_VLAN_OPER_EXTVLAN_GEN` (7,
`uapi/omci_bdgconn.h`). Each carries **one direction**: omcid works out the
downstream rule of an entry (the "inverse" of G.988, or what DsMode says)
itself, from `src/omci/extvlan.c`, and sends it as a second descriptor, so the
driver derives nothing here. The two halves of the rule are the two tag
positions the classifier can act on: the "outer" half is the S-tag (TPID
0x88a8) and the "inner" half the C-tag (0x8100). A descriptor that names a VID
or priority on both, that is not one direction, or that asks an S-tag action
without the S-tag priority, is refused (`-EOPNOTSUPP`, nothing written). Rows
of these rules never use the bottom region: the ordering class is how much the
row names (VID and priority 0, one of them 1, neither 2), so the more
specific row has the lower index, which is the one that wins.

What the rows rely on, and where each fact comes from. "Read" is a register
read on a stick on the current image; "other" is another open implementation's
measurement on the same chip, with its own lab OLT, which agrees with our
stock register table where the table says anything.

| fact | source |
|---|---|
| `CLASSIFY_TEMPLATE(t)`, 0x016030 + 4t, t 0..1: the tag a rule takes VID and priority from, per direction (US_VID 1:0, US_PRI 3:2, DS_VID 5:4, DS_PRI 7:6; 0 the C-tag, also the one behind an S-tag, 1 the S-tag). Both words read 0, so every rule compares the C-tag until template 1 is written | the stock register table (two words, four 2-bit fields); read; the meaning of the values: other |
| `CLASSIFY_PATTERN_SEL`: the row's template bit, 32 rows to a word. Written for every row, and now carries the bits of the 31 rows beside it | table; other. All words read 0 |
| template 1 is written 0x55 (S-tag, both directions) when the first row that compares the S-tag goes in, and never otherwise, so a line with no such rule sees the register writes it always saw | ours |
| per-row action classes `CLS_ACT_EN(i)`, 0x016038 + 4i: bit 5 stream, bits 4:0 the rest. Read 0x3f on every row looked at (0..15, 64..87, 240..255) | table; read |
| C-tag action 1 ("tag") adds a tag to an untagged frame and replaces the VID of one that has it; the VID is the assigned one when the VID source (bit 18) is set, the priority the assigned one when its source (bit 21) is, **else the one the frame had**; 2 removes the tag; 0 and 3 leave it. A translation of C-VID 410 to 1410 reads back as action word 1 0x00042c12 there and is written the same here | other |
| S-tag action 1 puts an S-tag (TPID `SVLAN_TPID_CFG` primary, 0x88a8) on a frame without one and replaces the S-tag of one with one; 2 rewrites the S-tag into a 0x8100 tag; 3 removes it; 0, 4 leave it (5 acts as 1). VID and priority are assigned under their source bits (21, 18). Rows here use 1, 3 and 4 | other |
| the S-tag is only recognised on a port in S-VLAN uplink mode: `SVLAN_UPLINK_PORTS` (0x0230c4) reads 0xf, every port | read |
| `SVLAN_TPID_CFG`: primary 0x88a8, second 0x8100 disabled (0x0230c8 reads 0, 0x0230cc reads 0x810088a8). So an added or compared S-tag has TPID 0x88a8 and nothing else (0x9100 is refused) | read |
| the classifier acts on the lowest-numbered matching row | other (an operator OLT's pass-through at a lower index shadowed a translation) |
| a frame forwarded to the PON port with no stream is dropped silently in the PON MAC, so a rule up the PON names its stream (`us_flow`) like every other upstream row, and a frame no row matches has no stream | other |
| `PON_EN` (`CLASSIFY_SETUP` bit 3) must be set for the C-tag actions to apply to frames crossing the PON port. Reads set (0x1009) | other; read |
| rows from the boundary in `CLASSIFY_SETUP` 12:5 (`PATTERN1_COUNT`, reads 128) up are matched with a second rule layout; the fields our rows use sit in the same place in both, the S-tag template bit is not known to work there, so such a row is not placed at or past the boundary (`cf_insert()` refuses with "classification table full") | other; read; the placement is ours |
| `CLASSIFY_SETUP` bits 1:0 apply to downstream frames too (value 2 drops those no row matches). We write 1 | other |

A frame with two C-tags cannot be told from one with a single C-tag, so a rule
on a double-C filter is refused by omcid before it gets here.

#### Shapes

Every shape of a class 171 entry (G.988 9.3.13), and what the switch does with
it. "Now" is what this firmware does; the unproven ones use only registers
and fields known from the table above, but in a combination nobody we have
read has measured.

| shape | now | why |
|---|---|---|
| untagged in, add one C-tag, VID and priority assigned, TPID 0x8100 | done, as before (the manual-VLAN rule; a table of only this and plain defaults keeps the old path byte for byte) | |
| untagged in, add one S-tag (Output TPID 0x88a8), VID and priority assigned | done | S-tag action 1 |
| untagged in, add an S-tag outside a C-tag | done, **unproven**: both planes in one row | |
| single C-tag in, VID named (priority named or not), replace the VID (or priority) with an assigned one, priority kept or assigned | done | C-tag action 1 |
| the same with the priority named in the filter and copied: one VID, several priorities, one rule each (the operator vector) | done | each rule is a row, the more specific lower |
| single C-tag in, VID named, remove it | done; the way back tags untagged frames with that VID | C-tag action 2 |
| single C-tag in, push an S-tag in front | done | S-tag action 1 |
| single C-tag in, replace it with an S-tag, or an S-tag and a C-tag | done, **unproven** (two planes in one row) | |
| single S-tagged in (Input TPID 0x88a8), replace, pop, or replace with a C-tag | replace and pop done; S-to-C (action 2) **not done**, the register is known | |
| S-tag over a C-tag in, filter on either tag (not both), pop, replace or push | done | |
| both tags removed, either one replaced, one or both pushed | done, **unproven** when both planes change | |
| filter on the S-VID and the C-VID of one frame | refused: the classifier compares one tag per row | needs a second compare nobody has measured |
| double C-tag (both 0x8100) | refused: a frame with two C-tags cannot be told from one with one | |
| filter or treatment TPID 0x9100, 0x9200 | refused | the second S-VLAN TPID register (0x0230c8) reads disabled; nothing says it can be used |
| filter on the DEI bit, or an added tag with DEI 1 | refused | no DEI field in the classifier is known |
| EtherType filter (IPoE, PPPoE, ARP, IPv6) | refused | the EtherType field of a rule (bits 47..32) is not known |
| VID copied from a tag whose VID the filter does not name (any-VID priority remap) | refused | whether C-tag action 1 with no VID source keeps the VID is not known |
| priority copied between the two tags, or from the DSCP | refused unless the source is named in the filter, then done with its value | the source codes other than keep and assign are not known; DSCP needs a DSCP match |
| entry that discards what it matches (RemoveTags 3, not a default rule) | refused | the classifier drop action is not known |
| default rules: single-tag discard | done: tagged frames the table does not name are not passed | no row matches them, so they have no stream and are dropped |
| default rules: double-tag discard, or a default that adds or removes a tag | discard ignored (not enforced); the other refused | |
| downstream, DsMode 0 | done, the inverse: a tag the rule added is removed, one it removed is put back with the filter VID and priority, one it replaced is replaced back; a rule that merged frames (a filter that did not name the VID) has no downstream rule | |
| DsMode 1 | no downstream rule (forwarded unchanged) | |
| DsMode 2, 3, 4 | done: the inverse on VID and priority, VID, or priority only | |
| DsMode 5, 6, 7 | as 2, 3, 4, and logged that what they discard is forwarded | no drop action |
| DsMode 8 | no downstream rule, logged | no drop action |

A table is applied whole: one entry in the refused rows leaves every frame as
it comes and the log names the entry and the reason once.

#### What is not known

- the classifier's drop action (so no entry that discards what it matches, and
  no downstream discard of DsMode 5 to 8, is carried out);
- the EtherType field of a rule, bits 47..32, which no capture or measurement
  has cared about;
- the VID and priority source codes other than "keep" and "assign" (so no
  priority copied between tags, and none from the DSCP);
- whether a C-tag action 1 with no VID source keeps the VID (so a priority
  remap whose filter does not name the VID is refused);
- what `SVLAN_POLICY` does to an S-tagged frame whose S-VID has no VLAN row
  (rows for the VIDs a rule matches are made, tagged on both ports).

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

## Multicast

How a group the OLT sends on the multicast GEM port reaches the host, and
what IGMP snooping adds. The data path needs no programming: a frame on the
multicast GEM port is flagged by the downstream flow's type (below), the
GPON MAC passes it, and the switch floods it to the host unless a row or a
drop says otherwise. Snooping is opt-in (`IGMP_SNOOPING`, `docs/TOOLS.md`,
`igmpd`) and only prunes.

What was read on a stick (ISP1, read-only) and what is taken from another
open implementation's measurements on the same chip. Nothing below was
exercised with a multicast stream: neither of our lines carries IPTV.

- **The downstream flow's multicast flag.** omcid sets bit 0 of the flow's
  `DSF_GEM_FLOW_TYPE` word when a multicast GEM interworking TP (ME 281)
  that points at its CTP holds an address row, whatever the port number. ISP1
  reads `0x3` (multicast and Ethernet) on its flow for GEM port 4095. The
  GPON MAC applies its multicast filter only to flows with that bit.
- **The GPON multicast filter passes everything while its table is empty.**
  `GPON_GEM_DS_MC_CFG` (0x704080) reads `0x59` on ISP1 (the boot table writes the same word on both lines): bit 0 set, the
  table lists the multicast to drop, and it is empty; bits 3, 4 and 6 are
  FCS check, unicast pass and broadcast pass; no IPv4 or IPv6 override.
  Another implementation measured exactly this configuration passing 224.0.0.1,
  239.2.2.1 and ff02::1 on a multicast-flagged flow, and the opposite
  configuration (an including filter with an empty table) dropping all of
  them. So the stock stack's rows (ME 281 range to the filter table) are
  not needed for the stream to arrive, and nothing writes the 256-row table
  (it is emptied at boot).
- **Flooding.** A group no row names is a lookup miss and goes by
  `FLOOD_UNKN_MCAST_PORTS`: `0x7` on ISP1 (the module-load replay
  writes `0xf` and `odi_switch_init_modload()` takes the CPU bit out on
  purpose; see above). So the stream reaches the UNI and port 1, never the
  CPU, which is what the CPU-RX guards want from a stream of thousands of
  frames a second. The PON port's isolation mask (`0x7`, never the CPU)
  decides where it may go from there.
- **IGMP is data to the switch.** `IGMP_PORT_ACTION` (0x01105c, a word a
  port) reads `0x400` on ports 0 to 2: every IGMP and MLD type is forwarded.
  Reports and queries cross the stick as plain multicast, so with no
  snooping the OLT sees every report and the host every query.

### Snooping

With the lease held (`odi_switch_mcast.c`, `ODI_SW_IOC_MC_SNOOP`):

- **The copy.** ACL entry 8, template 6 (EtherType in slot 0, IPv4 protocol
  in slot 1; the platform step empties the tables and the replayed rule is
  entry 1 on template 3, so both are free), ingress port 0 only, EtherType
  0x0800 and protocol 2, action "mirror to the CPU port", action class
  enable `0x10` (the forwarding decision alone). A rule acts on the frames of
  its own port and on no other (another implementation measured a drop on
  each of the two ports leaving the other's frames alone), so a frame from
  the PON never matches it. The copy arrives on the CPU port with source port
  0; `odi_omci.c` (an RX hook registered for the UNI port when the lease is
  first taken) recognises it by that and by IPv4 protocol 2 and hands it
  to igmpd, and drops it if nobody is registered. It does not reach the IP
  stack. The original frame is forwarded as if there were no rule.
- **Rows.** One static L2 multicast row a group (SVL, filtering id 0, the
  keys `docs/KERNEL.md` describes), the UNI and the PON as members. The PON
  has to be one: a report to a group is forwarded to the row's members, and
  another implementation measured that without the PON in the row the report
  "goes nowhere but back" and the OLT never sees it. The CPU is never one.
- **The drop.** `MC_UNKNOWN_IP4` (0x01c014), two bits a port, set to drop
  (1) for port 2: IPv4 multicast from the PON that no row names is dropped
  instead of flooded. `MC_UNKNOWN_CFG` (0x01c01c) bits 4:3 are set to 1 with
  it, which exempts 224.0.0.0/24: ISP1 reads `0x20` there (the reset value
  is `0x28`), without which the OLT's general queries to 224.0.0.1 would be
  dropped as unregistered, the host would never report, and the stream would
  lapse. Both are put back as found when the drop is released. IPv6 is not
  touched. igmpd turns the drop on only after one membership interval, so
  every group the host already watches has reported again.

The lease is held by the open file that takes it: closing it, or the daemon
dying, releases the copy and the drop and deletes the rows that file wrote
(up to 128). A second holder is refused with EBUSY. `odi_switch_init_modload()`
and the isolation masks are not touched, and `test/odi_switch_mcast_test.c`
checks the rule by what it matches (the UNI port and IGMP, nothing else, the
PON least of all).

What is not known. The copy rule follows another implementation's decoded
layout and the one rule the stock replay writes; it has not been run here,
and neither has a frame been matched against a static multicast row (the row
layout was checked by reading it back, `docs/KERNEL.md`). Whether the
lookup is keyed on the MAC or on the group address follows
`L2_LOOKUP_SETUP` bit 23, which reads 0 here (MAC); another implementation
measured the MAC-keyed row steering the frames. Those are what a first run
with a real stream has to check, with `diag l2-table get all`, the port
counters (`diag mib dump counter port all`, ports 0 and 2) and
`/proc/odi_omci` (`igmp_delivered`, `igmp_dropped`).

## PON port isolation

Nothing that arrives from the fibre reaches the stick's own IP stack: not
the web UI, ssh, the exporter, nor an ARP or ping answer. Two independent
layers hold this, each enough on its own, and neither depends on what an
OLT provisions, on VLANs or on tags: both key on the port a frame entered
by. OMCI is the one exception, and it is not Ethernet forwarding: the OLT's
management channel arrives as a trapped frame (reason 246, the OMCC stream)
that `odi_omci.c` hands to omcid, never to a network interface.

### Why two layers

What stood between the fibre and the CPU before: the switch forwards a
frame for the stick's MAC (learned on the CPU port) to the CPU, and floods
broadcast to the CPU (`FLOOD_BCAST_PORTS` 0xf, see
[Module-load replay](#module-load-replay)); the port isolation masks were
all open, as on the stock image; and the kernel handed any frame from the
PON port to `eth0`, up but outside `br0`, where the IP stack still answered
for `br0`'s address (a Linux host accepts a frame for any of its addresses
on any interface, and answers ARP for it). What remained was the
classifier: every downstream service row of
[Bridge connections](#bridge-connections) filters the egress by a port
mask that never holds the CPU, and row 255 forwards whatever no service row
matches to the port mask its action carries, which is empty. That this
removes the CPU from a lookup result is read from the field layout, not
measured, and it is one layer only.

### The switch: the PON port's isolation mask

`PORT_ISOLATION` holds, per ingress port, the ports a frame from it may be
forwarded to (bits 3:0, bit n = switch port n) and the CPU-side extension
ports (bits 10:4). Two 11-bit items share a word: port n is in the word at
0x027000 + 4 * (n / 2), at bit 11 * (n % 2). The register table of the
stock binary gives the width and the port range (11 bits, ports 0-3), and
puts the extension-port table at 0x027008; a word per port would put ports
2 and 3 on it. A stick reads `0x003fffff` in both words (two fields of
0x7ff, every port open) and `0x000007ff` in the last word of the extension
table (one field alone), on the stock modules and on this image before the
step below.

The platform step "pon isolate" (`odi_sw_pon_isolate_apply()`,
`odi_switch_port.c`) clears the CPU bit and bits 10:4 of port 2's field,
and the PON bit (2) of port 3's field ([Frames the stick
sends](#frames-the-stick-sends)), in one read-modify-write of the word the
two ports share: `0x003fffff` becomes `0x003fd807` (port 2 0x007, port 3
0x7fb), and logs

    odi_switch: PON port isolation on: the PON port forwards to 0x007 (never the CPU), the CPU port to 0x7fb (never the PON by lookup)

It runs before omcid starts, so before any service exists. The host still
reaches the CPU and the fibre, the CPU still reaches the host, and a frame
from the fibre still reaches the UNI. Isolation filters the
egress whatever chose it, a flood or an L2 hit; that it filters a trap to
the CPU as well is suggested, not measured, by the chip's per-protocol
isolation "leaky" controls (reserved multicast, IP multicast), which
exist to let trapped control frames through it.

Open: OMCI is itself a frame from the PON port trapped to the CPU. Whether
isolation applies to that trap is not known; if it did, OMCI would stop
with the step on. The trial checks it first (below), and
`odi_switch.pon_isolate` is the way back:

- `pon_isolate` (bool, default `Y`, 0644): `N` on the command line leaves
  both masks open; written to `/sys/module/odi_switch/parameters/pon_isolate`
  after the platform settings it is applied at once, one register write,
  in either direction. The NIC layer below holds either way.

### The kernel: frames from the PON port are dropped

The CPU tag of every received frame carries the port it entered the switch
by (`odi_nic_hw.h`, the source port of RX word 3). That the field is right
is shown by function, on both ISPs: OMCI replies are sent to the source
port of the request and reach the OLT, and host frames are delivered to
`eth0.2` by source port 0.

`odi_poll()` (`odi_nic.c`) hands a frame to the RX hooks first, as before;
`odi_omci.c` takes the OMCI frames. A frame from the PON port that no hook
took is dropped there, before any net_device sees it, whatever its type.
`/proc/odi_nic` counts both, one line each (`rx_pon <n>`,
`rx_pon_dropped <n>`).
`rx_pon` is every frame from the PON port (OMCI included), `rx_pon_dropped`
the ones dropped; with the switch layer in place it stays 0, and `rx_pon`
equals the OMCI frames of `/proc/odi_omci` (`frames_delivered` +
`dropped_unregistered` + `unicast_fail`). A frame dropped here also counts
as `rx_dropped` of `eth0`. There is no switch to turn it off: nothing on
the stick wants a frame from the PON other than OMCI. The one other RX hook,
the copy of the host's IGMP to igmpd (`odi_omci.c`), is registered for the
UNI port alone, and only once igmpd first takes its lease, so it never sees a
frame from the PON and costs nothing on an image that never snoops; an IGMP daemon that
wanted the OLT's queries would need a hook for the PON port, which is a
decision about this rule and not a detail of a daemon. There is no
netfilter in this kernel, and only the driver knows the ingress port.

### Frames the stick sends

The rule is that the only frames the CPU may send toward the fibre are the
OMCI replies. The NIC transmits two kinds of descriptor:

- the OMCI replies (`odi_omci.c`, through `odi_nic_tx_words()`, its only
  caller): `PORT_SEL`, `NO_LEARN` and bit 25 in word 0, the PON port's bit
  and the OMCC stream in word 3. `PORT_SEL` is what puts a frame onto a
  PON stream (the vendor SDK's debug CLI calls it "force pon forward" and
  takes the stream id with it); a reply without it never reached the OLT;
- everything from the IP stack (`odi_start_xmit()`): no `PORT_SEL`, and the
  port mask of the device it leaves on, `odi_nic_stack_tx_mask()`
  (`odi_nic_hw.h`): 0x1 for `eth0.2`, 0x2 for `eth0.3`, 0x3 for `eth0`,
  nothing for `nas0` and `pon0`. Never the PON port, never the CPU port,
  and never 0, which would be a hardware lookup.

Whether the switch honours a port mask without `PORT_SEL` is not measured.
The vendor's own NIC driver sends every protocol-stack frame that way and
calls a non-zero mask a direct transmit (a zero mask is the lookup); read
so, the stick's ARP, broadcasts and replies already leave by the host port
alone. Read the other way (the mask ignored, the frame forwarded by
lookup), a broadcast from the stick (`FLOOD_BCAST_PORTS` 0xf) or an unknown
unicast would also reach the PON port. Setting `PORT_SEL` on those frames
is not the cure: it is the "onto the PON" bit.

So the switch layer covers the second reading: the CPU port's isolation
mask loses the PON port (above), and a frame the CPU sends that the switch
forwards by lookup can reach the host only. The OMCI replies are a
directed transmit, not a lookup, and the vendor forwarding engine applies
the isolation masks to directed transmits in software, which says the
hardware does not; that is a reading too, so the first trial checks that
OMCI keeps working with the mask in place. It is a property of the chip,
not of the OLT: if OMCI works with it on one OLT, it does on any.
`pon_isolate=N` is the way back here as well. The OMCI transmit path
itself (`odi_omci.c`, `odi_nic_tx_words()`, `odi_nic_xmit_raw()`) is not
changed by any of this.

### The stick's MAC

The switch learns a frame's source MAC on the port it entered by, whatever
the isolation masks let it reach afterwards. A frame from the fibre
carrying the stick's MAC as its source would move the stick's learned row
from the CPU port to the PON port, and frames from the host to the stick
would then be switched toward the fibre until the stick sent again.

The stick's MACs are therefore static rows on the CPU port
(`odi_switch_pin.c`, `odi_switch_l2.c`): bit 62 of an L2 row, the SDK's
"no SA learning", is never learned over. A frame whose source hits a static
row from another port is an "illegal port move", whose action is
`SA_MOVED` (0x01c000, two bits a port: 0 moves a learned row to the port
the frame came in by, 1 drops the frame and leaves the row). The boot replay
writes 0 on every port, and `odi_sw_sa_moved_apply()` sets the PON port to 1
afterwards (`0x00000010`), leaving the others: a frame from the fibre whose
source is a host's learned row, such as one an OLT reflects, is dropped
instead of moving the host to the PON port, which would switch traffic for
that host toward the fibre. A static row, as the stick's pin, is not moved
either way (another open implementation's reading). The row is the one the hardware
writes when it learns the stick's MAC from the CPU port (SVL, filtering id
0, port 3, see [the L2 table readout](KERNEL.md)) plus the static bit, with
the age non-zero as the SDK writes a static unicast row; written through
the hash, it lands on the learned row and replaces it.

Which MACs: the address of every `odi_nic` device that is up and
transmits (`eth0`, `eth0.2`, `eth0.3`), and of the bridge it is a port of
(`br0`, which takes `eth0.2`'s). A netdevice notifier (up, down, a new
address, a new bridge) recomputes the set and brings the table in step, so
`network.sh` setting `ELAN_MAC_ADDR` (link down, address, link up) moves
the pin with it; `switch_init` syncs once more before omcid starts. Each
change is logged:

    odi_switch: 02:00:00:00:00:04 pinned to the CPU port (static row 912)

The table is emptied once at the platform step, before the pins are
written. Nothing removes a static row after that except a MAC that is no
longer the stick's (the delete is by the row's own words, looked up again
afterwards, and logs a warning when the row stayed): no replay writes the
L2 table, `L2_TBL_FLUSH_CTRL` (0x017024) is set to flush dynamic rows only
and is never started (0x017028 is read as the flush port mask, not "the
flush itself"; claro reads 0 there), the bridge connections of cmd 51
do not touch the table, and ageing (cmd 62) and the learning limits (the
MAC bridge port MEs) apply to learned rows. Learning on the PON port stays
on: in a bridge, the OLT side's MACs are learned there.

What cannot be checked without the OLT side: a frame from the fibre with
the stick's MAC as its source. The static semantics rest on the field and
the port-move register, not on a measurement.

### Checking it on a stick

- `register get 0x27000 2` in `diag`: `0x003fffff 0x003fd807`.
- `l2-table get all` in `diag`: the stick's MAC `Static`, port 3, after
  the boot, after the OLT's provisioning and after an `apply.sh`.
- `/proc/odi_nic`: `rx_pon_dropped` 0 over a day, and `rx_pon` equal to the
  OMCI frame count; `diag mib dump counter port 2` shows the OLT's
  broadcasts arriving (`ifInBroadcastPkts` growing) all the while.
- OMCI: the ONU in O5 with its services, and `/proc/odi_omci`
  `frames_delivered` and `replies_sent_to_pon` growing together.
- The host still reaches the web UI, ssh and the exporter.

`test/odi_pon_isolation_test.c` starts from the words a stick reads,
checks the words the platform step leaves, asks a forwarding model where
each kind of frame from the fibre, and each frame the CPU sends by
lookup, can go, runs the NIC's PON test on RX descriptor words logged on a
stick (an OMCI frame and host frames), and checks the port mask and
`PORT_SEL` of every device's transmit descriptor.
`test/odi_switch_l2_test.c` checks the static row against the row the
stick learned for its own MAC and against the static bit as igmpd's row
showed it, and pins, moves and unpins MACs through its model of the table.
