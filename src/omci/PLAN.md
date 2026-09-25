# Replacing omci_app

`omci_app` is the OMCI stack: it terminates the OLT's management channel,
holds the MIB, and turns what the OLT provisions into switch and GPON
configuration. Everything `omcicli` can do is a thin shell over it, so the CLI
cannot be fixed from the client side -- 19 of its 45 commands answer by
`printf`ing into a `mkstemp` file that the client reads after `usleep(12000)`,
and one of them deadlocks the daemon outright. Those are properties of the
daemon, not of the CLI.

This is the scoping note for replacing it. Everything below is read out of our
own `base-a4k-260909` (`V1.0-220923`); nothing here has been exercised on
hardware yet.

## What the daemon actually sits between

    OLT  --OMCI frames--  [netlink redirect, libpr.so]  --+
                                                           |
                                  omci_app  --sockopt 0x310a, cmd 1..73--  driver
                                       |
                        SysV msgq 0x800 (msgType 4)
                                       |
                                   omcicli

Three interfaces, all small, all recovered:

### 1. Southbound control: one socket option, 73 commands

`omci_wrapper_createCtrlDev` is `socket(AF_INET, SOCK_RAW, 0xff)` -- the same
socket librtk uses for the switch core, so this is the ABI already documented
for `diag`. Every hardware-touching wrapper funnels into one helper:

    omci_drv_call(u32 cmd, void *buf, u32 len)      /* len <= 256 */
        struct { u32 cmd; u32 len; u8 data[256]; } req = { cmd, len };
        memcpy(req.data, buf, len);
        getsockopt(ctrlFd, 0, 0x310a, &req, &optlen /* 264 */);
        memcpy(buf, req.data, len);                 /* in and out share the buffer */

The command space is dense, 1..73, and `tools/omci-drv-api.py` prints it with
the vendor's own wrapper name against each: 1 setLog, 2 setDevMode, 13
getOnuState, 14/15 set/getSerialNum, 17 activateGpon, 21 createTcont, 25
cfgGemFlow, 28 getPortLinkStatus, 42 getPortStat, 50/51 de/activeBdgConn, 63
setPortBridging, 72/73 get/clearWanData, and so on. Four of the 77 wrappers
pass a command this extractor cannot see yet; `omci_wrapper_setPriQueue` and
`omci_wrapper_resetMib` build theirs conditionally.

There is one bypass worth knowing: the helper checks a global and **skips the
syscall entirely when it holds 2**, returning the request buffer unchanged.
A simulation mode, and a free way to dry-run a replacement.

### 2. Southbound data: netlink, three opcodes

OMCI frames do not come through the control socket. `omci_wrapper_createMsgDev`
opens `socket(AF_NETLINK, SOCK_RAW, 2 /* NETLINK_USERSOCK */)`, raises
`SO_RCVBUFFORCE` to 256000 (halving on refusal), and calls
`ptk_redirect_userApp_reg(fd, type=1, mtu=1500)`.

`libpr.so` is 4.8 KB and holds the whole protocol. Every call builds an
`nlmsghdr` -- `nlmsg_len`, `nlmsg_pid = gettid()` via `syscall(4222)`,
`nlmsg_flags = 0`; `nlmsg_type` is left uninitialised -- followed by a body:

    reg      opcode 1: u16 op, u16 type, u32 tid, u8 enable=1, u16 mtu
    sendPkt  opcode 2: u16 op, u16 type, u32 0, u32 port, u32 len, u8 data[len]
    recvPkt  opcode 3: same header shape, answered by recvmsg

`omci_wrapper_sendOmciMsg(buf, len)` accepts 48..1500 bytes, zero-pads to 1504
and sends with `type=1, port=0`.

### 3. Northbound: the CLI, already fully mapped

SysV message queue key 0x800, 20-byte header, 240-byte payload, command id in
word 0, 45 ids.
`tools/omci-cli-table.py` prints the server
half, `tools/omcicli-grammar.py` the client half.

## The MIB is data, and the data is extractable

This is what makes a replacement tractable rather than heroic.

