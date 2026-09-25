"""Recover the payload of every omci_wrapper_* call.

Each wrapper funnels into one helper:

    omci_drv_call(u32 cmd, void *buf, u32 len)   /* len <= 256, buf is in/out */

so the wrapper's whole job is packing its arguments into `buf` and unpacking
the answer. This reads that packing: which of the wrapper's own arguments lands
at which offset and width, and whether the buffer is the caller's own pointer
passed straight through.

The tracing rule is the one the diag extractors settled on: only the dominator
chain of the call may be assumed to have run, and anything written on a path
that merely *may* have run is unknown rather than assumed. gcc puts the early
`if (!p) return` between the argument moves and their uses, so a linear scan
loses the provenance of every argument.

    omci-drv-abi.py [libomci_mib.so] [--c DIR]
"""
import os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'diag', 'tools'))
from rtkelf import Image
from mipscfg import Function

HELPER = 0x17de4
STORE = {0x28: 1, 0x29: 2, 0x2b: 4}          # sb, sh, sw
LOAD = {0x20: 1, 0x24: 1, 0x21: 2, 0x25: 2, 0x23: 4}
ARGS = {4: 'a0', 5: 'a1', 6: 'a2', 7: 'a3'}


def s16(x):
    x &= 0xffff
    return x - 0x10000 if x & 0x8000 else x


class Wrapper:
    """One omci_wrapper_*, traced to its omci_drv_call."""

    def __init__(self, img, name, gp, slots, vals):
        self.img, self.name = img, name
        self.cmd = self.len = None
        self.fields = []            # (offset, width, source)
        self.outs = []              # (offset, width, destination)
        self.passthru = False
        self.note = None
        va = img.by_name[name]
        size = img.funcs[va][1]
        self.f = f = Function(img, va, size)
        self._find(f, gp, vals)

    def _call_site(self, f, gp, vals):
        """Index of the `jalr t9` that calls omci_drv_call."""
        for i in range(f.n - 1):
            w = f.words[i]
            if (w >> 26) != 0x23 or ((w >> 21) & 31) != 28 or ((w >> 16) & 31) != 25:
                continue
            base = vals.get(gp + s16(w))
            if base is None:
                continue
            for k in range(i + 1, min(i + 6, f.n)):
                y = f.words[k]
                if (y >> 26) == 9 and ((y >> 21) & 31) == 25 and ((y >> 16) & 31) == 25:
                    if base + s16(y) == HELPER:
                        for j in range(k, min(k + 6, f.n)):
                            if f.words[j] == 0x0320f809:
                                return j
                    break
        return None

    def _find(self, f, gp, vals):
        call = self._call_site(f, gp, vals)
        if call is None:
            self.note = 'no omci_drv_call'
            return
        # Registers along the dominator chain, plus the delay slot which runs
        # before the call lands.
        # Seed the argument registers before tracing, not after: a wrapper
        # spills its arguments into the buffer in its first few instructions,
        # and a tracer that only learns what a0 means at the end reports every
        # one of those stores as being of unknown origin.
        reg = {r: ('arg', n) for r, n in ARGS.items()}
        stack = {}
        trace = [(k, j) for k, idx in f.dom_trace(call) for j in idx]
        trace.append(('exec', call + 1))
        for how, i in trace:
            w = f.words[i]
            op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            if how == 'skip':
                reg.pop(rt, None)
                reg.pop((w >> 11) & 31, None)
                continue
            if op == 9:                                   # addiu / li
                if rs == 0:
                    reg[rt] = ('imm', s16(w))
                elif rs == 29:
                    reg[rt] = ('sp', s16(w))
                elif rs in reg and reg[rs][0] == 'arg':
                    reg[rt] = reg[rs]
                else:
                    reg.pop(rt, None)
            elif op == 0 and (w & 0x7ff) == 0x21:         # move rd, rs
                rd = (w >> 11) & 31
                if rt == 0 and rs in reg:
                    reg[rd] = reg[rs]
                else:
                    reg.pop(rd, None)
            elif op in STORE and rs == 29:
                stack[s16(w)] = (STORE[op], reg.get(rt))
            elif op in LOAD:
                reg.pop(rt, None)
            elif op == 0x0f:
                reg.pop(rt, None)
            elif op == 0:
                # Any other R-type -- sll, srl, addu, or, sub -- writes rd, and
                # a value computed from an argument is not that argument. The
                # skip path above has always killed both rt and rd; this path
                # had no catch-all at all, so a stale ('arg', n) could survive
                # an ALU op and be emitted as `buf[off] = aN`.
                reg.pop((w >> 11) & 31, None)
            else:
                # Everything not modelled above -- andi, ori, xori, slti, lui
                # into a non-gp register -- likewise.
                reg.pop(rt, None)
        a = {}
        for r in (4, 5, 6):
            v = reg.get(r)
            a[r] = v
        self.cmd = a[4][1] if a.get(4) and a[4][0] == 'imm' else None
        self.len = a[6][1] if a.get(6) and a[6][0] == 'imm' else None
        buf = a.get(5)
        if not buf:
            self.note = 'buffer of unknown origin'
            return
        if buf[0] == 'arg':
            self.passthru = True
            self.note = 'caller\'s buffer, in and out'
            return
        if buf[0] != 'sp':
            self.note = 'buffer is not a local'
            return
        base = buf[1]
        for off, (width, src) in sorted(stack.items()):
            if base <= off < base + (self.len or 256):
                kind = src[0] if src else '?'
                if kind == 'arg':
                    what = src[1]
                elif kind == 'imm':
                    what = '%d' % src[1]
                else:
                    what = '<unknown>'
                self.fields.append((off - base, width, what))


