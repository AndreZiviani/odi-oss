# diag — the switch, GPON and optics CLI

A freestanding, statically linked MIPS binary for the ODI DFP-34X-2C2 running
our own Linux 6.18 image. It reads the transceiver, the GPON state machine
and the switch through the interfaces our kernel provides, and nothing else:

| interface | driver | used for |
|---|---|---|
| `/dev/odi_sw` ioctls | `odi_reg.c`, `odi_ddm.c` | register get/set, MIB counters, DDM |
| `/proc/odi_gpon` | `odi_gpon.c` | ONU state, the LOS sample |
| odi_omci netlink, `OMCI_FLOWS_CMD` | `odi_omci.c`, `odi_switch_cmd.c` | GEM flows |

It started as a reimplementation of the stock `/bin/diag`, with its 1,973
commands lifted out of the vendor binary. Almost all of those reached the
hardware through the stock switch driver's private socket options, which our
kernel does not have, so they could only fail. What is left is a short
hand-written command table of our own. The commands the Prometheus exporter
runs keep the stock syntax and output (see [the contract](#the-exporter-contract)).

|  | before | now |
|---|---|---|
| size | 373,280 bytes | 18,168 bytes |
| commands | 1,973 parsed, 929 wired, a handful working on 6.18 | 16, all working on 6.18 |
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
    gpon get onu-state                GPON state machine state, O1-O7
    gpon get alarm-status             LOS, LOF and LOM, live from the GPON block
    gpon get flows                    GEM flows omcid programmed, as odi_switch recorded them
    mib dump counter port <ports>     switch port MIB counters; all = every port that answers
    register get <address> <words>    read switch-core registers, four per line
    register set <address> <value>    write one switch-core register
    help                              list the commands
    exit                              leave the shell

- `<ports>` is `all`, a port, a range or a list: `2`, `0-3`, `0,2-3`.
- Numbers are decimal or `0x` hex, and must fit 32 bits.
- Register addresses are offsets into the switch-core window: 4-byte
  aligned and below `0x2000000`. The driver may refuse a narrower range.
- `gpon get alarm-status` reads the live condition register
  (`GPON_GTC_DS_INTR_STS`, 0x701008) for LOS, LOF and LOM. It falls back to
  the LOS sample in `/proc/odi_gpon` when `/dev/odi_sw` cannot be read. The
  other four stock alarms (SF, SD, TX Too Long, TX Mismatch) are not tracked
  by `odi_gpon`, so they are named on a note line and never printed as clear.
- `gpon get flows` shows what `odi_switch` recorded when omcid programmed each
  flow, not a hardware table read. It never registers for a redirect type,
  so omcid keeps the OMCI channel while it runs.
- `mib dump counter port all` prints every port whose counter 0 answers
  (0 to 3 on this board). A counter the driver has no register for is left
  out, not printed as zero.
- The module serial number has no DDM selector in the driver, so there is no
  command for it.

Everything else in `/proc/odi_gpon`, `/proc/odi_omci`, `/proc/odi_intr` and
`/proc/rtk_init` is plain text; read those with `cat`.

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

The Prometheus exporter (`metricsd`, the separate sfp-exporter project) runs
one diag per scrape and pipes this batch into it (its `diag_secs` table):

    pon get transceiver bias-current
    pon get transceiver rx-power
    pon get transceiver tx-power
    pon get transceiver temperature
    pon get transceiver voltage
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
first two fields of each output line (`0x<address> 0x<value>`), and
`regreplay` feeds thousands of `register set` lines through one diag. Keep
that layout too.

## Build

Everything runs in a container; nothing but Docker is needed on the host.

    make diag                                    # the binary
    make test                                    # parser, conversions, ioctl numbers, natively
    make exporter-test                           # the exporter contract, under qemu
    make selftest                                # exporter-test plus the conversion vectors on MIPS
    make run ARGS='pon get transceiver rx-power' # under qemu-user (no hardware there)
    make verify                                  # ELF32 / big-endian / MIPS / static

`qemu-mips-static` emulates a full MIPS32 CPU and runs `mul` and `clz`,
which trap on the RLX5281. Before shipping a build, run the exporter
project's `scripts/isa-audit.sh build/diag` in the toolchain container.

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
    src/hw.[ch]             /dev/odi_sw, /proc/odi_gpon, netlink
    src/odi_sw_ioctl.h      restated copy of the kernel's odi_reg.h ABI
    src/ddm.[ch]            SFF-8472 conversions, fixed point
    src/gpon_status.[ch]    /proc/odi_gpon parsing and alarm bits
    src/mib.h               MIB counter names, in ioctl index order
    src/io.[ch]             buffered output, line input, string primitives
    src/sys.h, src/start.S  o32 syscalls and the freestanding entry point
    test/                   host tests, qemu self-tests, the exporter contract
    tools/                  register naming, for tools/regtrace

`src/start.S`, `src/sys.h` and `src/io.[ch]` are also the freestanding
runtime of `omcid`, `omcli`, `omciprobe`, `omcicap`, `igmpd` and `nv`, whose
Makefiles build them from here.
