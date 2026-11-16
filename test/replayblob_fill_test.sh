#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# Exercises the call records of tools/regtrace/replayblob.py (compact,
# expand, the modload filter) and pins the shipped sdkinit.bin and
# modload.bin to the generator output they were compacted from:
#
# 1. Expanding each shipped blob gives that output byte for byte (its
#    SHA-256 below, the blob as mksdkinit.py wrote it, and as mkmodload.py
#    plus the production filter and trim-modload-gpon wrote it), and compacting the expansion gives the
#    shipped blob back. A regenerated blob updates the hash here.
# 2. compact on synthetic records: a sweep from FILL_MIN_ROWS rows on
#    becomes one call, a shorter one or a row with data stays, a run does
#    not cross a verb or category, and the CF pattern-1 row is found.
# 3. The modload filter keeps or drops a call record by its category, and
#    filtering commutes with compacting on the shipped modload.bin.
# 4. A call record the kernel loader refuses is refused here too.
set -eu
cd "$(dirname "$0")/.."
# The replay tables the test reads, built from tools/regtrace/tables/.
python3 tools/regtrace/replayblob.py build-tables

SDKINIT_EXPANDED_SHA256=defb1db3727d85a507bd6dce5467cb1388334cff6fa51baafec6423517d6e1cd
MODLOAD_EXPANDED_SHA256=0fb7fc7932c74c3412ab4674e998b4702a478e3ddde6eefab05602487b242b8c

python3 - "$SDKINIT_EXPANDED_SHA256" "$MODLOAD_EXPANDED_SHA256" <<'PY'
import hashlib
import sys

sys.path.insert(0, "tools/regtrace")
import replayblob as rb

FW = "out/firmware/odi/"
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


# 1. The shipped blobs against the generator output.
for name, want in (("sdkinit", sys.argv[1]), ("modload", sys.argv[2])):
    with open(FW + name + ".bin", "rb") as f:
        shipped = f.read()
    full = rb.expand(shipped)
    check(hashlib.sha256(full).hexdigest() == want,
          "%s.bin expands to the generator output byte for byte" % name)
    check(rb.compact(full) == shipped, "%s.bin is the compacted generator output" % name)
    check(rb.expand(full) == full, "%s: expanding a blob without calls changes nothing" % name)


def calls(records):
    return [r for r in records if r[0] == rb.KIND_CALL]


# 2. compact on synthetic sdkinit records.
vlan = rb.SDKINIT_VERBS.index("vlan")
cls = rb.SDKINIT_VERBS.index("classify")


def vlan_rows(first, n, verb=vlan):
    return [rb.switch_table(rb.TBL_VLAN_MEMBERS, i, [rb.VLAN_MEMBERS_SENTINEL], verb=verb)
            for i in range(first, first + n)]


def cf_rows(fill, first, n, split, verb=cls):
    return [rb.switch_table(t, row, words, verb=verb)
            for t, row, words in rb.fill_rows(fill, first, n, (split,))]


short = vlan_rows(0, rb.FILL_MIN_ROWS - 1)
check(rb.compact_records(rb.TABLE_SDKINIT, short) == short,
      "a run shorter than FILL_MIN_ROWS stays table records")
out = rb.compact_records(rb.TABLE_SDKINIT, vlan_rows(3, rb.FILL_MIN_ROWS))
check(out == [rb.switch_call(rb.FILL_VLAN_MEMBERS, 3, rb.FILL_MIN_ROWS, verb=vlan)],
      "a run of FILL_MIN_ROWS rows becomes one call, first row and count kept")

data = vlan_rows(0, 40)
data[20] = rb.switch_table(rb.TBL_VLAN_MEMBERS, 20, [0xff], verb=vlan)
out = rb.compact_records(rb.TABLE_SDKINIT, data)
check(len(out) == 3 and out[1] == data[20] and
      out[2] == rb.switch_call(rb.FILL_VLAN_MEMBERS, 21, 19, verb=vlan),
      "a row with other words ends the run, stays a table record, and a new run starts after it")