def main():
    path = os.path.expanduser('~/tmp/odi/rootfs/lib/libomci_mib.so')
    rest = sys.argv[1:]
    if '--c' in rest:
        i = rest.index('--c')
        rest = rest[:i] + rest[i + 2:]
    argv = [a for a in rest if not a.startswith('--')]
    if argv:
        path = argv[0]
    img = Image(path)
    gp = img.tags[3] + 0x7ff0
    slots, vals = img.got_slots(), img.got_values()
    names = sorted(n for a, (n, s) in img.funcs.items()
                   if n.startswith('omci_wrapper_') and s)
    ok = 0
    rows = []
    for n in names:
        try:
            w = Wrapper(img, n, gp, slots, vals)
        except Exception as e:
            print('%-38s ERROR %s' % (n, e))
            continue
        if w.cmd is None:
            continue
        rows.append(w)
        ok += 1
        short = n.replace('omci_wrapper_', '')
        if w.passthru:
            print('%-30s cmd %-3s len %-4s  caller buffer, in and out' %
                  (short, w.cmd, w.len))
        elif w.fields:
            print('%-30s cmd %-3s len %-4s  %s' % (short, w.cmd, w.len,
                  ', '.join('+%d:%d=%s' % f for f in w.fields)))
        else:
            print('%-30s cmd %-3s len %-4s  %s' % (short, w.cmd, w.len, w.note or '-'))
    print('\n%d wrappers reach the driver; %d pack a local buffer, %d pass the '
          'caller\'s' % (ok, sum(1 for r in rows if r.fields),
                         sum(1 for r in rows if r.passthru)))
    if '--c' in sys.argv:
        emit(apply_hand(rows), sys.argv[sys.argv.index('--c') + 1])


HDR = '''/* The RTL9601 OMCI driver interface.
 *
 * Generated by tools/omci-drv-abi.py from libomci_mib.so. Every call is one
 * socket option:
 *
 *     struct { uint32_t cmd; uint32_t len; uint8_t data[256]; } req;
 *     getsockopt(socket(AF_INET, SOCK_RAW, 0xff), 0, 0x310a, &req, &264);
 *
 * with the payload both the argument and the answer. Two shapes appear: a
 * wrapper either packs its arguments into the payload, in which case the
 * generated function takes them, or hands the caller's own buffer straight
 * through, in which case it takes that buffer and its length is fixed.
 *
 * Names and command numbers are the vendor's. Do not edit.
 */
#ifndef OMCI_DRV_H
#define OMCI_DRV_H

#include <stdint.h>

/* Issue one command. `buf` is `len` bytes in and the same `len` bytes out. */
int omci_drv_call(uint32_t cmd, void *buf, uint32_t len);

'''


