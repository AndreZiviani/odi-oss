#!/usr/bin/env python3
"""Build the GPON MAC block init-write replay table from a regtrace v5
boot capture.

    mkgponinit.py <boot-capture.txt> [out.bin]

Reads a raw `W`/`R`/`T`/`D` regtrace dump (same format decode.py/compare.py
read: `<ns> KIND addr val` per line, `#`-prefixed header/marker lines ignored
except the `== <uptime> pon step <name>` markers used to find each step own
span) and writes the GPON boot init table as a firmware blob (replayblob.py
has the format; the checked-in copy is rootfs/skeleton/lib/firmware/odi/
gpon_init.bin, which the kernel loads with request_firmware() at the first
gponact) -- one record per EVERY register write or table-row write the vendor driver makes
across the five PON steps gpondrv/gpondev/gponsn/gponpw/gponact, in the
capture own order, for the whole switch-core address space (not only the
GPON MAC block own 0x700000-0x706fff window, as an earlier generation of
this table restricted itself to -- s18 own boot/act captures show the
vendor steps also write plain switch-core registers outside that block, e.g.
MAX_FRAME_LEN_1/PORT_MAX_FRAME_SEL at 0x011018/0x011010 and the PONQ_COUNT_MASK
block at 0xf02000+, and those matter too: MAX_FRAME_LEN_1 at 0 makes every
non-empty frame checked against that profile look oversize, the "frames
never reach the OLT" candidate odi_switch_dal.c own parity table already
flags for the same address).

A step own span is [its marker line, the NEXT marker line in the file),
using every marker the capture records, not only the five this table
covers -- so a step that is not one of these five (e.g. omcimods, between
gpondev and gponsn on the capturing board) is excluded from its neighbour own
own span rather than silently absorbed into it.

Two kinds of write are reconstructed, exactly as tools/regtrace/mkmodload.py
already does for a different capture (its own docstring has the T/D/R
format and the run-length-run caveat, restated briefly here):

  - a plain `W` line is a register write, kept unless its address falls in
    0x012000-0x012fff (TABLE_CMD/STATUS/WRITE_WORD/READ_WORD, the switch-core
    indirect-table-access handshake -- mechanics behind the T/D table rows
    below, not a register value of its own; replaying it as a second, raw
    write divorced from that handshake own WR_DATA-then-CTRL-then-poll
    order could fire the indirect-access "start" bit against stale WR_DATA,
    the same reason mkmodload.py drops this same address range).
  - a `T` line opens a table row (table id and start index packed as
    table<<16|index), each `D` line after it is one data word, and a
    trailing `R` (run length) replays the SAME row content across
    index..index+run_len-1 -- one odi_switch_table_write() call per index,
    exactly as odi_switch_init_modload() already does for its own generated
    table. Neither boot capture available to this pass exercises a table
    write inside these five steps at all (confirmed: zero T/D lines fall
    inside any of the five step spans on the board these captures came
    from) -- this path exists so a future capture that does hit one is
    handled the same way, not left to silently vanish.

Every entry from the two captures available to this pass came from the
switch-core register file odi_switch_hw.h own ODI_SWITCH_MMIO_BASE window
-- both odi_reg_write() (register entries) and odi_switch_table_write()
(table entries) already resolve into that same window, so one generated
table needs no per-entry space marker for it.

Two of the register writes are config-derived, not fixed constants: the six
USF_PLOAM_TX_WORD words the gponsn step writes (the real serial
number) are tagged ODI_GPON_INIT_SN_WORD(n) instead of a literal value, so
odi_gpon_init_apply() can substitute the box own configured serial number
at run time instead of replaying this one capture own board SN --
odi-oss is public, and this project own rule is no board-specific secrets
checked in (see the caller substitution in odi_gpon_init.c). Every other
entry, including the Realtek-default serial-number PLACEHOLDER gpondev
pre-arms (vendor-id bytes spelling "RTKG"), replays as a literal constant
-- that placeholder is not this box identity, just a fallback fixed
pattern the real chip vendor ships. sn tagging only ever applies inside the
gponsn step own span (never to a same-looking address elsewhere).

gponpw (the password step) captured zero entries on the board this capture
came from (empty config) -- nothing to tag or replay for it; a future
capture with a non-empty password would show up here as ordinary literal
writes in the gponpw step own span, same as any other step.

The lowercase `w` kind -- a WRITE, to a different physical register block
entirely (the SoC GPIO/timer window, 0xb8xxxxxx, not switch-core) -- used
to be excluded the same way mkmodload.py/mksdkinit.py own LINE_RE did,
by requiring the exact string "W", case-sensitive: silently, not
deliberately. That mattered here: the reference boot capture
(isp1-260922-s18-boot.txt) carries a `w 0xb8003324`/`0xb8003328`
GPIO-mux keepalive pair inside THREE of the five tracked steps
(gpondrv, gpondev, gponact) -- confirmed by re-running this generator own
window logic against that capture, not asserted from memory. This
generator does not yet reproduce SoC writes (odi_gpon_init_event has no
kind for one, and odi_gpon_init_apply() has no allowlisted-write path the
way odi_switch_sdkinit_apply() now does) -- `w` is recognised (so it no
longer disappears into an implicit "anything else" case) but is a FATAL
error if actually encountered, so generating from a capture that has one
requires a human decision first, not a silent copy of the old bug.
"""
import os
import re
import sys

