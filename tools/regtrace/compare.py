#!/usr/bin/env python3
"""Compare two register-trace ring dumps by command bracket, or as ordered
write streams (--stream).

    compare.py <dump-a> <dump-b> [--addr-only] [--ignore ADDR[,ADDR...]]
    compare.py <dump-a> <dump-b> --stream

Both files are raw ring dumps in the format written by /proc/rtk_regtrace
and consumed by decode.py: one line per entry, `<uptime_ns> W <addr> <val>`
for a register write or `<uptime_ns> M <tag> 0` for a command mark (tag bit
31 is phase -- 0 before, 1 after -- bits 0..30 the OMCI driver command
number; see the decode.py docstring). `T`/`t`/`D` entries (a table write/
read op and its data words, packed the same way decode.py unpacks them) are
part of the bracket sequence too, in trace order alongside the W entries --
a table commit silently dropped from one dump's comparison would defeat the
whole point of tracking it. `R` (a table run's closing entry -- see
decode.py's docstring for the kernel-side run-length coding and its
ambiguity with a plain register read) is compared the same way, in its
run form: run-length coding is deterministic given the same input, so a
run in one dump should close at the same length in the other, and a run
form that expanded differently between two otherwise-identical dumps is
exactly the kind of difference compare.py exists to catch -- it is never
expanded back into per-row entries before comparing. `w` (lowercase -- a
write to the SoC-window physical block, 0xb8xxxxxx, a different bus
entirely from the switch-register `W`) is compared the same way too, in
position, as its own single-line write; this is what mksdkinit.py own
ODI_SW_MODLOAD_SOC events reproduce, and dropping it from the comparison
would hide exactly the kind of silent omission that bug was. Lines that are
neither, plus a leading `#` header line, are ignored.

Command brackets (the entries between a before/after mark pair for the same
command number -- W, T, t, D and R alike) are aligned between the two dumps by
command number and by occurrence order within that command number -- the
Nth bracket for command C in dump A is compared against the Nth bracket for
command C in dump B, regardless of where either falls in the overall trace
or what other commands interleave around it. A bracket with no counterpart
in the other dump (extra or missing occurrence) is reported as its own
failure.

For each aligned bracket the write sequences are diffed write-by-write, in
order: address and value must match unless --addr-only is given, in which
case only the address sequence is compared and values are ignored. A
register in --ignore is dropped from both sequences before comparing (so a
register that free-runs, e.g. a counter, does not fail an otherwise
identical bracket).

One line is printed per bracket:
    cmd N #k: PASS
    cmd N #k: DIFF at write i: expected <addr> <val> got <addr> <val>
(a length mismatch reports the first index past the shorter sequence, with
the missing side shown as "-- --"), then a final summary line. Exit code is
0 only when every bracket passes and neither dump has a stray unmatched
bracket; 1 otherwise.

--stream ignores brackets (and marks) entirely and compares the two dumps
as ordered WRITE streams instead -- for a full-stream capture (a
regtrace-all capture, armonly off, so every write from omcid start onward
is recorded whether a mark brackets it or not) the question is no longer
"does this command own writes match", it is "does the switch end up in
the same state", regardless of which write path put it there. Two final-
state maps are built per dump: one register address -> its last written
value (a W entry; a plain register read, kind R with no table op open, is
not a write and is skipped, keeping this a write-only comparison as its
name says), and one (table, row index) -> its last written word tuple (a
T/t + the D words that follow it, with a closing R own run-length expanded
back into one identical entry per row it covers -- the one place this mode
DOES expand a run, unlike bracket mode above, because "the final value per
... table row" is meaningless while multiple rows are still folded into
one run entry). Building these final-state maps replays the same T/t/D/R
folding decode.py own flush_pending() does, kept independent here rather
than imported, since decode.py own version is entangled with printing and
per-command bucketing this mode has no use for.

The two maps are then diffed key by key and reported in two sections,
registers first then table rows:
    register 0x<addr>: <dump-a>=0x<val> <dump-b>=0x<val>          (present in both, differ)
    register 0x<addr>: only in <dump>, final 0x<val>              (written in one, never the other)
    table <n> idx <i>: <dump-a>=0x<w0> 0x<w1> ... <dump-b>=...    (present in both, differ)
    table <n> idx <i>: only in <dump>, final 0x<w0> 0x<w1> ...    (written in one, never the other)
followed by one summary line per section ("registers: N compared, D
differ, A only in <dump-a>, B only in <dump-b>", same shape for "table
rows"). Exit code is 0 only when both sections report zero differences and
zero one-sided entries; 1 otherwise. --addr-only and --ignore do not apply
to --stream (there is no bracket to align, and dropping one address from a
final-state map would just delete that comparison rather than skip a
known-noisy one; filter the input file before diffing if a register needs
excluding).
"""
import sys


