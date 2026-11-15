#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

"""Register-replay firmware blobs: the on-disk format the odi driver set
loads with request_firmware() from /lib/firmware/odi/, written by
mksdkinit.py, mkmodload.py and mkgponinit.py, read back here for tests and
for review.

    replayblob.py build <in.txt> <out.bin>
    replayblob.py build-tables [<outdir>]

The text in tools/regtrace/tables/ is the source of the three blobs: build
turns one text file into its blob (out.bin "-" writes it to stdout), and
build-tables writes all three into outdir, out/firmware/odi/ when none is
given, rewriting a file only when its bytes change. The image build, the
host tests and the SBOM read that output; nothing under rootfs/ holds a
blob. The text format is the one dump prints (one line per record, see
dump_record()), plus comments: a line starting with "#", a blank line, or
"#" and anything after it at the end of a record line. The first line is
the header dump writes; build takes the table and the format version from
it, and when it also names records= and crc32=, both must match the
records that follow, so a change to the records shows in the header of
the diff. build refuses a field it does not know, a value that does not
fit its field and any record the kernel loader would refuse
(check_record()).

    replayblob.py dump [--names <regnames.txt>] <file.bin>

prints one line per record, the same text a generator prints on stdout
when it is given no output path, so a blob can be reviewed (and diffed)
as text. With --names, a register record whose offset
src/diag/tools/regnames.txt names gets that name as a trailing comment.

    replayblob.py compact <in.bin> <out.bin>
    replayblob.py expand <in.bin> <out.bin>

compact replaces each uniform table sweep of an sdkinit or modload blob
(a run of identical cleared rows, FILL_* below) by one CALL record the
driver generates in code; expand writes the rows back. The shipped
sdkinit.bin and modload.bin are compacted, and expanding them gives the
generator output byte for byte (test/replayblob_fill_test.sh).

    replayblob.py filter <in-modload.bin> <out.bin> <mask>

applies a module-load replay mask to a modload blob ahead of time, the
way the kernel once applied it at every boot, and writes the result: the
shipped modload.bin is the mkmodload.py output filtered with
PRODUCTION_MODLOAD_MASK, and the kernel replays every record it holds.
The mask bits (filter_modload() has the rules): bit N (0-5) keeps the
table records of category N, and bit 0 every category-0 register as
captured; with bit 0 clear, bit 6 + G keeps the registers of reg_group G,
and bit 30 keeps reg_group 0 (the three LUT flood masks) with the CPU
port added, value FLOOD_CPU_VALUE.

    replayblob.py trim-gpon <in-gpon_init.bin> <out.bin>

removes the GPON_INIT_TRIMMED sweeps from a gpon_init blob: the shipped
gpon_init.bin is the mkgponinit.py output trimmed this way. The driver
does the table clean in code, and the counter index sweep reads nothing.

    replayblob.py trim-modload-gpon <in-modload.bin> <gpon_init.bin> <out.bin>

removes the GPON MAC register writes (0x700000-0x706fff) from a modload
blob, after checking that every one of those registers is written again
after the gpon_init PONMAC reset, by gpon_init or by the driver code that
took over a GPON_INIT_TRIMMED sweep: the shipped modload.bin is the filtered
mkmodload.py output trimmed this way, then compacted.

Format (kernel/extra/drivers/net/ethernet/odi/odi_replay_blob.h is the C
side and must agree field for field). Every multi-byte field is
big-endian, the byte order of the RTL9602C, and nothing depends on a C
compiler struct layout.

Header, HEADER_SIZE (24) bytes:

    0   u32  magic        MAGIC, "ODIR"
    4   u16  version      VERSION
    6   u16  table        TABLE_SDKINIT, TABLE_MODLOAD or TABLE_GPON_INIT
    8   u16  header_size  HEADER_SIZE
    10  u16  record_size  RECORD_SIZE
    12  u32  count        number of records
    16  u32  reserved     0
    20  u32  crc32        zlib CRC-32 of bytes 0..19 followed by every
                          record, i.e. of the whole file with this field
                          left out

then `count` records of RECORD_SIZE (36) bytes each. The file size must be
exactly HEADER_SIZE + count * RECORD_SIZE.

Record, switch tables (sdkinit and modload):

    0   u8   kind         KIND_REG, KIND_TABLE, KIND_SOC or KIND_CALL
    1   u8   category     modload: 0-5 (a CALL record 1-5); sdkinit: 0
    2   u8   reg_group    modload category 0: 0..MAX_REG_GROUP; else 0
    3   u8   verb         sdkinit: index into SDKINIT_VERBS; modload: 0
    4   u16  table        TABLE: switch table id; CALL: FILL_*; else 0
    6   u16  n_words      TABLE: 1..MAX_WORDS; CALL: FILL_WORDS; else 0
    8   u32  offset       REG: MMIO offset; TABLE: row index; SOC: address;
                          CALL: first row
    12  u32  value        REG and SOC records; CALL: row count; 0 for TABLE
    16  u32  words[5]     TABLE and CALL records, zero past n_words; else 0

A CALL record (sdkinit and modload only) stands for rows first ..
first + count - 1, each written as fill_rows() says (enum odi_replay_fill
in odi_replay_blob.h is the C side).

Record, GPON init table:

    0   u8   kind         KIND_REG or KIND_TABLE
    1   u8   sn_word      SN_WORD_NONE, or 1..4 (serial-number word)
    2   u16  reserved     0
    4.. same as the switch record from byte 4 on

sdkinit records are stored grouped by verb, in SDKINIT_VERBS order, each
verb in capture order; a verb with nothing to replay has no records.
"""
import os
import struct
import sys
import zlib