data = vlan_rows(0, 20) + vlan_rows(20, 20, verb=vlan + 1)
out = rb.compact_records(rb.TABLE_SDKINIT, data)
check([r[3] for r in calls(out)] == [vlan, vlan + 1], "a run does not cross a verb")

out = rb.compact_records(rb.TABLE_SDKINIT, cf_rows(rb.FILL_CF_RULES_ACTIONS, 0, 256, 1))
check(out == [rb.switch_call(rb.FILL_CF_RULES_ACTIONS, 0, 256, (1,), verb=cls)],
      "cf rules and actions: row 0 pattern 0, pattern 1 from row 1")
out = rb.compact_records(rb.TABLE_SDKINIT, cf_rows(rb.FILL_CF_RULES, 0, 256, rb.CF_ROW_COUNT))
check(out == [rb.switch_call(rb.FILL_CF_RULES, 0, 256, (rb.CF_ROW_COUNT,), verb=cls)],
      "cf rules: every row pattern 0 gives the pattern-1 row CF_ROW_COUNT")
out = rb.compact_records(rb.TABLE_SDKINIT, cf_rows(rb.FILL_CF_RULES, 100, 50, 100))
check(out == [rb.switch_call(rb.FILL_CF_RULES, 100, 50, (100,), verb=cls)],
      "cf rules: every row pattern 1 gives its first row")

data = cf_rows(rb.FILL_CF_RULES, 0, 20, 10) + cf_rows(rb.FILL_CF_RULES, 20, 20, rb.CF_ROW_COUNT)
out = rb.compact_records(rb.TABLE_SDKINIT, data)
check(out == [rb.switch_call(rb.FILL_CF_RULES, 0, 20, (10,), verb=cls),
              rb.switch_call(rb.FILL_CF_RULES, 20, 20, (rb.CF_ROW_COUNT,), verb=cls)],
      "cf rules: a pattern-0 row after pattern 1 ends the run")

try:
    rb.compact_records(rb.TABLE_GPON_INIT, [])
    check(False, "gpon init blobs take no call records")
except ValueError:
    pass

# 3. The modload filter and call records.
call = rb.switch_call(rb.FILL_CF_RULES_ACTIONS, 0, 256, (32,), category=1)
check(rb._modload_keep(0x7fffffbe, call) == call, "a category-1 call is kept with mask bit 1")
check(rb._modload_keep(0x7fffffbc, call) is None, "a category-1 call is dropped without mask bit 1")
check(rb._modload_keep(0x40000000, call) is None,
      "a call never takes the flood value of a category-0 register")
with open(FW + "modload.bin", "rb") as f:
    full = rb.expand(f.read())
for mask in (rb.PRODUCTION_MODLOAD_MASK, rb.PRODUCTION_MODLOAD_MASK & ~2, 0x40000000):
    check(rb.filter_modload(rb.compact(full), mask) == rb.compact(rb.filter_modload(full, mask)),
          "filter and compact commute, mask 0x%08x" % mask)

# 4. Malformed call records.
good = rb.switch_call(rb.FILL_CF_RULES, 0, 256, (1,), verb=cls)
check(rb.check_call(good) == (rb.FILL_CF_RULES, 0, 256, (1,)), "a good call record passes")
bad = {
    "unknown fill": rb.switch_call(7, 0, 1),
    "no rows": rb.switch_call(rb.FILL_VLAN_MEMBERS, 0, 0),
    "past the table": rb.switch_call(rb.FILL_VLAN_MEMBERS, 4095, 2),
    "a VLAN fill with a word": rb.switch_call(rb.FILL_VLAN_MEMBERS, 0, 1, (1,)),
    "a CF fill without its word": rb.switch_call(rb.FILL_CF_RULES, 0, 1),
    "pattern-1 row past the table": rb.switch_call(rb.FILL_CF_RULES, 0, 1, (257,)),
}
for what, rec in bad.items():
    try:
        rb.check_call(rec)
        check(False, "a call record with %s is refused" % what)
    except ValueError:
        pass

for f in fails:
    print("FAIL: " + f, file=sys.stderr)
if fails:
    sys.exit(1)
PY
echo "replayblob_fill_test: ok"
