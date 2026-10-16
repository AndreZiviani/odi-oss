#!/usr/bin/env python3
"""Default attribute values for the entities the ONU itself creates, from a
live `omcicli mib get all` of the vendor stack on the same device.

    mib-defaults.py <dump> [--c DIR]

Until this existed omcid answered ZEROS for every attribute of its 301
autonomous instances that the OLT had not written -- so an ONT2-G with 0
priority queues, 0 GEM ports and no connectivity capability, an ANI-G with 0
T-CONTs. ISP1's OLT provisioned management only against that and never the
unicast service. The dump labels each attribute with
the plugin's own descriptor name, the same names generated/omci_mib.json
carries, so matching is by name; values are decimal, 0x-hex, or text.

Only autonomous instances (generated/omci_autonomous.c) are taken, and never
classes 2 (OntData), 148, 157, 340, 134 (OLT-written credentials), 65530
(LOID: from the config store), 131 (OLT-G: what the OLT wrote about itself),
ONT-G attributes 1-3 and 14 (the live identity and PON state, mibstore
answers them), the circuit pack serial number, version and vendor id (6/3-5,
the configured serial and the active image), the private VLAN config's manual
VID (10212/3, one ISP's VLAN), the T-CONT Alloc-IDs (262/1: what one OLT
assigned that line by PLOAM, which omcid binds from the kernel at run time,
respond/apply_qos.c), and every attribute omcid answers from live state
(ONU2-G uptime, ANI-G optical levels, UNI link) or always overrides
(SKIP_ATTRS below), so the table carries no identity, no measurement and no
provisioning, only capability values. The dump itself is private."""
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, '..', 'generated')
# 7: software image, every attribute is answered from the image flags.
SKIP_CLASSES = {2, 7, 131, 148, 157, 340, 134, 65530}
SKIP_ATTRS = {
    (256, 1), (256, 2), (256, 3),   # ONU-G identity, from the serial
    (256, 14),                      # ONU-G OntState, the live PON state
    (6, 3), (6, 4), (6, 5),         # circuit pack serial, version, vendor id
    (257, 1), (257, 2),             # ONU2-G EqtID and OMCC version, always overridden
    (257, 10),                      # ONU2-G SysUpTime, live
    (263, 10), (263, 14),           # ANI-G optical levels, live from the DDM
    (11, 2), (11, 7),               # PPTP Ethernet UNI sensed type, duplex: link state
    (10212, 3),                     # one ISP's VLAN
    (262, 1),                       # T-CONT Alloc-ID, bound at run time
    (277, 1), (277, 2), (277, 3), (277, 6), (277, 8),   # always overridden
    (277, 13),                      # reserved queues hold ASCII noise, see below
    (277, 14), (277, 15),           # always overridden
    (278, 1), (278, 3),             # traffic scheduler, always overridden
}
# An attribute the OLT can write (OltAcc write or set-by-create) holds, in a
# dump taken from a provisioned stick, whatever that stick's OLT wrote. So no
# writable attribute is taken from the dump, except the ones listed here: the
# values the ONU initialises itself, each checked against the vendor stack's
# own initialiser (reference reading) or the G.988 default. Everything else
# writable is served from its power-on default by omcid (zero unless
# mibstore.c says otherwise).
INIT_VERIFIED = {
    (5, 2), (5, 3),                 # cardholder: expected type and ports, from the slot
    (6, 1),                         # circuit pack type, from the slot
    (11, 1), (11, 8), (11, 10), (11, 11),   # UNI: expected type, frame size, pause time, bridged
    (242, 1), (242, 2),             # default policy, 0xff pointer
    (256, 4),                       # traffic management option
    (257, 5), (257, 14),            # security mode (G.988 default 1), queue scale factor
    (263, 3), (263, 6), (263, 7), (263, 11), (263, 12), (263, 15), (263, 16),
}
# Vendor-private entities the ONU creates itself, taken whole: omcid gates
# and answers them by key (see docs/SETTINGS.md).
INIT_CLASSES = {350, 370, 373}
MAX_W = 32

def autonomous():
    ids = set()
    for l in open(os.path.join(GEN, 'omci_autonomous.c')):
        m = re.match(r'\s*\{\s*(\d+),\s*0x([0-9a-fA-F]+)\s*\}', l)
        if m: ids.add((int(m.group(1)), int(m.group(2), 16)))
    return ids

