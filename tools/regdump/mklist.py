#!/usr/bin/env python3
"""Build the read-only address list for regdump from the RTL9602C register listing.

    mklist.py <regmap.txt> [out.txt]

Prints (or writes to out.txt) one line per distinct 32-bit word address the
map defines, after the exclusions below:

    <addr_hex> <name>

addr_hex is 8 lowercase hex digits, no 0x prefix, the PHYSICAL switch-core
address (0x1B000000 + the map byte offset -- see ODI_SWITCH_MMIO_BASE and
odi_switch_hw.h). name is the register name,
or several names joined by "|" when the map declares more than one register
at the same computed word address (see "Packing model" below) -- an
ambiguous address is reported as ambiguous, not silently resolved to one
guess.

The map itself is the listing src/diag/tools/regmap-extract.py writes with
-t (the register table read out of the stock firmware binary, named from
src/diag/tools/regnames.txt or by address; generated, not kept in this
repository), the same file tools/regtrace/decode.py reads. Each register
line is

    <id> <hex byte offset> width <W> array <a0>..<a1> port <p0>..<p1> <NAME>

W is the bit width of ONE item value (not always 32 -- this map uses it
for indirect-table entries and multi-word registers too); an array/port
range multiplies out to item count = (a1-a0+1) * (p1-p0+1).

Packing model
-------------
The decode.py model -- every array/port item gets its own 4-byte word at
offset + 4*i, unconditionally -- is only correct when the whole register is
wide (W * count > 32). odi-oss commit a39e06f ("odi_switch_hw: fix narrow
field widths from a wrong per-item address model") found that a narrow
register (W * count <= 32) instead packs every item into ONE 32-bit word,
item i at bit (i * W) of that word -- e.g. SW_0x011004 (width 1, port
0..3) is four ports packed one bit apiece into a single word at 0x011004,
not four separate words. Treating it as wide does not just mislabel a
field: it invents word addresses that were never real register slots, and
some of those invented addresses collide with a different, unrelated,
genuinely-wide register a few words over -- the odi_switch_hw.h header
comment says as much: "The register map declares some registers at
overlapping addresses". Reading a nonexistent invented address is exactly
the kind of read this tool exists to avoid.

For an item wider than 32 bits (W > 32 -- the 48-bit MAC address
registers, the 64-bit loop-detect magic at 0x01a014, ...), each item spans
ceil(W/32) consecutive words. This is not a guess: it is what
odi_switch_hw.h itself resolved to for the 0x01a014 register, added by
hand in the same commit -- the high word of item 0 at offset+4.

So, per register:

    W * count <= 32    -> ONE word, at offset                    (narrow)
    W <= 32 < W*count  -> count words, at offset + 4*i            (wide,
                           the decode.py model, correct for this case)
    W > 32             -> count * ceil(W/32) words, the words of item i
                           at offset + i*stride + 4*k, k in
                           0..ceil(W/32)-1, stride = 4*ceil(W/32)  (wide,
                           multi-word)

A register surviving to the output keeps its FULL word range -- this tool
lists addresses to read, not fields to decode, so no field-level ambiguity
resolution (the kind a39e06f does against real trace values) is attempted
here; an address collision is reported, not adjudicated.
"""
import math
import os
import re
import sys
from collections import OrderedDict

SWITCH_CORE_BASE = 0x1B000000

REG_RE = re.compile(
    r"^\s*(\d+)\s+0x([0-9a-fA-F]+)\s+width\s+(\d+)\s+array\s+(\d+)\.\.(\d+)\s+"
    r"port\s+(\d+)\.\.(\d+)\s+(\S+)"
)

