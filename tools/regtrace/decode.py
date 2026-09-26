#!/usr/bin/env python3
"""Decode a /proc/rtk_regtrace dump against the RTL9602C register listing.

    decode.py <regmap.txt> <trace.txt> [--summary] [--collapse]

The listing is what src/diag/tools/regmap-extract.py writes with -t: the
register table read out of the stock firmware binary, names from
src/diag/tools/regnames.txt (or the address default). A register with
`array a..b` or `port p..q` occupies consecutive 4-byte slots; an address
inside that range decodes as NAME+<slot>.

Kind M entries (patch 0015 `mark`, written by apply.c around omci_drv_call)
carry a tag: bit 31 is the phase (0 before the getsockopt, 1 after), bits
0..30 the OMCI driver command number. Writes between a before/after pair are
grouped into that command's block; writes outside any pair (autonomous
events, netdevice notifier activity) go in an explicit "unattributed"
bucket, never silently dropped or misattributed to the nearest command.

Kind T/t entries (the stock driver own table write/read entry points,
wrapping the switch-core table funnel one level above the skip-filtered
TABLE_CMD/TABLE_*_WORD registers) carry table id and row index packed as table<<16 | index;
each is followed by one kind-D entry
per data word, packed as word-index<<24 | table, value = the word. A t entry
(a table read) is recorded only when the trace is also `on rw`, same as a
register read; a T (write) is unconditional, same as a register write.

Run-length coding (kernel-side, patch 0015): a row identical to the
previous one except for index+1 is not pushed at all -- only a run counter
advances -- and the run closes with one kind-R entry (addr = table<<16 |
first_index, val = run length) when it breaks or the ring is read out.
decode.py folds a T/t entry, the D entries that follow it, and a trailing R
if present into one printed line:

    table <name or id> idx N: w0 w1 ...                (no run)
    table <name or id> idx A..B (run N): w0 w1 ...      (run of N rows)

Table names are ours, by table id (TABLE_NAMES below).

--summary also lists every command bracket in trace order, one line each,
including brackets with zero writes (a polling command that only reads is
otherwise invisible). --collapse merges a run of consecutive brackets with
the same command number and the same write sequence (same registers, in the
same order, values ignored) into one line with a repeat count -- the
indirect-table poll that motivated armonly is exactly
this shape.
"""
import re
import sys
from collections import Counter, OrderedDict

REG_RE = re.compile(r"^\s*(\d+)\s+0x([0-9a-fA-F]+)\s+width\s+(\d+)\s+array\s+(\d+)\.\.(\d+)\s+port\s+(\d+)\.\.(\d+)\s+(\S+)")
FLD_RE = re.compile(r"^\s+(\d+)\s+(\S+)\s+lsp\s+(\d+)\s+len\s+(\d+)")


def load_map(path):
    """Return {base_offset: (name, slots, [(fieldname, lsp, len), ...])}."""
    regs = OrderedDict()
    cur = None
    with open(path) as f:
        for line in f:
            m = REG_RE.match(line)
            if m:
                off = int(m.group(2), 16)
                slots = (int(m.group(5)) - int(m.group(4)) + 1) * (int(m.group(7)) - int(m.group(6)) + 1)
                cur = (m.group(8), max(slots, 1), [])
                regs[off] = cur
                continue
            m = FLD_RE.match(line)
            if m and cur is not None:
                cur[2].append((m.group(2), int(m.group(3)), int(m.group(4))))
    return regs


def build_lookup(regs):
    """{address: (name, slot_index, fields)} for every slot of every register."""
    lookup = {}
    for off, (name, slots, fields) in regs.items():
        for i in range(slots):
            lookup.setdefault(off + 4 * i, (name, i, fields))
    return lookup


def decode_fields(fields, val):
    parts = []
    for fname, lsp, ln in fields:
        if fname == "RESERVED":
            continue
        parts.append("%s=0x%x" % (fname, (val >> lsp) & ((1 << ln) - 1)))
    return parts


def new_stat():
    return {"n": 0, "regs": Counter(), "unknown": Counter(), "tables": Counter(), "runs": 0}


UNATTRIBUTED = "unattributed"