def parse(path):
    lines = open(path, errors='replace').read().splitlines()
    tables, cur, inst, rows = {}, None, None, None
    for i, l in enumerate(lines):
        if l.startswith('XXXXXXXX'):
            nxt = lines[i + 1].strip() if i + 1 < len(lines) else ''
            if re.match(r'^[A-Za-z][A-Za-z0-9 _.]*$', nxt):
                cur = nxt; tables.setdefault(cur, [])
            continue
        m = re.match(r'\s*([A-Za-z][A-Za-z0-9_]*)\s*:\s*(.*)$', l)
        if not m or not cur: continue
        k, v = m.group(1), m.group(2).strip()
        if k.lower() == 'entityid':
            rows = {}
            try: eid = int(v, 16)          # the dump prints entity ids in hex, 0x or bare
            except ValueError: eid = 0
            tables[cur].append((eid, rows))
        elif rows is not None:
            rows[k.lower()] = v
    return tables

def encode(v, typ, width):
    if not width or width > MAX_W or not typ: return None
    if typ.startswith('u'):
        # A 0x value is hex digits and nothing else; anything else is not a
        # number and is refused rather than guessed at.
        if v.lower().startswith('0x') and not re.fullmatch(r'0[xX][0-9a-fA-F]+', v):
            return None
        try: n = int(v, 0) if v else 0
        except ValueError: return None
        if n < 0 or n >= (1 << (8 * width)): return None
        return [(n >> (8 * (width - 1 - i))) & 0xff for i in range(width)]
    if typ == 'string':
        if v.lower().startswith('0x'):
            h = v[2:]
            if len(h) % 2: h = '0' + h
            b = [int(h[i:i + 2], 16) for i in range(0, len(h), 2)]
        else:
            b = list(v.encode('latin-1'))
        b = (b + [0] * width)[:width]
        return b
    return None

def main():
    if len(sys.argv) < 2: print(__doc__); sys.exit(1)
    model = json.load(open(os.path.join(GEN, 'omci_mib.json')))
    byname = {e['table']['name'].lower(): e for e in model if e['table']['classId'] is not None}
    auto = autonomous(); tables = parse(sys.argv[1])
    out = []; skipped_tables = []
    for tname, insts in tables.items():
        e = byname.get(tname.lower())
        if not e: skipped_tables.append(tname); continue
        cls = e['table']['classId']
        if cls in SKIP_CLASSES: continue
        attrs = e['attrs']
        for inst, vals in insts:
            if (cls, inst) not in auto: continue
            for a in attrs:
                k = a['index']
                if k == 0 or (cls, k) in SKIP_ATTRS: continue
                if ((a['oltAcc'] or 0) & 6) and (cls, k) not in INIT_VERIFIED and cls not in INIT_CLASSES:
                    continue
                v = vals.get(a['name'].lower())
                if v is None: continue
                b = encode(v, a['type'], a['len'])
                if b is None or not any(b): continue
                out.append((cls, inst, k, a['len'], b, a['name']))
    out.sort()
    outdir = GEN
    for i, arg in enumerate(sys.argv):
        if arg == '--c' and i + 1 < len(sys.argv): outdir = sys.argv[i + 1]
    with open(os.path.join(outdir, 'omci_defaults.c'), 'w') as fh:
        fh.write('/* GENERATED by tools/mib-defaults.py from a vendor `omcicli mib get all`\n'
                 ' * on the same device -- do not edit. Non-zero attribute values of the\n'
                 ' * ONU-created entities; identity and credentials excluded (see the tool). */\n'
                 '#include "omci_defaults.h"\n\nconst struct omci_default omci_defaults[] = {\n')
        for cls, inst, k, w, b, name in out:
            fh.write('\t{ %d, 0x%04x, %d, %d, { %s } },\t/* %s */\n' % (cls, inst, k, w, ', '.join('0x%02x' % x for x in b), name))
        fh.write('};\nconst unsigned omci_defaults_count = %d;\n' % len(out))
    with open(os.path.join(outdir, 'omci_defaults.h'), 'w') as fh:
        fh.write('/* GENERATED by tools/mib-defaults.py -- do not edit. */\n#ifndef ODI_OMCI_DEFAULTS_H\n#define ODI_OMCI_DEFAULTS_H\n#include <stdint.h>\n'
                 'struct omci_default { uint16_t cls, inst; uint8_t attr, width; uint8_t data[%d]; };\n'
                 'extern const struct omci_default omci_defaults[];\nextern const unsigned omci_defaults_count;\n#endif\n' % MAX_W)
    sys.stderr.write('%d default values from %d tables; tables not in the model: %s\n' % (len(out), len(tables), ', '.join(skipped_tables)))

if __name__ == '__main__': main()