MAGIC = 0x4f444952  # "ODIR"
VERSION = 1
HEADER_SIZE = 24
RECORD_SIZE = 36
MAX_WORDS = 5
# ODI_REPLAY_BLOB_MAX_RECORDS: the kernel loader refuses a longer blob.
MAX_RECORDS = 65536

TABLE_SDKINIT = 1
TABLE_MODLOAD = 2
TABLE_GPON_INIT = 3
TABLE_NAMES = {TABLE_SDKINIT: "sdkinit", TABLE_MODLOAD: "modload",
               TABLE_GPON_INIT: "gpon_init"}

KIND_REG = 0
KIND_TABLE = 1
KIND_SOC = 2
KIND_CALL = 3
KIND_NAMES = {KIND_REG: "REG", KIND_TABLE: "TABLE", KIND_SOC: "SOC",
              KIND_CALL: "CALL"}

# The switch tables the fills write (enum odi_sw_table, odi_switch_hw.h).
TBL_CLS_DS_ACTION = 11
TBL_CLS_US_ACTION = 12
TBL_CLS_MASK_A = 13
TBL_CLS_MASK_B = 14
TBL_CLS_RULE_A = 16
TBL_CLS_RULE_B = 17
TBL_VLAN_MEMBERS = 23

# The table fills of a CALL record (enum odi_replay_fill), their row
# bounds and how many argument words each takes. The CF fills take one:
# the first row of pattern 1, CF_ROW_COUNT when every row is pattern 0.
FILL_VLAN_MEMBERS = 1
FILL_CF_RULES = 2
FILL_CF_RULES_ACTIONS = 3
FILL_NAMES = {FILL_VLAN_MEMBERS: "vlan_members", FILL_CF_RULES: "cf_rules",
              FILL_CF_RULES_ACTIONS: "cf_rules_actions"}
VLAN_ROW_COUNT = 4096
CF_ROW_COUNT = 256
FILL_ROWS = {FILL_VLAN_MEMBERS: VLAN_ROW_COUNT, FILL_CF_RULES: CF_ROW_COUNT,
             FILL_CF_RULES_ACTIONS: CF_ROW_COUNT}
FILL_WORDS = {FILL_VLAN_MEMBERS: 0, FILL_CF_RULES: 1, FILL_CF_RULES_ACTIONS: 1}
VLAN_MEMBERS_SENTINEL = 0xf0
# compact turns a run into a CALL record only from this many rows on: the
# sweeps are hundreds of rows, a single cleared row stays a TABLE record.
FILL_MIN_ROWS = 16

# reg_group 0..25 is mask bit 6..31 of filter; the kernel loader refuses
# anything past that, and so does mkmodload.py.
MAX_REG_GROUP = 25
MAX_CATEGORY = 5
# The mask rcS applied at every boot, now baked into modload.bin.
PRODUCTION_MODLOAD_MASK = 0x7FFFFFBE
FLOOD_CPU_BIT = 30
FLOOD_CPU_VALUE = 0x0000000f

SN_WORD_NONE = 0xff
SN_WORD_MIN = 1
SN_WORD_MAX = 4

# The fixed verb order of odi_switch_sdkinit.h (enum
# odi_sw_sdkinit_verb_id): index N is the verb byte of a
# record. Append only.
SDKINIT_VERBS = (
    "switch", "svlan", "stp", "oam", "acl", "qos", "sec", "rate",
    "classify", "stat", "trunk", "l2", "vlan", "port", "mirror", "cpu",
    "rldp", "trap", "gpio", "time", "ponmac",
    "i2c", "i2cen", "gpon", "rxsd",
)

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
TABLES_DIR = os.path.join(_HERE, "tables")
OUT_DIR = os.path.join(_ROOT, "out", "firmware", "odi")
REGNAMES = os.path.join(_ROOT, "src", "diag", "tools", "regnames.txt")

_HEADER = struct.Struct(">IHHHHII")   # everything before crc32
_CRC = struct.Struct(">I")
_RECORD_SWITCH = struct.Struct(">BBBBHHII5I")
_RECORD_GPON = struct.Struct(">BBHHHII5I")
assert _HEADER.size + _CRC.size == HEADER_SIZE
assert _RECORD_SWITCH.size == RECORD_SIZE == _RECORD_GPON.size


def _words(words):
    if len(words) > MAX_WORDS:
        raise ValueError("table row has %d words, max %d" % (len(words), MAX_WORDS))
    return list(words) + [0] * (MAX_WORDS - len(words))


