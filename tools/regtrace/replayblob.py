#!/usr/bin/env python3
"""Register-replay firmware blobs: the on-disk format the odi driver set
loads with request_firmware() from /lib/firmware/odi/, written by
mksdkinit.py, mkmodload.py and mkgponinit.py, read back here for tests and
for review.

    replayblob.py dump <file.bin>

prints one line per record, the same text a generator prints on stdout
when it is given no output path, so a checked-in blob can be reviewed (and
diffed) as text.

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

    0   u8   kind         KIND_REG, KIND_TABLE or KIND_SOC
    1   u8   category     modload: 0-5; sdkinit: 0
    2   u8   reg_group    modload category 0: 0..MAX_REG_GROUP; else 0
    3   u8   verb         sdkinit: index into SDKINIT_VERBS; modload: 0
    4   u16  table        TABLE records: switch table id; else 0
    6   u16  n_words      TABLE records: 1..MAX_WORDS; else 0
    8   u32  offset       REG: MMIO offset; TABLE: row index; SOC: address
    12  u32  value        REG and SOC records; 0 for TABLE
    16  u32  words[5]     TABLE records, zero past n_words; else 0

Record, GPON init table:

    0   u8   kind         KIND_REG or KIND_TABLE
    1   u8   sn_word      SN_WORD_NONE, or 1..4 (serial-number word)
    2   u16  reserved     0
    4.. same as the switch record from byte 4 on

sdkinit records are stored grouped by verb, in SDKINIT_VERBS order, each
verb in capture order; a verb with nothing to replay has no records.
"""
import struct
import sys
import zlib

MAGIC = 0x4f444952  # "ODIR"
VERSION = 1
HEADER_SIZE = 24
RECORD_SIZE = 36
MAX_WORDS = 5

TABLE_SDKINIT = 1
TABLE_MODLOAD = 2
TABLE_GPON_INIT = 3
TABLE_NAMES = {TABLE_SDKINIT: "sdkinit", TABLE_MODLOAD: "modload",
               TABLE_GPON_INIT: "gpon_init"}

KIND_REG = 0
KIND_TABLE = 1
KIND_SOC = 2
KIND_NAMES = {KIND_REG: "REG", KIND_TABLE: "TABLE", KIND_SOC: "SOC"}

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
    return "%s%s off=0x%08x val=0x%08x" % (prefix, KIND_NAMES.get(kind, "KIND%d" % kind),
                                          offset, value)


def dump(data):
    """The whole blob as text: a header comment line, then one line per
    record.
    """
    table, crc, records = unpack(data)
    lines = ["# odi replay blob: table=%s version=%d records=%d crc32=0x%08x"
             % (TABLE_NAMES[table], VERSION, len(records), crc)]
    lines.extend(dump_record(table, r) for r in records)
    return "\n".join(lines) + "\n"


def _modload_keep(mask, rec):
    """The record as the mask applies it, or None when the mask drops it."""
    kind, cat, grp, verb, tbl, n, offset, value, *words = _RECORD_SWITCH.unpack(rec)
    if kind == KIND_TABLE:
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


def main(argv):
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
    if len(argv) != 3 or argv[1] != "dump":
        sys.exit(__doc__)
    with open(argv[2], "rb") as f:
        data = f.read()
    try:
        sys.stdout.write(dump(data))
    except ValueError as e:
        sys.exit("replayblob.py: %s: %s" % (argv[2], e))


if __name__ == "__main__":
    main(sys.argv)