# ---------------------------------------------------------------------------
# Exclusion table -- every rule the address list is filtered through, kept
# in one place so it can be reviewed in one read. A register dropped here
# is dropped WHOLE (every word of it), not just a risky field.
#
#   GPON_BLOCK_BASE   offset >= 0x700000: the GPON block. A different
#                      subsystem from the switch-core A/B this tool is for,
#                      and the single largest chunk of the table
#                      (202 of 1530 registers) -- cutting it here keeps the
#                      switch-core list from ballooning on its own.
#
#   COUNTER_BLOCK_BASE  offset >= 0xF00000: the PON queue and counter block
#                      (per the ODI_SWITCH_MMIO_SIZE comment in
#                      odi_switch_hw.h). Hardware counters on this
#                      switch-core family are commonly clear-on-read; a dump
#                      perturbing the very counters a later reader might
#                      want undisturbed is the one silent failure mode this
#                      list must not have.
#
#   EXCLUDED_REGISTERS  66 switch-core registers, by base address, that are
#                      not idempotent to read, or not worth the risk:
#                        LATCH    indirect-access read-data latches, only
#                                 meaningful right after their own
#                                 busy-gated command (PHY, eFuse, SerDes,
#                                 I2C, the table funnel, the EPON table)
#                        QUEUE    a FIFO; reading one can drain it
#                        STATUS   status, interrupt-status and statistics
#                                 registers, clear-on-read is common for
#                                 these, and the MIB counter blocks
#                        COUNTER  counters and counter controls
#                        CLEAR    acknowledge/clear handshakes (and three
#                                 neighbours kept out with them by the first
#                                 cut of this list, conservatively)
#                      By address because names are no guide: most of these
#                      carry only the default SW_0x<address> name. The list
#                      is the one the first version of this tool applied;
#                      extend it, do not shrink it, without a stick-side
#                      check that the register reads clean.
# ---------------------------------------------------------------------------
GPON_BLOCK_BASE = 0x700000
COUNTER_BLOCK_BASE = 0xF00000

LATCH, QUEUE, STATUS, COUNTER, CLEAR = (
    "read-data latch", "queue", "status", "counter", "clear handshake")

EXCLUDED_REGISTERS = {
    # LATCH (6)
    0x000008: LATCH, 0x000020: LATCH, 0x000044: LATCH, 0x0000c8: LATCH,
    0x01201c: LATCH, 0x03610c: LATCH,
    # QUEUE (2)
    0x0001cc: QUEUE, 0x0001dc: QUEUE,
    # STATUS (27)
    0x000014: STATUS, 0x0001e4: STATUS, 0x015000: STATUS, 0x016000: STATUS,
    0x016004: STATUS, 0x01c098: STATUS, 0x01d014: STATUS, 0x01d018: STATUS,
    0x01d01c: STATUS, 0x032000: STATUS, 0x032200: STATUS, 0x032400: STATUS,
    0x032600: STATUS, 0x032680: STATUS, 0x032994: STATUS, 0x032c00: STATUS,
    0x032e00: STATUS, 0x032e40: STATUS, 0x034000: STATUS, 0x034004: STATUS,
    0x034008: STATUS, 0x03400c: STATUS, 0x034010: STATUS, 0x034014: STATUS,
    0x034018: STATUS, 0x03401c: STATUS, 0x034020: STATUS,
    # COUNTER (23)
    0x017010: COUNTER, 0x01701c: COUNTER, 0x020000: COUNTER,
    0x0230fc: COUNTER, 0x023100: COUNTER, 0x023104: COUNTER,
    0x023108: COUNTER, 0x02310c: COUNTER, 0x02d024: COUNTER,
    0x02d028: COUNTER, 0x02d038: COUNTER, 0x0329b8: COUNTER,
    0x0329bc: COUNTER, 0x0329c0: COUNTER, 0x0329c4: COUNTER,
    0x0329c8: COUNTER, 0x0329cc: COUNTER, 0x034024: COUNTER,
    0x034028: COUNTER, 0x036104: COUNTER, 0x036158: COUNTER,
    0x03616c: COUNTER, 0x036178: COUNTER,
    # CLEAR (8)
    0x02303c: CLEAR, 0x026014: CLEAR, 0x026018: CLEAR, 0x032928: CLEAR,
    0x0360f8: CLEAR, 0x03613c: CLEAR, 0x03618c: CLEAR, 0x036198: CLEAR,
}

# Registers carrying one of our curated names (tools/regnames.txt) go
# first when the list is capped -- they are the ones our code touches. A
# default name is <block>_0x<address>.
DEFAULT_NAME_RE = re.compile(r"^[A-Z]+_0x[0-9a-f]{6}$")

# Overridable only for test/regdump_test.sh, which cannot otherwise exercise
# the cap logic without generating 3000+ synthetic registers -- a real run
# never needs to set this.
MAX_ADDRESSES = int(os.environ.get("REGDUMP_MAX_ADDRESSES", "3000"))


def excluded_reason(offset, name):
    if offset >= COUNTER_BLOCK_BASE:
        return "counter/MIB block"
    if offset >= GPON_BLOCK_BASE:
        return "GPON block"
    return EXCLUDED_REGISTERS.get(offset)


