"""Recover the OMCI information model from the /lib/omci plugins.

Each `mib_<Name>.so` exports the same five symbols:

    gMib<Name>TableInfo   44 bytes    class id, names, attribute count, row size
    gMib<Name>AttrInfo    40 * N      one descriptor per attribute, [0] = entity id
    gMib<Name>DefRow      row size    the default row
    gMib<Name>Oper        68 bytes    the per-ME vtable
    mibTable_init()                   fills all four, then registers the table

All four live in bss, so they are not in the file. `mibTable_init` is
straight-line code that stores constants, string pointers and function pointers
into them, so the model is recovered by executing that store sequence rather
than by reading data -- the same technique the diag extractors use, and the
reason none of this needs a running device.
"""
import os, sys, struct, glob
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'diag', 'tools'))
from rtkelf import Image
# The caller-saved set and the delay-slot rule live in one place now: this
# file is where the rule was learned, at the cost of nine plugins' class ids,
# and diag's own tracer had the same shape of mistake. mipscfg.py carries both
# with the reasoning attached.
from mipscfg import CALL_CLOBBER

ATTR_STRIDE = 40

# I-type opcodes that write rt. Listed rather than assumed: a blanket "kill rt"
# also hits beq/bne, which READ rt, and killing a register a loop branches on
# cost nine plugins their class id when this was first attempted.
WRITES_RT = (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x0a, 0x0b, 0x0e)

# Every field of both structures is named by the vendor's own accessor in
# libomci_mib.so -- each is a 128-byte function whose only load is the offset it
# returns -- so none of this layout is guessed.
TABLE_FIELDS = {0: 'name', 4: 'shortName', 8: 'desc', 12: 'classId',
                16: 'initType', 20: 'stdType', 24: 'actionType',
                28: 'attrNum', 32: 'entrySize'}
ATTR_FIELDS = {0: 'name', 4: 'desc', 8: 'dataType', 12: 'len', 16: 'isIndex',
               20: 'mibSave', 24: 'outStyle', 28: 'oltAcc', 32: 'avcFlag',
               36: 'optionType'}

# MIB_GetAttrSize dispatches on dataType through a six-entry jump table.
DTYPE = {0: 'u8', 1: 'u16', 2: 'u32', 3: 'u64', 4: 'string', 5: 'octets'}
DSIZE = {0: lambda n: 1, 1: lambda n: 2, 2: lambda n: 4, 3: lambda n: 8,
         4: lambda n: n + 1, 5: lambda n: n}

# actionType is `1 << msgType`, verified against omci_app rather than assumed:
# OMCI_CheckIsActionSupported(classId, msgType) indexes a 25-entry jump table by
# msgType - 4 and each arm loads exactly 1 << msgType before calling
# MIB_TableSupportAction. Five message types have no arm (5, 7, 10, 16, 17, 27)
# but bit 10 does appear in a stored mask, so the bit space is denser than the
# dispatch.
ACTIONS = {4: 'create', 5: 'create-complete', 6: 'delete', 7: 'delete-complete',
           8: 'set', 9: 'get', 10: 'get-complete', 11: 'get-all-alarms',
           12: 'get-all-alarms-next', 13: 'mib-upload', 14: 'mib-upload-next',
           15: 'mib-reset', 16: 'alarm', 17: 'avc', 18: 'test',
           19: 'start-sw-download', 20: 'download-section', 21: 'end-sw-download',
           22: 'activate-sw', 23: 'commit-sw', 24: 'sync-time', 25: 'reboot',
           26: 'get-next', 27: 'test-result', 28: 'get-current-data',
           29: 'set-table'}


def actions(mask):
    return [n for b, n in sorted(ACTIONS.items()) if mask and mask & (1 << b)]


def s16(x):
    x &= 0xffff
    return x - 0x10000 if x & 0x8000 else x


