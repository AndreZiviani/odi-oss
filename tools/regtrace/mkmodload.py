#!/usr/bin/env python3
"""Build the module-load replay data table ("module-load replay" /
"modload category 0 split") from a regtrace v5 capture of omcidrv.ko/
pf_rtk.ko loading.

    mkmodload.py <capture.txt> <regmap.txt> [out.bin]

regmap.txt is the register listing src/diag/tools/regmap-extract.py
writes with -t (the table read out of the stock firmware binary, names
from src/diag/tools/regnames.txt), the same file mklist.py/decode.py
read -- needed to resolve each address to its register for the family
groups inside category 0 (see below); the table-row categories (1-5)
never needed it.

Reads a raw `W`/`T`/`D` regtrace dump (same format decode.py/compare.py
read: `<ns> KIND addr val` per line, `#`-prefixed header lines ignored) and
writes the module-load replay table as a firmware blob (replayblob.py has
the format; the checked-in copy is rootfs/skeleton/lib/firmware/odi/
modload.bin, which the kernel loads with request_firmware() when the
`init_modload` trigger fires) -- one record per register write or
table-row write, IN THE CAPTURE'S OWN ORDER,
each tagged with the family it belongs to. The shipped modload.bin is this
output filtered with the production mask (replayblob.py filter, whose
docstring has the bits); the kernel replays every record of it:

    0  plain register writes (every W line except the TABLE_CMD/
       TABLE_STATUS/TABLE_WRITE_WORD/TABLE_READ_WORD plumbing at
       0x012000-0x01202c -- see below --
       and except the lowercase `w` kind, a different register block
       entirely, not switch-core). Sub-selectable by `reg_group`, below.
    1  classification-table bulk-clear sweep rows (CLS_DS_ACTION,
       CLS_US_ACTION, CLS_MASK_A/B, CLS_RULE_A/B) -- the FIRST write this
       capture makes
       to a given (table, index) pair
    2  classification-table specific rule rows -- a REPEAT write to a (table, index)
       pair already swept once; mechanical, content-independent (a second
       write to the same slot can only be a deliberate overwrite, sweep
       or not), so this generator does not need to know in advance which
       index is "the" rule -- a future capture with a different specific
       index, or more than one, is handled the same way
    3  the VLAN table row
    4  the L2_UNICAST table row
    5  the ACL_ACTIONS/ACL_PATTERN/ACL_PATTERN_MASK rows

Category 0 (register) events carry one more tag, `reg_group`, added by
the "modload category 0 split" after s13/s13-continued found the
OLT-acceptance fix and the LAN-broadcast cut both live inside category 0:
0 for the three LUT flood-mask addresses (0x1c020/0x1c024/0x1c028,
FLOOD_BCAST_PORTS/FLOOD_UNKN_MCAST_PORTS/FLOOD_UNKN_UCAST_PORTS -- these tie to the
asymmetric LAN cut), 1..N for every other register
address's own family, from the FAMILIES table below, keyed by register
address (a register it does not list gets a family of its own -- see
that table's own comment). replayblob.py filter maps a reg_group to mask
bit 6+reg_group.

Preserving the capture's own order matters beyond bookkeeping: this
tracer records ZERO run-length-folded rows in this specific capture (no
`R` entries at all) because the stock driver's sweep interleaves four classification tables
per loop iteration (CLS_RULE_A, CLS_MASK_A, CLS_DS_ACTION,
CLS_US_ACTION, repeating per index) -- no two consecutive table ops in the
raw file ever share the same table id, so the kernel-side (and this
project's own host mock, test/odi_switch_mock.h) run-length folder never
has two candidate rows to fold. Replaying the events in a different order
(e.g. grouped by category, all of table X before table Y) would risk
folding rows the real hardware never folded, breaking an exact replay
comparison for no functional reason -- so this generator keeps one flat,
ordered array; the runtime mask only skips entries in place, it never
reorders them.

Table-block plumbing: unlike an OMCI-command capture (boot3/boot5), where
the kernel-side skip list already drops `W` entries for the 0x012000-
0x01202c handshake registers before they reach the ring, THIS capture is
a module-load capture -- the skip list is not set up until
regtrace_mark() runs, which only happens once omcid processes its first
OMCI command, well after both modules have already loaded.
So this capture's raw `W` lines DO include 4131 writes to
0x012000-0x01202c -- the real, unfiltered mechanics (WR_DATA words, then
the CTRL "fire" write) behind every one of the 1032 table rows this same
capture also records as its own `T`/`D` entries. Replaying those raw
writes as a second, independent set of register events would not just be
redundant: divorced from odi_switch_table_write()'s own WR_DATA-then-
CTRL-then-poll-BUSY lockstep, a mask that replayed some but not all of
them (or replayed them out of the handshake's required order) could fire
the indirect-access "start" bit against stale WR_DATA. The `T`/`D`
category rules below already reconstruct each row correctly through
`odi_switch_table_write()`, which performs its own correct handshake --
so this filter drops the raw 0x012000-0x01202c writes from the register
category entirely, on purpose, not because the ring is expected to be
free of them.

The lowercase `w` kind (4 lines in the reference capture, at
0xb8003324/0xb8003328) is a WRITE, to a different physical register block
(the SoC GPIO/timer window, not switch-core) -- NOT a read, despite what an
earlier revision of this docstring claimed: this capture own header line
reads `rw=0` (writes only, no reads recorded at all), so a `w` line here
cannot be anything but a write. That earlier claim was never checked
against the capture it described. mksdkinit.py own near-identical
capture (isp1-260923-g4-boot.txt) turned out to have this exact same
address pair (0xb8003324/0xb8003328) be a GPIO-mux keepalive write, plus a
third address (0xb800063c) this capture does not happen to touch that is
load-bearing -- see mksdkinit.py own docstring for the fix that gave
the `switch`/`gpon` verbs a new event kind for exactly this. This generator
does NOT reproduce that fix: `w` is now a recognised kind (no longer
silently absorbed by parse_events() own catch-all), but hitting one is a
FATAL error, so a future capture that carries one is caught at generation
time rather than shipped silently -- the blob validation refuses a SoC
record in the modload table, although odi_replay_run() has an allowlisted
path for one; allowing it is a decision, not a generator change.
"""
import os
import re
import sys

