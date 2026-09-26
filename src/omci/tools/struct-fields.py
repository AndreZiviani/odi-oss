"""Field map of a structure a function is handed by pointer.

`omci_wrapper_cfgGemFlow` takes the caller's 68-byte GEM-flow descriptor and
both reads and rewrites it, so the structure's shape is visible in the accesses
rather than in any declaration. This follows the argument pointer through moves
and copies and records every load and store made through it, with the width and
-- where the value is used immediately -- what it feeds.

    struct-fields.py <file.so> <function> [argument register, default a0]
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'diag', 'tools'))
from rtkelf import Image

LOAD = {0x20: ('lb', 1), 0x24: ('lbu', 1), 0x21: ('lh', 2), 0x25: ('lhu', 2), 0x23: ('lw', 4)}
STORE = {0x28: ('sb', 1), 0x29: ('sh', 2), 0x2b: ('sw', 4)}


def s16(x):
    x &= 0xffff
    return x - 0x10000 if x & 0x8000 else x


def main():
    img = Image(sys.argv[1])
    name = sys.argv[2]
    argreg = {'a0': 4, 'a1': 5, 'a2': 6, 'a3': 7}[sys.argv[3] if len(sys.argv) > 3 else 'a0']
    va = img.by_name[name]
    n = img.funcs[va][1] // 4
    gp = img.tags[3] + 0x7ff0
    slots = img.got_slots()

    # Registers that alias the argument pointer. A copy is an alias; anything
    # else written to a register clears it.
    # The pointer is spilled to the stack almost immediately and reloaded, so
    # the slot that holds it is an alias too; without that, a function like
    # cfgGemFlow shows no accesses at all.
    alias = {argreg}
    spill = set()
    acc = {}
    pend = None
    for i in range(n):
        a = va + i * 4
        w = img.word(a)
        op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        if op == 0 and (w & 0x7ff) == 0x21:                  # move rd, rs
            if rs in alias and rt == 0:
                alias.add(rd)
            else:
                alias.discard(rd)
            continue
        if op == 0x23 and rs == 28 and rt == 25:
            pend = slots.get(gp + s16(w))
            continue
        if op == 0x2b and rs == 29 and rt in alias:      # sw ptr, K(sp)
            spill.add(s16(w))
            continue
        if op == 0x23 and rs == 29:                      # lw rt, K(sp)
            if s16(w) in spill:
                alias.add(rt)
            else:
                alias.discard(rt)
            continue
        if op in LOAD and rs in alias:
            nm, wd = LOAD[op]
            acc.setdefault(s16(w), set()).add('%s%d' % ('r', wd))
            alias.discard(rt)
            continue
        if op in STORE and rs in alias:
            nm, wd = STORE[op]
            acc.setdefault(s16(w), set()).add('%s%d' % ('w', wd))
            continue
        # anything else that writes a register breaks the alias
        if op in LOAD or op in (9, 0x0f, 0x0d, 0x0c, 0x0e):
            alias.discard(rt)
        elif op == 0:
            alias.discard(rd)
        if w == 0x0320f809:
            alias = {r for r in alias if r >= 16}          # callee-saved survive
            pend = None

    print('%s: %d distinct offsets through %s' % (name, len(acc), sys.argv[3] if len(sys.argv) > 3 else 'a0'))
    for off in sorted(acc):
        kinds = sorted(acc[off])
        width = max(int(k[1:]) for k in kinds)
        rw = ''.join(sorted({k[0] for k in kinds}))
        print('  +%-4d %-2s %s' % (off, '%d' % width, 'read/written' if rw == 'rw' else
                                   ('written' if rw == 'w' else 'read')))


if __name__ == '__main__':
    main()
