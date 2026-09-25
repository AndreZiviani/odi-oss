# omci — replacing omci_app

Our own OMCI stack for the ODI DFP-34X-2C2: it terminates the OLT's
management channel, holds the MIB, and turns what the OLT provisions into
switch and GPON configuration. This directory builds:

- `omcid` (`respond/`) -- the daemon; answers the OLT and programs the switch
- `omcli` / `omcicli` (`cli/`) -- the client, one binary, symlinked twice
- `omciprobe` (`probe/`) -- one raw driver command, bypassing omcid
- `omcicap` (`capture/`) -- captures the OMCI channel

User-facing flags, config-store keys and log format are in
`docs/TOOLS.md`; this file is the architecture and the design decisions
behind it.

## The three interfaces

    OLT  --OMCI frames--  [netlink redirect, type 1]  --+
                                                         |
                                omcid  --sockopt 0x310a, cmd 1..73--  odi_switch
                                     |
                      SysV msgq 0x800 (mtype 4)
                                     |
                                 omcli / omcicli

1. **Southbound control**: one socket option (`getsockopt(fd, 0, 0x310a,
   ...)`, a 264-byte request/reply buffer), 73 commands, dense. This is the
   same ABI `src/diag` and `src/igmp` already document and drive.
2. **Southbound data**: `odi_omci` netlink redirect, type 1. **The redirect
   has one receiver per type** -- registering takes it from whoever holds
   it, there is no passive tap. That is why every one of these binaries
   deregisters on every exit path, signals included (`redirect_guard.h`),
   and why `omciprobe` never registers at all (it goes through the sockopt
   path only) while `omcicap` refuses to start against a live `omcid`
   unless forced.
3. **Northbound**: the CLI, over the vendor's own System V message queue
   (key 0x800, 20-byte header, 240-byte payload, command id in word 0).
   `omcid` answers on this queue too, in the stock output format, because
   `confd`'s MIB/Services pages and the exporter's `omcicli dump srvflow`
   already depend on it.

## The MIB model is data

The device's `/lib/omci/mib_<Name>.so` plugins each export five fixed
symbols (`gMib<Name>TableInfo`, `gMib<Name>AttrInfo`, `gMib<Name>DefRow`,
`gMib<Name>Oper`, `mibTable_init()`); `tools/mib-tables.py` reads those
exports straight out of the shipped `.so` files and emits
`generated/omci_mib.[ch]` and `omci_mib.json` -- 81 managed entities,
~740 attributes, each with name, type, width, OLT access and index. That is
what makes a from-scratch responder tractable: the information model does
not need to be reverse-engineered by hand, and `generated/` regenerates from
the shipped plugins rather than being hand-maintained. `tools/me-drv-map.py`
shows only 21 of the 81 managed entities reach the driver at all (T-CONT,
GEM port, bridge port and friends); the other 60 are pure MIB and need no
driver call.

## Where this differs from the vendor daemon, and why

`omcid` drives the same wire protocols (southbound sockopt, northbound
message queue) so it is a drop-in for `confd`, the exporter and the stock
`/bin/omcicli` client, but fixes several problems observed in the vendor
daemon along the way:

- **Dumps are waited for, not slept through.** The vendor client sleeps a
  fixed 12 ms after a dump request and reads whatever landed in the temp
  file, which truncates long dumps. `omcid`'s handler loop is single
  threaded and CLI messages are FIFO with respect to each other, so
  `omcli` treats the next queue-answering reply as the completion signal
  instead of a timer.
- **The reply queue is per process and `IPC_PRIVATE`.** The vendor client
  hardcodes one reply key, so two concurrent runs read each other's
  answers; a killed client's queue also cannot outlive it and leak.
- **`get tables` no longer deadlocks.** The vendor handler takes a table
  mutex twice; `omcid` has no such mutex and answers it directly, though
  `omcli` still refuses to send it to a real vendor daemon (`--vendor`).
- **Every blocking request gets a reply**, even a bare acknowledgement,
  so a caller can never hang waiting for one that was silently dropped.
- **The reply buffer is zeroed before it is filled**, not after, so a
  field the handler forgets to set never leaks whatever the buffer held
  before.
- **The message queue is recreated, not attached to, at startup**, since
  its id changes across every restart of whichever daemon owns key 0x800.

One accepted difference from the vendor, worth stating because a test
depends on it: **a Set that changes only one field of a multi-pointer table
row re-sends every pointer in that row**, not only the one that changed
(the vendor gates re-send on which pointer changed against the
previously-stored row; our store keeps one row and overwrites it, so the
guard is dropped). This is strictly more traffic to the switch driver than
the vendor sends, never less, and is exercised by `qemu-test.sh`.

## Status

Every phase of the original build-out (southbound probe, MIB extraction,
identification, service programming including bridge connections, and the
CLI) is complete and covered by `qemu-test.sh` / `ontest.sh` / `bdgtest.sh`.
There is no outstanding design work tracked outside this file and the
per-binary source comments.