def switch_reg(offset, value, category=0, reg_group=0, verb=0):
    return _RECORD_SWITCH.pack(KIND_REG, category, reg_group, verb, 0, 0,
                               offset, value, *_words(()))


def switch_soc(addr, value, verb=0):
    return _RECORD_SWITCH.pack(KIND_SOC, 0, 0, verb, 0, 0, addr, value, *_words(()))


def switch_table(table, index, words, category=0, verb=0):
    return _RECORD_SWITCH.pack(KIND_TABLE, category, 0, verb, table, len(words),
                               index, 0, *_words(words))


def switch_call(fill, first, count, args=(), category=0, verb=0):
    return _RECORD_SWITCH.pack(KIND_CALL, category, 0, verb, fill, len(args),
                               first, count, *_words(args))


def fill_rows(fill, first, count, args):
    """The (table, row, words) writes of a fill, in order."""
    out = []
    for i in range(first, first + count):
        if fill == FILL_VLAN_MEMBERS:
            out.append((TBL_VLAN_MEMBERS, i, (VLAN_MEMBERS_SENTINEL,)))
            continue
        b = i >= args[0]
        out.append((TBL_CLS_RULE_B if b else TBL_CLS_RULE_A, i, (0, 0)))
        out.append((TBL_CLS_MASK_B if b else TBL_CLS_MASK_A, i, (0, 0)))
        if fill == FILL_CF_RULES_ACTIONS:
            out.append((TBL_CLS_DS_ACTION, i, (0, 0, 0)))
            out.append((TBL_CLS_US_ACTION, i, (0, 0, 0)))
    return out


def check_call(rec):
    """Raises ValueError for a CALL record the kernel loader would refuse
    (odi_replay_check_call()), returns (fill, first, count, args).
    """
    _k, _cat, _grp, _verb, fill, n, first, count, *words = _RECORD_SWITCH.unpack(rec)
    if fill not in FILL_NAMES:
        raise ValueError("call record fill %d unknown" % fill)
    rows = FILL_ROWS[fill]
    if count == 0 or first >= rows or count > rows - first:
        raise ValueError("call record rows 0x%x+%d out of range" % (first, count))
    if n != FILL_WORDS[fill]:
        raise ValueError("call record has %d words, fill %s takes %d"
                         % (n, FILL_NAMES[fill], FILL_WORDS[fill]))
    if n and words[0] > CF_ROW_COUNT:
        raise ValueError("call record pattern-1 row %d out of range" % words[0])
    if any(words[n:]):
        raise ValueError("call record has nonzero padding past its words")
    return fill, first, count, tuple(words[:n])


def _row(rec):
    """(kind, category, reg_group, verb, table, row, words) of a record."""
    kind, cat, grp, verb, tbl, n, offset, _value, *words = _RECORD_SWITCH.unpack(rec)
    return kind, cat, grp, verb, tbl, offset, tuple(words[:n])


def _match_fill(fill, records, i):
    """The run of records from i that fill writes, as (count, args), or
    None when it is shorter than FILL_MIN_ROWS rows. A CF run is pattern 0
    up to its first B row and pattern 1 from there on.
    """
    head = _row(records[i])
    if head[0] != KIND_TABLE:
        return None
    first = head[5]
    per_row = len(fill_rows(fill, 0, 1, (CF_ROW_COUNT,)))
    split = None
    count = 0
    while first + count < FILL_ROWS[fill]:
        j = i + count * per_row
        got = [_row(r) for r in records[j:j + per_row]]
        if len(got) < per_row or any(g[:4] != head[:4] for g in got):
            break
        row = first + count
        args = ()
        if fill != FILL_VLAN_MEMBERS:
            is_b = got[0][4] == TBL_CLS_RULE_B
            args = (split if split is not None else row if is_b else CF_ROW_COUNT,)
        if [(g[4], g[5], g[6]) for g in got] != fill_rows(fill, row, 1, args):
            break
        if args and args[0] == row:
            split = row
        count += 1
    if count < FILL_MIN_ROWS:
        return None
    if fill == FILL_VLAN_MEMBERS:
        return count, ()
    return count, (split if split is not None else CF_ROW_COUNT,)


def compact_records(table, records):
    """records with each uniform sweep replaced by a CALL record."""
    if table not in (TABLE_SDKINIT, TABLE_MODLOAD):
        raise ValueError("only sdkinit and modload blobs take call records")
    out = []
    i = 0
    while i < len(records):
        _kind, cat, _grp, verb, _tbl, first, _w = _row(records[i])
        # The widest row first: a CF_RULES_ACTIONS row starts like a
        # CF_RULES one.
        for fill in (FILL_CF_RULES_ACTIONS, FILL_CF_RULES, FILL_VLAN_MEMBERS):
            m = _match_fill(fill, records, i)
            if m:
                count, args = m
                out.append(switch_call(fill, first, count, args, category=cat, verb=verb))
                i += len(fill_rows(fill, first, count, args))
                break
        else:
            out.append(records[i])
            i += 1
    return out