import replayblob

TABLE_BLOCK_LO = 0x012000
TABLE_BLOCK_HI = 0x01202c

FLOOD_ADDRS = {0x01c020, 0x01c024, 0x01c028}  # FLOOD_BCAST_PORTS, FLOOD_UNKN_MCAST_PORTS, FLOOD_UNKN_UCAST_PORTS

# Classification-table ids (tools/regtrace/decode.py's TABLE_NAMES, same
# numbering as odi_switch_hw.h's enum odi_sw_table) that get the sweep-
# vs-specific repeat-write rule (category 1 or 2). Every other table id
# this capture contains gets a fixed category by id instead.
CLS_TABLE_IDS = {11, 12, 13, 14, 16, 17}  # CLS_DS_ACTION, CLS_US_ACTION, CLS_MASK_A/B, CLS_RULE_A/B
VLAN_TABLE_ID = 23
L2_UNICAST_TABLE_ID = 20
ACL_TABLE_IDS = {8, 9, 10}  # ACL_ACTIONS, ACL_PATTERN, ACL_PATTERN_MASK

CATEGORY_REG = 0
CATEGORY_CLS_SWEEP = 1
CATEGORY_CLS_SPECIFIC = 2
CATEGORY_VLAN = 3
CATEGORY_L2_UNICAST = 4
CATEGORY_ACL = 5


# ---------------------------------------------------------------------
# Register map loading -- two independent-but-agreeing address->name
# resolvers over the same map, because a single naive "array item i is
# at offset+4*i" model (decode.py's) gets a handful of switch-core
# registers wrong: mklist.py's own "Packing model" docstring (tools/
# regdump/mklist.py) explains why a NARROW register (width*count<=32)
# packs every port/array item into ONE shared word instead, and getting
# that wrong does not just mislabel a field, it invents addresses that
# collide with a different, real, wide register nearby (ODI_SW_CLASSIFY_SETUP_OFF/
# ACL_PORT_ENABLE/SW_0x015048 were exactly this bug the first time this codebase
# hit it). For the switch-core
# span (<0x200000) this generator resolves through the SAME corrected
# model mklist.py already uses (reimplemented here, not imported, to keep
# this script self-contained); for the
# GPON (0x700000+) and PON queue (0xF00000+) blocks -- never exercised
# by the DAL own earlier, careful register work, and not known to hit
# the narrow-packing collision -- the plain per-slot model is used, same
# as decode.py's own lookup.
# ---------------------------------------------------------------------
REG_RE = re.compile(
    r"^\s*(\d+)\s+0x([0-9a-fA-F]+)\s+width\s+(\d+)\s+array\s+(\d+)\.\.(\d+)\s+"
    r"port\s+(\d+)\.\.(\d+)\s+(\S+)"
)


