"""Which driver commands each managed entity reaches.

Every plugin in /lib/omci implements its own behaviour behind a four-entry
vtable -- DrvCfg, ConnCfg, ConnCheck, DumpMib -- and the hardware it touches it
touches through libomci_mib's omci_wrapper_* calls, each of which is one
`omci_drv_call(cmd, ...)` on socket option 0x310a. So the map from a managed
entity to the hardware is the set of wrappers its plugin imports, and that is
readable straight out of the dynamic symbol table.

This is the specification for the part of a replacement that actually carries
traffic: when the OLT creates ME 268, these are the commands that have to be
issued.

    me-drv-map.py [/lib/omci] [librtk-style libomci_mib.so]
"""
import json, os, re, subprocess, sys, glob

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'diag', 'tools'))
from rtkelf import Image


def drv_commands():
    """wrapper name -> (cmd, payload length), from omci-drv-api.py."""
    # check=True, and an empty map is fatal. Without both, a child that cannot
    # find the rootfs or dies on an exception returns no output, every lookup
    # below misses, and this tool reports "0 of 81 managed entities reach the
    # driver directly" -- which reads as a finding rather than as a failure.
    # This tool's count gets quoted in commit messages and reports, so a
    # confident zero is worse than a traceback.
    r = subprocess.run([sys.executable, os.path.join(HERE, 'omci-drv-api.py')],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('omci-drv-api.py failed (%d):\n%s' % (r.returncode, r.stderr.strip()))
    out = r.stdout
    m = {}
    for line in out.splitlines():
        g = re.match(r'\s*cmd (\S+)\s+len (\S+)\s+(.*)', line)
        if not g or g.group(1) == 'None' or g.group(2) == 'None':
            continue
        for name in g.group(3).split(', '):
            m[name.strip()] = (int(g.group(1)), int(g.group(2)))
    if not m:
        sys.exit('omci-drv-api.py produced no command map; its output format '
                 'has probably changed. Refusing to report a zero.')
    return m


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/tmp/odi/rootfs/lib/omci')
    cmds = drv_commands()
    with open(os.path.join(HERE, '..', 'generated', 'omci_mib.json')) as fh:
        model = {e['plugin']: e['table'] for e in json.load(fh)}

    rows = []
    for path in sorted(glob.glob(os.path.join(root, 'mib_*.so'))):
        base = os.path.basename(path)
        img = Image(path)
        used = set()
        for a, n in img.any_func.items():
            if a in img.funcs:
                continue
            if n in cmds:
                used.add(n)
        t = model.get(base, {})
        rows.append((t.get('classId'), t.get('name', base), sorted(used)))

    rows.sort(key=lambda r: (r[0] is None, r[0]))
    touch = [r for r in rows if r[2]]
    print('%d of %d managed entities reach the driver directly\n' % (len(touch), len(rows)))
    for c, name, used in touch:
        print('%-6s %-26s' % (c, name))
        for w in used:
            cmd, ln = cmds[w]
            print('        cmd %-3d len %-4d %s' % (cmd, ln, w.replace('omci_wrapper_', '')))
    quiet = [r[1] for r in rows if not r[2]]
    print('\npure MIB, no driver call: %d -- %s' % (len(quiet), ', '.join(quiet[:12]) +
          (' ...' if len(quiet) > 12 else '')))


if __name__ == '__main__':
    main()