def expand_records(table, records):
    """records with every CALL record written out as its table rows."""
    out = []
    for rec in records:
        if rec[0] != KIND_CALL:
            out.append(rec)
            continue
        if table not in (TABLE_SDKINIT, TABLE_MODLOAD):
            raise ValueError("call record in a %s blob" % TABLE_NAMES[table])
        fill, first, count, args = check_call(rec)
        _k, cat, _grp, verb, *_rest = _RECORD_SWITCH.unpack(rec)
        out.extend(switch_table(t, row, words, category=cat, verb=verb)
                   for t, row, words in fill_rows(fill, first, count, args))
    return out


def compact(data):
    table, _crc, records = unpack(data)
    return pack(table, compact_records(table, records))


def expand(data):
    table, _crc, records = unpack(data)
    return pack(table, expand_records(table, records))


def gpon_reg(offset, value, sn_word=SN_WORD_NONE):
    return _RECORD_GPON.pack(KIND_REG, sn_word, 0, 0, 0, offset, value, *_words(()))


def gpon_table(table, index, words):
    return _RECORD_GPON.pack(KIND_TABLE, SN_WORD_NONE, 0, table, len(words),
                             index, 0, *_words(words))


def pack(table, records):
    """Returns the complete blob (header plus records) as bytes."""
    body = b"".join(records)
    for r in records:
        if len(r) != RECORD_SIZE:
            raise ValueError("record of %d bytes, want %d" % (len(r), RECORD_SIZE))
    head = _HEADER.pack(MAGIC, VERSION, table, HEADER_SIZE, RECORD_SIZE,
                        len(records), 0)
    crc = zlib.crc32(body, zlib.crc32(head)) & 0xffffffff
    return head + _CRC.pack(crc) + body


def write(path, table, records):
    with open(path, "wb") as f:
        f.write(pack(table, records))


def unpack(data):
    """Validates a blob as strictly as the kernel loader does and returns
    (table, crc32, [record bytes, ...]). Raises ValueError on any defect.
    """
    if len(data) < HEADER_SIZE:
        raise ValueError("short header: %d bytes" % len(data))
    magic, version, table, hsize, rsize, count, reserved = _HEADER.unpack_from(data)
    (crc,) = _CRC.unpack_from(data, _HEADER.size)
    if magic != MAGIC:
        raise ValueError("bad magic 0x%08x" % magic)
    if version != VERSION:
        raise ValueError("unsupported version %d" % version)
    if table not in TABLE_NAMES:
        raise ValueError("unknown table id %d" % table)
    if hsize != HEADER_SIZE or rsize != RECORD_SIZE or reserved != 0:
        raise ValueError("bad header geometry %d/%d/%d" % (hsize, rsize, reserved))
    if len(data) != HEADER_SIZE + count * RECORD_SIZE:
        raise ValueError("size %d does not match %d records" % (len(data), count))
    body = data[HEADER_SIZE:]
    want = zlib.crc32(body, zlib.crc32(data[:_HEADER.size])) & 0xffffffff
    if crc != want:
        raise ValueError("crc32 0x%08x, computed 0x%08x" % (crc, want))
    return table, crc, [body[i:i + RECORD_SIZE] for i in range(0, len(body), RECORD_SIZE)]


def _hex_words(words, n):
    return " ".join("0x%08x" % w for w in words[:n]) if n else "-"


def dump_record(table, rec):
    """One text line per record; the format generators print on stdout."""
    if table == TABLE_GPON_INIT:
        kind, sn_word, _res, tbl, n, offset, value, *words = _RECORD_GPON.unpack(rec)
        if kind == KIND_TABLE:
            return "TABLE table=%d idx=0x%08x n=%d words=%s" % (
                tbl, offset, n, _hex_words(words, n))
        sn = "-" if sn_word == SN_WORD_NONE else "%d" % sn_word
        return "REG sn=%s off=0x%08x val=0x%08x" % (sn, offset, value)

    kind, cat, grp, verb, tbl, n, offset, value, *words = _RECORD_SWITCH.unpack(rec)
    if table == TABLE_SDKINIT:
        prefix = "%s " % (SDKINIT_VERBS[verb] if verb < len(SDKINIT_VERBS) else "verb%d" % verb)
    else:
        prefix = "cat=%d grp=%d " % (cat, grp)
    if kind == KIND_TABLE:
        return "%sTABLE table=%d idx=0x%08x n=%d words=%s" % (
            prefix, tbl, offset, n, _hex_words(words, n))
    if kind == KIND_CALL:
        return "%sCALL fill=%s first=0x%08x count=%d words=%s" % (
            prefix, FILL_NAMES.get(tbl, "%d" % tbl), offset, value,
            _hex_words(words, n))
    return "%s%s off=0x%08x val=0x%08x" % (prefix, KIND_NAMES.get(kind, "KIND%d" % kind),
                                          offset, value)