class Plugin:
    def __init__(self, path):
        self.path = path
        self.img = img = Image(path)
        self.name = os.path.basename(path)[4:-3]
        # The five symbols are spelled inconsistently across the 81 plugins --
        # gMibAsmTblInfo, gMibTDtableInfo, gMibHSQDefTableInfo -- so match the
        # suffix without case rather than a fixed name.
        self.objs = {}
        for a, n in img.objects.items():
            if not n.startswith('gMib'):
                continue
            low = n.lower()
            for suffix, tag in (('tableinfo', 'TableInfo'), ('tblinfo', 'TableInfo'),
                                ('attrinfo', 'AttrInfo'), ('defrow', 'DefRow'),
                                ('oper', 'Oper')):
                if low.endswith(suffix):
                    self.objs[a] = tag
                    break
        self.got = img.got_values()
        self.slots = img.got_slots()
        self.store = {t: {} for t in ('TableInfo', 'AttrInfo', 'DefRow', 'Oper')}
        self._run()

    def _run(self):
        img = self.img
        va = img.by_name.get('mibTable_init')
        if va is None:
            return
        n = img.funcs[va][1] // 4
        gp = None
        hi = None
        reg = {}
        # A call clobbers the caller-saved registers, but its DELAY SLOT
        # executes first and may legitimately still read one of them.
        # mib_LargeString.so does exactly that twice:
        #
        #     jalr   t9
        #     sw     v0,112(s0)      <- stores the v0 from before the call
        #
        # so the clobber is queued here and applied after the next instruction
        # has been processed. Killing at the call instead silently zeroes an
        # attribute's avc flag and blanks one attribute's name.
        pending_clobber = None
        for i in range(n):
            a = va + i * 4
            w = img.word(a)
            op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            was_call = (op == 3) or (op == 0 and (w & 0x3f) == 9)
            if (w >> 16) == 0x3c1c:
                hi = (w & 0xffff) << 16
            elif (w >> 16) == 0x279c and hi is not None:
                gp = hi + s16(w) + a - 4
            elif op == 0x0f:                                  # lui
                reg[rt] = (w & 0xffff) << 16
            elif op == 0x0d and rs in reg:                    # ori
                reg[rt] = reg[rs] | (w & 0xffff)
            elif op == 9:                                     # addiu / li
                if rs == 0:
                    reg[rt] = s16(w)
                elif rs in reg:
                    reg[rt] = (reg[rs] + s16(w)) & 0xffffffff
                else:
                    reg.pop(rt, None)
            elif op == 0x23 and rs == 28 and gp is not None:  # lw rt, off(gp)
                slot = gp + s16(w)
                if slot in self.slots:
                    reg[rt] = ('fn', self.slots[slot])
                else:
                    reg[rt] = self.got.get(slot, 0)
            elif op == 0 and (w & 0x7ff) == 0x21:             # move rd, rs
                rd = (w >> 11) & 31
                if rs in reg and rt == 0:
                    reg[rd] = reg[rs]
                else:
                    reg.pop(rd, None)
            elif op in (0x28, 0x29, 0x2b):                    # sb / sh / sw
                base = reg.get(rs)
                if isinstance(base, int) and base in self.objs:
                    self.store[self.objs[base]][s16(w)] = (
                        {0x28: 1, 0x29: 2, 0x2b: 4}[op], reg.get(rt, 0))
            elif op == 0:
                # Any other R-type -- addu, sll, and, subu. These write rd, so
                # the I-type kill below never reaches them; without this a
                # register keeps claiming to hold a constant after it has been
                # computed into something else, and the next store records that
                # stale constant as a table value.
                reg.pop((w >> 11) & 31, None)
                if (w & 0x3f) == 9:                           # jalr
                    pending_clobber = CALL_CLOBBER
            elif op == 3:                                     # jal
                pending_clobber = CALL_CLOBBER
            elif op == 0x1c:                                  # SPECIAL2: mul
                # Writes rd only. Treating it as a call, which the first
                # attempt at this did, zeroes nine plugins' class ids.
                reg.pop((w >> 11) & 31, None)
            elif op in WRITES_RT and rt != 0:
                reg.pop(rt, None)

            if pending_clobber is not None and not was_call:
                for d in pending_clobber:
                    reg.pop(d, None)
                pending_clobber = None

    def _s(self, v):
        return self.img.cstr(v) if isinstance(v, int) else None

    def table(self):
        t = {off: val for off, (_, val) in self.store['TableInfo'].items()}
        out = {}
        for off, nm in TABLE_FIELDS.items():
            v = t.get(off)
            out[nm] = self._s(v) if nm in ('name', 'shortName', 'desc') else v
        out['actions'] = actions(out['actionType'])
        out['extra'] = {k: v for k, v in sorted(t.items()) if k not in TABLE_FIELDS}
        return out

    def attrs(self):
        by = {}
        for off, (_, val) in sorted(self.store['AttrInfo'].items()):
            i, f = divmod(off, ATTR_STRIDE)
            by.setdefault(i, {})[f] = val
        out = []
        for i, a in sorted(by.items()):
            at = {}
            for off, nm in ATTR_FIELDS.items():
                v = a.get(off)
                at[nm] = self._s(v) if nm in ('name', 'desc') else v
            dt = at['dataType']
            at['type'] = DTYPE.get(dt, '?%s' % dt)
            at['size'] = DSIZE[dt](at['len'] or 0) if dt in DSIZE else None
            out.append((i, at))
        return out

    def oper(self):
        return {off: val[1] for off, val in sorted(self.store['Oper'].items())}


