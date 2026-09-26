#!/usr/bin/env python3
"""Turn librtk's chip register table into C we can link, and name it.

`reg_field_read(reg, field, &v)` resolves a register index and a field index
through tables reached from `hal_ctrl`, which is filled in at startup by chip
detection. That reads like "only exists on a running device", which is what
PENDING.md item 4 said for weeks. It is not true: `librtk.so` exports
`rtk_rtl9602c_reg_list` outright, and every register's field array is a
separate symbol the table points at through a relocation.

WHERE EVERYTHING COMES FROM. The table -- every address, width, array and
port range, every field's bit position and width, every field's numeric id --
is read out of the stock firmware binary, the librtk.so the device runs.
Nothing else is an input to it. It has to be that exact binary:
`reg_field_read` takes a register INDEX, the commands we generate pass that
index as a compile-time constant lifted from the stock handlers, and an index
only means something against the table those handlers were built with. A
table from any other firmware build is not a near miss -- after the first
inserted row every index lands on the wrong register, with no error.
test/regmap_test.c pins rows that would move if that ever happened.

NAMES ARE OURS. The binary carries no names we use: a field's name is a bare
numeric id. Every name printed by this tool, or emitted with --names, comes
from tools/regnames.txt -- a short curated list, in our own words, of the
registers our code, scripts and docs actually touch -- or from a mechanical
default built from the address and bit range:

    register   <BLOCK>_0x<addr>, e.g. SW_0x023038, GPON_0x701010
    field      F_<msb>_<lsb>, or F_<bit> for one bit; RSVD_<msb>_<lsb> for
               the fields the binary marks reserved (id 0)

regnames.txt is checked against the binary, not trusted: a name for an
address the table does not have, or for a bit range that is not one of that
register's fields, is an error, so a curated name cannot silently drift off
what it names when the base firmware changes.

Layout, confirmed against the first rows of the table:

    register row, 24 bytes
      +0  address u32      +4  field count u32
      +8  array stride u16, first array index u16
      +12 last array index u16, first port u16
      +16 last port u16, 16 bits padding
      +20 fields pointer, an R_MIPS_REL32 against the field array's symbol
    field row, 8 bytes
      +0  field id u32 (GLOBAL, chip-wide)   +4 lsb u16   +6 width u16

The field id matters: the stock field lookup walks the register's
field array comparing ids, so a command names a field by that global id, not
by its position in the register. Ids are unique chip-wide (3060 distinct, max
3059) except id 0, which marks a reserved field and appears 1345 times.

Names do NOT go in the emitted C by default: callers look registers up by
id, so 1500 strings would add tens of KB to serve nobody. They go to the
text listing, which tools/regtrace and tools/regdump read.

--only prunes the emitted table to the registers named, which is how it is
normally built. The whole table is 1530 registers and ~65 KB of image; the
generated handlers reference a few dozen of them. Since nothing looks a
register up at runtime by anything but a constant the generator already
knows, carrying the rest costs image for nobody. The emitted table is sorted
by register id and searched, rather than indexed, so pruning does not
renumber anything -- a handler still passes the stock firmware's own id.

Usage: regmap-extract.py <librtk.so> <chip> [-o outdir] [-t listing]
                         [--regnames file] [--names] [--only ids|@file]
"""
import argparse, os, re, struct, sys

REG_SIZE = 24
FIELD_SIZE = 8
HERE = os.path.dirname(os.path.abspath(__file__))


