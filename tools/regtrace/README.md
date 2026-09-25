# Register trace

The odi driver set (`kernel/extra/drivers/net/ethernet/odi/odi_switch.c`,
`odi_switch_reg.h`) keeps a ring of every register write made through it
(32768 entries, 512 KB static); `/proc/rtk_regtrace` reads it. With
`/etc/config/regtrace.on` on the stick, rcS dumps the ring at every boot
phase into `/tmp/regtrace.txt`.

    scp -O root@<stick>:/tmp/regtrace.txt boot.txt
    sh tools/regtrace/capture.sh <stick> <root-password> "omci provisioning" boot.txt
    python3 tools/regtrace/decode.py <regmap.txt> boot.txt --summary
    python3 tools/regtrace/decode.py <regmap.txt> boot.txt > boot.decoded.txt

The register listing (1,530 registers and their fields) is what
`src/diag/tools/regmap-extract.py <librtk.so> rtl9602c -t <regmap.txt>`
writes: the table read out of the stock firmware binary, named from
`src/diag/tools/regnames.txt` or by address. It is generated, not kept in
this repository; the decoder takes its path.
`echo on rw > /proc/rtk_regtrace` adds reads to the ring, `echo off` stops it.

## Attributing writes to an OMCI command

`echo mark <hex32> > /proc/rtk_regtrace` pushes one kind-M ring entry
carrying a caller-defined tag; `omci_drv_call()` in `src/omci/respond/
apply.c` writes one before and one after every `getsockopt()` it issues,
tagging bit 31 with the phase (0 before, 1 after) and bits 0..30 with the
OMCI driver command number, whenever `/var/config/regtrace-mark` exists
on the stick. `decode.py` groups the writes between a before/after pair
into that command's block and keeps an explicit unattributed bucket for
writes outside any pair (autonomous alarm/AVC events, netdevice notifier
activity).

`--summary` prints the trace's own header line first (`total=`, `dropped=`,
`armonly=`, `skipped=`, `tables=`, `runs=`, straight from `/proc/rtk_regtrace`'s first line), then the usual
per-phase and per-command breakdowns, then a "blocks in order" section: one
line per command bracket, in the order it appeared in the trace, including a
bracket with zero writes inside it (`cmd 13: 0 writes`) -- a polling command
that only reads would otherwise never show up, because the per-command
breakdown above only creates an entry for a command once a write lands in
it. `--collapse` merges a run of consecutive brackets that share both the
command number and the exact write sequence (same registers, in the same
order, values ignored) into one line with a repeat count
(`cmd 13: 1 writes (repeated 2x)`) -- the shape a polling loop takes.

The first mark written by a process also clears the ring once, before the
mark itself: the OLT re-provisions every ME right after O5 on every boot,
and by then rtk_init has already filled some 12k of the 16k ring with boot
writes already captured separately (phase 0), so the first command capture
starts from a clean ring instead of competing with them for space.

The same clear also turns on `armonly` (`echo armonly 1 > /proc/rtk_regtrace`),
because clearing the ring is not enough on its own: a background register
writer with no command bracket around it -- the indirect-table handshake on
0x1d00c is the one observed in practice -- runs at around 1600 writes/second
and refills the whole ring in seconds, before OMCI provisioning even starts,
so an unarmed capture attributes nothing. With `armonly` on, a write (and a
read too, if `on rw` is also set) is recorded only while armed, and only the
M entries themselves -- always recorded, never gated by `armonly` -- control
arming: a mark with the phase bit clear (before a command) arms, one with it
set (after) disarms. `echo armonly 0` turns it back off.

`armonly` is not always enough by itself: a single long command can be noisy
enough on its own to wrap the whole ring before its own after-mark lands.
On isp1, cmd 51 (MIB reset) alone pushed over 16k writes with `armonly`
already on, more than 16k of them at 0x012000-0x01202c -- what the v3
comment here called "CPU-port NIC interrupt registers" (0x12000/0x12008)
turned out, once the register map identified them, to be
`RTL9602C_TBL_ACCESS_CTRL`/`_STS`/`_WR_DATA`/`_RD_DATA`, the switch's own
indirect table-access handshake under sustained polling, not a NIC IRQ. That
volume of polling wrapped a (then) 16384-entry ring within that single
command and evicted its own before-mark before the after-mark was ever
written; the surviving trace held only the after-mark of cmd 51, followed by
dozens of zero-write cmd 13 polls, and no before-mark of cmd 51 anywhere
(`isp1-260922-boot2.txt`). `echo skip <lo_hex> <hi_hex> > /proc/rtk_regtrace`
adds an inclusive address range (up to 4) that is dropped before it ever
reaches the ring, regardless of `on`/`rw`/`armonly` -- an M entry is never
skipped, only W/R. `echo skip clear` empties the list.
`regtrace_clear_once()` in apply.c seeds two ranges right after
`armonly 1`: the `TBL_ACCESS_*` range above, and the switch's own
`CHIP_IRQ_ENABLE` pair on 0x1d00c/0x1d010 seen the same way in the v1 boot tail.
The header line printed by `cat /proc/rtk_regtrace` carries `armonly=`, the
current `armed=` state, and `skipped=` (entries dropped by the skip list,
separate from `dropped=`, which counts entries evicted by ring
wraparound).

