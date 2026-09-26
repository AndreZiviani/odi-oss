#!/usr/bin/env python3
"""Hold the kernel register headers to src/diag/tools/regnames.txt.

odi_gpon_hw.h and odi_switch_hw.h name registers and fields the same way
diag does: a curated name from regnames.txt, or the mechanical default
(<block>_0x<address>, F_<msb>_<lsb>). This test reads both headers and
fails when a name in them is anything else:

  - every address macro (<prefix><REG>_OFF, <prefix><REG>_BASE) must name
    the register regnames.txt gives that address;
  - every register comment tag ("/* REG: 0x...") must agree with it;
  - every field accessor (<prefix><REG>_<FIELD>_GET/_SET) and one-bit mask
    (<prefix><REG>_<FIELD>, (1U << n)) must use the field name
    regnames.txt gives that bit range, or -- for a slice of a named field
    -- that name plus a suffix, or the F_ default when the range is not
    curated at all;
  - any other macro must be on the short list of constants below, so a
    new register-looking name cannot slip in beside the checked ones.

Needs no firmware image and no compiler.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOP = os.path.dirname(HERE)
DIAG = os.path.join(TOP, 'src', 'diag')
ODI = os.path.join(TOP, 'kernel', 'extra', 'drivers', 'net', 'ethernet', 'odi')

spec = importlib.util.spec_from_file_location(
    'regmap_extract', os.path.join(DIAG, 'tools', 'regmap-extract.py'))
rx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rx)

HEADERS = [
    ('odi_gpon_hw.h', 'ODI_GPON_'),
    ('odi_switch_hw.h', 'ODI_SW_'),
]

# Constants that are not register or field names: counts, slot numbers,
# the MMIO window, the table ids and descriptors.
OTHER = {
    'ODI_GPON_HW_H', 'ODI_SWITCH_HW_H', 'ODI_SWITCH_MMIO_BASE',
    'ODI_SWITCH_MMIO_SIZE', 'ODI_GPON_DS_PLOAM_WORDS', 'ODI_GPON_US_PLOAM_WORDS',
    'ODI_GPON_PLOAM_TYPE_REPLY', 'ODI_GPON_DSK_KEY_WORDS',
    'ODI_GPON_DSK_SLOT_CURRENT', 'ODI_GPON_DSK_SLOT_NEXT',
    'ODI_SW_TABLE_DESC_INIT', 'ODI_SW_CHIP_IRQ_SOURCES',
}
OTHER_RE = re.compile(r'^ODI_SW_TBL_[A-Z0-9_]+$')   # enum odi_sw_table

TAG_RE = re.compile(r'^/\* ([A-Z][A-Z0-9_]*): (0x[0-9a-f]{6})\b', re.M)
ADDR_RE = re.compile(r'^#define\s+(\w+)_(OFF|BASE)\s+(0x[0-9a-fA-F]+)U\b', re.M)
ARRAY_RE = re.compile(r'^#define\s+(\w+)\((?:port, )?n\)', re.M)
MASK_RE = re.compile(r'^#define\s+(\w+)\s+\(1U << (\d+)\)', re.M)
FUNC_RE = re.compile(
    r'static inline uint32_t (\w+)_(GET|SET)\(uint32_t reg[^)]*\)\n\{\n'
    r'(?:\tuint32_t sh = (\d+) \+ item \* \d+U;\n)?'
    r'\treturn \(reg (?:>> \(?(\d+)[^)]*\)\)?|& ~\((0x[0-9a-f]+)U << (?:(\d+)|sh)\)\))'
    r'(?: & (0x[0-9a-f]+)U)?')
DEFINE_RE = re.compile(r'^#define\s+(\w+)', re.M)


def field_range(m):
    """(msb, lsb) of one accessor, from its shift and mask."""
    if m.group(5):                        # a _SET: ~(mask << lsb)
        mask = int(m.group(5), 16)
        lsb = int(m.group(6) if m.group(6) is not None else m.group(3))
    else:                                 # a _GET: (reg >> lsb) & mask
        lsb = int(m.group(4))
        mask = int(m.group(7), 16)
    return lsb + mask.bit_length() - 1, lsb


def check_header(text, prefix, blocks, curated, report):
    def want_reg(addr):
        return curated[addr][0] if addr in curated else rx.default_reg_name(blocks, addr)

    regs = {}                             # register name -> address
    for stem, _kind, val in ADDR_RE.findall(text):
        if not stem.startswith(prefix):
            continue
        name, addr = stem[len(prefix):], int(val, 16)
        if name != want_reg(addr):
            report('%s%s: 0x%06x is %s in regnames.txt' % (prefix, name, addr, want_reg(addr)))
        regs[name] = addr

    for name, val in TAG_RE.findall(text):
        addr = int(val, 16)
        if name != want_reg(addr):
            report('comment tag %s: 0x%06x is %s in regnames.txt' % (name, addr, want_reg(addr)))

    def split(ident):
        """(register, field) for an identifier, by the longest register
        name it starts with."""
        body = ident[len(prefix):]
        best = None
        for r in regs:
            if body.startswith(r + '_') and (best is None or len(r) > len(best)):
                best = r
        return best, (body[len(best) + 1:] if best else None)

    def check_field(ident, msb, lsb):
        reg, field = split(ident)
        if reg is None:
            report('%s: no register in this header it belongs to' % ident)
            return
        names = curated.get(regs[reg], (None, {}))[1]
        if (msb, lsb) in names:
            if field != names[(msb, lsb)]:
                report('%s: bits %d:%d of %s are %s in regnames.txt'
                       % (ident, msb, lsb, reg, names[(msb, lsb)]))
            return
        for (hi, lo), parent in names.items():
            if lo <= lsb and msb <= hi:
                if not field.startswith(parent + '_'):
                    report('%s: bits %d:%d are inside %s.%s; name it %s_<what>'
                           % (ident, msb, lsb, reg, parent, parent))
                return
        default = 'F_%d' % lsb if msb == lsb else 'F_%d_%d' % (msb, lsb)
        if field != default:
            report('%s: bits %d:%d of %s are not named in regnames.txt; '
                   'name them there, or use %s' % (ident, msb, lsb, reg, default))

    checked = set(p + '_' + k for p, k, _ in ADDR_RE.findall(text))
    for m in FUNC_RE.finditer(text):
        msb, lsb = field_range(m)
        check_field(m.group(1), msb, lsb)
    funcs = set(re.findall(r'static inline uint32_t (\w+)_(?:GET|SET)\(', text))
    parsed = set(m.group(1) for m in FUNC_RE.finditer(text))
    for f in sorted(funcs - parsed):
        report('%s: accessor body not in the shape this test reads' % f)
    for ident, bit in MASK_RE.findall(text):
        check_field(ident, int(bit), int(bit))
        checked.add(ident)
    for ident in ARRAY_RE.findall(text):
        if ident[len(prefix):] not in regs:
            report('%s(n): no _BASE/_OFF for it in this header' % ident)
        checked.add(ident)

    for ident in DEFINE_RE.findall(text):
        if ident in checked or ident in OTHER or OTHER_RE.match(ident):
            continue
        report('%s: not a register, field or listed constant' % ident)


def run(text, prefix, blocks, curated):
    problems = []
    check_header(text, prefix, blocks, curated, problems.append)
    return problems


failures = 0


def expect(what, cond):
    global failures
    print(('ok    ' if cond else 'FAIL  ') + what)
    if not cond:
        failures += 1


blocks, curated = rx.load_regnames(os.path.join(DIAG, 'tools', 'regnames.txt'))

# The checker itself, against headers written to break it.
good = ('#define ODI_GPON_DSF_ONU_STATE_OFF\t0x701010U\n'
        'static inline uint32_t ODI_GPON_DSF_ONU_STATE_ASSIGNED_ONU_ID_SET(uint32_t reg, uint32_t val)\n'
        '{\n\treturn (reg & ~(0xffU << 8)) | ((val & 0xffU) << 8);\n}\n')
expect('a curated register and field pass', run(good, 'ODI_GPON_', blocks, curated) == [])
expect('a register under another name fails',
       run(good.replace('DSF_ONU_STATE_OFF', 'GTC_ONU_OFF'), 'ODI_GPON_', blocks, curated) != [])
expect('a field under another name fails',
       run(good.replace('ASSIGNED_ONU_ID', 'ONUID'), 'ODI_GPON_', blocks, curated) != [])
expect('an uncurated register must use the default name',
       run('#define ODI_SW_WHATEVER_OFF\t0x23000U\n', 'ODI_SW_', blocks, curated) != []
       and run('#define ODI_SW_SW_0x023000_OFF\t0x23000U\n', 'ODI_SW_', blocks, curated) == [])
expect('an unlisted constant fails',
       run('#define ODI_SW_SOMETHING\t3U\n', 'ODI_SW_', blocks, curated) != [])

# The real headers.
for fn, prefix in HEADERS:
    problems = run(open(os.path.join(ODI, fn)).read(), prefix, blocks, curated)
    for p in problems:
        print('      ' + p)
    expect('%s uses only regnames.txt names' % fn, not problems)

print('FAILED' if failures else 'all ok')
sys.exit(1 if failures else 0)