def main():
    import argparse, json
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('root', nargs='?', default=os.path.expanduser('~/tmp/odi/rootfs/lib/omci'))
    ap.add_argument('--detail', help='print one table in full, by name fragment')
    ap.add_argument('--json', help='write the whole model here')
    ap.add_argument('--c', help='emit omci_mib.[ch] into this directory')
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.root, 'mib_*.so')))
    model, ok, bad = [], 0, 0
    for f in files:
        base = os.path.basename(f)
        try:
            p = Plugin(f)
        except Exception as e:
            print('%-40s ERROR %s' % (base, e))
            bad += 1
            continue
        t, a = p.table(), p.attrs()
        # The plugin's TableInfo carries 10440 for LoidAuth, but the entity
        # the OLT addresses -- and the vendor daemon answers -- is the China
        # Telecom LOID authentication ME, 65530 (the class id the China
        # Telecom OMCI extension assigns it; isp1's OLT reads it three times and sets AuthStatus on it before
        # deciding whether to provision services, 2026-09-17).
        if base == 'mib_LoidAuth.so':
            t['classId'] = 65530
        # entrySize is stored independently of the descriptors, so comparing
        # the two checks the decode of both. It is a lower bound, not an
        # equality: the row aligns each attribute naturally, and a table-valued
        # attribute reserves room for many entries where `len` is one entry --
        # ME 157's fifteen 25-byte parts are why its row is 400 bytes.
        total = sum(at['size'] or 0 for _, at in a)
        good = (t['classId'] is not None and t['attrNum'] == len(a)
                and t['entrySize'] is not None and total <= t['entrySize'])
        ok += good
        bad += not good
        model.append({'plugin': base, 'table': t, 'oper': p.oper(),
                      'attrs': [dict(index=i, **at) for i, at in a]})
        if args.detail and args.detail not in base:
            continue
        why = ''
        if not good:
            why = '  <-- attrNum %s vs %d, entrySize %s vs %d packed' % (
                t['attrNum'], len(a), t['entrySize'], total)
        print('%-40s class %-6s %2d attrs %3s bytes (+%s pad)  %s%s' % (
            base, t['classId'], len(a), t['entrySize'],
            (t['entrySize'] - total) if t['entrySize'] is not None else '?',
            ','.join(t['actions']), why))
        if args.detail:
            print('   %r / %r' % (t['name'], t['desc']))
            print('   initType=%s stdType=%s extra=%s' % (t['initType'], t['stdType'], t['extra']))
            for i, at in a:
                print('   [%2d] %-38s %-7s %3s  index=%s save=%s oltAcc=%s avc=%s opt=%s' % (
                    i, at['name'], at['type'], at['size'], at['isIndex'],
                    at['mibSave'], at['oltAcc'], at['avcFlag'], at['optionType']))
                if at['desc']:
                    print('        %r' % at['desc'])
            print('   oper:', {hex(k): v for k, v in p.oper().items()})

    print('\n%d plugins, %d consistent, %d not; %d attributes total'
          % (len(files), ok, bad, sum(len(m['attrs']) for m in model)))
    if args.json:
        # The desc strings are the plugins' own prose, and nothing here
        # reads them: the committed model carries names and layout only.
        def nodesc(o):
            if isinstance(o, dict):
                return {k: nodesc(v) for k, v in o.items() if k != 'desc'}
            if isinstance(o, list):
                return [nodesc(v) for v in o]
            return o
        with open(args.json, 'w') as fh:
            json.dump(nodesc(model), fh, indent=1, default=str)
        print('wrote %s' % args.json)
    if args.c:
        emit_c(model, args.c)