## Table trace (kind T/t/D/R)

Skipping 0x012000-0x01202c to silence the polling above has a cost:
that range is also the *only* path every switch-core table commit takes --
classify (CF) rule/action, ACL data/mask/action, VLAN, and L2 unicast/
multicast LUT all funnel through one table read/write layer, which
dispatches into that same `TBL_ACCESS_*` handshake.
Skipping the register range for noise also silences the one place a table
commit's actual payload (the CF rule pattern, the VLAN entry, the ACL
action) would show up. A second, independent hook one level
above the register pair closes that gap: a table write is recorded before it descends
into the handshake (using the caller's own payload, so there is nothing to
wait for), and a table read is recorded after -- its result is
meaningless before the call fills it in, and is only recorded when the call
returns success.

Each table op pushes one kind-`T` (write) or kind-`t` (read) entry with
`addr = table<<16 | index` (table id, row index), immediately followed by
one kind-`D` entry per data word with `addr = word-index<<24 | table` and
`val` = the word. Same `on`/`armonly` gate as a plain W/R; a read (`t` and
its `D`'s) is additionally gated on `rw`, exactly like a plain register
read -- the default `rw=0` capture is writes only. **No skip check either
way** -- these `addr` values are packed table/index tags, not MMIO
addresses, so running them past the MMIO skip ranges would be meaningless.
The header line's `tables=N` counts T/t ops (not D words).

**Run-length coding.** `isp1-260922-boot4.txt` showed a single cmd 51
bracket alone push over 24k T/t/D entries and wrap even the 32768-entry ring
by itself -- one bridge connection rewrites whole tables row by row (a
table-clearing loop, most likely every CF rule row). `regtrace_table_row()`
keeps the last pushed row (kind, table, index, data words)
in a small static buffer; a new row of the same kind and table, at index+1,
with byte-identical data words, is not pushed at all -- only a run counter
advances. The run closes with one kind-`R` entry, `addr = table<<16 |
first_index`, `val` = the run length, when a non-matching row arrives, or
when the ring is read out (every open of `/proc/rtk_regtrace` -- a `cat` or
a control write alike -- flushes any run in progress, so a live read never
finds a dangling one). `clear` resets the run state directly, without
closing it first: whatever it would push is wiped by the same clear a
moment later anyway. A table-clearing loop of 512 identical rows this way
costs one T/D group plus one R entry instead of 512. The header line's
`runs=N` counts R entries pushed.

**Kind `R` is dual-purpose** and this is a known, accepted ambiguity: it is
both a table run's closing entry (this section) and, unrelated, a plain
register read, the original kind the tracer supported first. The two
cannot be told apart by the kind byte alone. In practice
they rarely coexist: a run's `R` only exists at all when writes with
run-length-eligible rows occurred, while a plain register-read `R` only
exists in an `on rw` capture, and `on rw` is documented above as a
throwaway diagnostic capture, not the default. `decode.py` and `compare.py`
disambiguate the only way they safely can -- an `R` immediately following an
in-progress `T`/`t` (i.e. its `D`'s already seen) is a run close; any other
`R` decodes as an ordinary register read.

`decode.py` folds a T/t entry, its D's, and a trailing R if present into
one line:

    table <name or id> idx <N>: <word0> <word1> ...                     (no run)
    table <name or id> idx <A>..<B> (run <N>): <word0> <word1> ...       (run of N rows)

e.g. `table CLS_RULE_A idx 3: 0xdeadbeef 0xcafef00d`, or
`table CLS_RULE_A idx 3..7 (run 5): 0xdeadbeef 0xcafef00d`. Table names
are ours, by table id (`tools/regtrace/decode.py`'s `TABLE_NAMES`, the
same list as `odi_switch_hw.h`'s `enum odi_sw_table`); an id past the end of
that list prints as `TABLE_<id>` instead of crashing. `--summary` counts
table ops and runs per command, both in the per-command breakdown (`N table
ops, M runs`, plus a `table <name>` line per distinct table touched) and in
the "blocks in order" section (`cmd 51: 0 writes, 1 table ops, 1 runs`).
`compare.py` treats T/t/D/R as part of the bracket sequence exactly like W,
comparing a run in its run form (never expanded into per-row entries), so a
changed table row -- or a run that closed at a different length -- fails a
comparison the same way a changed register value would.

Capture recipe (one ME per capture window, so its markers and writes do not
mix with the next ME's):

    # on the stick: /etc/config/regtrace.on and /etc/config/regtrace-mark
    # both present, omcid running in apply mode (-a)
    echo clear > /proc/rtk_regtrace
    # issue ONE ME create/set from the OLT side, or via the test harness
    scp -O root@<stick>:/tmp/regtrace.txt <me-name>.txt
    python3 tools/regtrace/decode.py <regmap.txt> <me-name>.txt --summary

Repeat clear/apply/pull per ME. The recommended order matches the handler
dispatch apply.c already expects: OntData/Ontg/Ont2g/SWImage first (no
hardware writes -- a negative control that no marker ever brackets an empty
write set), then GemPortNetworkCtp/TCont, then the MacBridgeServiceProfile
family, then VlanTaggingFilterData/dscp remap, then multicast/flood config.
`echo on rw` is not required for attribution itself (writes alone carry the
signal); use it once, on a throwaway capture, to check for a
read-modify-write pattern inside a single command that a writes-only trace
would under-report.

## Replay

`sequence.py` turns one phase of a RAW trace (not decode.py's output) into a
compact replay script: an indirect-table fill loop (five `TABLE_WRITE_WORD`
writes, 0x12008, then one `TABLE_CMD` write, 0x12000) becomes one `t`
line, plain register writes stay `w` lines in their original position.

    python3 tools/regtrace/sequence.py <trace.txt> --phase "rtk_init vlan" > seq.txt
    python3 tools/regtrace/sequence.py <trace.txt> --phase "rtk_init vlan" --summary

No poll line is emitted. The original design expected the stock driver to
spin-wait on `TABLE_CMD` bit 31 between table operations; the isp1
reads-included baseline capture showed neither half of that: the recorded
CTRL write never carries the busy bit, and the read that follows it is
one-shot, not a loop. The `t` line therefore carries the CTRL word RAW, as recorded, and
`regreplay` writes it verbatim rather than reconstructing it from spa/method
fields. `--rw <trace-with-reads.txt>` is still accepted, for a later phase
whose reads-included trace does show real polling.

Also load-bearing: `TABLE_WRITE_WORD` holds its value on real hardware
across `TABLE_CMD` writes -- a common vendor pattern (seen clearing
`vlan`) writes the data words once, then issues many CTRL-only writes with
just the index changing. `sequence.py` tracks that (never resets the
pending WR_DATA words on a table write); replaying it as zero for every
entry after the first would be silently wrong.

`replay.sh <host> <root-password> <script>` pushes the CURRENT
`rootfs/skeleton/etc/scripts/regreplay` from this checkout to `/tmp` on the
stick and runs that copy (all its diag commands go through one `/bin/diag`
process), rather than trusting whatever regreplay the running image was
flashed with. A stick flashed before a sequence.py/regreplay format change
(the `t` line lost its `spa=/method=` comment and gained a raw CTRL word as
its last column, commit `9d6c934`) would otherwise silently misparse
table-entry lines: the Task 4 dry run on isp1's `odi-oss-260921-r2` (flashed
one commit before that fix) hit exactly this, on the harmless `stp` script
(no table entries) before it could reach one that had them. This is for
POST-boot experiments only -- testing a replay script by hand without a
reboot.

rcS reads two flags from the config partition for the skip/replace
experiment (`docs/TRIAL-BOOT.md` has the full procedure):
`/etc/config/skip-steps` (space- or newline-separated `rtk_init` step names
to leave out of the boot loop; never `intr`, `irq`, `ponmac`) and
`/etc/config/regtrace.rw` (switches the ring to `on rw`).

**In-place replay.** Replaying after boot is too late
for any step before `cpu`: without the switch-to-CPU-port path up, the
host's own IP stack cannot cross the switch, so the post-boot ssh replay
described above can never even run, and the userland watchdog reverts the
board first. So for a skipped step, rcS itself replays it, in place, the
instant it would have run the vendor step: if
`/etc/config/replay/<step>.txt` (or `<step>.txt.gz`, `zcat`'d to `/tmp`
first) exists, rcS runs it through `/etc/scripts/regreplay` right there and
leaves the crumb `rtk_init step <step>: REPLAYED <last regreplay line>`
(the full log is `/tmp/regreplay-<step>.log`); otherwise it leaves the old
`SKIPPED` crumb. `regreplay` batches its diag commands at 2000 lines per
`/bin/diag` process (`vlan`'s ~8300 script lines are ~49000 diag commands,
well past what one process should be asked to run inside a 120s timeout);
its summary line now reports the batch count too.

## Replay tables in the image

Three captured write sequences are replayed by the kernel drivers
themselves: the per-verb `rtk_init` writes (`mksdkinit.py`), the
module-load writes (`mkmodload.py`) and the GPON boot init
(`mkgponinit.py`). They are not compiled into the kernel: each generator
writes a firmware blob, checked in under `rootfs/skeleton/lib/firmware/odi/`
(`sdkinit.bin`, `modload.bin`, `gpon_init.bin`), and the driver loads it
with `request_firmware()` when the trigger fires, validates it whole
(header, size, CRC-32, every record) and releases it once applied.
`replayblob.py` documents the format; `replayblob.py dump <file>` prints a
blob as text, which is also what a generator prints when it is given no
output path:

    python3 tools/regtrace/mksdkinit.py boot.txt rootfs/skeleton/lib/firmware/odi/sdkinit.bin
    python3 tools/regtrace/replayblob.py dump rootfs/skeleton/lib/firmware/odi/sdkinit.bin