# Wrappers whose buffer the tracer cannot follow, recovered by reading them.
#
# The tracer classifies a wrapper by where its `buf` argument comes from: an
# argument of the wrapper (passthru) or a local it packed (fields). A wrapper
# that builds its payload in a FILE-SCOPE GLOBAL is neither, and it says
# "buffer of unknown origin" rather than guessing -- which is the right default
# and is why this table is separate from the tracer instead of a relaxation
# inside it.
#
# name -> (length the driver is given, one line saying what the payload is)
HAND = {
    # omci_wrapper_setDscpRemap, libomci_mib.so 0x1a3e8, 316 bytes.
    #
    # Takes the 24-byte DscpToPbitMapping attribute and unpacks it into a
    # global: three bits per DSCP code point, MSB first, one byte each. The
    # global is memset to 130 bytes and the wrapper keeps two private fields
    # past the payload -- buf[64..127] is its shadow of what it last
    # programmed, buf[128] a bitmask of the P-bit values that changed -- but
    # only buf[0..63] is sent. Sizing this from the memset rather than from the
    # call would put 66 bytes of the wrapper's own bookkeeping on the wire.
    'omci_wrapper_setDscpRemap': (64, "the caller's 64 unpacked P-bits, one per DSCP"),

    # omci_wrapper_setDot1RateLimiter / delDot1RateLimiter, libomci_mib.so
    # 0x184cc and 0x18360.
    #
    # Neither is a pass-through: each takes a 16-byte descriptor, finds the
    # slot in a 12-byte-stride table that already holds its first two words
    # (portMask, kind) -- or, for the setter, the first free one -- and sends
    # { slot, then those 16 bytes }. So the wire payload is 20 bytes and the
    # slot is the caller's business, which is why these are declared as taking
    # the whole buffer rather than the wrapper's own argument.
    #
    # The table's size and its one reserved index are gInfo fields we have not
    # recovered. omcid keeps its own table: what matters is that a delete names
    # the same slot the set did.
    'omci_wrapper_setDot1RateLimiter': (20, "{ slot, portMask, kind, CIR, CBS }"),
    'omci_wrapper_delDot1RateLimiter': (20, "{ slot, portMask, kind, CIR, CBS }"),
}


def apply_hand(rows):
    """Mark the hand-recovered wrappers passthru, leaving the rest alone.

    HAND wins outright, including over a field list the analyser did derive.
    It used to defer to one, which is wrong for the two rate limiters: their
    analysis finds a single `+0:4=<unknown>` -- the slot word the wrapper writes
    ahead of the caller's buffer -- and stopping there emits nothing at all.
    An entry is in HAND because the wrapper was read by hand; that reading is
    better evidence than the analyser's partial one, and the note says why.
    """
    for w in rows:
        if w.name in HAND:
            w.len, w.note = HAND[w.name]
            w.passthru = True
            w.fields = []
    return rows


def cname(w):
    return 'omci_' + w.name.replace('omci_wrapper_', '')


def emit(rows, out):
    os.makedirs(out, exist_ok=True)
    h, c = [HDR], ['#include "omci_drv.h"', '']
    for w in sorted(rows, key=lambda r: r.cmd):
        if w.len is None:
            continue
        n = cname(w)
        if w.passthru:
            h.append('/* command %d, %d bytes in and out */' % (w.cmd, w.len))
            h.append('int %s(void *buf);' % n)
            c.append('int %s(void *buf)\n{\n\treturn omci_drv_call(%du, buf, %du);\n}\n'
                     % (n, w.cmd, w.len))
        elif w.fields and all(f[2] in ARGS.values() or f[2].isdigit() for f in w.fields):
            args = [f for f in w.fields if f[2] in ARGS.values()]
            sig = ', '.join('uint32_t %s' % f[2] for f in args) or 'void'
            h.append('/* command %d, %d bytes: %s */'
                     % (w.cmd, w.len, ', '.join('+%d:%d=%s' % f for f in w.fields)))
            h.append('int %s(%s);' % (n, sig))
            body = ['int %s(%s)\n{\n\tuint8_t buf[%d] = { 0 };\n' % (n, sig, w.len)]
            for off, width, src in w.fields:
                v = src if src in ARGS.values() else '%su' % src
                if width == 4:
                    body.append('\tbuf[%d] = (uint8_t)(%s >> 24); buf[%d] = (uint8_t)(%s >> 16);'
                                % (off, v, off + 1, v))
                    body.append('\tbuf[%d] = (uint8_t)(%s >> 8);  buf[%d] = (uint8_t)%s;'
                                % (off + 2, v, off + 3, v))
                elif width == 2:
                    body.append('\tbuf[%d] = (uint8_t)(%s >> 8);  buf[%d] = (uint8_t)%s;'
                                % (off, v, off + 1, v))
                else:
                    body.append('\tbuf[%d] = (uint8_t)%s;' % (off, v))
            body.append('\treturn omci_drv_call(%du, buf, %du);\n}\n' % (w.cmd, w.len))
            c.append('\n'.join(body))
        else:
            h.append('/* command %d, %d bytes -- payload not recovered: %s */'
                     % (w.cmd, w.len, w.note or 'fields of unknown origin'))
            h.append('/* int %s(void *buf); */' % n)
        h.append('')
    h.append('#endif')
    with open(os.path.join(out, 'omci_drv.h'), 'w') as fh:
        fh.write('\n'.join(h) + '\n')
    with open(os.path.join(out, 'omci_drv.c'), 'w') as fh:
        fh.write('\n'.join(c) + '\n')
    print('wrote %s/omci_drv.[ch]' % out)


if __name__ == '__main__':
    main()