C_TYPE = {'u8': 'OMCI_U8', 'u16': 'OMCI_U16', 'u32': 'OMCI_U32', 'u64': 'OMCI_U64',
          'string': 'OMCI_STRING', 'octets': 'OMCI_OCTETS'}

HEADER = '''/* The OMCI information model this device implements.
 *
 * Generated by tools/mib-tables.py from the 81 plugins in /lib/omci of
 * base-a4k-260909. Every field name below is the one the vendor's own accessor
 * in libomci_mib.so returns -- MIB_GetAttrDataType reads +8, MIB_GetAttrLen
 * +12, and so on -- so the layout is read, not guessed. Do not edit.
 */
#ifndef OMCI_MIB_H
#define OMCI_MIB_H

#include <stdint.h>

/* MIB_GetAttrSize dispatches on this through a six-entry jump table: the first
 * four are fixed widths, OMCI_STRING is len + 1 and OMCI_OCTETS is len. */
enum omci_type { OMCI_U8, OMCI_U16, OMCI_U32, OMCI_U64, OMCI_STRING, OMCI_OCTETS };

struct omci_attr {
\tconst char *name;
\tuint8_t type;       /* enum omci_type */
\tuint16_t size;      /* bytes, as MIB_GetAttrSize computes them */
\tuint8_t oltAcc;     /* what the OLT may do with it */
\tuint8_t isIndex;
\tuint8_t avc;        /* raises an attribute value change */
};

struct omci_class {
\tuint16_t classId;
\tconst char *name;
\tuint16_t rowSize;   /* naturally aligned, so >= the sum of the sizes */
\tconst struct omci_attr *attrs;
\tuint8_t nattr;
\tuint32_t actions;   /* 1 << msgType, for every message type the ME accepts */
};

extern const struct omci_class omci_classes[];
extern const unsigned omci_class_count;

#endif
'''


def emit_c(model, outdir):
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, 'omci_mib.h'), 'w') as fh:
        fh.write(HEADER)
    lines = ['/* Generated by tools/mib-tables.py -- %d managed entities, %d attributes.'
             % (len(model), sum(len(m['attrs']) for m in model)),
             ' * Do not edit; see omci_mib.h. */',
             '#include "omci_mib.h"', '']
    rows = []
    for e in sorted(model, key=lambda x: x['table']['classId'] or 0):
        t, nm = e['table'], e['plugin'][4:-3]
        if t['classId'] is None:
            continue
        lines.append('static const struct omci_attr attr_%s[] = {' % nm)
        for a in e['attrs']:
            lines.append('\t{ "%s", %s, %s, %s, %s, %s },' % (
                a['name'], C_TYPE.get(a['type'], 'OMCI_U8'), a['size'] or 0,
                a['oltAcc'] or 0, a['isIndex'] or 0, a['avcFlag'] or 0))
        lines.append('};')
        rows.append('\t{ %5d, "%s", %s, attr_%s, %s, 0x%08xu },' % (
            t['classId'], t['name'], t['entrySize'], nm, len(e['attrs']),
            t['actionType'] or 0))
    lines += ['', 'const struct omci_class omci_classes[] = {'] + rows + ['};', '',
              'const unsigned omci_class_count = %d;' % len(rows)]
    with open(os.path.join(outdir, 'omci_mib.c'), 'w') as fh:
        fh.write('\n'.join(lines) + '\n')
    print('wrote %s/omci_mib.[ch]: %d classes' % (outdir, len(rows)))


if __name__ == '__main__':
    main()