import replayblob

# USF_PLOAM_TX_WORD array, 6 words carrying message content
# (odi_gpon_hw.h ODI_GPON_USF_PLOAM_TX_WORD_BASE/ODI_GPON_US_PLOAM_WORDS).
US_PLOAM_DATA_BASE = 0x7050e0
US_PLOAM_DATA_WORDS = 6

# TABLE_CMD/STATUS/WRITE_WORD/READ_WORD -- the switch-core indirect-table
# handshake (odi_switch_hw.h ODI_SW_TABLE_*_OFF), not a register value
# of its own; dropped from the register category for the same reason
# mkmodload.py already drops it (this file own docstring above).
TBL_ACCESS_LO = 0x012000
TBL_ACCESS_HI = 0x012fff

# The five PON steps this table covers, in the order they run at boot.
STEPS_ORDER = ("gpondrv", "gpondev", "gponsn", "gponpw", "gponact")

# odi_sw_modload_event own bound (odi_switch_dal.h): the widest table row
# this codebase has ever needed to carry (ACL_PATTERN/ACL_PATTERN_MASK). Neither
# capture available to this pass exercises a table write inside these five
# steps at all, so this is a defensive cap, not a value fit to real data.
MAX_WORDS = replayblob.MAX_WORDS

# Deliberately permissive on the kind itself -- see KNOWN_KINDS below and
# this file own module docstring for why.
DATA_LINE_RE = re.compile(r"^\d+\s+(\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s*$")
STEP_RE = re.compile(r"^==\s+\S+\s+pon step\s+(\S+)\s*$")

# T/D/R/W are real replay data (handled explicitly below); M (a
# command-bracket mark) and "r" (a SoC-window READ -- never replayed by
# anything in this codebase, same as a plain switch-register read) are
# known and inert. "w" is a write this generator has confirmed exists in
# its own reference capture and does NOT reproduce (module docstring) --
# recognised so it fails loudly instead of silently vanishing. Anything
# else is unrecognised outright.
KNOWN_KINDS = frozenset("WRTDMwr")


def find_step_windows(lines):
    """Returns {step_name: (start_line, end_line)} for the first occurrence
    of each of the five PON steps this table covers -- end_line is the line
    of the NEXT marker the capture records, of any name, or end-of-file for
    the last one, so a step outside this table own five (e.g. omcimods)
    is excluded from its neighbour own span rather than silently absorbed
    into it. Missing steps are simply absent from the dict -- the caller
    only needs "gpondrv" not to be missing, and tolerates gponpw being
    entirely absent (as on both captures available to this pass).
    """
    markers = []
    for i, line in enumerate(lines):
        m = STEP_RE.match(line.strip())
        if m:
            markers.append((i, m.group(1)))

    windows = {}
    for idx, (line_no, name) in enumerate(markers):
        if name in STEPS_ORDER and name not in windows:
            end = markers[idx + 1][0] if idx + 1 < len(markers) else len(lines)
            windows[name] = (line_no, end)
    return windows