def record_offset(table, rec):
    """The register offset of a register record, else None."""
    if table == TABLE_GPON_INIT:
        kind, _sn, _res, _tbl, _n, offset, *_rest = _RECORD_GPON.unpack(rec)
    else:
        kind, _c, _g, _v, _tbl, _n, offset, *_rest = _RECORD_SWITCH.unpack(rec)
    return offset if kind == KIND_REG else None


def load_regnames(path=REGNAMES):
    """{offset: name} of the reg lines of src/diag/tools/regnames.txt."""
    names = {}
    with open(path) as f:
        for line in f:
            t = line.split("#", 1)[0].split()
            if len(t) >= 3 and t[0] == "reg":
                names[int(t[1], 16)] = t[2]
    return names


def dump(data, names=None):
    """The whole blob as text: a header comment line, then one line per
    record. names ({offset: name}) adds the name of a named register as a
    trailing comment.
    """
    table, crc, records = unpack(data)
    lines = ["# odi replay blob: table=%s version=%d records=%d crc32=0x%08x"
             % (TABLE_NAMES[table], VERSION, len(records), crc)]
    for r in records:
        line = dump_record(table, r)
        name = names.get(record_offset(table, r)) if names else None
        lines.append("%s  # %s" % (line, name) if name else line)
    return "\n".join(lines) + "\n"


def _payload_ok(kind_table, n, value, tbl, words):
    """The shared payload rules of odi_replay_check_payload()."""
    if kind_table:
        if not 1 <= n <= MAX_WORDS:
            return "table record word count out of range"
        if value:
            return "table record carries a value"
    elif n or tbl:
        return "register record carries a table or words"
    if any(words[n:]):
        return "record has nonzero padding past its words"
    return None


def check_record(table, rec, prev_verb=0):
    """Raises ValueError for a record the kernel loader refuses
    (odi_replay_blob_parse() in odi_replay_blob.c, which this follows rule
    for rule; test/odi_replay_blob_test.sh runs both on the same files).
    """
    if table == TABLE_GPON_INIT:
        kind, sn, res, tbl, n, _off, value, *words = _RECORD_GPON.unpack(rec)
        if res:
            why = "gpon init record reserved field nonzero"
        elif kind == KIND_REG:
            why = None
            if sn != SN_WORD_NONE and not SN_WORD_MIN <= sn <= SN_WORD_MAX:
                why = "gpon init record sn_word out of range"
            why = why or _payload_ok(False, n, value, tbl, words)
        elif kind == KIND_TABLE:
            why = ("gpon init table record carries an sn_word" if sn != SN_WORD_NONE
                   else _payload_ok(True, n, value, tbl, words))
        else:
            why = "gpon init record kind not register or table"
        if why:
            raise ValueError(why)
        return
    kind, cat, grp, verb, tbl, n, _off, value, *words = _RECORD_SWITCH.unpack(rec)
    if table == TABLE_MODLOAD:
        if verb:
            raise ValueError("modload record carries a verb")
        if kind == KIND_REG:
            if cat or grp > MAX_REG_GROUP:
                raise ValueError("modload register record category or reg_group out of range")
        elif kind in (KIND_TABLE, KIND_CALL):
            if not 1 <= cat <= MAX_CATEGORY or grp:
                raise ValueError("modload table record category or reg_group out of range")
        else:
            raise ValueError("modload record kind not register, table or call")
    else:
        if cat or grp:
            raise ValueError("sdkinit record carries a category or reg_group")
        if verb >= len(SDKINIT_VERBS):
            raise ValueError("sdkinit record verb out of range")
        if verb < prev_verb:
            raise ValueError("sdkinit records not grouped by verb")
        if kind not in (KIND_REG, KIND_TABLE, KIND_SOC, KIND_CALL):
            raise ValueError("sdkinit record kind out of range")
    if kind == KIND_CALL:
        check_call(rec)
        return
    why = _payload_ok(kind == KIND_TABLE, n, value, tbl, words)
    if why:
        raise ValueError(why)


def _field(tok, key, lo, hi, base=0):
    """The value of tok, which must read key=<number>, within lo..hi."""
    if not tok.startswith(key + "="):
        raise ValueError("expected %s=, got %r" % (key, tok))
    try:
        v = int(tok[len(key) + 1:], base)
    except ValueError:
        raise ValueError("%s: not a number: %r" % (key, tok)) from None
    if not lo <= v <= hi:
        raise ValueError("%s=%d out of range %d..%d" % (key, v, lo, hi))
    return v


def _hex32(tok, key):
    if not tok.startswith(key + "=0x"):
        raise ValueError("expected %s=0x..., got %r" % (key, tok))
    return _field(tok, key, 0, 0xffffffff, 16)


def _words_field(toks, n):
    """The words= list of a TABLE or CALL line: n words, or "-" for none."""
    if not toks or not toks[0].startswith("words="):
        raise ValueError("expected words=")
    first = toks[0][len("words="):]
    if n == 0:
        if first != "-" or len(toks) != 1:
            raise ValueError("n=0 takes words=-")
        return []
    vals = [first] + toks[1:]
    if len(vals) != n:
        raise ValueError("n=%d but %d words" % (n, len(vals)))
    out = []
    for v in vals:
        if not v.startswith("0x"):
            raise ValueError("word %r is not 0x..." % v)
        out.append(int(v, 16))
        if out[-1] > 0xffffffff:
            raise ValueError("word %r wider than 32 bits" % v)
    return out


