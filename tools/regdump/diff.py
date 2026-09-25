#!/usr/bin/env python3
"""Diff two regdump captures against the mklist.py address list.

    diff.py <namelist.txt> <dump-a.txt> <dump-b.txt>

namelist.txt is the output of mklist.py (comment lines start with "#",
every other line "<addr_hex> <name>", "|"-joined when mklist.py found more
than one register at that address -- see its own docstring). dump-a.txt
and dump-b.txt are the output of dump.sh run against that same list on two
boots: one line per address in the format memprobe already prints,
"<addr_hex> = <value_hex>", in whatever order dump.sh visited them (list
order, so normally address order already, but this does not assume that).

Prints one line per address where the two values differ, name(s) from
the list, both values and their XOR, sorted by address:

    <addr_hex> <name>: a=<val_a> b=<val_b> xor=<val_a^val_b>

then a blank line and a count of differing addresses per register family
(the token before the first "_" in each name; a "|"-joined collision name
counts once per distinct family it touches, since which of the two
logical registers actually changed cannot be told apart from the value
alone).

An address the namelist defines but a capture is missing (a dump.sh run
that stopped early, or was made against a different list) is reported
separately, not silently dropped from the family counts -- a stall
mid-capture is exactly the failure this needs to be visible.
"""
import sys
from collections import OrderedDict


def load_names(path):
    """Return {addr:int -> name:str} in the line order of namelist.txt."""
    names = OrderedDict()
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(None, 1)
            addr = int(parts[0], 16)
            name = parts[1] if len(parts) > 1 else "UNKNOWN"
            names[addr] = name
    return names


def load_dump(path):
    """Return {addr:int -> value:int} from a dump.sh capture.

    Tolerant of blank lines and a leading "#" header line, so a capture can
    be hand-annotated before diffing without breaking the parse. A
    malformed data line is reported, not silently skipped -- a truncated
    capture (the stick rebooted mid-dump) must not look like a clean
    subset.
    """
    values = OrderedDict()
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) != 3 or parts[1] != "=":
                sys.exit(
                    "%s:%d: malformed dump line, expected addr = val format: %r"
                    % (path, lineno, line)
                )
            addr = int(parts[0], 16)
            val = int(parts[2], 16)
            values[addr] = val
    return values


def family(name):
    """Every family token in name (split on | for a collision, then on
    the first underscore of each side)."""
    fams = set()
    for sub in name.split("|"):
        fams.add(sub.split("_", 1)[0])
    return fams


def main(argv):
    if len(argv) != 4:
        sys.exit(__doc__)
    names = load_names(argv[1])
    dump_a = load_dump(argv[2])
    dump_b = load_dump(argv[3])

    missing_a = [a for a in names if a not in dump_a]
    missing_b = [a for a in names if a not in dump_b]
    if missing_a:
        print("missing from %s: %d addresses (capture stopped early?)" % (argv[2], len(missing_a)))
        for a in missing_a[:10]:
            print("    %08x %s" % (a, names[a]))
        if len(missing_a) > 10:
            print("    ... and %d more" % (len(missing_a) - 10))
    if missing_b:
        print("missing from %s: %d addresses (capture stopped early?)" % (argv[3], len(missing_b)))
        for a in missing_b[:10]:
            print("    %08x %s" % (a, names[a]))
        if len(missing_b) > 10:
            print("    ... and %d more" % (len(missing_b) - 10))

    diffs = []
    for addr in names:
        if addr not in dump_a or addr not in dump_b:
            continue
        va, vb = dump_a[addr], dump_b[addr]
        if va != vb:
            diffs.append(addr)
    diffs.sort()

    for addr in diffs:
        va, vb = dump_a[addr], dump_b[addr]
        print("%08x %s: a=%08x b=%08x xor=%08x" % (addr, names[addr], va, vb, va ^ vb))

    fam_counts = OrderedDict()
    for addr in diffs:
        for fam in sorted(family(names[addr])):
            fam_counts[fam] = fam_counts.get(fam, 0) + 1

    print()
    print("%d addresses differ" % len(diffs))
    for fam, n in sorted(fam_counts.items(), key=lambda kv: (-kv[1], kv[0])):
        print("    %6d  %s" % (n, fam))

    return 1 if (missing_a or missing_b) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
