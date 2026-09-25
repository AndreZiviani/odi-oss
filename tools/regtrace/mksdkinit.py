#!/usr/bin/env python3
"""Build the SDK-init replay table ("SDK-init replay") from a regtrace v5
boot capture of the rtk_init verbs and PON bring-up steps.

    mksdkinit.py <boot-capture.txt> [out.bin]

Reads a raw `W`/`R`/`T`/`D` regtrace dump (same format decode.py/compare.py/
mkmodload.py/mkgponinit.py already read: `<ns> KIND addr val` per line,
`#`-prefixed header lines ignored) and writes the sdkinit replay table as a
firmware blob (replayblob.py has the format; the checked-in copy is
rootfs/skeleton/lib/firmware/odi/sdkinit.bin, which the kernel loads with
request_firmware() for each verb) -- one ordered event list PER VERB, the
records grouped by verb, in the order the capture itself records within
that verb own span, for every verb
in VERBS_ORDER below: the twenty rtk_init module inits (switch svlan stp
oam acl qos sec rate classify stat trunk l2 vlan port mirror cpu rldp trap
gpio time ponmac) and four PON steps (i2c i2cen gpon rxsd). intr and irq
are deliberately NOT in this list -- they register software interrupt
handlers, not switch state, and stay on the vendor path until a later
phase (odi_intr) replaces them.

A verb own span is [its own `== <uptime> rtk_init <verb>` or `== <uptime>
pon step <verb>` marker line, the NEXT marker line in the file), using
every marker the capture records, not only the ones VERBS_ORDER lists --
so a verb this table does not cover (intr, irq, gpondrv, gponsn, ...) is
excluded from its neighbour own span rather than silently absorbed into
it. Only the FIRST occurrence of a given verb own marker is used (this
capture -- one clean boot -- has exactly one of each; a future capture
with a retry would still generate from the first attempt, matching
mkgponinit.py own convention).

Each record decodes to a struct odi_sw_modload_event (odi_switch_dal.h,
"module-load replay") -- the same register/table-row event shape
odi_switch_init_modload() already replays through, and the same
odi_switch_table_write()/odi_reg_write() apply loop, generalised from one
flat category-selected table to one run of records per verb. category and
reg_group are always 0 here and carry no meaning for this table:
sdkinit own bisection granularity is one verb (odi_switch_sdkinit_mask,
one bit per verb, kernel/extra/drivers/net/ethernet/odi/
odi_switch_sdkinit.h), not a sub-verb split.

Two write kinds, exactly as mkmodload.py/mkgponinit.py already reconstruct
(their own docstrings have the full format):

  - a plain `W` line is a register write, kept unless its address falls in
    0x012000-0x01202c (TABLE_CMD/STATUS/WRITE_WORD/READ_WORD -- the
    switch-core indirect-table-access handshake, mechanics behind the T/D
    table rows below, not a register value of its own: replaying it
    separately, divorced from that handshake own WR_DATA-then-CTRL-then-poll
    lockstep, could fire the indirect-access "start" bit against stale
    WR_DATA).
  - a `T` line opens a table row (table id and start index packed as
    table<<16|index), each `D` line after it is one data word, and a
    trailing `R` (run length -- this capture has exactly one, in `vlan`:
    a 4096-row sweep of the VLAN table, all rows identical) replays the
    SAME row content across index..index+run_len-1, one
    odi_switch_table_write() call per index -- exactly what
    odi_switch_init_modload() already does for its own generated table,
    and what mkgponinit.py own flush() already implements; this generator
    reuses that same expansion.

The lowercase `w` kind is a WRITE too, to a different physical register
block entirely (the SoC GPIO/timer window, 0xb8xxxxxx, MIPS KSEG1 -- not
switch-core): kept IN POSITION as a THIRD event kind, ODI_SW_MODLOAD_SOC
(odi_switch_dal.h), carrying the full physical address rather than a
switch-core MMIO offset. This generator used to exclude it the same way it
excludes the unrelated TBL_ACCESS range above -- by requiring the exact
string "W", case-sensitive, the same rule mkmodload.py/mkgponinit.py used
-- which silently dropped it rather than skipping it deliberately. That
turned out to matter: the `switch` verb own reference capture opens with
`w 0xb800063c 0x00000030` (bit 5, "Enable PONPBO IP") immediately before
writing PONQ_0xf02190 (0x00f02190) -- a switch-core register inside the same
PBO block that hangs the bus if it is never powered on, and this generator was
producing a `switch` verb replay that skipped the enable and went straight
to the hang-prone write. The lowercase `r` kind (the same block own reads,
never replayed by anything in this codebase) is recognised and dropped,
same as a plain register read.

Recognised kinds are exactly `W R T D w r` (`M`, the command-bracket mark
this format also defines, is recognised and dropped too, on the same
footing as `r`, in case a boot capture ever picks one up incidentally).
Anything else that reaches a data line -- a kind this generator has never
seen and so cannot know is safe to fold into an existing case -- is a fatal
error, not a silent skip: that silent skip is exactly the shape of bug this
file own history now is the argument against repeating.
"""
import os
import re
import sys