`/lib/omci/` holds 81 `mib_<Name>.so` plugins, loaded by `dlopen`/`dlsym`.
Each exports the same five symbols:

    gMib<Name>TableInfo   44 bytes   class id, names, attribute count, row size
    gMib<Name>AttrInfo    40 * N     one descriptor per attribute, index 0 = entity id
    gMib<Name>DefRow      row size   the default row
    gMib<Name>Oper        68 bytes   the per-ME vtable (ConnCheck, DrvCfg, DumpMib, ...)
    mibTable_init()                  fills the four above, then MIB_Register()

The four objects are in bss, so they do not exist in the file -- but
`mibTable_init` is straight-line code that stores constants and string pointers
into them, which is exactly the shape `src/diag/tools` already extracts.
`mib_ExtVlanTagOperCfgData` reads out as class 171, 9 attributes, 64-byte row,
`0x04000350` of flags, with each attribute's name pointer at descriptor+0 and a
stride of 40. `mib_EthUni` is class 11 with 17 descriptors in 680 bytes -- the
same stride, independently.

So the whole OMCI information model on this device -- 81 tables, roughly 600
attributes, with names, sizes, types and access -- comes out as one generated C
table, the way the 1973-command `diag` tree did.

## What has to be written rather than extracted

1. **The OMCI message machine.** `OMCI_HandleMsg` and the `OMCI_On*Msg`
   handlers: create, delete, set, get, get-next, set-table, MIB upload, MIB
   upload next, MIB reset, get all alarms (+next), get current data, test,
   synchronize time, reboot, and the software-download quartet. These are
   named functions in `omci_app` and the message formats are G.988, so this is
   ordinary work with a reference implementation to check against.
2. **The per-ME apply logic** -- each plugin's `DrvCfg` / `ConnCheck` /
   `ConnCfg`. This is the real cost, and it is not uniform: most plugins are
   trivial, and a handful carry the service -- `ExtVlanTagOperCfgData`,
   `MacBriPortCfgData`, `GemPortCtp`, `Tcont`, `PriQ`, `Map8021pServProf`.
   `omci_wrapper_setPriQueue` alone is 3072 bytes, `setUsVeipPriQ` 3144,
   `cfgGemFlow` 3196, `activeBdgConn` 2716.
3. **Persistence.** The MIB is rebuilt from the OLT at every re-registration,
   so this matters less than it looks; what must survive is the serial number,
   LOID, device mode and customised flags, which live in the jffs2 config store.

## Sequencing

Each phase is useful on its own and the risky ones come late.

**0. Speak the southbound from a test tool.** **Done** -- `probe/` builds
`omciprobe`, which issues any command from the generated table and dumps the
reply, refusing the writers unless forced. Run against `isp1` beside a
running `omci_app`: `getOnuState` returns O5, `getDevIdVersion` "RTL9602C",
`getDrvVersion` "v1.9.2.5", and `getPortStat` on port 2 matches
`diag mib dump counter port 2` counter for counter. Port 1 returns -1 where 0,
2 and 3 answer, which is the port map already measured through librtk. The ABI
is confirmed end to end.

**1. Extract the MIB model.** **Done** -- `tools/mib-tables.py` emits
`generated/omci_mib.[ch]` and `omci_mib.json`: 81 managed entities, 739
attributes, each with name, data type, width, OLT access, index and AVC flags,
and the set of OMCI actions the ME accepts. 80 of the 81 are self-consistent
(entrySize against the sum of the attribute widths); ME 245 is proprietary and
its own attrNum disagrees with its descriptors.

**2. Capture one re-ranging.** Settled statically, and the answer changes the
shape of this phase: **the redirect has one receiver per type.** The kernel's
userland registration (0x8017f5d0 in the decompressed `uImage`) walks its list
and, on a type that is already registered, overwrites that entry's pid and
frees the new one. Registering type 1 therefore takes the OMCI channel away
from `omci_app`; there is no passive tap through this API.

That makes the disruption the experiment. Losing OMCI makes the OLT deregister
and re-range the ONU, and a re-ranging is the sequence worth recording anyway:
MIB reset, MIB upload, then the create/set run that builds the service. So:
stop `omci_app` rather than race it, register, capture one full re-ranging
against the extracted model, deregister, restart `omci_app`.