def parse_window(lines, start, end):
    """Yields ("reg", addr, val) or ("table", table_id, index, words) for
    every write reconstructed from lines[start:end], in file order -- the
    same T/D/R reconstruction mkmodload.py own parse_events() already
    does (that script own docstring has the full format), restated here so
    this generator stays self-contained.
    """
    pending = None  # {"table":, "index":, "words": [...]} between a T and the D/R entries after it

    def flush():
        if pending is None:
            return []
        run_len = pending.get("run_len", 1)
        return [("table", pending["table"], pending["index"] + k, tuple(pending["words"]))
                for k in range(run_len)]

    for i in range(start, end):
        raw = lines[i].rstrip("\n")
        m = DATA_LINE_RE.match(raw)
        if not m:
            continue
        kind, addr_s, val_s = m.groups()
        if kind not in KNOWN_KINDS:
            sys.exit(
                "mkgponinit.py: line %d has kind %r, which this generator "
                "does not understand -- add explicit handling (or confirm "
                "it is safe to ignore) before generating from this "
                "capture: %r" % (i + 1, kind, raw)
            )
        if kind == "w":
            sys.exit(
                "mkgponinit.py: line %d is a kind-w (SoC-window) write "
                "(addr %s val %s) -- this generator does not reproduce SoC "
                "writes (see the module docstring) and dropping one "
                "silently is the exact bug mksdkinit.py had. Refusing to "
                "generate until a human decides whether this write needs "
                "replaying: %r" % (i + 1, addr_s, val_s, raw)
            )
        if kind == "M" or kind == "r":
            # Known, inert (a command-bracket mark, or a SoC-window read)
            # -- flush any pending table op first so its D lines are not lost,
            # same treatment as W below gives a stray line.
            for ev in flush():
                yield ev
            pending = None
            continue
        addr = int(addr_s, 16)
        val = int(val_s, 16)

        if kind == "T":
            for ev in flush():
                yield ev
            pending = {"table": (addr >> 16) & 0xffff, "index": addr & 0xffff, "words": []}
            continue
        if kind == "D":
            if pending is not None:
                pending["words"].append(val)
            continue
        if kind == "R":
            if pending is not None:
                pending["run_len"] = val
            for ev in flush():
                yield ev
            pending = None
            continue
        # kind == "W" (the only other value LINE_RE matches)
        for ev in flush():
            yield ev
        pending = None
        if TBL_ACCESS_LO <= addr <= TBL_ACCESS_HI:
            continue
        yield ("reg", addr, val)

    for ev in flush():
        yield ev


def build_events(path):
    with open(path) as f:
        lines = f.readlines()

    windows = find_step_windows(lines)
    if "gpondrv" not in windows:
        sys.exit("mkgponinit.py: no `== ... pon step gpondrv` marker found in %s" % path)

    events = []
    sn_words_seen = set()
    for step in STEPS_ORDER:
        if step not in windows:
            continue
        start, end = windows[step]
        for ev in parse_window(lines, start, end):
            if ev[0] == "reg":
                _, addr, val = ev
                is_sn = False
                sn_idx = 0
                if step == "gponsn" and US_PLOAM_DATA_BASE <= addr < US_PLOAM_DATA_BASE + 4 * US_PLOAM_DATA_WORDS:
                    idx = (addr - US_PLOAM_DATA_BASE) // 4
                    # Word 0 is the fixed onu_id/type header (0xff01) and word 5
                    # the fixed trailer (0x0005) -- only words 1..4 carry the
                    # eight serial-number bytes themselves, two per word, and
                    # are what needs substituting per box; 0 and 5 replay as
                    # literals like everything else.
                    if 1 <= idx <= 4:
                        is_sn = True
                        sn_idx = idx
                        sn_words_seen.add(sn_idx)
                events.append(("reg", addr, val, is_sn, sn_idx))
            else:
                _, table, index, words = ev
                if len(words) > MAX_WORDS:
                    sys.exit("mkgponinit.py: table %d row has %d words, max %d supported"
                              % (table, len(words), MAX_WORDS))
                events.append(("table", table, index, words))

    if sn_words_seen and sn_words_seen != {1, 2, 3, 4}:
        sys.stderr.write(
            "mkgponinit.py: warning: gponsn wrote SN-carrying words %s, expected {1,2,3,4}\n"
            % sorted(sn_words_seen)
        )
    return events


def build_records(events):
    """Returns (records, config-substituted serial-number words, table rows)."""
    records = []
    n_sn = 0
    n_table = 0
    for ev in events:
        if ev[0] == "reg":
            _, addr, val, is_sn, sn_idx = ev
            if is_sn:
                n_sn += 1
                # value is never read for a substituted entry
                # (odi_gpon_init_apply() overwrites it with the box own
                # configured serial number) -- zeroed here, not the
                # generating capture own real value, since the blob is
                # checked into this PUBLIC tree and the real value is this
                # one board own serial number (repo rule: no board-specific
                # identity in public source).
                records.append(replayblob.gpon_reg(addr, 0, sn_idx))
            else:
                records.append(replayblob.gpon_reg(addr, val))
        else:
            _, table, index, words = ev
            n_table += 1
            records.append(replayblob.gpon_table(table, index, words))
    return records, n_sn, n_table


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    capture_path = argv[1]
    out_path = argv[2] if len(argv) > 2 else None

    events = build_events(capture_path)
    records, n_sn, n_table = build_records(events)
    blob = replayblob.pack(replayblob.TABLE_GPON_INIT, records)

    if out_path:
        with open(out_path, "wb") as f:
            f.write(blob)
        # Only the capture basename: wherever the operator own capture
        # lives is not this public repo own concern.
        sys.stderr.write(
            "%d events written to %s from %s (%d config-substituted serial-number words, "
            "%d table rows)\n"
            % (len(records), out_path, os.path.basename(capture_path), n_sn, n_table)
        )
    else:
        sys.stdout.write(replayblob.dump(blob))


if __name__ == "__main__":
    main(sys.argv)