def parse_record(table, line):
    """One record line (no comment) as record bytes, the inverse of
    dump_record().
    """
    t = line.split()
    if table == TABLE_GPON_INIT:
        if t[:1] == ["TABLE"] and len(t) >= 5:
            n = _field(t[3], "n", 0, MAX_WORDS)
            return gpon_table(_field(t[1], "table", 0, 0xffff), _hex32(t[2], "idx"),
                              _words_field(t[4:], n))
        if t[:1] == ["REG"] and len(t) == 4:
            sn = SN_WORD_NONE if t[1] == "sn=-" else _field(t[1], "sn", 0, 0xfe)
            return gpon_reg(_hex32(t[2], "off"), _hex32(t[3], "val"), sn)
        raise ValueError("not a gpon_init record")
    cat = grp = verb = 0
    if table == TABLE_SDKINIT:
        if not t or t[0] not in SDKINIT_VERBS:
            raise ValueError("unknown verb %r" % (t[0] if t else ""))
        verb = SDKINIT_VERBS.index(t[0])
        t = t[1:]
    else:
        if len(t) < 2:
            raise ValueError("not a modload record")
        cat = _field(t[0], "cat", 0, 0xff)
        grp = _field(t[1], "grp", 0, 0xff)
        t = t[2:]
    kind = t[0] if t else ""
    if kind in ("REG", "SOC") and len(t) == 3:
        off, val = _hex32(t[1], "off"), _hex32(t[2], "val")
        if kind == "SOC":
            if cat or grp:
                raise ValueError("SOC record with a category or reg_group")
            return switch_soc(off, val, verb)
        return switch_reg(off, val, cat, grp, verb)
    if grp:
        raise ValueError("%s record with a reg_group" % kind)
    if kind == "TABLE" and len(t) >= 5:
        n = _field(t[3], "n", 0, MAX_WORDS)
        return switch_table(_field(t[1], "table", 0, 0xffff), _hex32(t[2], "idx"),
                            _words_field(t[4:], n), cat, verb)
    if kind == "CALL" and len(t) >= 5:
        if not t[1].startswith("fill="):
            raise ValueError("expected fill=")
        fills = {v: k for k, v in FILL_NAMES.items()}
        if t[1][5:] not in fills:
            raise ValueError("unknown fill %r" % t[1][5:])
        args = _words_field(t[4:], len(t[4:]) if t[4] != "words=-" else 0)
        return switch_call(fills[t[1][5:]], _hex32(t[2], "first"),
                           _field(t[3], "count", 0, 0xffffffff), args, cat, verb)
    raise ValueError("unknown record %r" % kind)


def parse_text(text):
    """A table's text (dump's format, with comments) as (table, records).
    Raises ValueError naming the line on any defect.
    """
    lines = text.splitlines()
    head = lines[0].split() if lines else []
    if head[:4] != ["#", "odi", "replay", "blob:"]:
        raise ValueError("line 1: not an odi replay blob header")
    fields = {}
    for tok in head[4:]:
        k, _, v = tok.partition("=")
        if k not in ("table", "version", "records", "crc32") or k in fields:
            raise ValueError("line 1: unexpected %r" % tok)
        fields[k] = v
    names = {v: k for k, v in TABLE_NAMES.items()}
    if fields.get("table") not in names:
        raise ValueError("line 1: unknown table %r" % fields.get("table"))
    if fields.get("version") != str(VERSION):
        raise ValueError("line 1: version %r, this reads %d" % (fields.get("version"), VERSION))
    table = names[fields["table"]]
    records = []
    prev_verb = 0
    for i, line in enumerate(lines[1:], 2):
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        try:
            rec = parse_record(table, body)
            check_record(table, rec, prev_verb)
        except (ValueError, struct.error) as e:
            raise ValueError("line %d: %s" % (i, e)) from None
        if table == TABLE_SDKINIT:
            prev_verb = rec[3]
        records.append(rec)
        if len(records) > MAX_RECORDS:
            raise ValueError("line %d: more than %d records, the kernel loader refuses it"
                             % (i, MAX_RECORDS))
    if "records" in fields and fields["records"] != str(len(records)):
        raise ValueError("line 1: records=%s, the text has %d" % (fields["records"], len(records)))
    if "crc32" in fields:
        crc = _CRC.unpack_from(pack(table, records), _HEADER.size)[0]
        if fields["crc32"] != "0x%08x" % crc:
            raise ValueError("line 1: crc32=%s, the records give 0x%08x" % (fields["crc32"], crc))
    return table, records


def build(text):
    """The blob a table's text describes, as bytes."""
    table, records = parse_text(text)
    return pack(table, records)