import replayblob

TBL_ACCESS_LO = 0x012000
TBL_ACCESS_HI = 0x01202c

# The 20 rtk_init verbs (rootfs/skeleton/etc/init.d/rcS own step loop,
# vendor order) then the 4 PON steps this table covers, in the FIXED order
# odi_switch_sdkinit.h own mask bit enum uses (bit N == this list own index
# N, and the verb byte of each record). intr and irq are never in this list
# -- see this file own docstring.
VERBS_ORDER = replayblob.SDKINIT_VERBS

# odi_sw_modload_event own bound (odi_switch_dal.h): the widest table row
# this codebase has ever needed to carry (ACL_PATTERN/ACL_PATTERN_MASK).
MAX_WORDS = replayblob.MAX_WORDS

# Deliberately permissive on the kind itself: a `#`/`==`-prefixed line, or
# anything else that is not a data line at all, still will not start with a
# digit and so still will not match -- but ANY data line, whatever its
# kind, now reaches the dispatch in parse_window() below, which is where an
# unrecognised kind is refused instead of silently vanishing.
DATA_LINE_RE = re.compile(r"^\d+\s+(\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s*$")
MARKER_RE = re.compile(r"^==\s+\S+\s+(?:rtk_init|pon step)\s+(\S+)\s*$")

# Every kind this generator understands, and what happens to it -- see
# parse_window() own dispatch. Anything not in this set that still matches
# DATA_LINE_RE (i.e. genuinely looks like a data line) is a fatal error.
KNOWN_KINDS = frozenset("WRTDwrM")


def find_verb_windows(lines):
    """Returns {verb: (start_line, end_line)} for the FIRST occurrence of
    each verb VERBS_ORDER names -- end_line is the line of the NEXT marker
    the capture records, of any name, or end-of-file for the last one.
    A verb whose marker never appears is simply absent from the dict; the
    caller emits no records for it, same as a verb whose
    marker appears but whose span holds zero replayable events (e.g.
    `trunk`, `total=0` in the reference capture).
    """
    markers = []
    for i, line in enumerate(lines):
        m = MARKER_RE.match(line.strip())
        if m:
            markers.append((i, m.group(1)))

    windows = {}
    for idx, (line_no, name) in enumerate(markers):
        if name in VERBS_ORDER and name not in windows:
            end = markers[idx + 1][0] if idx + 1 < len(markers) else len(lines)
            windows[name] = (line_no, end)
    return windows


def parse_window(lines, start, end):
    """Yields ("reg", addr, val), ("soc", addr, val), or ("table", table_id,
    index, words) for every write reconstructed from lines[start:end], in
    file order.

    A table row OPENING entry (its T plus the D lines right after it) is
    committed and yielded the moment the D lines are done -- as soon as
    any other line arrives, of any kind -- exactly the "T+D print
    IMMEDIATELY, only the closing R is deferred" rule test/
    odi_switch_mock.h own odi_mock_table() already implements (its own
    file comment), so a replay through that mock re-folds back into the
    identical bracket position decode.py/compare.py show for the raw
    capture. Only a run OWN CONTINUATION -- the silently-folded rows a
    closing R reveals, index+1..index+run_len-1 -- is deferred and
    expanded at the point the R itself appears, never at the row own
    opening position: neither `regtrace_table_row()` (tools/regtrace/
    README.md "Run-length coding") nor the mock know a run own length
    until it closes, so a replay cannot place those extra rows any
    earlier than the real hardware ring did.

    This split matters for exactly one span in the reference capture:
    `vlan` own one R (a 4096-row VLAN-table sweep, index 0..4095, every
    row identical) is preceded by nine W lines (the VLAN_INGRESS_CHECK/
    EGRESS_TAG per-port group) sitting BETWEEN the run own opening T/D
    and its closing R -- `regtrace_table_row()` own "last pushed row"
    state is entirely separate from the plain register-write ring push,
    so those nine W lines neither close nor extend the run; they are
    simply interleaved in the ring by whatever order the driver issued
    them in. Emitting the opening row immediately (rather than deferring
    the whole row to flush time, as mkmodload.py/mkgponinit.py do --
    neither of their own captures ever interleaves a W between a row own
    T/D and its R, so neither needed this) keeps that interleaving
    intact: opening row, then the nine W lines, then the run own
    remaining 4095 rows, all expanded together the moment the R arrives
    -- byte-for-byte the position the raw capture itself shows, and what
    compare.py own bracket-by-bracket diff needs to match.

    decode.py own disambiguation rule ("an R immediately following an
    in-progress T/t is a run close, any other R is a plain register
    read") would mis-read this exact spot as a register read of a bogus
    address (0x00170000), because decode.py flushes pending on every W.
    That is a safe default for a capture that might carry genuine
    register reads (`on rw`), but every regtrace header line in this
    capture reads `rw=0` -- writes only -- so R can ONLY ever be a
    run-close here, never a real register read, regardless of what sits
    between a run own T/D and its R.
    """
    pending = None  # {"table":, "index":, "words": [...], "yielded": bool}

    def commit_opening_row():
        """Yields the pending row own opening entry exactly once, the
        first time anything asks for it after its D lines are read. A
        no-op once already yielded, or with nothing pending.
        """
        if pending is None or pending["yielded"]:
            return []
        pending["yielded"] = True
        return [("table", pending["table"], pending["index"], tuple(pending["words"]))]

    for i in range(start, end):
        raw = lines[i].rstrip("\n")
        m = DATA_LINE_RE.match(raw)
        if not m:
            continue
        kind, addr_s, val_s = m.groups()
        if kind not in KNOWN_KINDS:
            sys.exit(
                "mksdkinit.py: line %d has kind %r, which this generator "
                "does not understand -- add explicit handling (or confirm "
                "it is safe to ignore) before generating from this "
                "capture, do not let it vanish silently: %r"
                % (i + 1, kind, raw)
            )
        addr = int(addr_s, 16)
        val = int(val_s, 16)

        if kind == "r" or kind == "M":
            # SoC-window read, or a command-bracket mark: neither is ever
            # replayed, but both still commit any table row already open
            # (a stray read/mark between a T/D and its R must not look
            # like it broke or silently swallowed that run).
            for ev in commit_opening_row():
                yield ev
            continue
        if kind == "w":
            # SoC-window write ("kind w") -- kept IN POSITION, same
            # commit-but-do-not-clear-pending treatment as W below, since
            # (like W) it can appear between a table run own open and its
            # close without being part of that run.
            for ev in commit_opening_row():
                yield ev
            yield ("soc", addr, val)
            continue
        if kind == "T":
            for ev in commit_opening_row():
                yield ev
            pending = {"table": (addr >> 16) & 0xffff, "index": addr & 0xffff,
                       "words": [], "yielded": False}
            continue
        if kind == "D":
            if pending is not None:
                pending["words"].append(val)
            continue
        if kind == "R":
            for ev in commit_opening_row():
                yield ev
            if pending is not None:
                run_len = val
                for k in range(1, run_len):
                    yield ("table", pending["table"], pending["index"] + k,
                           tuple(pending["words"]))
            pending = None
            continue
        # kind == "W" (the only kind KNOWN_KINDS has left unhandled above)
        # -- commits the opening row if not already done, but does NOT
        # clear `pending`: see this function own docstring.
        for ev in commit_opening_row():
            yield ev
        if TBL_ACCESS_LO <= addr <= TBL_ACCESS_HI:
            continue
        yield ("reg", addr, val)

    for ev in commit_opening_row():
        yield ev


