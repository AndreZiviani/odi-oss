"""The GEM flow descriptor, which is a 68-byte structure passed BY VALUE.

The three wrappers that send command 25 all look at first like they pack a
local buffer. They do not. o32 puts a by-value structure in the caller's
argument area, so `addiu a1, sp, <framesize>` is the address of that structure,
not of a local -- `omci_wrapper_updateGemFlow` then does

    memcpy(local, &args, 68)

and sends the copy. Seventeen words, and what the small wrappers add is the
last one: updateGemFlow writes 2 there, updateUsGemFlow writes 1, and
cfgGemFlow leaves whatever the caller set.

So a buffer address equal to the frame size is the signature of a by-value
struct, and the `argN` this reports is word N of it.
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'diag', 'tools'))
from rtkelf import Image
from mipscfg import Function

STORE = {0x28: 1, 0x29: 2, 0x2b: 4}
LOAD = {0x20: 1, 0x24: 1, 0x21: 2, 0x25: 2, 0x23: 4}
REGARG = {4: 'arg1', 5: 'arg2', 6: 'arg3', 7: 'arg4'}


def s16(x):
    x &= 0xffff
    return x - 0x10000 if x & 0x8000 else x


def main():
    img = Image(sys.argv[1] if len(sys.argv) > 1 else
                os.path.expanduser('~/tmp/odi/rootfs/lib/libomci_mib.so'))
    name = sys.argv[2] if len(sys.argv) > 2 else 'omci_wrapper_cfgGemFlow'
    va = img.by_name[name]
    size = img.funcs[va][1]
    f = Function(img, va, size)
    gp = img.tags[3] + 0x7ff0
    vals = img.got_values()

    frame = -s16(f.words[3]) if (f.words[3] >> 16) == 0x27bd else 0
    for i in range(6):
        w = f.words[i]
        if (w >> 16) == 0x27bd:
            frame = -s16(w)
            break

    def argname(off):
        if off >= frame:
            return 'arg%d' % ((off - frame) // 4 + 1)
        return None

    # where the payload buffer is: the a1 of the omci_drv_call
    buf = None
    call = None
    for i in range(f.n):
        if f.words[i] != 0x0320f809:
            continue
        # a1 at the call
        reg = {}
        for k in range(max(0, i - 30), i + 2):
            w = f.words[k]
            op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            if op == 9 and rs == 29:
                reg[rt] = s16(w)
            elif op == 9 or op == 0x0f:
                reg.pop(rt, None)
        # the helper takes (cmd, buf, len); cmd 25 is what this one sends
        immr = {}
        for k in range(max(0, i - 30), i + 2):
            w = f.words[k]
            if (w >> 26) == 9 and ((w >> 21) & 31) == 0:
                immr[(w >> 16) & 31] = s16(w)
        if immr.get(4) == 25 and 5 in reg:
            buf, call = reg[5], i
            break
    if buf is None:
        print('no omci_drv_call(25, ...) found in %s' % name)
        return

    print('%s: frame %d bytes, payload buffer at sp+%d, call at %#x'
          % (name, frame, buf, f.va(call)))

    # every store into the buffer, and where the value came from
    src = {}
    fields = {}
    for i in range(f.n):
        w = f.words[i]
        op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op in LOAD and rs == 29:
            a = argname(s16(w))
            src[rt] = a or 'local+%d' % s16(w)
            continue
        if op == 9 and rs == 0:
            src[rt] = '%d' % s16(w)
            continue
        if op == 0 and (w & 0x7ff) == 0x21 and rt == 0:
            src[(w >> 11) & 31] = src.get(rs, '?')
            continue
        if op in STORE and rs == 29 and buf <= s16(w) < buf + 68:
            fields.setdefault(s16(w) - buf, set()).add(
                (STORE[op], src.get(rt, '?')))
            continue
        if op in LOAD or op in (9, 0x0f, 0x0d):
            src.pop(rt, None)
        elif op == 0:
            src.pop((w >> 11) & 31, None)

    for off in sorted(fields):
        for width, what in sorted(fields[off]):
            print('  +%-3d %d  <- %s' % (off, width, what))


if __name__ == '__main__':
    main()