def load_registers(regmap_path):
    """Yield (offset, width, count, name) for every register the map
    declares -- same shape as tools/regdump/mklist.py's own function.
    """
    with open(regmap_path) as f:
        for line in f:
            m = REG_RE.match(line)
            if not m:
                continue
            offset = int(m.group(2), 16)
            width = int(m.group(3))
            a0, a1 = int(m.group(4)), int(m.group(5))
            p0, p1 = int(m.group(6)), int(m.group(7))
            count = (a1 - a0 + 1) * (p1 - p0 + 1)
            yield offset, width, count, m.group(8)


def register_words_corrected(offset, width, count):
    """The corrected packing model (mklist.py's own docstring): a narrow
    register (width*count<=32) gets ONE word at offset; a wide one
    (width<=32<width*count) gets one word per item at offset+4*i; a
    register with an item wider than 32 bits gets ceil(width/32) words
    per item.
    """
    if width * count <= 32:
        return [offset]
    if width <= 32:
        return [offset + 4 * i for i in range(count)]
    words_per_item = -(-width // 32)  # ceil
    stride = 4 * words_per_item
    return [offset + i * stride + 4 * k for i in range(count) for k in range(words_per_item)]


def register_words_naive(offset, width, count):
    """decode.py's own model: one word per item unconditionally, at
    offset+4*i -- correct only when the register is not narrow. Used only
    for addresses this generator does not apply the corrected model to
    (see build_resolver()'s own comment).
    """
    return [offset + 4 * i for i in range(max(count, 1))]


def build_resolver(regmap_path):
    """Return addr -> (register base address, listing name), or None:
    the corrected model for the switch-core span, the naive per-slot model
    for the GPON/PON-queue span. First register wins at a given address in
    each model (an address collision picks the first-declared register,
    same as decode.py's build_lookup() -- this generator only needs a
    register for grouping, not a fully disambiguated one).
    """
    corrected = {}
    naive = {}
    for offset, width, count, name in load_registers(regmap_path):
        for word_off in register_words_corrected(offset, width, count):
            corrected.setdefault(word_off, (offset, name))
        for word_off in register_words_naive(offset, width, count):
            naive.setdefault(word_off, (offset, name))

    def resolve(addr):
        if addr < 0x200000:
            return corrected.get(addr)
        return naive.get(addr)

    return resolve


# ---------------------------------------------------------------------
# Register family grouping ("modload category 0 split"), by register
# address. Each family lists the base addresses (the register an address
# resolves to, above) it covers; a register not listed here gets a family
# of its own, "OTHER_<its listing name>", so a future capture's new
# registers still land somewhere deterministic and show up by name in the
# generator reg_group listing (stdout and stderr).
#
# The order of FAMILIES IS the reg_group assignment (reg_group 1 is the
# first entry, 2 the second, ...; 0 is always FLOOD) and so the mask-bit
# assignment (bit 6 + reg_group, replayblob.py filter) -- append a new family
# at the end, never insert or reorder, or every bisection mask anyone has
# written down changes meaning. Families this capture does not touch
# still keep their index; OTHER_* families follow the listed ones,
# alphabetically.
# ---------------------------------------------------------------------
FAMILIES = [
    ("ACL", [0x015000, 0x015008]),
    ("CLASSIFY", [0x016004, 0x016010]),
    ("PONQ_MASKS", [0xf020a8, 0xf0a07c]),
    ("FLOW_CONTROL", [0x02311c, 0x023120, 0x02312c]),
    ("GPON_DS_KEYS", [0x703010, 0x703014, 0x703020, 0x703024]),
    ("GPON_DS_GEM", [0x704040, 0x70404c, 0x704064, 0x704080, 0x704084,
                     0x704098, 0x70409c, 0x7040a0]),
    ("GPON_US_GEM", [0x706020, 0x706024, 0x706048, 0x706260]),
    ("GPON_DS_FRAMER", [0x701004, 0x701010, 0x701014, 0x70101c, 0x701040,
                        0x7010c0, 0x701100, 0x701140, 0x701204, 0x701208,
                        0x70126c, 0x701400]),
    ("GPON_US_FRAMER", [0x705010, 0x705014, 0x705018, 0x705040, 0x70504c,
                        0x7050c0, 0x7050e0, 0x705100, 0x705140, 0x705180,
                        0x705188, 0x705200, 0x70526c]),
    ("GPON_MAC", [0x70000c, 0x700014, 0x700040, 0xf02568]),
    ("PIN_MUX", [0x000048, 0xf05434, 0xf05438, 0xf0d434, 0xf0d438]),
    ("L2_LOOKUP", [0x017000]),
    ("FRAME_LENGTH", [0x011008, 0x011018]),
    ("PONQ_SCHED", [0xf023e8, 0xf023f8, 0xf02458]),
    ("PONQ_PORT_CFG", [0xf04040, 0xf04044, 0xf0404c, 0xf05400, 0xf0c040,
                       0xf0c044, 0xf0c04c, 0xf0d400]),
    ("PORT_FORCE", [0x000180, 0x0001b4, 0x0220e4]),
    ("QOS", [0x01c0b0]),
    ("METER", [0x025004, 0x02d82c]),
    ("SERDES_ANALOG", [0x0001d0, 0x022500, 0x022504, 0x022508, 0x0225ac,
                       0x0225b0, 0x0225d8, 0x022738]),
    ("SVLAN", [0x014004, 0x0230c4]),
    ("UNRESOLVED", []),   # an address in no register at all
    ("VLAN", [0x013008, 0x01300c]),
    ("SERDES_DIGITAL", [0x022030, 0x022034, 0x022038, 0x02203c, 0x022090]),
]
FAMILY_OF_REGISTER = {base: fam for fam, bases in FAMILIES for base in bases}


def family_of(register):
    """register is (base address, listing name) or None."""
    if register is None:
        return "UNRESOLVED"
    return FAMILY_OF_REGISTER.get(register[0], "OTHER_" + register[1])


def build_family_table(resolver, addrs):
    """Return ({addr: reg_group}, [family_name, ...]) -- family_name[0] is
    always "FLOOD" (reg_group 0), then FAMILIES in order, then any OTHER_*
    family, alphabetically (reg_group 1..N).
    """
    names = {}
    for addr in addrs:
        if addr in FLOOD_ADDRS:
            continue
        names[addr] = family_of(resolver(addr))

    listed = [fam for fam, _ in FAMILIES]
    family_names = listed + sorted(set(names.values()) - set(listed))
    family_index = {fam: i + 1 for i, fam in enumerate(family_names)}  # 1-based, 0 is FLOOD

    reg_group = {}
    for addr in addrs:
        reg_group[addr] = 0 if addr in FLOOD_ADDRS else family_index[names[addr]]

    return reg_group, ["FLOOD"] + family_names


# Kinds this generator understands. T/D/W are real replay data (handled
# below); R and M are known, deliberately-inert marks (a table run close --
# never actually seen in this capture, per this file own docstring -- and
# a command-bracket mark). "w" is a WRITE this generator has confirmed
# exists in its own reference capture and does NOT reproduce (see the
# module docstring) -- recognised so it fails loudly instead of vanishing
# into the same catch-all R/M do. Anything else is unrecognised outright.
KNOWN_KINDS = frozenset("TDWRMw")


def parse_events(path):
    """Yield ('reg', offset, value) or ('table', table, index, words) in
    file order. A malformed line (not enough fields, or a field that does
    not parse as hex) is skipped -- this reads a real trace file, not a
    format this project controls line-by-line -- but a well-formed line
    whose KIND this generator has never evaluated is fatal: see
    KNOWN_KINDS above and this file own module docstring for why a
    silent skip is exactly the bug this generator used to have.
    """
    pending = None  # [table, index, words] between a T and its D's
    with open(path) as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            _ns, kind, addr_s, val_s = parts[0], parts[1], parts[2], parts[3]
            try:
                addr = int(addr_s, 16)
                val = int(val_s, 16)
            except ValueError:
                continue
            if kind not in KNOWN_KINDS:
                sys.exit(
                    "mkmodload.py: line %d has kind %r, which this generator "
                    "does not understand -- add explicit handling (or "
                    "confirm it is safe to ignore) before generating from "
                    "this capture: %r" % (line_no, kind, line)
                )
            if kind == "w":
                sys.exit(
                    "mkmodload.py: line %d is a kind-w (SoC-window) write "
                    "(addr %s val %s) -- this generator does not reproduce "
                    "SoC writes (see the module docstring) and dropping one "
                    "silently is the exact bug mksdkinit.py had. Refusing "
                    "to generate until a human decides whether this write "
                    "needs replaying: %r" % (line_no, addr_s, val_s, line)
                )
            if kind == "T":
                if pending is not None:
                    yield ("table", pending[0], pending[1], tuple(pending[2]))
                pending = [(addr >> 16) & 0xffff, addr & 0xffff, []]
                continue
            if kind == "D":
                if pending is not None:
                    pending[2].append(val)
                continue
            if kind == "W":
                if pending is not None:
                    yield ("table", pending[0], pending[1], tuple(pending[2]))
                    pending = None
                if TABLE_BLOCK_LO <= addr <= TABLE_BLOCK_HI:
                    continue
                yield ("reg", addr, val)
                continue
            # R or M: known, inert -- flush any pending table op first so
            # its D lines are not lost.
            if pending is not None:
                yield ("table", pending[0], pending[1], tuple(pending[2]))
                pending = None
        if pending is not None:
            yield ("table", pending[0], pending[1], tuple(pending[2]))


def categorize(events, reg_group_of):
    """Yield (kind, category, reg_group, *rest) for each parsed event,
    applying the sweep-vs-specific repeat rule to classification-table events
    and reg_group_of() to register events. reg_group is 0 (unused) for
    every table event.
    """
    seen = set()  # (table, index) pairs already written, classification tables only
    for ev in events:
        if ev[0] == "reg":
            yield ("reg", CATEGORY_REG, reg_group_of(ev[1]), ev[1], ev[2])
            continue
        _, table, index, words = ev
        if table in CLS_TABLE_IDS:
            key = (table, index)
            category = CATEGORY_CLS_SPECIFIC if key in seen else CATEGORY_CLS_SWEEP
            seen.add(key)
        elif table == VLAN_TABLE_ID:
            category = CATEGORY_VLAN
        elif table == L2_UNICAST_TABLE_ID:
            category = CATEGORY_L2_UNICAST
        elif table in ACL_TABLE_IDS:
            category = CATEGORY_ACL
        else:
            sys.exit("mkmodload.py: table id %d has no category rule -- "
                      "add one before generating from this capture" % table)
        yield ("table", category, 0, table, index, words)


MAX_WORDS = replayblob.MAX_WORDS


def build_records(events, family_names):
    """Returns (records, n, counts per category, counts per reg_group)."""
    records = []
    counts = [0] * 6
    reg_group_counts = [0] * len(family_names)
    for ev in events:
        if ev[0] == "reg":
            _, category, reg_group, offset, value = ev
            counts[category] += 1
            reg_group_counts[reg_group] += 1
            records.append(replayblob.switch_reg(offset, value, category, reg_group))
        else:
            _, category, _reg_group, table, index, words = ev
            counts[category] += 1
            if len(words) > MAX_WORDS:
                sys.exit("mkmodload.py: table %d row has %d words, max %d supported"
                          % (table, len(words), MAX_WORDS))
            records.append(replayblob.switch_table(table, index, words, category))
    return records, len(records), counts, reg_group_counts


def family_comment(family_names):
    """The reg_group assignment, as comment lines: index == reg_group, mask
    bit == 6 + reg_group (replayblob.py filter).
    """
    return "".join("# reg_group %2d  %s\n" % (i, fam) for i, fam in enumerate(family_names))


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    capture_path = argv[1]
    regmap_path = argv[2]
    out_path = argv[3] if len(argv) > 3 else None

    resolver = build_resolver(regmap_path)
    raw_events = list(parse_events(capture_path))
    reg_addrs = [ev[1] for ev in raw_events if ev[0] == "reg"]
    reg_group_map, family_names = build_family_table(resolver, reg_addrs)
    if len(family_names) > replayblob.MAX_REG_GROUP + 1:
        sys.exit("mkmodload.py: %d register families need bits 6..%d, past bit 31 -- "
                  "coalesce some families in family_of() before generating"
                  % (len(family_names), 6 + len(family_names) - 1))

    events = list(categorize(raw_events, lambda a: reg_group_map[a]))
    records, n, counts, reg_group_counts = build_records(events, family_names)
    blob = replayblob.pack(replayblob.TABLE_MODLOAD, records)

    if out_path:
        with open(out_path, "wb") as f:
            f.write(blob)
        sys.stderr.write(
            "%d events written to %s from %s (category 0 reg=%d, 1 cls-sweep=%d, "
            "2 cls-specific=%d, 3 vlan=%d, 4 l2_unicast=%d, 5 acl=%d)\n"
            % (n, out_path, os.path.basename(capture_path), counts[0], counts[1],
               counts[2], counts[3], counts[4], counts[5])
        )
        sys.stderr.write(
            "%d register families (reg_group 0..%d, mask bits 6..%d): %s\n"
            % (len(family_names), len(family_names) - 1, 6 + len(family_names) - 1,
               ", ".join("%s=%d" % (fam, c) for fam, c in zip(family_names, reg_group_counts)))
        )
    else:
        sys.stdout.write(family_comment(family_names) + replayblob.dump(blob))


if __name__ == "__main__":
    main(sys.argv)
