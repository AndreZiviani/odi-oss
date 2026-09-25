"""Recover omcicli's command grammar: keyword -> command id, and the payload
fields each arm fills.

omcicli has no dispatch table. It is a chain of strcmp against literals; each
arm zeroes a 240-byte payload on the stack, stores the command id in word 0,
fills a few fields and calls one of the two IPC entries. Two things make a
linear scan wrong here, both already learned on diag:

  * the IPC call usually sits in a tail several arms branch to, so anchoring on
    the call finds no arm at all;
  * the arms are guarded, so what ran before a store is the store's dominator
    chain, not the instructions above it in the layout.

So the anchor is the store of the command id into the payload, and everything
else is read off that store's dominator trace.
"""
import os, sys, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'diag', 'tools'))
from rtkelf import Image
from mipscfg import Function

img = Image(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/tmp/odi/rootfs/bin/omcicli'))
gp = img.tags[3] + 0x7ff0
slots = img.got_slots()
TEXT = (0x400950, 0x403e10)
STR_FIELDS = {'strcpy', 'strncpy', 'memcpy', 'snprintf', 'sprintf', 'strcat'}
NUM = {'strtoul', 'strtol', 'atoi', 'sscanf'}
IPC = {'OMCI_SendMsg', 'omci_SendCmdAndGet'}


def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def func_starts():
    d, tg = img.d, set()
    for va, fsz, fo in img.loads:
        for o in range(fo, fo + fsz - 4, 4):
            x = struct.unpack_from('>I', img.d, o)[0]
            if (x >> 26) == 3:                                  # jal
                tg.add((((va + (o - fo) + 4) & 0xf0000000) | ((x & 0x3ffffff) << 2)))
    return sorted({t for t in tg if TEXT[0] <= t < TEXT[1]} | {img.by_name['main']})


class Arm:
    __slots__ = ('cid', 'va', 'keys', 'fields', 'nums', 'ipc')

    def __init__(self, cid, va):
        self.cid, self.va, self.keys, self.fields, self.nums, self.ipc = cid, va, [], [], [], None


def keywords(f, i):
    """Literals on the dominator chain of instruction i, in order."""
    reg, out = {}, []
    for how, idx in f.dom_trace(i):
        for j in idx:
            y = f.words[j]
            op, rs, t = y >> 26, (y >> 21) & 31, (y >> 16) & 31
            if how == 'skip':
                reg.pop(t, None)
                continue
            if op == 0x0f:
                reg[t] = (y & 0xffff) << 16
            elif op == 9 and rs in reg:
                s = img.cstr(reg[rs] + s16(y))
                if s and 0 < len(s) < 48:
                    out.append(s)
            elif op == 9:
                reg.pop(t, None)
    return out


def scan(f):
    """Every command-id store in f, with what its dominator trace shows."""
    n, W = f.n, f.words
    # payload bases: the a0 of a memset(.., 0, 240)
    bases = set()
    for i in range(n):
        if W[i] != 0x0320f809:
            continue
        if slots.get(gp + s16(W[i - 1])) != 'memset' if (W[i - 1] >> 26) == 0x23 else True:
            pass
        # walk back a little for the gp load and the argument setup
        name, a0, a2 = None, None, None
        for j in range(max(0, i - 8), i + 2):
            x = W[j]
            if (x >> 26) == 0x23 and ((x >> 21) & 31) == 28 and ((x >> 16) & 31) == 25:
                name = slots.get(gp + s16(x))
            elif (x >> 26) == 9 and ((x >> 21) & 31) == 29 and ((x >> 16) & 31) == 4:
                a0 = s16(x)
            elif (x >> 26) == 9 and ((x >> 21) & 31) == 0 and ((x >> 16) & 31) == 6:
                a2 = s16(x)
        if name == 'memset' and a2 == 240 and a0 is not None:
            bases.add(a0)
    out = []
    for i in range(n):
        x = W[i]
        if (x >> 26) != 0x2b or ((x >> 21) & 31) != 29 or s16(x) not in bases:
            continue                                            # sw rX, base(sp)
        rt = (x >> 16) & 31
        base = s16(x)
        trace = [(k, j) for k, idx in f.dom_trace(i) for j in idx]
        cid, pend = None, None
        cand = set()
        last_call = None
        reg, lits = {}, []
        for how, j in trace:
            y = W[j]
            op, rs, t = y >> 26, (y >> 21) & 31, (y >> 16) & 31
            if how == 'skip':
                # An id stored from a tail several arms share is written by a
                # `li` in each arm, none of which is on the chain. Those are the
                # candidates; the chain cannot say which arm ran.
                # `li ID; b <tail>` -- the id an arm feeds a shared store is
                # always the last thing the arm computes, so a `li` further from
                # its block's end is some other value passing through.
                if op == 9 and rs == 0 and t == rt and 0 <= s16(y) < 45:
                    b = f.block_of(j)
                    if b is not None and f.blocks[b][0] - j <= 3:
                        cand.add((s16(y), j))
                if op in (9, 0x0f) or (op == 0 and (y & 0x3f) == 0x21):
                    reg.pop(t, None)
                    reg.pop((y >> 11) & 31, None)
                continue
            if op == 0x0f:
                reg[t] = ('hi', (y & 0xffff) << 16)
            elif op == 9:
                if rs == 0:
                    reg[t] = ('imm', s16(y))
                elif reg.get(rs, ('', 0))[0] == 'hi':
                    s = img.cstr(reg[rs][1] + s16(y))
                    reg[t] = ('str', s) if s else ('?', 0)
                    if s and 0 < len(s) < 48:
                        lits.append((j, s))
                elif rs == 29:
                    reg[t] = ('sp', s16(y))
                else:
                    reg.pop(t, None)
            elif op == 0x23 and rs == 28 and t == 25:
                pend = slots.get(gp + s16(y))
            elif y == 0x0320f809:
                if pend in STR_FIELDS or pend in NUM:
                    a0 = reg.get(4)
                    if a0 and a0[0] == 'sp' and 0 <= a0[1] - base < 240:
                        lits.append((j, '%s -> p+%d' % (pend, a0[1] - base)))
                if pend in NUM:
                    last_call = pend
                pend = None
            elif op in (0x28, 0x29, 0x2b) and rs == 29 and 0 <= s16(y) - base < 240:
                off = s16(y) - base
                if off:
                    w_ = {0x28: 'b', 0x29: 'h', 0x2b: ''}[op]
                    v = reg.get(t)
                    src = str(v[1]) if v and v[0] == 'imm' else (last_call or '<arg>')
                    lits.append((j, 'p%s+%d = %s' % (w_, off, src)))
        # The fields are filled *after* the id is stored, so the dominator
        # chain that proves which arm this is cannot show them. Walk forward in
        # layout order instead, as far as the send.
        freg, fpend, fcall = {}, None, None
        for j in range(i + 1, min(n, i + 90)):
            y = W[j]
            op, rs, t = y >> 26, (y >> 21) & 31, (y >> 16) & 31
            # An arm ends by branching to the tail that sends. Past that point
            # the layout belongs to the next keyword, not this one.
            if op == 2 or (op == 4 and (y & 0x03ff0000) == 0):
                break
            if op == 0x0f:
                freg[t] = ('hi', (y & 0xffff) << 16)
            elif op == 9:
                if rs == 0:
                    freg[t] = ('imm', s16(y))
                elif rs == 29:
                    freg[t] = ('sp', s16(y))
                elif freg.get(rs, ('',))[0] == 'hi':
                    txt = img.cstr(freg[rs][1] + s16(y))
                    freg[t] = ('str', txt) if txt else ('?', 0)
                else:
                    freg.pop(t, None)
            elif op == 0x23 and rs == 28 and t == 25:
                fpend = slots.get(gp + s16(y))
            elif y == 0x0320f809:
                if fpend in IPC:
                    break
                a0 = freg.get(4)
                if fpend and a0 and a0[0] == 'sp' and 0 <= a0[1] - base < 240:
                    a1 = freg.get(5)
                    arg = a1[1] if a1 and a1[0] == 'str' else '<arg>'
                    lits.append((j, '%s(p+%d, %s)' % (fpend, a0[1] - base, arg)))
                fcall = fpend
                fpend = None
            elif op in (0x28, 0x29, 0x2b) and rs == 29 and 0 <= s16(y) - base < 240 and s16(y) != base:
                wd = {0x28: 'b', 0x29: 'h', 0x2b: 'w'}[op]
                v2 = freg.get(t)
                src = str(v2[1]) if v2 and v2[0] == 'imm' else (fcall or '<arg>')
                lits.append((j, 'p%s+%d = %s' % (wd, s16(y) - base, src)))
        v = reg.get(rt)
        cid = v[1] if v and v[0] == 'imm' else None
        if cid is not None:
            out.append((f.va(i), cid, [s for _, s in lits]))
        else:
            # The id is stored from a tail these arms share; each arm's own `li`
            # is the anchor that still has a dominator chain of its own.
            for v, j in sorted(cand):
                out.append((f.va(j), v, keywords(f, j)))
    return out


rows = []
starts = func_starts()
for k, st in enumerate(starts):
    en = starts[k + 1] if k + 1 < len(starts) else TEXT[1]
    for va, cid, lits in scan(Function(img, st, en - st)):
        rows.append((st, va, cid, lits))

# omcicli dispatches argv[1] in main and argv[2] in one function per group, so
# the function an arm lives in names the group, and the *last* literal on the
# arm's dominator chain is the keyword that selected it -- the earlier ones are
# the strcmp chain it fell through to get there.
GROUPS = {}
for st, va, cid, lits in rows:
    keys = [l for l in lits if ' ' not in l and '->' not in l and '=' not in l]
    GROUPS.setdefault(st, []).append(keys[0] if keys else '')

seen = {}
for st, va, cid, lits in sorted(rows, key=lambda r: (r[2] if r[2] is not None else 999, r[1])):
    keys = [l for l in lits if ' ' not in l and '->' not in l and '=' not in l]
    rest = [l for l in lits if l not in keys]
    if cid is None or not keys:
        continue
    kw = keys[-1]
    prev = seen.get(cid)
    if prev and len(prev[1]) >= len(kw):
        continue
    seen[cid] = (st, kw, rest, va)

for cid in sorted(seen):
    st, kw, rest, va = seen[cid]
    print('id %-3d  fn %08x  %-14s  %s' % (cid, st, kw, '; '.join(rest)))

