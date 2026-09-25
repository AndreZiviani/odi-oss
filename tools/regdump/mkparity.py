#!/usr/bin/env python3
"""Build a v6-parity load table from a regdump diff.py capture.

    mkparity.py <diff.txt> [out.table]

Reads a diff.py output file (one "<addr8> <name>: a=<v6> b=<other> xor=<..>"
line per differing register, plus a trailing "N addresses differ" summary
and per-family count lines this tool ignores) and writes a v6-parity load
table: one comment line naming the register plus one data line
"<offset> <value> <mask> <name>" (all hex, no 0x prefix) per surviving
entry, in the diff's own order. This is odi_switch_init_parity()'s loadable
table file format (odi_switch_dal.c/.h, "parity table from a file") --
rootfs/skeleton/etc/scripts/parity-load.sh reads exactly this format from
/var/config/parity.table and
feeds it to the kernel through /proc/odi_omci, one `parity_add <offset>
<value> <mask>` write per data line, comments and blank lines skipped.

Exclusion: an address in one of the EXCLUDED_RANGES below is dropped --
the header/action snapshot debug latches, the thermal sensor, the SerDes
register windows, and PTP/EPON time and state that move on their own, not
boot-time configuration. That judgment call is applied here as a literal,
mechanical rule, by switch-core offset (the names in a diff are mostly
the default SW_0x<address> ones and say nothing), so a future diff can be
turned into a table the same way without re-deriving it by hand each
time. Nothing else is excluded -- unlike odi_switch_dal.c's compiled-in
14-entry default, which also held back the ACL setup words, ACL_PORT_ENABLE,
the three flood masks and CLASSIFY_SETUP as probable OLT provisioning,
this generator's job is
a complete, mechanical translation of one diff.py capture, not a curated
subset -- the "which registers are safe to try" judgment stays with
whoever writes the /var/config/parity.table that gets deployed, not with
this script.

Offset translation: diff.py prints FULL physical addresses (SWITCH_CORE_
BASE + MMIO byte offset, the same convention mklist.py's own address list
uses -- see that script's own docstring). The parity table format needs
the bare MMIO offset odi_reg_read()/_write() and
odi_switch_mmio_offset_in_bounds() take, so SWITCH_CORE_BASE is subtracted
back out for every kept entry.

Value and mask: every surviving entry's value is diff.py's own "a="
column (the FIRST file passed to diff.py -- v6, the working image, for
regdump-v6-vs-s7.txt; whatever the first file was for any other diff this
generator is pointed at) and the mask is always ffffffff (full-word) --
odi_switch_init_parity() reproduces that captured value bit-for-bit,
reserved bits included, the same posture the compiled default entries
already use.
"""
import re
import sys

SWITCH_CORE_BASE = 0x1B000000

# (first, last) switch-core offsets, inclusive, 4-byte words.
EXCLUDED_RANGES = [
    (0x000130, 0x000140), (0x000214, 0x000214), (0x0111d4, 0x0111f4),
    (0x01b000, 0x01b02c), (0x01c0c8, 0x01c0c8), (0x020024, 0x020030),
    (0x021000, 0x021048), (0x021800, 0x02187c), (0x021a00, 0x021a7c),
    (0x021c00, 0x021c20), (0x021c34, 0x021c5c), (0x021c68, 0x021c68),
    (0x021c70, 0x021c7c), (0x021e00, 0x021e20), (0x021e34, 0x021e7c),
    (0x022780, 0x0227d8), (0x028000, 0x028000), (0x028040, 0x0280ac),
    (0x0280c0, 0x02812c), (0x028140, 0x028178), (0x02a070, 0x02a0b4),
    (0x02a100, 0x02a110), (0x02d898, 0x02d898), (0x032904, 0x03290c),
    (0x036000, 0x036020), (0x036034, 0x036034), (0x03609c, 0x036124),
    (0x03612c, 0x036134), (0x036144, 0x036144), (0x03614c, 0x0361b8),
]

DIFF_RE = re.compile(
    r"^([0-9a-fA-F]{8})\s+([^\s:]+):\s+a=([0-9a-fA-F]+)\s+b=([0-9a-fA-F]+)\s+xor=([0-9a-fA-F]+)\s*$"
)


def excluded(addr):
    off = addr - SWITCH_CORE_BASE
    return any(lo <= off <= hi for lo, hi in EXCLUDED_RANGES)


def load_diff(path):
    """Yield (addr, name, value_a) for every differing-register line, in
    file order. Any line that does not match DIFF_RE (the header, the
    trailing "N addresses differ" summary, the per-family count lines) is
    silently skipped -- diff.py's own output format, not this tool's to
    validate.
    """
    with open(path) as f:
        for line in f:
            m = DIFF_RE.match(line)
            if not m:
                continue
            addr = int(m.group(1), 16)
            name = m.group(2)
            value_a = int(m.group(3), 16)
            yield addr, name, value_a


def build_table(diff_path):
    """Return (kept, excluded_count, total_count) -- kept is a list of
    (offset, name, value_a) tuples in diff order, offset already translated
    from a full physical address to a bare MMIO offset.
    """
    kept = []
    excluded_n = 0
    total = 0
    for addr, name, value_a in load_diff(diff_path):
        total += 1
        if excluded(addr):
            excluded_n += 1
            continue
        if addr < SWITCH_CORE_BASE:
            sys.exit("mkparity.py: address 0x%08x below the switch-core base 0x%08x -- "
                      "not a regdump physical address" % (addr, SWITCH_CORE_BASE))
        kept.append((addr - SWITCH_CORE_BASE, name, value_a))
    return kept, excluded_n, total


def render(kept, diff_path, excluded_n):
    lines = []
    lines.append("# v6 parity load table -- generated by mkparity.py from %s" % diff_path)
    lines.append("# odi_switch_init_parity()'s loadable table format (odi_switch_dal.c/.h,")
    lines.append("# \"parity table from a file\"):")
    lines.append("# one comment line naming the register, then one data line")
    lines.append("# \"offset value mask name\" (hex, no 0x prefix), in the diff's own order.")
    lines.append(
        "# %d entries (%d excluded: snapshot latches, thermal, SerDes windows, "
        "PTP and EPON state that move on their own)" % (len(kept), excluded_n)
    )
    for off, name, value_a in kept:
        lines.append("# %s" % name)
        lines.append("%08x %08x ffffffff %s" % (off, value_a, name))
    return "\n".join(lines) + "\n"


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    diff_path = argv[1]
    out_path = argv[2] if len(argv) > 2 else None

    kept, excluded_n, total = build_table(diff_path)
    if total == 0:
        sys.exit("mkparity.py: no diff.py-shaped lines found in %s -- wrong file?" % diff_path)
    text = render(kept, diff_path, excluded_n)

    if out_path:
        with open(out_path, "w") as f:
            f.write(text)
        sys.stderr.write(
            "%d entries written to %s (%d excluded, %d total)\n"
            % (len(kept), out_path, excluded_n, total)
        )
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main(sys.argv)