def build_tables(outdir=OUT_DIR, srcdir=TABLES_DIR):
    """Writes <outdir>/<table>.bin for each table text in srcdir. A file
    whose bytes would not change is left alone, and a new one is written
    under a temporary name first, so a reader never sees half a blob.
    """
    os.makedirs(outdir, exist_ok=True)
    for name in sorted(TABLE_NAMES.values()):
        src = os.path.join(srcdir, name + ".txt")
        with open(src) as f:
            try:
                data = build(f.read())
            except ValueError as e:
                raise ValueError("%s: %s" % (src, e)) from None
        dst = os.path.join(outdir, name + ".bin")
        try:
            with open(dst, "rb") as f:
                if f.read() == data:
                    continue
        except FileNotFoundError:
            pass
        tmp = "%s.%d.tmp" % (dst, os.getpid())
        with open(tmp, "wb") as f:
            f.write(data)
        os.chmod(tmp, 0o644)
        os.replace(tmp, dst)


def _modload_keep(mask, rec):
    """The record as the mask applies it, or None when the mask drops it."""
    kind, cat, grp, verb, tbl, n, offset, value, *words = _RECORD_SWITCH.unpack(rec)
    # A call record stands for table records and keeps their category.
    if kind in (KIND_TABLE, KIND_CALL):
        return rec if mask & (1 << cat) else None
    if mask & 1:
        return rec
    if grp == 0 and mask & (1 << FLOOD_CPU_BIT):
        return _RECORD_SWITCH.pack(kind, cat, grp, verb, tbl, n, offset,
                                   FLOOD_CPU_VALUE, *words)
    return rec if mask & (1 << (6 + grp)) else None


def filter_modload(data, mask):
    """A modload blob with mask applied, as bytes."""
    table, _crc, records = unpack(data)
    if table != TABLE_MODLOAD:
        raise ValueError("not a modload blob (table %s)" % TABLE_NAMES[table])
    kept = [r for r in (_modload_keep(mask, rec) for rec in records) if r is not None]
    return pack(TABLE_MODLOAD, kept)


# The gpon_init sweeps the blob no longer carries, each as (name, rows,
# per-row writes). The first three are the clean that
# odi_gpon_init_clean_tables() does in code after the PONMAC reset wait,
# with the DONE poll and a read-back: the flow type words and the
# GEM-port CAM, the Alloc-ID CAM, and the downstream multicast filter, a
# request of OP 3 (clear) per row with REQ clear, then set. The replay ran
# them again after that clean, on tables already clear. The fourth selects
# every per-flow counter of the four indirect pairs (odi_gpon_hw.h) without
# reading one: the capture records writes only, so the read that clears a
# counter is not in it, and odi_gpon_flow.c writes the index itself before
# every read.
GPON_INIT_TRIMMED = (
    ("Alloc-ID CAM clean", 32,
     lambda r: ((0x7010c0, 0x300 | r), (0x7010c0, 0x8300 | r))),
    ("GEM-port CAM and flow type clean", 128,
     lambda r: ((0x701100, 0x300 | r), (0x701100, 0x8300 | r), (0x701400 + 4 * r, 0))),
    ("multicast filter clean", 256,
     lambda r: ((0x704084, 0x300 | r), (0x704084, 0x8300 | r))),
    ("per-flow counter index sweep", 128,
     lambda r: ((0x701140, r), (0x701140, 0x100 | r), (0x704040, r), (0x70404c, r),
                (0x706048, 0x8080 | r), (0x706048, 0x8000 | r))),
)


def _gpon_plain_reg(rec):
    """(offset, value) of a gpon_init register record that carries no
    serial-number word, else None.
    """
    kind, sn_word, _res, _tbl, _n, offset, value, *_words = _RECORD_GPON.unpack(rec)
    if kind != KIND_REG or sn_word != SN_WORD_NONE:
        return None
    return offset, value


def trim_gpon_init(data):
    """A gpon_init blob without the GPON_INIT_TRIMMED sweeps, as bytes.
    Each sweep must appear exactly once, whole and in order; anything else
    raises ValueError, so a blob of another shape is never trimmed by
    guess.
    """
    table, _crc, records = unpack(data)
    if table != TABLE_GPON_INIT:
        raise ValueError("not a gpon_init blob (table %s)" % TABLE_NAMES[table])
    regs = [_gpon_plain_reg(r) for r in records]
    drop = set()
    for name, rows, row_writes in GPON_INIT_TRIMMED:
        want = [w for r in range(rows) for w in row_writes(r)]
        at = [i for i in range(len(regs) - len(want) + 1)
              if regs[i:i + len(want)] == want]
        if len(at) != 1:
            raise ValueError("%s: found %d times, want once" % (name, len(at)))
        drop.update(range(at[0], at[0] + len(want)))
    return pack(TABLE_GPON_INIT, [r for i, r in enumerate(records) if i not in drop])


# The GPON MAC register window. The stock modules loading write a GPON MAC
# bring-up of their own, a PONMAC_RESET pulse included, which gpon_init.bin
# repeats in full at the first gponact, after its own reset.
GPON_MAC_FIRST = 0x700000
GPON_MAC_END = 0x707000
PONMAC_RESET = 0x70000c