# Table ids, in the numbering the T/t trace lines carry (the same numbers
# as odi_switch_hw.h's enum odi_sw_table); the names are ours.
TABLE_NAMES = [
    "PARSER_SNAP", "PARSER_SNAP_PARAM", "ACTION_SNAP_NATMC",
    "ACTION_SNAP_OMCI", "ACTION_SNAP_PTP", "ACTION_SNAP", "ACTION_SNAP_V1",
    "ACTION_SNAP_DEBUG", "ACL_ACTIONS", "ACL_PATTERN", "ACL_PATTERN_MASK",
    "CLS_DS_ACTION", "CLS_US_ACTION", "CLS_MASK_A", "CLS_MASK_B",
    "CLS_MASK_C", "CLS_RULE_A", "CLS_RULE_B", "CLS_RULE_C", "L2_MCAST_DSL",
    "L2_UNICAST", "L3_IP6_MCAST", "L3_MCAST_ROUTE", "VLAN_MEMBERS",
    "EPON_GRANTS", "NAT_ARP_CAM", "NAT_BINDING", "NAT_EXT_IP",
    "NAT_FLOW_V4", "NAT_FLOW_V6", "NAT_FLOW_V6_EXT", "NAT_ROUTE_V6",
    "NAT_ROUTE_DROP", "NAT_ROUTE_GLOBAL", "NAT_ROUTE_LOCAL", "NAT_NAPT",
    "NAT_NAPTR", "NAT_NEIGHBOR", "NAT_NETIF", "NAT_NEXT_HOP", "NAT_PPPOE",
    "NAT_WAN_TYPE", "NAT_ACTION_SNAP", "NAT_PARSER_SNAP",
]


def table_name(table_id):
    if 0 <= table_id < len(TABLE_NAMES):
        return TABLE_NAMES[table_id]
    return "TABLE_%d" % table_id


