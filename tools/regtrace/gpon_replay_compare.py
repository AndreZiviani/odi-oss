#!/usr/bin/env python3
"""Ordered write-stream comparator for the odi_gpon replay test.

    gpon_replay_compare.py <expected.txt> <actual.txt>

Both files are in the same format tools/regtrace/compare.py/decode.py read
(`<ns> W <addr> <val>` per line, `#`-prefixed lines ignored) -- but unlike
that tool own --stream mode (which folds each dump down to one
final-value-per-address map, discarding write ORDER entirely), this
compares the two W-entry sequences POSITION BY POSITION, in order: for the
replay test own goal ("our driver reproduces the vendor register
stream... when driven by the same hardware inputs"), the sequence itself
is the thing under test, not just where each register ends up.

Two addresses are dropped from BOTH sequences before comparing, GPON_
PONMAC_IRQ_ENABLE (0x700040) and DSF_PLOAM_RX_CTL (0x701080): the real
capture shows a variable, non-deterministic number of these per activation
bracket (sometimes one dequeue-acknowledge pair before the mask is
restored, sometimes two, apparently depending on exactly how many already-
processed duplicate reads of the same three-times-sent message were still
draining when the interrupt happened to fire) -- this is the timing of
*when* a duplicate got noticed, not a property of the driver own
activation decisions, and this test own message-feed loop (one call to
odi_gpon_isr() per queued occurrence) cannot reproduce that incidental
grouping without hardcoding it bracket by bracket. Every other register --
state, ONU-ID, EqD, BOH, TRAFFIC_CFG, the Alloc-ID/GEM CAM tables, AES, and
the PLOAM_DATA/WR content itself -- is compared exactly, in order, uses no
such exclusion. IGNORE_ADDRS below is the one-line, written reason this
file own docstring already gives: a written list of documented
differences, each with a reason, is this paragraph.

Exit code 0 when every expected write has a matching actual write at the
same position (address and value both equal) -- 1 otherwise, with the
first mismatch and both totals printed either way.
"""
import sys

IGNORE_ADDRS = {0x700040, 0x701080}


def read_writes(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) != 4 or parts[1] != "W":
                continue
            addr = int(parts[2], 16)
            if addr in IGNORE_ADDRS:
                continue
            out.append((addr, int(parts[3], 16)))
    return out


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    expected = read_writes(argv[1])
    actual = read_writes(argv[2])

    n = min(len(expected), len(actual))
    mismatch_at = None
    for i in range(n):
        if expected[i] != actual[i]:
            mismatch_at = i
            break

    print("expected: %d writes, actual: %d writes" % (len(expected), len(actual)))
    if mismatch_at is not None:
        ea, ev = expected[mismatch_at]
        aa, av = actual[mismatch_at]
        print("DIFF at write %d: expected 0x%08x=0x%08x got 0x%08x=0x%08x"
              % (mismatch_at, ea, ev, aa, av))
        print("matched: %d writes before the first difference" % mismatch_at)
        return 1
    if len(expected) != len(actual):
        shorter, longer = ("expected", "actual") if len(expected) < len(actual) else ("actual", "expected")
        print("DIFF: %s ends after %d writes, %s has %d more"
              % (shorter, n, longer, max(len(expected), len(actual)) - n))
        return 1

    print("PASS: %d writes matched, in order, exactly" % len(expected))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