def parse_ignore(spec):
    if not spec:
        return set()
    out = set()
    for tok in spec.split(","):
        tok = tok.strip()
        if not tok:
            continue
        out.add(int(tok, 16) if tok.lower().startswith("0x") else int(tok))
    return out


def load_brackets(path):
    """Return a list of (cmd, [(addr, val), ...]) in trace order.

    A W, T, t, D or R entry outside any before/after mark pair is not part
    of any bracket and is dropped -- compare.py only ever judges command
    brackets, same as the "blocks in order" section of decode.py --summary.
    T/t/D are included alongside W so a table commit is compared exactly
    like a plain register write; addr/val already carry decode.py's packed
    table<<16|index (T/t) or word-index<<24|table (D) encoding, which
    compares byte-for-byte the same way a real address/value pair does. R
    (a run's closing entry, addr = table<<16|first_index, val = run length)
    is included the same way, in its run form -- never expanded back into
    per-row entries. Note the same ambiguity decode.py documents: in an
    `on rw` capture an R can also be a plain register read: this function
    does not disambiguate either, so comparing two `on rw` captures that mix
    table tracing and register reads may compare a read against a run by
    coincidence of position -- an existing limitation of raw kind-based
    parsing, not new here.
    """
    brackets = []
    cur_cmd = None
    cur_writes = None
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            _us, kind, addr_s, val_s = parts[0], parts[1], parts[2], parts[3]
            try:
                addr = int(addr_s, 16)
                val = int(val_s, 16)
            except ValueError:
                continue
            if kind == "M":
                cmd = addr & 0x7FFFFFFF
                after = (addr >> 31) & 1
                if after:
                    if cur_cmd is not None:
                        brackets.append((cur_cmd, cur_writes))
                    cur_cmd = None
                    cur_writes = None
                else:
                    cur_cmd = cmd
                    cur_writes = []
            elif kind in ("W", "T", "t", "D", "R", "w"):
                if cur_writes is not None:
                    cur_writes.append((addr, val))
    return brackets


def group_by_cmd(brackets):
    """{cmd: [writes, writes, ...]} preserving occurrence order."""
    grouped = {}
    for cmd, writes in brackets:
        grouped.setdefault(cmd, []).append(writes)
    return grouped


def filtered(writes, ignore):
    return [(a, v) for (a, v) in writes if a not in ignore]


def diff_bracket(a_writes, b_writes, addr_only, ignore):
    a = filtered(a_writes, ignore)
    b = filtered(b_writes, ignore)
    n = max(len(a), len(b))
    for i in range(n):
        av = a[i] if i < len(a) else None
        bv = b[i] if i < len(b) else None
        if av is None or bv is None:
            exp = "0x%08x 0x%08x" % av if av else "-- --"
            got = "0x%08x 0x%08x" % bv if bv else "-- --"
            return i, exp, got
        if addr_only:
            if av[0] != bv[0]:
                return i, "0x%08x --" % av[0], "0x%08x --" % bv[0]
        else:
            if av != bv:
                return i, "0x%08x 0x%08x" % av, "0x%08x 0x%08x" % bv
    return None


def load_stream(path):
    """Return (registers, tables): the final-state maps a --stream compare
    needs. registers is {addr: last_written_value}, tables is
    {(table, index): word_tuple}, both write-only (a plain register read --
    kind R with no table op currently open -- updates neither and is
    skipped). Replays the same T/t + D + R folding decode.py own
    flush_pending() does, including a closed run own expansion into one
    identical entry per row index it covers -- kept independent of
    decode.py rather than imported, since decode.py own version is entangled
    with printing and per-command bucketing this function has no use for.
    """
    registers = {}
    tables = {}
    pending = None  # {"table", "index", "words": [...]} between a T/t and
    # whatever closes it -- another T/t, a W, an M, an R, or end of file.

    def flush(run_len=None):
        if pending is None:
            return
        words = tuple(pending["words"])
        n = run_len if run_len is not None else 1
        for i in range(n):
            tables[(pending["table"], pending["index"] + i)] = words

    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#") or line.startswith("=="):
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            kind, addr_s, val_s = parts[1], parts[2], parts[3]
            try:
                addr = int(addr_s, 16)
                val = int(val_s, 16)
            except ValueError:
                continue
            if kind in ("T", "t"):
                flush()
                pending = {"table": (addr >> 16) & 0xFFFF, "index": addr & 0xFFFF, "words": []}
            elif kind == "D" and pending is not None:
                pending["words"].append(val)
            elif kind == "R" and pending is not None:
                flush(run_len=val)
                pending = None
            elif kind == "W" or kind == "w":
                # "w" (lowercase) is a write too, to the SoC-window
                # physical block (0xb8xxxxxx, ODI_SW_MODLOAD_SOC) rather
                # than the switch-register bus "W" reaches -- numerically
                # disjoint address ranges, so sharing one map with "W"
                # cannot alias the two.
                flush()
                pending = None
                registers[addr] = val
            elif kind == "M":
                flush()
                pending = None
            # Anything else -- an orphan D (no T/t ever opened it, should
            # not happen given the kernel side always emits T/t first), or
            # a plain register read (kind R with no table op pending, only
            # possible under an `on rw` capture) -- is not a write to
            # attribute to either map, and is silently skipped, matching
            # the "write streams" this mode compares, not read traffic.
    flush()  # a trailing T/t (+ D words, no R) with nothing after it in the file
    return registers, tables