def _gpon_after_reset(gpon_init):
    """The offsets gpon_init writes after it releases PONMAC_RESET (the
    first write with RESET_REQ, bit 0, clear that follows one with it set).
    """
    table, _crc, records = unpack(gpon_init)
    if table != TABLE_GPON_INIT:
        raise ValueError("not a gpon_init blob (table %s)" % TABLE_NAMES[table])
    regs = [_gpon_plain_reg(r) for r in records]
    pulsed = False
    for i, reg in enumerate(regs):
        if reg is None or reg[0] != PONMAC_RESET:
            continue
        if reg[1] & 1:
            pulsed = True
        elif pulsed:
            return {r[0] for r in regs[i + 1:] if r is not None}
    raise ValueError("gpon_init has no PONMAC_RESET pulse")


def trim_modload_gpon(modload, gpon_init):
    """A modload blob without its GPON MAC register writes, as bytes. Every
    register it drops but PONMAC_RESET must be written again after the
    gpon_init reset, by gpon_init or by the code that took over a trimmed
    sweep, or this raises ValueError: a write gpon_init does not
    repeat is not dropped by guess.
    """
    table, _crc, records = unpack(modload)
    if table != TABLE_MODLOAD:
        raise ValueError("not a modload blob (table %s)" % TABLE_NAMES[table])
    # The registers of the trimmed sweeps are written after the reset too:
    # by the driver clean, or by odi_gpon_flow.c before every read.
    again = _gpon_after_reset(gpon_init) | {
        o for _n, rows, row_writes in GPON_INIT_TRIMMED
        for r in range(rows) for o, _v in row_writes(r)}
    kept = []
    dropped = 0
    for rec in records:
        kind, _cat, _grp, _verb, _tbl, _n, offset, _value, *_w = _RECORD_SWITCH.unpack(rec)
        if kind == KIND_REG and GPON_MAC_FIRST <= offset < GPON_MAC_END:
            if offset != PONMAC_RESET and offset not in again:
                raise ValueError("0x%06x: gpon_init does not write it after its reset" % offset)
            dropped += 1
            continue
        kept.append(rec)
    if not dropped:
        raise ValueError("no GPON MAC register write to drop")
    return pack(TABLE_MODLOAD, kept)


def _transform(argv, fn):
    with open(argv[2], "rb") as f:
        data = f.read()
    try:
        out = fn(data)
    except ValueError as e:
        sys.exit("replayblob.py: %s: %s" % (argv[2], e))
    with open(argv[3], "wb") as f:
        f.write(out)


def main(argv):
    if len(argv) == 4 and argv[1] in ("compact", "expand"):
        _transform(argv, compact if argv[1] == "compact" else expand)
        return
    if len(argv) == 5 and argv[1] == "trim-modload-gpon":
        with open(argv[2], "rb") as f:
            modload = f.read()
        with open(argv[3], "rb") as f:
            gpon_init = f.read()
        try:
            out = trim_modload_gpon(modload, gpon_init)
        except ValueError as e:
            sys.exit("replayblob.py: %s: %s" % (argv[2], e))
        with open(argv[4], "wb") as f:
            f.write(out)
        return
    if len(argv) == 4 and argv[1] == "trim-gpon":
        with open(argv[2], "rb") as f:
            data = f.read()
        try:
            out = trim_gpon_init(data)
        except ValueError as e:
            sys.exit("replayblob.py: %s: %s" % (argv[2], e))
        with open(argv[3], "wb") as f:
            f.write(out)
        return
    if len(argv) == 5 and argv[1] == "filter":
        with open(argv[2], "rb") as f:
            data = f.read()
        try:
            out = filter_modload(data, int(argv[4], 0))
        except ValueError as e:
            sys.exit("replayblob.py: %s: %s" % (argv[2], e))
        with open(argv[3], "wb") as f:
            f.write(out)
        return
    if len(argv) == 4 and argv[1] == "build":
        with open(argv[2]) as f:
            text = f.read()
        try:
            out = build(text)
        except ValueError as e:
            sys.exit("replayblob.py: %s: %s" % (argv[2], e))
        if argv[3] == "-":
            sys.stdout.buffer.write(out)
        else:
            with open(argv[3], "wb") as f:
                f.write(out)
        return
    if len(argv) in (2, 3) and argv[1] == "build-tables":
        try:
            build_tables(argv[2] if len(argv) == 3 else OUT_DIR)
        except ValueError as e:
            sys.exit("replayblob.py: %s" % e)
        return
    names = None
    if len(argv) == 5 and argv[1] == "dump" and argv[2] == "--names":
        names = load_regnames(argv[3])
        argv = argv[:2] + argv[4:]
    if len(argv) != 3 or argv[1] != "dump":
        sys.exit(__doc__)
    with open(argv[2], "rb") as f:
        data = f.read()
    try:
        sys.stdout.write(dump(data, names))
    except ValueError as e:
        sys.exit("replayblob.py: %s: %s" % (argv[2], e))


if __name__ == "__main__":
    main(sys.argv)