Cost is one interruption of about the ranging time. Recovery is measured --
`killall omci_app; PATH=$PATH:/etc/scripts /etc/runomci.sh`, roughly six
seconds -- and nothing touches flash, since the capture binary runs from
`/tmp`.

**3. Answer enough to register.** **Done** -- Create/set/get/get-next, MIB upload, MIB
reset, and the identity MEs (ONU-G, ONT2G, Cardholder, CircuitPack, ANI-G,
UNI-G, PPTP Eth UNI / VEIP, SW image). Success criterion is concrete and
binary: the OLT completes MIB upload and the stick reaches O5.

**4. Build the service.** **All but one piece done.** On isp2 the T-CONT and
both directions of the service GEM flow program from our own stack; the GEM
flow id, the downstream broadcast port and the UNI-to-switch-port table are all
resolved and the last two were checked on isp1.

`tools/me-drv-map.py` shows only **21 of 81** managed entities reach the driver
at all, and names the commands each one issues -- T-CONT to `createTcont`, GEM
port to `cfgGemFlow`, bridge port to `setDsBcGemFlow` and friends. The other 60
are pure MIB and already work.

Two things that were guesswork are now read from the device instead.
`getDevCapabilities` (command 3, a `get`, safe beside a running `omci_app`) is
copied straight into `gInfo+112` by `OMCI_Init`, so its 120 bytes *are* every
`gInfo` field the vendor's code quotes: the UNI table in the first 64, the
priority-queue count at +88, the GEM flow count at +92.

**What is left is `activeBdgConn`, and it is not optional.** Measured on isp1:
with every GEM flow still programmed and the twelve bridge connections torn out
with `deactiveBdgConn`, upstream forwarding stopped dead -- zero bytes in
thirty seconds -- and resumed the moment `omci_app` restarted and replayed its
saved MIB. The flows are the pipe; the 160-byte connection descriptor is the
rule, and it carries the ingress UNI, the egress GEM port and the VLAN
filter/treatment. Reproducing it means reproducing `MIB_TreeConnUpdate` and the
rule generators, about 6 KB, with `omcicli dump conn` printing the finished
article to check against. Success criterion: traffic forwards with the same
VLAN treatment as the vendor stack, checked against the forwarding counters.

**5. The CLI, properly.** **The read half is done** -- `cli/` builds `omcli`,
8.7 KB, freestanding, and its output is byte-identical to the stock `omcicli`
on every read command: 18 of 19 compared byte for byte on isp1, and the
nineteenth differs only because the stock client has no keyword for it.

    get      sn log logfile devmode dmmode loid loidauth cflag authuptime
             version oltloc
    mib      get getcurr getalm getattr
    dump     avltree qmap conn srvflow tasks

Same queue key and the same 240-byte payload, so it drives the stock daemon
today and ours later. What is different:

- **Dumps are waited for, not slept through.** `omci_app`'s handler loop is one
  thread and every CLI message carries mtype 2, so CLI messages are FIFO with
  respect to each other: a cheap queue-answering command sent straight after a
  dump cannot be answered until the dump handler has returned, which is after
  its `fflush`. That reply is an exact completion signal in place of the stock
  client's `usleep(12000)`. (On an idle isp1 the stock sleep is long enough --
  both clients produced the same 126,321 bytes five times running -- so this
  removes a documented race rather than one reproduced today.)
- **The reply queue is per process**, not the hardcoded 0x6868 two concurrent
  runs would share.
- **The temp file is read and removed**, not left for the next command's
  `rm -rf` and not shelled out to `cat`.
- **`get tables` is refused outright**, because its handler deadlocks the
  daemon.
- **Two commands the stock client cannot reach** are reachable: `get version`
  (id 19) and `get oltloc` (id 24, ME 131).

The wildcards are per command and not guessable: `mib get` takes its selector
as *text* and uses -1 at +20 only to mean "every table", `mib getalm` uses
**0x7fffffff** for "every class", `dump avltree` uses -1, and an entity id is a
halfword whose wildcard is 0xffff. Each came out of its own handler.