def load_registers(path):
    """Yield (offset, width, count, name) for every register the map declares."""
    with open(path) as f:
        for line in f:
            m = REG_RE.match(line)
            if not m:
                continue
            offset = int(m.group(2), 16)
            width = int(m.group(3))
            a0, a1 = int(m.group(4)), int(m.group(5))
            p0, p1 = int(m.group(6)), int(m.group(7))
            count = (a1 - a0 + 1) * (p1 - p0 + 1)
            name = m.group(8)
            yield offset, width, count, name


def register_words(offset, width, count):
    """Return the list of byte offsets (within the map) this register occupies.

    See the "Packing model" section of the module docstring.
    """
    if width * count <= 32:
        return [offset]
    if width <= 32:
        return [offset + 4 * i for i in range(count)]
    words_per_item = math.ceil(width / 32)
    stride = 4 * words_per_item
    return [offset + i * stride + 4 * k for i in range(count) for k in range(words_per_item)]


def build_address_map(path):
    """Return {byte_offset: [name, ...]} after exclusions, names in map order."""
    addr_names = OrderedDict()
    excluded = {"counter/MIB block": 0, "GPON block": 0, "listed": 0}
    kept_registers = 0
    for offset, width, count, name in load_registers(path):
        reason = excluded_reason(offset, name)
        if reason:
            if reason in ("counter/MIB block", "GPON block"):
                excluded[reason] += 1
            else:
                excluded["listed"] += 1
            continue
        kept_registers += 1
        for word_off in register_words(offset, width, count):
            addr_names.setdefault(word_off, [])
            if name not in addr_names[word_off]:
                addr_names[word_off].append(name)
    return addr_names, kept_registers, excluded


def is_named(names):
    """Whether any of names (a list, for a collision) is a curated name."""
    return any(not DEFAULT_NAME_RE.match(n) for n in names)


def prioritise(addr_names, cap):
    """Keep every address with a curated name first, then fill up to cap."""
    priority = OrderedDict()
    rest = OrderedDict()
    for off, names in addr_names.items():
        if is_named(names):
            priority[off] = names
        else:
            rest[off] = names
    if len(priority) >= cap:
        # Even the priority-only set is over cap: keep it in map (offset)
        # order and stop at the cap rather than returning something larger
        # than what was asked for.
        kept = OrderedDict(list(priority.items())[:cap])
        return kept, True
    kept = OrderedDict(priority)
    for off, names in rest.items():
        if len(kept) >= cap:
            break
        kept[off] = names
    return kept, True


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    map_path = argv[1]
    out_path = argv[2] if len(argv) > 2 else None

    addr_names, kept_registers, excluded = build_address_map(map_path)
    capped = False
    if len(addr_names) > MAX_ADDRESSES:
        addr_names, capped = prioritise(addr_names, MAX_ADDRESSES)

    lines = []
    lines.append("# regdump address list -- generated by mklist.py from %s" % map_path)
    lines.append("# physical address (0x%08x + map offset), register name(s)" % SWITCH_CORE_BASE)
    lines.append(
        "# %d registers kept, %d excluded (gpon %d, counter/mib %d, listed %d)"
        % (kept_registers,
           sum(excluded.values()),
           excluded["GPON block"],
           excluded["counter/MIB block"],
           excluded["listed"])
    )
    if capped:
        lines.append(
            "# CAPPED at %d addresses (raw list was larger) -- registers with "
            "a curated name first" % MAX_ADDRESSES
        )
    lines.append("# %d addresses" % len(addr_names))

    for off in sorted(addr_names):
        addr = SWITCH_CORE_BASE + off
        name = "|".join(addr_names[off])
        lines.append("%08x %s" % (addr, name))

    text = "\n".join(lines) + "\n"
    if out_path:
        with open(out_path, "w") as f:
            f.write(text)
        sys.stderr.write(
            "%d addresses written to %s (%d excluded)\n"
            % (len(addr_names), out_path, sum(excluded.values()))
        )
    else:
        sys.stdout.write(text)
    if len(addr_names) > MAX_ADDRESSES:
        # Cannot happen (prioritise() enforces the cap), kept as a hard
        # assertion so a future change to prioritise() cannot silently
        # regress the one invariant that matters here.
        sys.exit("mklist.py: internal error, address list still over cap")


if __name__ == "__main__":
    main(sys.argv)