def build_verb_events(path):
    """Returns {verb: [event, ...]} for every verb in VERBS_ORDER, in
    capture order within that verb own span -- a verb never seen in the
    capture, or seen with zero events, gets an empty list either way.
    """
    with open(path) as f:
        lines = f.readlines()

    windows = find_verb_windows(lines)
    if not windows:
        sys.exit("mksdkinit.py: no `== ... rtk_init <verb>` / `== ... pon step <verb>` "
                  "marker found in %s" % path)

    result = {}
    for verb in VERBS_ORDER:
        events = []
        if verb in windows:
            start, end = windows[verb]
            for ev in parse_window(lines, start, end):
                if ev[0] == "table" and len(ev[3]) > MAX_WORDS:
                    sys.exit("mksdkinit.py: verb %s table %d row has %d words, max %d supported"
                              % (verb, ev[1], len(ev[3]), MAX_WORDS))
                events.append(ev)
        result[verb] = events
    return result


def build_records(verb_events):
    """Returns (records, {verb: count}): one blob record per event, grouped
    by verb in VERBS_ORDER, capture order within a verb.
    """
    records = []
    counts = {}
    for verb_id, verb in enumerate(VERBS_ORDER):
        events = verb_events[verb]
        counts[verb] = len(events)
        for ev in events:
            if ev[0] == "reg":
                records.append(replayblob.switch_reg(ev[1], ev[2], verb=verb_id))
            elif ev[0] == "soc":
                records.append(replayblob.switch_soc(ev[1], ev[2], verb=verb_id))
            else:
                _, table, index, words = ev
                records.append(replayblob.switch_table(table, index, words, verb=verb_id))
    return records, counts


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    capture_path = argv[1]
    out_path = argv[2] if len(argv) > 2 else None

    verb_events = build_verb_events(capture_path)
    records, counts = build_records(verb_events)
    blob = replayblob.pack(replayblob.TABLE_SDKINIT, records)

    if out_path:
        with open(out_path, "wb") as f:
            f.write(blob)
        # Only the capture basename: the reference capture lives outside
        # this public repo.
        sys.stderr.write(
            "%d verbs written to %s from %s, %d events total (%s)\n"
            % (len(VERBS_ORDER), out_path, os.path.basename(capture_path),
               sum(counts.values()),
               ", ".join("%s=%d" % (v, counts[v]) for v in VERBS_ORDER))
        )
    else:
        sys.stdout.write(replayblob.dump(blob))


if __name__ == "__main__":
    main(sys.argv)
