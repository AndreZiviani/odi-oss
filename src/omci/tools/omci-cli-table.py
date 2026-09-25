"""Recover omci_app's CLI command table.

OMCI_HandleMsg dispatches msgType 4 through a 45-entry gp-relative jump table.
Each arm is a debug-log guard followed by a handler call -- but the `jalr` is
often in a *shared tail*, reached by an unconditional branch, so a scan that
stops at the end of the arm finds the argument setup and no call at all. The
branch has to be followed.
"""
import os, sys, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'diag', 'tools'))
from rtkelf import Image

img = Image(sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/tmp/odi/rootfs/bin/omci_app'))
gp = img.tags[3] + 0x7ff0
slots = img.got_slots()
TABLE, NARM = 0x41c9b4, 45
NOISE = {'printf', 'putchar', 'puts', 'getpid', 'abort', 'memset'}
REG = {4: 'a0', 5: 'a1', 6: 'a2', 7: 'a3'}

def w(a): return struct.unpack_from('>I', img.d, img.off(a))[0]
def s16(a): return struct.unpack_from('>h', img.d, img.off(a) + 2)[0]

# OMCI_HandleMsg keeps the payload pointer in s0 and the caller's reply-queue
# key in s1. A `get` arm passes s1 and no payload at all: the handler builds its
# own 240-byte answer and sends it back itself.
SRC = {16: 'p', 17: 'replyKey'}


def apply(x, a, args):
    rt = (x >> 16) & 31
    if (x >> 26) == 9 and rt in REG:                       # addiu/li
        rs = (x >> 21) & 31
        if rs == 16: args[REG[rt]] = 'p+%d' % s16(a)
        elif rs == 17: args[REG[rt]] = 'replyKey'
        elif rs == 0: args[REG[rt]] = '%d' % s16(a)
    elif (x >> 26) == 0x23 and rt in REG and ((x >> 21) & 31) == 16:
        args[REG[rt]] = 'p[%d]' % (s16(a) // 4)            # lw aX,N(s0)
    elif (x >> 26) in (0x20, 0x24) and rt in REG and ((x >> 21) & 31) == 16:
        args[REG[rt]] = 'p.b[%d]' % s16(a)                 # lb/lbu
    elif (x >> 26) in (0x21, 0x25) and rt in REG and ((x >> 21) & 31) == 16:
        args[REG[rt]] = 'p.h[%d]' % s16(a)                 # lh/lhu -- the
        # entity id is a halfword, and leaving this out made every handler that
        # takes one look as though it took the word at +4 instead
    elif (x & 0xfc1f07ff) == 0x00000021 and ((x >> 21) & 31) in SRC \
            and ((x >> 16) & 31) == 0 and ((x >> 11) & 31) in REG:   # move aX, s0|s1
        args[REG[(x >> 11) & 31]] = SRC[(x >> 21) & 31]

for i in range(NARM):
    a = (struct.unpack_from('>i', img.d, img.off(TABLE + i * 4))[0] + gp) & 0xffffffff
    args, pend, label, steps = {}, None, None, 0
    out = []
    while steps < 300:
        steps += 1
        x = w(a)
        if x == 0x0320f809:                                 # jalr t9
            apply(w(a + 4), a + 4, args)
            if pend and pend not in NOISE:
                out.append((pend, dict(args)))
                break
            if pend == 'printf' and label is None:
                label = args.get('a0')
            args, pend = {}, None
            a += 8
            continue
        if (x >> 16) == 0x8f99:
            pend = slots.get(gp + s16(a))
        elif (x >> 26) == 4 and (x & 0x03ff0000) == 0:      # b
            apply(w(a + 4), a + 4, args)
            a += 4 + (s16(a) << 2)
            continue
        elif (x >> 26) == 0x0f and ((x >> 16) & 31) == 4:   # lui a0 -- a log format string
            hi = (x & 0xffff) << 16
            nx = w(a + 4)
            if (nx >> 26) == 9 and ((nx >> 16) & 31) == 4:
                label = img.cstr(hi + s16(a + 4)) or label
        else:
            apply(x, a, args)
        a += 4
    if out:
        n, ar = out[0]
        sig = ', '.join('%s' % ar[k] for k in ('a0', 'a1', 'a2', 'a3') if k in ar)
        print('%2d  %-34s(%s)' % (i, n, sig))
    else:
        print('%2d  -- no call found' % i)