class Dyn:
    """Just enough ELF to read a stripped shared object's dynamic tables.

    Section headers are gone in this image, so everything comes from
    PT_DYNAMIC: the symbol table for resolving relocations, and DT_REL for the
    relocations themselves.
    """

    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 2:
            sys.exit('%s is not a 32-bit big-endian ELF' % path)
        phoff, = struct.unpack('>I', d[28:32])
        phentsize, = struct.unpack('>H', d[42:44])
        phnum, = struct.unpack('>H', d[44:46])
        self.loads = []
        dynoff = None
        for i in range(phnum):
            o = phoff + i * phentsize
            typ, off, vaddr, _, filesz, _, _, _ = struct.unpack('>8I', d[o:o + 32])
            if typ == 1:
                self.loads.append((vaddr, off, filesz))
            elif typ == 2:
                dynoff = off
        if dynoff is None:
            sys.exit('no PT_DYNAMIC')
        tags = {}
        k = dynoff
        while True:
            t, v = struct.unpack('>2I', d[k:k + 8])
            k += 8
            if t == 0:
                break
            tags.setdefault(t, v)
        for need in (6, 5, 17, 18):     # SYMTAB, STRTAB, REL, RELSZ
            if need not in tags:
                sys.exit('dynamic tag %d missing' % need)
        self.symtab = self.v2o(tags[6])
        self.strtab = self.v2o(tags[5])
        self.rel = self.v2o(tags[17])
        self.relsz = tags[18]

    def v2o(self, v):
        for (va, off, sz) in self.loads:
            if va <= v < va + sz:
                return off + (v - va)
        sys.exit('address 0x%x is in no PT_LOAD' % v)

    def u32(self, vaddr):
        o = self.v2o(vaddr)
        return struct.unpack('>I', self.d[o:o + 4])[0]

    def u16(self, vaddr):
        o = self.v2o(vaddr)
        return struct.unpack('>H', self.d[o:o + 2])[0]

    def sym(self, idx):
        o = self.symtab + idx * 16
        st_name, st_value, st_size, _, _, _ = struct.unpack('>3I2BH', self.d[o:o + 16])
        e = self.d.index(b'\0', self.strtab + st_name)
        return (self.d[self.strtab + st_name:e].decode(), st_value, st_size)

    def symbols(self):
        """Every dynamic symbol. The table's length is not recorded once the
        section headers are gone, so walk until a name runs off the string
        table -- the symbols sit immediately before it."""
        out = []
        i = 0
        while True:
            o = self.symtab + i * 16
            if o + 16 > self.strtab:
                break
            out.append(self.sym(i))
            i += 1
        return out

    def relocations(self):
        """r_offset -> symbol index, for every relocation."""
        out = {}
        for i in range(self.relsz // 8):
            r_off, r_info = struct.unpack('>2I', self.d[self.rel + i * 8:self.rel + i * 8 + 8])
            out[r_off] = r_info >> 8
        return out


NAME_RE = re.compile(r'^[A-Z][A-Z0-9_]*$')


def load_regnames(path):
    """tools/regnames.txt -> (blocks, {address: (name, {(msb, lsb): field})}).

    Only the syntax is checked here; whether the names fit the binary is
    checked in apply_regnames, once the table has been read."""
    blocks, regs = [], {}
    seen = {}
    for n, line in enumerate(open(path), 1):
        line = line.split('#', 1)[0].split()
        if not line:
            continue
        where = '%s:%d' % (os.path.basename(path), n)
        if line[0] == 'block' and len(line) == 4:
            lo, hi = int(line[1], 16), int(line[2], 16)
            if lo > hi or not NAME_RE.match(line[3]):
                sys.exit('%s: bad block line' % where)
            blocks.append((lo, hi, line[3]))
        elif line[0] == 'reg' and len(line) >= 3:
            addr, name = int(line[1], 16), line[2]
            if not NAME_RE.match(name):
                sys.exit('%s: %r is not a register name' % (where, name))
            if addr in regs:
                sys.exit('%s: 0x%06x is named twice' % (where, addr))
            if name in seen:
                sys.exit('%s: %s already names 0x%06x' % (where, name, seen[name]))
            seen[name] = addr
            fields = {}
            for tok in line[3:]:
                m = re.match(r'^(\d+):(\d+)=([A-Z][A-Z0-9_]*)$', tok)
                if not m:
                    sys.exit('%s: %r is not <msb>:<lsb>=<NAME>' % (where, tok))
                msb, lsb, fname = int(m.group(1)), int(m.group(2)), m.group(3)
                if (msb, lsb) in fields or fname in fields.values():
                    sys.exit('%s: field %s or %d:%d given twice' % (where, fname, msb, lsb))
                fields[(msb, lsb)] = fname
            regs[addr] = (name, fields)
        else:
            sys.exit('%s: not a block or reg line' % where)
    return blocks, regs


def default_reg_name(blocks, addr):
    for (lo, hi, prefix) in blocks:
        if lo <= addr <= hi:
            return '%s_0x%06x' % (prefix, addr)
    return 'R_0x%06x' % addr


def default_field_name(fid, lsp, ln):
    msb = lsp + ln - 1
    if fid == 0:
        return 'RSVD_%d_%d' % (msb, lsp)
    return 'F_%d' % lsp if ln == 1 else 'F_%d_%d' % (msb, lsp)


def apply_regnames(regs, fields, blocks, curated):
    """Give every register and field a name, and check the curated ones
    against what the binary actually says is there."""
    by_addr = {r['off']: r for r in regs}
    for addr, (name, fnames) in curated.items():
        r = by_addr.get(addr)
        if r is None:
            sys.exit('regnames: 0x%06x (%s) is not a register in this table'
                     % (addr, name))
        have = {}
        for k in range(r['nfld']):
            fid, lsp, ln = fields[r['fbase'] + k][:3]
            have[(lsp + ln - 1, lsp)] = fid
        for (msb, lsb), fname in fnames.items():
            if (msb, lsb) not in have:
                sys.exit('regnames: %s has no field at bits %d:%d'
                         % (name, msb, lsb))
            if have[(msb, lsb)] == 0:
                sys.exit('regnames: %s bits %d:%d is reserved; not naming it'
                         % (name, msb, lsb))

    used = set()
    for r in regs:
        cur = curated.get(r['off'])
        r['name'] = cur[0] if cur else default_reg_name(blocks, r['off'])
        if r['name'] in used:
            sys.exit('regnames: %s names two registers' % r['name'])
        used.add(r['name'])
        fnames = cur[1] if cur else {}
        for k in range(r['nfld']):
            i = r['fbase'] + k
            fid, lsp, ln = fields[i][:3]
            fields[i] = (fid, lsp, ln,
                         fnames.get((lsp + ln - 1, lsp))
                         or default_field_name(fid, lsp, ln))


def emit_c(args, chip, regs, emitted, emitted_fields):
    """regmap.{c,h} for the registers kept, in the order the ids run."""
    os.makedirs(args.outdir, exist_ok=True)
    stem = os.path.join(args.outdir, 'regmap')

    with open(stem + '.h', 'w') as f:
        f.write(HEADER % dict(chip=chip, nregs=len(emitted),
                              nfields=len(emitted_fields), total=len(regs),
                              names=NAMES_DECL if args.names else NAMES_ABSENT))

    with open(stem + '.c', 'w') as f:
        f.write('/* Generated by tools/regmap-extract.py from %s.\n'
                ' * Do not edit. */\n#include "regmap.h"\n\n'
                % os.path.basename(args.librtk))
        if args.names:
            # One NUL-terminated name per emitted register, in table order.
            f.write('const char rtk_regmap_names[] =\n')
            for _i, r in emitted:
                f.write('\t"%s\\0"\n' % r['name'])
            f.write('\t;\n\n')
        f.write('const struct rtk_regfield rtk_regmap_fields[] = {\n')
        for (fid, lsp, ln, _n) in emitted_fields:
            f.write('\t{ %5d, %3d, %3d },\n' % (fid, lsp, ln))
        f.write('};\n\nconst struct rtk_reg rtk_regmap_regs[] = {\n')
        for i, r in emitted:
            f.write('\t{ %5d, 0x%08x, %5d, %5d, %5d, %5d, %5d, %3d, %3d },'
                    '\t/* %s */\n'
                    % (i, r['off'], r['fbase'], r['nfld'], r['aoff'],
                       r['larr'], r['harr'], r['lport'], r['hport'],
                       r['name']))
        f.write('};\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('librtk')
    ap.add_argument('chip')
    # No default. Writing the register table is a build step, and this tool is
    # also the obvious thing to reach for when asking a question about one
    # register -- `--only 668 -t -` and the like. With a default outdir, such a
    # query silently replaced generated/regmap.{c,h} with a one-register table
    # that still compiled and still passed every decode assertion, because the
    # registers it kept were the ones being asked about. Make emitting explicit.
    ap.add_argument('-o', '--outdir',
                    help='write regmap.{c,h} here; omit to only list')
    ap.add_argument('-t', '--listing')
    ap.add_argument('--regnames', default=os.path.join(HERE, 'regnames.txt'),
                    help='our register and field names (default: %(default)s)')
    ap.add_argument('--names', action='store_true',
                    help='also put register name strings in the C')
    ap.add_argument('--only', help='emit only these register ids: a comma-'
                    'separated list, or @file with one id per line. The '
                    'listing is always complete.')
    args = ap.parse_args()

    chip = args.chip
    el = Dyn(args.librtk)
    syms = el.symbols()
    want = 'rtk_%s_reg_list' % chip
    base = size = None
    for (name, value, sz) in syms:
        if name == want:
            base, size = value, sz
    if base is None:
        sys.exit('%s exports no %s' % (args.librtk, want))
    if not size or size % REG_SIZE:
        sys.exit('%s has size %s, not a multiple of %d' % (want, size, REG_SIZE))
    nregs = size // REG_SIZE

    relocs = el.relocations()

    keep = None
    if args.only:
        text = (open(args.only[1:]).read() if args.only.startswith('@')
                else args.only)
        text = re.sub(r'#[^\n]*', '', text)      # the generated file is commented
        keep = {int(t) for t in re.split(r'[\s,]+', text.strip()) if t}
        if not keep:
            sys.exit('--only named no registers')

    regs = []
    fields = []
    for i in range(nregs):
        a = base + i * REG_SIZE
        off = el.u32(a)
        nfld = el.u32(a + 4)
        aoff, larr = el.u16(a + 8), el.u16(a + 10)
        harr, lport = el.u16(a + 12), el.u16(a + 14)
        hport = el.u16(a + 16)

        # The fields pointer is resolved through its relocation. The symbol
        # it names is used only as a size check -- its byte size must be
        # exactly nfld field entries -- never as a name.
        sidx = relocs.get(a + 20)
        if sidx is None:
            sys.exit('register %d has no relocation for its fields pointer' % i)
        _fsym, fval, fsz = el.sym(sidx)
        if fsz % FIELD_SIZE or fsz // FIELD_SIZE != nfld:
            sys.exit('register %d declares %d fields, its field array is %d bytes'
                     % (i, nfld, fsz))

        fbase = len(fields)
        for k in range(nfld):
            fa = fval + k * FIELD_SIZE
            fields.append((el.u32(fa), el.u16(fa + 4), el.u16(fa + 6), None))
        regs.append(dict(off=off, fbase=fbase, nfld=nfld, aoff=aoff,
                         larr=larr, harr=harr, lport=lport, hport=hport))

    blocks, curated = load_regnames(args.regnames)
    apply_regnames(regs, fields, blocks, curated)

    # The emitted table: every register, or just the ones asked for. Field
    # arrays are re-flattened over the subset so fbase stays a valid index.
    emitted, emitted_fields = [], []
    for i, r in enumerate(regs):
        if keep is not None and i not in keep:
            continue
        r = dict(r, fbase=len(emitted_fields))
        emitted_fields.extend(fields[regs[i]['fbase']:
                                     regs[i]['fbase'] + regs[i]['nfld']])
        emitted.append((i, r))
    if keep is not None:
        missing = sorted(keep - {i for i, _ in emitted})
        if missing:
            sys.exit('--only names registers this chip does not have: %s'
                     % missing[:10])

    if args.outdir is None and args.listing is None:
        sys.exit('nothing to do: pass -o <dir> to emit, -t <file> to list')

    if args.outdir is not None:
        emit_c(args, chip, regs, emitted, emitted_fields)

    if args.listing:
        # tools/regtrace/decode.py, mkmodload.py and tools/regdump/mklist.py
        # parse this: keep the register line and the "<k> <name> lsp N len N"
        # prefix of the field line as they are.
        f = sys.stdout if args.listing == '-' else open(args.listing, 'w')
        f.write('# %s registers, read out of %s by tools/regmap-extract.py.\n'
                '# Index is the register id reg_field_read takes; "id" is the\n'
                '# field id. Addresses and bit layout are the binary; names\n'
                '# are ours, from tools/regnames.txt or the default scheme.\n'
                % (chip, os.path.basename(args.librtk)))
        for i, r in enumerate(regs):
            f.write('\n%4d  0x%06x  width %3d  array %d..%-3d  port %d..%-2d  %s\n'
                    % (i, r['off'], r['aoff'] or 32, r['larr'], r['harr'],
                       r['lport'], r['hport'], r['name']))
            for k in range(r['nfld']):
                (fid, lsp, ln, fname) = fields[r['fbase'] + k]
                f.write('        %2d  %-28s lsp %3d  len %3d  id %4d\n'
                        % (k, fname, lsp, ln, fid))
        if f is not sys.stdout:
            f.close()

    sys.stderr.write('%d registers, %d fields, from %s%s; %d named in %s\n'
                     % (len(emitted), len(emitted_fields),
                        os.path.basename(args.librtk),
                        '' if keep is None
                        else ' (pruned from %d; listing is complete)' % len(regs),
                        len(curated), os.path.basename(args.regnames)))


NAMES_DECL = ("/* One NUL-terminated name per row of rtk_regmap_regs, in order. */\n"
              "extern const char rtk_regmap_names[];\n")
NAMES_ABSENT = ("/* Name strings are deliberately not here: every caller looks a\n"
                " * register up by id. Re-run the extractor with --names if that\n"
                " * changes; the names are ours, from tools/regnames.txt. */\n")

HEADER = '''/* Generated by tools/regmap-extract.py from the shipped librtk.so.
 * Do not edit.
 *
 * Every number here -- addresses, widths, array and port ranges, field bit
 * positions and field ids -- is read out of the stock firmware binary the
 * device runs, and only that binary will do: reg_field_read takes an index,
 * and an index resolved against any other build's table gives the wrong
 * register with no error. The row comments are our names, from
 * tools/regnames.txt; they are not in the binary.
 *
 * Holds %(nregs)d of the chip's %(total)d registers -- the ones the generated
 * handlers actually name. Nothing looks a register up at runtime except by a
 * constant the generator already knew, so the rest would be image for nobody;
 * regenerate without --only to get them all. Rows are sorted by `id` and
 * searched, so a handler still passes the stock firmware's own register id.
 *
 * Field arrays are flattened into one array and a register names its base
 * index, so nothing here needs a relocation.
 */
#ifndef RTK_REGMAP_H
#define RTK_REGMAP_H

#include <stdint.h>

#define RTK_REGMAP_NREGS   %(nregs)d
#define RTK_REGMAP_NFIELDS %(nfields)d

struct rtk_regfield {
	uint16_t id;            /* the field's global id -- what a caller
				 * names it by, NOT its position here */
	uint16_t lsp;           /* least significant bit position */
	uint16_t len;           /* width in bits */
};

struct rtk_reg {
	uint16_t id;            /* the register id -- the table is SEARCHED by
				 * this, not indexed, so pruning it does not
				 * renumber anything */
	uint32_t offset;        /* byte address of element [lport][larray] */
	uint16_t fbase;         /* first field, in rtk_regmap_fields */
	uint16_t nfields;
	uint16_t aoff;          /* array stride in BITS -- also the width */
	uint16_t larray, harray;
	uint8_t  lport, hport;
};

%(names)sextern const struct rtk_regfield rtk_regmap_fields[RTK_REGMAP_NFIELDS];
extern const struct rtk_reg rtk_regmap_regs[RTK_REGMAP_NREGS];

#endif
'''

if __name__ == '__main__':
    main()
