#!/usr/bin/env python3
"""Find who references a string in a section-header-stripped MIPS ELF, and
which imported function is called right after it.

Works off PT_LOAD program headers and the .dynamic table, so a stripped .so or
executable is fine. Same approach as tools/omci-drv-api.py, narrowed to one
question.
"""
import struct, sys

path, needle = sys.argv[1], sys.argv[2].encode()
d = open(path, 'rb').read()
assert d[:4] == b'\x7fELF' and d[5] == 2, 'expect big-endian ELF32'
phoff, = struct.unpack_from('>I', d, 28)
phentsize, phnum = struct.unpack_from('>HH', d, 42)
loads, dynoff = [], None
for i in range(phnum):
    o = phoff + i * phentsize
    typ, off, va, _, filesz, memsz, _, _ = struct.unpack_from('>8I', d, o)
    if typ == 1:
        loads.append((va, filesz, off))
    elif typ == 2:
        dynoff = off

def f2v(o):
    for va, sz, fo in loads:
        if fo <= o < fo + sz:
            return va + (o - fo)
    return None

def v2f(a):
    for va, sz, fo in loads:
        if va <= a < va + sz:
            return fo + (a - va)
    return None

# dynamic table -> symtab, strtab, GOT, and the local-GOT count
dyn = {}
o = dynoff
while True:
    tag, val = struct.unpack_from('>iI', d, o)
    if tag == 0:
        break
    dyn.setdefault(tag, val)
    o += 8
DT_STRTAB, DT_SYMTAB, DT_SYMENT = 5, 6, 11
DT_PLTGOT, DT_MIPS_LOCAL_GOTNO, DT_MIPS_GOTSYM = 3, 0x7000000a, 0x70000013
strtab = v2f(dyn[DT_STRTAB]); symtab = v2f(dyn[DT_SYMTAB])
syment = dyn.get(DT_SYMENT, 16)
got = dyn.get(DT_PLTGOT); localno = dyn.get(DT_MIPS_LOCAL_GOTNO)
gotsym = dyn.get(DT_MIPS_GOTSYM)

DT_HASH = 4
nsyms = None
if DT_HASH in dyn:
    h = v2f(dyn[DT_HASH])
    _, nchain = struct.unpack_from('>2I', d, h)
    nsyms = nchain

def symname(i):
    if nsyms is not None and i >= nsyms:
        return None
    n, = struct.unpack_from('>I', d, symtab + i * syment)
    e = d.find(b'\0', strtab + n)
    if e < 0:
        return None
    return d[strtab + n:e].decode(errors='replace')

gotmap = {}
if got is not None and localno is not None and gotsym is not None:
    # entry k (k >= localno) resolves symbol gotsym + (k - localno)
    k = localno
    while True:
        a = got + k * 4
        if v2f(a) is None:
            break
        nm = symname(gotsym + (k - localno))
        if nm is None:
            break
        gotmap[a] = nm
        k += 1
        if k - localno > 4000:
            break

# where does the string live?
hits = [f2v(i) for i in range(len(d)) if d.startswith(needle, i)]
hits = [h for h in hits if h is not None]
print('string %r at %s' % (needle.decode(), [hex(h) for h in hits]))

for va, sz, fo in loads:
    gp = None
    for o in range(fo, fo + sz - 4, 4):
        x, = struct.unpack_from('>I', d, o)
        a = va + (o - fo)
        # track the gp the compiler set up
        if (x >> 16) == 0x3c1c:
            hi = (x & 0xffff) << 16
        elif (x >> 16) == 0x279c:
            lo = x & 0xffff
            gp = hi + (lo - 0x10000 if lo > 0x7fff else lo) + a - 4
        # lui reg, hi ; addiu reg, reg, lo  ->  a string address
        if (x >> 26) == 15:                      # lui
            rt = (x >> 16) & 31
            hi2 = (x & 0xffff) << 16
            for k in range(o + 4, min(o + 40, fo + sz - 4), 4):
                y, = struct.unpack_from('>I', d, k)
                if (y >> 26) == 9 and ((y >> 21) & 31) == rt:   # addiu
                    l = y & 0xffff
                    tgt = hi2 + (l - 0x10000 if l > 0x7fff else l)
                    if tgt in hits:
                        # first jalr after this, and the gp load feeding it
                        callee = '?'
                        for m in range(k, min(k + 48, fo + sz - 4), 4):
                            z, = struct.unpack_from('>I', d, m)
                            if (z >> 26) == 35 and ((z >> 21) & 31) == 28 \
                               and ((z >> 16) & 31) == 25 and gp:
                                off = z & 0xffff
                                off = off - 0x10000 if off > 0x7fff else off
                                callee = gotmap.get(gp + off, hex(gp + off))
                            if z == 0x0320f809:          # jalr t9
                                print('  %#x  loads the string, then calls %s'
                                      % (va + (o - fo), callee))
                                break
                    break

# The pic case: the string address lives in a LOCAL GOT entry and is loaded
# with `lw reg, off(gp)`, sometimes plus an addiu for a field inside it.
print('--- gp-relative ---')
slots = []
if got is not None and localno is not None:
    for k in range(localno):
        a = got + k * 4
        fo2 = v2f(a)
        if fo2 is None:
            continue
        v, = struct.unpack_from('>I', d, fo2)
        for h in hits:
            if v <= h < v + 0x10000:
                slots.append((a, v))
print('  candidate GOT slots:', [(hex(a), hex(v)) for a, v in slots])

for va, sz, fo in loads:
    gp = None
    for o in range(fo, fo + sz - 4, 4):
        x, = struct.unpack_from('>I', d, o)
        a = va + (o - fo)
        if (x >> 16) == 0x3c1c:
            hi = (x & 0xffff) << 16
        elif (x >> 16) == 0x279c:
            lo = x & 0xffff
            gp = hi + (lo - 0x10000 if lo > 0x7fff else lo) + a - 4
        if (x >> 26) == 35 and ((x >> 21) & 31) == 28 and gp:   # lw r, off(gp)
            off = x & 0xffff
            off = off - 0x10000 if off > 0x7fff else off
            slot = gp + off
            base = dict(slots).get(slot)
            if base is None:
                continue
            # the string is reached as base + K by a following addiu; only
            # that one is our reference.
            rt = (x >> 16) & 31
            wanted = None
            for m in range(o + 4, min(o + 32, fo + sz - 4), 4):
                y, = struct.unpack_from('>I', d, m)
                if (y >> 26) == 9 and ((y >> 21) & 31) == rt:
                    K = y & 0xffff
                    K = K - 0x10000 if K > 0x7fff else K
                    if base + K in hits:
                        wanted = base + K
                    break
            if wanted is None:
                continue
            callee = '?'
            for m in range(o, min(o + 64, fo + sz - 4), 4):
                z, = struct.unpack_from('>I', d, m)
                if (z >> 26) == 35 and ((z >> 21) & 31) == 28 \
                   and ((z >> 16) & 31) == 25:
                    off2 = z & 0xffff
                    off2 = off2 - 0x10000 if off2 > 0x7fff else off2
                    callee = gotmap.get(gp + off2, hex(gp + off2))
                if z == 0x0320f809:
                    print('  %#x  loads %#x, then calls %s'
                          % (a, wanted, callee))
                    break