HEADER_RE = re.compile(r"^#\s*regtrace\s+(.*)$")
HEADER_FIELD_RE = re.compile(r"(\w+)=(\S+)")


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    summary = "--summary" in argv
    collapse = "--collapse" in argv
    lookup = build_lookup(load_map(argv[1]))
    phase = "(no phase)"
    stats = OrderedDict()
    cmd_stats = OrderedDict()  # cmd number (int) -> new_stat(), across all instances
    cur_cmd = None  # command number open between a before/after mark pair, else None
    cur_block = None  # [cmd, [(kind, addr), ...]] while inside a bracket, else None
    blocks = []  # every bracket in trace order, including ones with zero writes
    header = None  # fields from the first "# regtrace ..." header line seen
    out = []
    pending = None  # {"kind", "table", "index", "words": [], "run_len"?} between a T/t and its D's/R

    def flush_pending():
        if pending is None:
            return
        words = " ".join("0x%08x" % w for w in pending["words"])
        if "run_len" in pending:
            end = pending["index"] + pending["run_len"] - 1
            out.append("table %s idx %d..%d (run %d): %s" % (
                table_name(pending["table"]), pending["index"], end, pending["run_len"], words))
        else:
            out.append("table %s idx %d: %s" % (table_name(pending["table"]), pending["index"], words))

    with open(argv[2]) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("== "):
                flush_pending()
                pending = None
                phase = line[3:].split(" ", 1)[-1] if " " in line[3:] else line[3:]
                stats.setdefault(phase, new_stat())
                out.append(line)
                continue
            if line.startswith("#"):
                if header is None:
                    m = HEADER_RE.match(line)
                    if m:
                        header = dict(HEADER_FIELD_RE.findall(m.group(1)))
                out.append(line)
                continue
            if not line.strip():
                out.append(line)
                continue
            us, kind, addr_s, val_s = line.split()[:4]
            addr, val = int(addr_s, 16), int(val_s, 16)
            if kind in ("T", "t"):
                flush_pending()
                pending = {"kind": kind, "table": (addr >> 16) & 0xffff,
                           "index": addr & 0xffff, "words": []}
                bucket = cmd_stats.setdefault(cur_cmd if cur_cmd is not None else UNATTRIBUTED, new_stat())
                bucket["tables"][table_name(pending["table"])] += 1
                if cur_block is not None:
                    cur_block[1].append((kind, addr))
                continue
            if kind == "D":
                if pending is not None:
                    pending["words"].append(val)
                    if cur_block is not None:
                        cur_block[1].append((kind, addr))
                    continue
                # A D with no preceding T/t is malformed (should not happen
                # given the kernel side always emits T/t before its D's) --
                # fall through and let it print as an ordinary unrecognised
                # line rather than silently vanish.
            if kind == "R" and pending is not None:
                # kind R is dual-purpose: a table run's closing entry
                # (always immediately after that run's T/t/D's, hence
                # pending is not None) and a plain register read (only in
                # an `on rw` capture, addr a real MMIO address, never right
                # after a pending table op in practice). Only the former is
                # handled here; pending is None falls through below to the
                # ordinary register-decode path, same as any other read.
                pending["run_len"] = val
                bucket = cmd_stats.setdefault(cur_cmd if cur_cmd is not None else UNATTRIBUTED, new_stat())
                bucket["runs"] += 1
                if cur_block is not None:
                    cur_block[1].append((kind, addr))
                flush_pending()
                pending = None
                continue
            flush_pending()
            pending = None
            if kind == "M":
                cmd = addr & 0x7FFFFFFF
                after = (addr >> 31) & 1
                if after:
                    out.append("== end cmd %d ==" % cmd)
                    cur_cmd = None
                    if cur_block is not None:
                        blocks.append(cur_block)
                        cur_block = None
                else:
                    out.append("== cmd %d ==" % cmd)
                    cur_cmd = cmd
                    cur_block = [cmd, []]
                continue
            if kind == "D":
                out.append("%s %s 0x%08x 0x%08x ORPHAN-D" % (us, kind, addr, val))
                continue
            st = stats.setdefault(phase, new_stat())
            st["n"] += 1
            bucket = cmd_stats.setdefault(cur_cmd if cur_cmd is not None else UNATTRIBUTED, new_stat())
            bucket["n"] += 1
            hit = lookup.get(addr)
            if hit is None:
                st["unknown"][addr] += 1
                bucket["unknown"][addr] += 1
                if cur_block is not None:
                    cur_block[1].append((kind, addr))
                out.append("%s %s 0x%08x 0x%08x UNKNOWN" % (us, kind, addr, val))
                continue
            name, slot, fields = hit
            label = name if slot == 0 else "%s+%d" % (name, slot)
            st["regs"][label] += 1
            bucket["regs"][label] += 1
            if cur_block is not None:
                cur_block[1].append((kind, addr))
            out.append(" ".join(["%s %s 0x%08x 0x%08x %s" % (us, kind, addr, val, label)] + decode_fields(fields, val)))
    flush_pending()  # a trailing T/t with no line after it (end of file)
    if not summary:
        print("\n".join(out))
        return
    if header:
        print("header: " + " ".join("%s=%s" % (k, header[k]) for k in ("total", "dropped", "armonly", "skipped", "tables", "runs") if k in header))
    for ph, st in stats.items():
        print("%s: %d entries, %d registers, %d unknown" % (ph, st["n"], len(st["regs"]) + len(st["unknown"]), len(st["unknown"])))
        for label, n in st["regs"].most_common():
            print("    %6d  %s" % (n, label))
        for addr, n in st["unknown"].most_common():
            print("    %6d  UNKNOWN 0x%08x" % (n, addr))
    if cmd_stats:
        print("\nper-command (writes between a before/after mark pair, grouped by OMCI driver command number):")
        for cmd, st in cmd_stats.items():
            if cmd == UNATTRIBUTED:
                print("%s: %d writes, %d unknown" % (UNATTRIBUTED, st["n"], len(st["unknown"])))
                for addr, n in st["unknown"].most_common():
                    print("    %6d  UNKNOWN 0x%08x" % (n, addr))
                continue
            groups = sorted(st["regs"])
            n_tables = sum(st["tables"].values())
            print("cmd %d: tag before=0x%08x after=0x%08x, %d writes, %d distinct registers, "
                  "groups [%s], %d unknown, %d table ops, %d runs" %
                  (cmd, cmd & 0x7FFFFFFF, (cmd & 0x7FFFFFFF) | 0x80000000, st["n"],
                   len(st["regs"]), ", ".join(groups), len(st["unknown"]), n_tables, st["runs"]))
            for reg, n in st["regs"].most_common():
                print("    %6d  %s" % (n, reg))
            for addr, n in st["unknown"].most_common():
                print("    %6d  UNKNOWN 0x%08x" % (n, addr))
            for name, n in st["tables"].most_common():
                print("    %6d  table %s" % (n, name))
    if blocks:
        print("\nblocks in order (one line per command bracket, including brackets"
              " with zero writes; --collapse merges runs of identical ones):")
        # Each printed row is (cmd, write-count, repeat-count). Consecutive
        # blocks collapse when --collapse is given and both the command
        # number and the (kind, addr) write sequence match exactly -- the
        # polling shape armonly was added for is one command number with the
        # same registers touched, in the same order, every time.
        # kind R is dual-purpose (a plain register read, only ever present
        # in an `on rw` capture, and a table run's closing entry, always
        # present when a run happened) -- the two cannot be told apart at
        # this point, so an R is counted as a run here, not as a read. Under
        # a combined `on rw` + table-tracing capture the "writes" count
        # below can therefore undercount genuine register reads by however
        # many runs closed in that bracket; the raw (non---summary) decode
        # is unambiguous per-line (a run's R always follows its T/t/D's).
        rows = []
        for cmd, seq in blocks:
            n_writes = sum(1 for k, _a in seq if k in ("W", "r", "w"))
            n_tables = sum(1 for k, _a in seq if k in ("T", "t"))
            n_runs = sum(1 for k, _a in seq if k == "R")
            if collapse and rows and rows[-1][0] == cmd and rows[-1][5] == seq:
                rows[-1][4] += 1
            else:
                rows.append([cmd, n_writes, n_tables, n_runs, 1, seq])
        for cmd, n, n_tables, n_runs, repeat, _seq in rows:
            suffix = ""
            if n_tables:
                suffix += ", %d table ops" % n_tables
            if n_runs:
                suffix += ", %d runs" % n_runs
            if repeat > 1:
                print("cmd %d: %d writes%s (repeated %dx)" % (cmd, n, suffix, repeat))
            else:
                print("cmd %d: %d writes%s" % (cmd, n, suffix))


if __name__ == "__main__":
    main(sys.argv)
