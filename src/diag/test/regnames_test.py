#!/usr/bin/env python3
"""Host test for the naming half of tools/regmap-extract.py.

Needs no firmware binary: the table here is synthetic, shaped like what the
extractor reads out of librtk.so. Checks the default scheme, that curated
names are refused when they do not fit the table, that tools/regnames.txt
parses.

The names serve the register listings tools/regtrace and tools/regdump read
and the kernel register headers (test/regnames_kernel_test.py at the top of
the repository); diag itself takes addresses, never names.
"""
import importlib.util
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
spec = importlib.util.spec_from_file_location(
    'regmap_extract', os.path.join(TOP, 'tools', 'regmap-extract.py'))
rx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rx)

failures = 0


def check(what, got, want):
    global failures
    if got == want:
        print('ok    %s' % what)
    else:
        print('FAIL  %s: got %r want %r' % (what, got, want))
        failures += 1


def refuses(what, fn):
    global failures
    try:
        fn()
    except SystemExit:
        print('ok    %s' % what)
        return
    print('FAIL  %s: accepted' % what)
    failures += 1


def table():
    """Two registers: one curated below, one left to the defaults."""
    fields = [(0, 16, 16, None), (7, 8, 8, None), (8, 0, 1, None),
              (9, 0, 32, None)]
    regs = [dict(off=0x701010, fbase=0, nfld=3),
            dict(off=0x023038, fbase=3, nfld=1)]
    return regs, fields


def names_file(text):
    f = tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False)
    f.write(text)
    f.close()
    return f.name


BLOCKS = 'block 0x000000 0x6fffff SW\nblock 0x700000 0x7fffff GPON\n'

# Defaults.
p = names_file(BLOCKS)
blocks, curated = rx.load_regnames(p)
regs, fields = table()
rx.apply_regnames(regs, fields, blocks, curated)
check('default register name, GPON block', regs[0]['name'], 'GPON_0x701010')
check('default register name, SW block', regs[1]['name'], 'SW_0x023038')
check('reserved field', fields[0][3], 'RSVD_31_16')
check('multi-bit field', fields[1][3], 'F_15_8')
check('one-bit field', fields[2][3], 'F_0')
check('whole-word field', fields[3][3], 'F_31_0')
check('address in no block', rx.default_reg_name(blocks, 0x800100),
      'R_0x800100')

# Curated names win, by address and bit range.
p = names_file(BLOCKS + 'reg 0x701010 DSF_X 15:8=ONU 0:0=BIT\n')
blocks, curated = rx.load_regnames(p)
regs, fields = table()
rx.apply_regnames(regs, fields, blocks, curated)
check('curated register name', regs[0]['name'], 'DSF_X')
check('curated field name', fields[1][3], 'ONU')
check('curated one-bit field', fields[2][3], 'BIT')
check('uncurated register unchanged', regs[1]['name'], 'SW_0x023038')


def apply(text):
    b, c = rx.load_regnames(names_file(BLOCKS + text))
    r, f = table()
    rx.apply_regnames(r, f, b, c)


refuses('an address the table does not have', lambda: apply('reg 0x701014 A\n'))
refuses('a bit range that is not a field', lambda: apply('reg 0x701010 A 9:8=B\n'))
refuses('naming a reserved field', lambda: apply('reg 0x701010 A 31:16=B\n'))
refuses('one name for two registers',
        lambda: apply('reg 0x701010 A\nreg 0x023038 A\n'))
refuses('one address named twice',
        lambda: apply('reg 0x701010 A\nreg 0x701010 B\n'))
refuses('a field name used twice in a register',
        lambda: apply('reg 0x701010 A 15:8=B 0:0=B\n'))
refuses('a lowercase name', lambda: apply('reg 0x701010 abc\n'))
refuses('a default name taken by a curated one',
        lambda: apply('reg 0x701010 SW_0x023038\n'))

# The real file parses.
blocks, curated = rx.load_regnames(os.path.join(TOP, 'tools', 'regnames.txt'))
check('regnames.txt parses', len(curated) > 0, True)

print('FAILED' if failures else 'all ok')
sys.exit(1 if failures else 0)