def fmt_words(words):
    return " ".join("0x%08x" % w for w in words)


def diff_stream(path_a, path_b):
    a_regs, a_tabs = load_stream(path_a)
    b_regs, b_tabs = load_stream(path_b)

    reg_diff = reg_only_a = reg_only_b = 0
    for addr in sorted(set(a_regs) | set(b_regs)):
        av, bv = a_regs.get(addr), b_regs.get(addr)
        if av is None:
            reg_only_b += 1
            print("register 0x%08x: only in %s, final 0x%08x" % (addr, path_b, bv))
        elif bv is None:
            reg_only_a += 1
            print("register 0x%08x: only in %s, final 0x%08x" % (addr, path_a, av))
        elif av != bv:
            reg_diff += 1
            print("register 0x%08x: %s=0x%08x %s=0x%08x" % (addr, path_a, av, path_b, bv))
    reg_total = len(set(a_regs) | set(b_regs))
    print("registers: %d compared, %d differ, %d only in %s, %d only in %s" %
          (reg_total, reg_diff, reg_only_a, path_a, reg_only_b, path_b))

    tab_diff = tab_only_a = tab_only_b = 0
    for table, index in sorted(set(a_tabs) | set(b_tabs)):
        aw, bw = a_tabs.get((table, index)), b_tabs.get((table, index))
        if aw is None:
            tab_only_b += 1
            print("table %d idx %d: only in %s, final %s" % (table, index, path_b, fmt_words(bw)))
        elif bw is None:
            tab_only_a += 1
            print("table %d idx %d: only in %s, final %s" % (table, index, path_a, fmt_words(aw)))
        elif aw != bw:
            tab_diff += 1
            print("table %d idx %d: %s=%s %s=%s" % (table, index, path_a, fmt_words(aw), path_b, fmt_words(bw)))
    tab_total = len(set(a_tabs) | set(b_tabs))
    print("table rows: %d compared, %d differ, %d only in %s, %d only in %s" %
          (tab_total, tab_diff, tab_only_a, path_a, tab_only_b, path_b))

    ok = reg_diff == 0 and reg_only_a == 0 and reg_only_b == 0 and \
        tab_diff == 0 and tab_only_a == 0 and tab_only_b == 0
    return 0 if ok else 1


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    path_a, path_b = argv[1], argv[2]
    if "--stream" in argv:
        return diff_stream(path_a, path_b)
    addr_only = "--addr-only" in argv
    ignore = set()
    for i, arg in enumerate(argv):
        if arg == "--ignore" and i + 1 < len(argv):
            ignore = parse_ignore(argv[i + 1])

    a = group_by_cmd(load_brackets(path_a))
    b = group_by_cmd(load_brackets(path_b))

    all_cmds = sorted(set(a) | set(b))
    total = 0
    failed = 0
    for cmd in all_cmds:
        a_list = a.get(cmd, [])
        b_list = b.get(cmd, [])
        n = max(len(a_list), len(b_list))
        for k in range(n):
            total += 1
            label = "cmd %d #%d" % (cmd, k + 1)
            if k >= len(a_list):
                print("%s: DIFF missing in %s (extra bracket in %s)" % (label, path_a, path_b))
                failed += 1
                continue
            if k >= len(b_list):
                print("%s: DIFF missing in %s (extra bracket in %s)" % (label, path_b, path_a))
                failed += 1
                continue
            d = diff_bracket(a_list[k], b_list[k], addr_only, ignore)
            if d is None:
                print("%s: PASS" % label)
            else:
                i, exp, got = d
                print("%s: DIFF at write %d: expected %s got %s" % (label, i, exp, got))
                failed += 1

    print("summary: %d brackets, %d pass, %d fail" % (total, total - failed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
