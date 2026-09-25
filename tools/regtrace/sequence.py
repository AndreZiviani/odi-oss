#!/usr/bin/env python3
"""Turn a decoded trace phase into a compact replay script.

    sequence.py <trace.txt> --phase "<label>" [--rw <trace-with-reads.txt>]
    sequence.py <trace.txt> --phase "<label>" --summary

Reads the RAW trace (not decode.py output), selects the entries between the
marker whose label equals <label> and the next marker, and prints a replay
script: table-fill loops (TABLE_WRITE_WORD x5 then TABLE_CMD) become
one "t" line each, plain writes stay "w" lines in their original position.

No poll line is emitted (see to_script's docstring: the isp1 reads-included
baseline capture showed the recorded CTRL write never carries the busy bit
the original design assumed, and the read that follows it is one-shot, not
a spin loop -- there was no evidence to place a wait from). --rw is still
accepted, in case a later phase own trace shows real polling that a future
revision should place from it.

Line kinds:
    w <addr> <val>
    t <tbl_type> <addr-index> <d0> <d1> <d2> <d3> <d4> <ctrl-raw>
    # comment
"""
import sys

CTRL, WR0 = 0x12000, 0x12008


def phase_entries(path, label):
    """Raw trace entries between the marker matching label and the next one."""
    take, out = False, []
    for line in open(path):
        if line.startswith("== "):
            rest = line[3:].strip()
            take = (rest.split(" ", 1)[1] if " " in rest else rest) == label
            continue
        if take and line[:1].isdigit():
            parts = line.split()
            if len(parts) < 4:
                continue
            us, kind, a, v = parts[:4]
            out.append((kind, int(a, 16), int(v, 16)))
    return out


def to_script(entries):
    """Yield replay-script lines from raw (kind, addr, val) entries.

    Deviation from the original design ("the poll pattern was not what the
    facts assumed"): the isp1 reads-included baseline trace showed two
    things the original design got wrong for the RTL9602C table registers
    actually hit here (tbl_type 1, ADDR TABLE_CMD/TABLE_WRITE_WORD at
    0x12000/0x12008):

    1. WR_DATA is NOT rewritten before every CTRL write. A common vendor
       pattern writes it once (for example all-zero, clearing a run of
       entries) and issues many CTRL writes with only ADDR changing. The
       recorded WR_DATA registers therefore hold their value across CTRL
       writes on real hardware, so `pending` here is never reset -- doing
       so silently replayed zeros for slots the vendor did not intend to
       change.
    2. The recorded CTRL value never has bit 31 (START, "busy") set,
       and the read that follows a table write is a single one-shot read
       of a DIFFERENT register (0x12004) returning a constant, not a
       bit-31 spin loop on 0x12000. So the CTRL word is replayed VERBATIM
       (no busy-bit reconstruction from spa/method), and no poll line is
       emitted: there is no observed evidence a wait is needed here, and
       inventing a spin-wait convention that does not match the trace
       risks hanging the replay (regreplay's poll loop is 200 reads, each
       a diag process, per table entry -- multiplied by 4096 entries in
       vlan that is not affordable, and was never validated on hardware).
    """
    pending = {}   # WR_DATA slot (0..4) -> last recorded value (persists)
    for kind, addr, val in entries:
        if kind not in ("W", "w"):
            continue
        if WR0 <= addr < WR0 + 20:
            pending[(addr - WR0) // 4] = val
            continue
        if addr == CTRL:
            is_write = (val >> 3) & 1
            if is_write:
                tbl = val & 7
                idx = (val >> 9) & 0xfff
                words = [pending.get(i, 0) for i in range(5)]
                yield "t %d 0x%03x %s 0x%08x" % (
                    tbl, idx, " ".join("0x%08x" % w for w in words), val)
            else:
                yield "w 0x%08x 0x%08x" % (CTRL, val)
            continue
        yield "w 0x%08x 0x%08x" % (addr, val)


def count_table_writes(entries):
    """{tbl_type: sorted list of indices} for --summary, plus non-table regs
    in order of first write."""
    tables = {}
    regs_seen = []
    regs_set = set()
    for kind, addr, val in entries:
        if kind not in ("W", "w"):
            continue
        if WR0 <= addr < WR0 + 20:
            continue
        if addr == CTRL:
            if (val >> 3) & 1:
                tbl = val & 7
                idx = (val >> 9) & 0xfff
                tables.setdefault(tbl, []).append(idx)
            continue
        if addr not in regs_set:
            regs_set.add(addr)
            regs_seen.append(addr)
    return tables, regs_seen


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    trace = argv[1]
    label = None
    rw_trace = None
    summary = False
    i = 2
    while i < len(argv):
        if argv[i] == "--phase":
            label = argv[i + 1]
            i += 2
        elif argv[i] == "--rw":
            rw_trace = argv[i + 1]
            i += 2
        elif argv[i] == "--summary":
            summary = True
            i += 1
        else:
            sys.exit("unknown argument: %s" % argv[i])
    if label is None:
        sys.exit(__doc__)

    entries = phase_entries(trace, label)
    n_writes = sum(1 for k, a, v in entries if k in ("W", "w"))

    if summary:
        tables, regs = count_table_writes(entries)
        print("# phase: %s  (%d writes)" % (label, n_writes))
        for tbl in sorted(tables):
            idxs = tables[tbl]
            print("table %d: %d entries 0x%03x..0x%03x" % (tbl, len(idxs), min(idxs), max(idxs)))
        for addr in regs:
            print("reg 0x%08x" % addr)
        return

    tables, regs = count_table_writes(entries)
    n_entries = sum(len(v) for v in tables.values())
    print("# phase: %s  (%d writes, %d table entries)" % (label, n_writes, n_entries))
    for line in to_script(entries):
        print(line)

    if rw_trace:
        # --rw is accepted but currently unused: the isp1 baseline capture
        # (260921) showed the table op is not a bit-31 spin loop (see
        # to_script's docstring), so there is no poll point left to place
        # from a reads-included trace. Kept as a flag rather than removed,
        # in case a later phase (switch, classify) does show real polling.
        phase_entries(rw_trace, label)


if __name__ == "__main__":
    main(sys.argv)
