# diag — the switch, GPON and optics CLI

A freestanding, statically linked MIPS binary for the ODI DFP-34X-2C2 running
our own Linux 6.18 image. It reads the transceiver, the GPON state machine
and the switch through the interfaces our kernel provides, and nothing else:

| interface | driver | used for |
|---|---|---|
| `/dev/odi_sw` ioctls | `odi_reg.c`, `odi_ddm.c`, `odi_switch_l2.c`, `odi_switch_acl_probe.c`, `odi_switch_mc_count.c` | register get/set, MIB counters, DDM, the L2 table, the ACL probes, the multicast group counters |
| `/proc/odi_gpon/status` | `odi_gpon.c` | ONU state, the LOS sample |
| `/proc/odi_gpon/flows` | `odi_gpon.c` | the per-flow GEM counters |
| odi_omci netlink, `OMCI_FLOWS_CMD` | `odi_omci.c`, `odi_switch_cmd.c` | GEM flows |

It started as a reimplementation of the stock `/bin/diag`, with its 1,973
commands lifted out of the vendor binary. Almost all of those reached the
hardware through the stock switch driver's private socket options, which our
kernel does not have, so they could only fail. What is left is a short
hand-written command table of our own. The commands the Prometheus exporter
runs keep the stock syntax and output (see [the contract](#the-exporter-contract)).

|  | before | now |
|---|---|---|
| size | 373,280 bytes | 21,208 bytes |
| commands | 1,973 parsed, 929 wired, a handful working on 6.18 | 31, all working on 6.18 |
| links | nothing | nothing |

## Commands

`diag help`, or a trailing `?`, prints this list:

    pon get transceiver vendor-name   module vendor name (SFF-8472 A0h)
    pon get transceiver part-number   module part number (SFF-8472 A0h)
    pon get transceiver temperature   module temperature, C
    pon get transceiver voltage       module supply voltage, V
    pon get transceiver bias-current  laser bias current, mA
    pon get transceiver tx-power      optical transmit power, dBm
    pon get transceiver rx-power      optical receive power, dBm
    pon get transceiver all           all seven transceiver readings above
    pon get transceiver alarm-status  alarm/warning flags and optical LOS (SFF-8472 A2h 110-117)
    pon get transceiver monitoring-type  how the module says its readings are calibrated (SFF-8472 A0h 92)
    pon get transceiver limits        the module's own alarm and warning limits (SFF-8472 A2h 0-39)
    gpon get onu-state                GPON state machine state, O1-O7
    gpon get alarm-status             LOS, LOF and LOM, live from the GPON block
    gpon get flows                    GEM flows omcid programmed, as odi_switch recorded them
    gpon get flow-counters            per-flow GEM counters the driver accumulates (/proc/odi_gpon/flows)
    mib dump counter port <ports>     switch port MIB counters; all = every port that answers
    l2-table get all                  every valid L2 table row: learned MACs, multicast groups
    l2-table get entry address valid  the same, in the stock spelling (odi-ui sends it)
    l2-table get index <index>        one L2 table row by number, valid or not, with raw words
    register get <address> <words>    read switch-core registers, four per line
    register set <address> <value>    write one switch-core register
    acl-probe count <slot> port <port> proto <proto> count the IPv4 frames of a protocol entering port 0 (UNI) or 2 (PON)
    acl-probe count <slot> port <port> group <group> <length> count the IPv4 frames to an address prefix entering port 0 or 2
    acl-probe bytes <slot> port <port> proto <proto> the same, counting bytes
    acl-probe bytes <slot> port <port> group <group> <length> the same, counting bytes
    acl-probe mirror <slot> igmp      copy the IGMP entering the UNI to the CPU, and count it
    acl-probe mirror <slot> igmp bytes the same, counting bytes
    acl-probe trap <slot> igmp        take the IGMP entering the UNI to the CPU instead, and count it
    acl-probe trap <slot> igmp bytes  the same, counting bytes
    acl-probe show <slot>             one probe: its rule, what it counted, what reached the CPU
    acl-probe delete <slot>           remove one probe
    acl-probe delete all              remove every probe
    mc-count frames <slot> pon mac <high> <low> <length> count the frames to a MAC prefix entering the PON (high: octets 0-1, low: 2-5)
    mc-count frames <slot> uni mac <high> <low> <length> the same, entering the UNI
    mc-count bytes <slot> pon mac <high> <low> <length> the same, counting bytes, entering the PON
    mc-count bytes <slot> uni mac <high> <low> <length> the same, counting bytes, entering the UNI
    mc-count frames <slot> pon ipv4 <address> <length> count the IPv4 frames to an address prefix entering the PON
    mc-count frames <slot> uni ipv4 <address> <length> the same, entering the UNI
    mc-count bytes <slot> pon ipv4 <address> <length> the same, counting bytes, entering the PON
    mc-count bytes <slot> uni ipv4 <address> <length> the same, counting bytes, entering the UNI
    mc-count show <slot>              one group counter: its rule and what it counted
    mc-count delete <slot>            remove one group counter
    mc-count delete all               remove every group counter
    help                              list the commands
    exit                              leave the shell

- `<ports>` is `all`, a port, a range or a list: `2`, `0-3`, `0,2-3`.
- Numbers are decimal or `0x` hex, and must fit 32 bits.
- Register addresses are offsets into the switch-core window: 4-byte
  aligned and below `0x2000000`. The driver may refuse a narrower range.
- `gpon get alarm-status` reads the live condition register
  (`GPON_GTC_DS_INTR_STS`, 0x701008) for LOS, LOF and LOM. It falls back to
  the LOS sample in `/proc/odi_gpon/status` when `/dev/odi_sw` cannot be
  read. SF and SD come from the `signal sf|sd` lines of
  `/proc/odi_gpon/status` (the driver's BER evaluation). TX Too Long and TX
  Mismatch are not tracked by `odi_gpon`, so they are named on a note line
  and never printed as clear (SF and SD join them when the file has no
  `signal` lines, an older kernel).
- `gpon get flows` shows what `odi_switch` recorded when omcid programmed each
  flow, not a hardware table read. It never registers for a redirect type,
  so omcid keeps the OMCI channel while it runs.
- `gpon get flow-counters` prints `/proc/odi_gpon/flows` as the driver
  writes it (`docs/TOOLS.md`, "/proc/odi_gpon/"): one line per flow with the
  GEM port and the totals since the flow was created. It never reads the
  registers itself: they clear on read, so the driver is their one reader,
  and `register get` on them would take counts away from it. A kernel
  without the file gets a `%` line and a failure status. It shares its first
  words with `gpon get flows`, so `gpon get flow` and `gpon g f` are
  ambiguous; spell out `flows` or `flow-c`.
- `mib dump counter port all` prints every port whose counter 0 answers
  (0 to 3 on this board). A counter the driver has no register for is left
  out, not printed as zero.
- The module serial number has no DDM selector in the driver, so there is no
  command for it.
- `pon get transceiver alarm-status` reads the module's own alarm and warning
  flags (SFF-8472 A2h bytes 112/113 and 116/117 -- the module's firmware sets
  these against its own internal thresholds, so this is a direct read, not a
  computed one) and the optical RX_LOS status bit (byte 110), one line each
  in the same "clear"/"occur" phrasing `gpon get alarm-status` uses. It is
  named "Optical LOS" there to avoid confusion with `gpon get alarm-status`'s
  own LOS, a different condition on a different block.
- `l2-table get all` walks the L2 lookup table in the kernel (1,024 rows, or
  1,088 with the CAM rows on) and prints one line per valid row, under the
  header words of the stock listing (`MACAddress Spa Fid Age Vid State Ext
  Hash`) plus `Type` (`uc`, `mc`, `ipmc`), `Ports` (multicast members) and
  `Index`. A reader that zips a row against its header, as odi-ui does,
  reads both. `l2-table get entry address valid` is the same listing under
  the stock syntax, kept because confd sends it to either firmware.
- `l2-table get index <n>` prints one row whether valid or not, and its
  three raw words (hardware bits 95..64, 63..32, 31..0): the check that the
  decode matches the hardware. "Valid" is the table engine answer to the
  read (its status HIT bit), not a bit of the row: read back, every row has
  bit 77 set, and an empty row carries its bucket (row / 4) in the low MAC
  octet, so `get index 0x6d` on an empty row prints
  `00:00:00:00:00:1B ... valid no  raw 0x00002000 0x00000000 0x0000001b`.
  `docs/KERNEL.md` ("The L2 table") has the readout behind this.

- `acl-probe` installs one of eight ACL rules (entries 88 to 95) that count
  the IPv4 frames of a protocol, or to an address prefix (`<group>` in hex,
  `0xefff4d01` for 239.255.77.1, and a prefix `<length>`), entering the UNI
  (port 0) or the PON (port 2), in frames or in bytes; or that copy
  (`mirror`) or take (`trap`) the IGMP entering the UNI to the CPU. Nothing
  else can be sent to the CPU, and nothing from the PON. A counting rule
  changes no frame's forwarding. The rules belong to the diag process that
  installed the first of them and go when it exits, so install and read them
  from one diag reading stdin; `show` works from any process. Each answer is
  one line of `key=value` words: `counted` is what the rule counted since it
  went in, `cpu` the frames the CPU received with the rule's receive
  reason, `uni_igmp_other` the IGMP from the UNI that reached the CPU by
  another reason (and `other_reason` the last one). `docs/SWITCH.md`,
  "Multicast", has what they are for and `tools/mcast-probe/` the script
  that runs them.

- `mc-count` installs one of sixteen multicast group counters (ACL entries
  48 to 63), the rules igmpd meters its groups with: each counts, in frames
  or in bytes (the FCS included), the frames entering the PON or the UNI to
  a destination MAC prefix (`mac <high> <low> <length>`: the first two
  octets, the last four, and the prefix length, so `0x3333 0x00000001 48`
  is 33:33:00:00:00:01 and `0x3333 0 16` every IPv6 multicast MAC) or to an
  IPv4 address prefix (`ipv4 <address> <length>`). A counter changes no
  frame's forwarding. Slots 2k and 2k+1 share a unit, so a pair is all
  frames or all bytes. Held like the probes, by the diag that installed the
  first, and the two exclude each other (while igmpd meters its groups
  neither goes in). `counted` is what the rule counted since it went in,
  64 bits, folded by the kernel on every read. `tools/mcast-probe/count-proof.sh`
  is the script that runs them on a stick.

Everything else in `/proc/odi_gpon/`, `/proc/odi_omci` and
`/proc/odi_init` is plain text; read those with `cat`.

### Matching

- a keyword matches on any prefix, so `pon get transc rx-power` and
  `reg g 0x1d0 1` work;
- a fully typed keyword is exact and beats a partial match;
- a word where two different keywords match only partially is ambiguous:
  `pon get transceiver t` is refused, because it could be `temperature` or
  `tx-power`.

A parse error prints a caret under the offending word and a `%` line.

## Two entry paths

    diag pon get transceiver rx-power    # argv: one command, then exit
    diag                                 # stdin: one command per line

On stdin, each line is read after a `RTK.0> ` prompt and, when stdin is not
a terminal, echoed after it. `exit` or end of input ends the run; at the end
diag prints one more prompt and a newline.

## The exporter contract

The Prometheus exporter (`metricsd`, the separate odi-sfp-exporter project) runs
one diag per scrape and pipes this batch into it (its `diag_secs` table):

    pon get transceiver bias-current
    pon get transceiver rx-power
    pon get transceiver tx-power
    pon get transceiver temperature
    pon get transceiver voltage
    pon get transceiver alarm-status
    gpon get onu-state
    gpon get alarm-status
    mib dump counter port all
    exit

It splits the output on the `RTK.0> ` prompt, matches each section by the
echoed command, and parses the values out of the lines below. The same
exporter build reads the stock firmware's `/bin/diag` too, so these commands
keep the stock CLI's syntax and line formats exactly:

    RTK.0> pon get transceiver bias-current
    Bias Current: 14.350000 mA
    RTK.0> pon get transceiver rx-power
    Rx Power: -22.924298  dBm
    ...
    RTK.0> gpon get onu-state
    ONU state: Operation State(O5) \n\r
    RTK.0> gpon get alarm-status
    Alarm LOS, status: clear
    ...
    RTK.0> mib dump counter port all
    Port: 0
    ifInOctets                         :                    605025
    ...

That means: the prompt and the echo; the labels; six decimals and the units,
including the two spaces before `dBm`; the `(On)` state names and the
trailing `" \n\r"` (in that order) of `onu-state`; `clear`/`occur`; the
`%-35s: %25llu` counter lines and the blank line after each port.

`pon get transceiver alarm-status` is ours (the stock CLI has no such
command; metricsd emits nothing for it there). It prints one
`Alarm|Warning <name>, status: clear|occur` line for each of the ten
SFF-8472 flags and `Optical LOS, status: clear|occur`, or `% transceiver
alarm status read failed`; the exporter turns them into
`gpon_transceiver_alarm`, `gpon_transceiver_rx_los` and `gpon_transceiver_up`.

Two more commands of ours describe the module itself, read from it:
`pon get transceiver monitoring-type` (A0h byte 92: whether diagnostics are
implemented, internal or external calibration, average or OMA received
power; the conversions here assume internal calibration) and
`pon get transceiver limits` (A2h 0-39: the alarm and warning limits the
module raises its own flags against, one `<measurement> <alarm|warning>
<high|low>: <value with unit>` line each). `make transceiver-test` pins
them.

Two differences from the stock CLI are deliberate and were already there
before this rework: `mib dump counter port all` lists every port that
answers rather than the stock CLI's fixed port class, and
`gpon get alarm-status` prints only the three alarms our driver tracks, then
the note line. Both keep the line format, so the exporter reads them the
same way.

`make exporter-test` pins it. It builds `build/diag_fake`, the real binary
with the four accessors those commands reach replaced at link time by
`test/hw_fake.c` (fixed DDM words, O5, LOF asserted, four ports with some
counters refused), runs `test/exporter.txt` through it under qemu, and
compares the output byte for byte with `test/exporter.golden`. It does it
again without the `exit`, against `test/exporter_eof.golden`. The golden
files were produced by the diag this rework replaced (the one whose output
had been checked against the stock CLI on a stick), built with the same fake
answers, so the test proves the rework changed nothing the exporter reads.
If the exporter ever sends a new command, add it to `test/exporter.txt`,
check its output against the stock CLI on a stick, and only then regenerate
the golden files.

`register get` has callers of its own: `rcS` and `network.sh` read the
first two fields of each output line (`0x<address> 0x<value>`). Keep that
layout too.

## Build

Everything runs in a container; nothing but Docker is needed on the host.

    make diag                                    # the binary
    make test                                    # parser, conversions, ioctl numbers, natively
    make exporter-test                           # the exporter contract, under qemu
    make l2-test                                 # the l2-table listing against test/l2.golden
    make selftest                                # both, plus the conversion vectors on MIPS
    make run ARGS='pon get transceiver rx-power' # under qemu-user (no hardware there)
    make verify                                  # ELF32 / big-endian / MIPS / static

`qemu-mips-static` emulates a full MIPS32 CPU and runs `mul` and `clz`,
which trap on the RLX5281, so a clean run under qemu says nothing about the
instruction set. `image/build.sh` runs `packages/isa-audit.sh` over every
ELF it ships, diag included, and refuses the image on a hit; to check a
build by hand, `packages/isa-audit.sh src/diag/build/diag` from the
repository root.

## Adding a command

A row in `src/table.c` (syntax, one-line description, handler id, selector)
and a case in `cmd_run()` in `src/commands.c`. Add parser cases to
`test/cases.txt` and regenerate `test/cases.expected` by running
`build/parse_test < test/cases.txt`, then read the diff.

## The optical conversions

Standard SFF-8472, in `src/ddm.c`, in fixed point:

| reading | conversion |
|---|---|
| temperature | `raw / 256` °C, signed |
| voltage | `raw / 10000` V |
| bias current | `raw × 2 / 1000` mA |
| tx/rx power | `10 × log10(raw / 10000)` dBm |

`make test` checks all four against double-precision references and
`make selftest` runs the same vectors on the target under qemu. The first
three are exact. The dBm path is within 5e-7 dB of double precision, well
below the 0.1 µW step of the DDMI word. It is not bit-identical to the stock
CLI's softfloat `%f` in the last decimal: on hardware the two disagreed by
about 1.2e-5 dB on tx-power, and ours is the exact value. The exporter
parses the number, so that is not a format change.

## Register names

`tools/regmap-extract.py` and `tools/regnames.txt` live here but are not
used by diag, which takes addresses, never names. The extractor reads the
register table out of the stock `librtk.so` into the listing `tools/regtrace`
and `tools/regdump` decode against; `regnames.txt` is our own names for the
registers our code, scripts and kernel headers touch, and every other
register gets a mechanical name (`SW_0x023038`, `F_15_8`).
`test/regnames_test.py` checks the naming rules, and the repository's
`test/regnames_kernel_test.py` holds the kernel headers to the list.

## Layout

    src/table.[ch]          the command table: syntax, description, handler id
    src/parse.[ch]          tokenising, matching, parameter decoding
    src/commands.[ch]       the handlers, and the help listing
    src/shell.[ch]          one line: help, parse errors, dispatch
    src/main.c              argv and stdin entry paths
    src/hw.[ch]             /dev/odi_sw, /proc/odi_gpon/status, netlink
    src/odi_sw_ioctl.h      restated copy of the kernel's odi_reg.h ABI
    src/ddm.[ch]            SFF-8472 conversions, fixed point
    src/gpon_status.[ch]    /proc/odi_gpon/status parsing and alarm bits
    src/mib.h               MIB counter names, in ioctl index order
    src/io.[ch]             buffered output, line input, string primitives
    src/sys.h, src/start.S  o32 syscalls and the freestanding entry point
    test/                   host tests, qemu self-tests, the exporter contract
    tools/                  register naming, for tools/regtrace

`src/start.S`, `src/sys.h` and `src/io.[ch]` are also the freestanding
runtime of `omcid`, `omcli`, `omciprobe`, `omcicap`, `igmpd` and `nv`, whose
Makefiles build them from here.