**The temp file is gone where we own both ends.** `omcid` carries its own
queue, `OMCLI_KEY` (0x9601 -- the vendor holds 0x800, 0x801 *and* 0x802), and
`omcli` finds out which daemon is listening by asking whether that queue
exists. No probe message, and no guessing at what an unknown message type does
to a daemon that is not ours.

The protocol is in `omci_cli_proto.h` and is what the vendor's would have been
if its dump functions had not been written to `printf`:

- **The request is argv.** The client forwards what it was given and prints
  what comes back, so a new command on the daemon needs no new client.
- **The reply is a length-delimited stream** of 4 KB chunks -- this kernel
  reports msgmax 8192 and msgmnb 16384, so two fit in a queue -- each carrying
  a sequence number, with an explicit end and a status. A dump cannot be
  truncated, a lost chunk cannot go unnoticed, and there is nothing to wait a
  fixed time for.
- **The reply queue is IPC_PRIVATE**, so two clients cannot collide, and the
  client ignores SIGPIPE so that being killed mid-stream still reaches its own
  cleanup. Both matter: the kernel has 53 queues in total and `omcli mib | wc`
  with no `wc` on the device leaked one per run until it did.

`omcid` answers `state`, `caps`, `flows`, `tcont`, `mib [class] [instance]` and
`help`. Three of those have no vendor equivalent -- the flow tables, the T-CONT
map and the decoded capability blob are state the stock CLI cannot reach.
Verified on isp1: a 53-row MIB dump spanning several chunks, ten consecutive
runs leaking no queues, and `/tmp` empty afterwards.

**The stock client works against omcid too.** `boa` and five scripts on this
device shell out to `/bin/omcicli`, so a daemon that replaces `omci_app` has to
answer on the vendor's queue or the web UI's OMCI pages stop working. `omcid`
takes key 0x800 and serves the 45-id protocol, temp file and all -- that part
*is* forced on us, because the client is the stock one and it sleeps 12 ms and
then reads `/tmp/temp_omcicli*`. Which is also why the main loop polls at 2 ms;
measured cost is about 40 ms of CPU per run, well under 1%.

Verified on isp1 with the real `/bin/omcicli`: `get sn`, `mib get 47`,
`dump srvflow`, `mib getattr` (values identical to our own client's) and
`get loidauth`. An id `omcid` does not implement says so in the file rather
than leaving the caller waiting.

Three traps, all found by running it:

- **`omci_app` removes queue 0x800 when it stops** -- the queue id changes
  across every restart, which is how you can tell. Attaching to its queue at
  startup buys an id that dies with it, after which neither daemon nor client
  can use the key. `omcid` creates the queue instead, retrying about once a
  second until it is free.
- **`omci_SendCmdAndGet` blocks with no timeout**, so any request that asked
  for a reply and did not get one hangs the client for good. `omcid` answers
  everything that set a reply key, even if only with zeros.
- **Write the reply buffer after zeroing it, not before.** The vendor's
  `OMCI_MibAttrGet_Cmd` stores its command id and *then* memsets, which is why
  that one reply always echoes zero.

**What is left**: the vendor's dump *format*, which differs between tables in
the same firmware -- a script that
parses `mib get 256` by column is not satisfied yet, though one that just wants
the data is. And `msgType 0`, the raw-frame injection path, which is the
cleanest way to exercise MIB behaviour with no OLT.

## Risks, stated up front

- **Phases 2-5 can take the stick off the line.** Every test wants a second
  stick or a quiet window. The way back is cheap, though: the binary runs from
  `/tmp`, which is ramfs, so killing it undoes it and a reboot undoes anything
  it left behind. Nothing here touches flash.
- **The OLT is the other half of the test.** Behaviour we cannot reproduce
  statically -- what our OLT actually sends, in what order -- only shows up in
  phase 2, which is the first phase that costs service.
- **The plugin vtables are code, not data.** Phase 1 gets the model; the
  behaviour behind `Oper` still has to be read function by function for the
  MEs that matter.
- **We do not have the driver's side.** `0x310a` and the redirect are kernel
  interfaces; if a command needs an argument shape we guess wrong, the failure
  is in the kernel module and is not visible in userland. Phase 0 exists to
  find that early and cheaply.
