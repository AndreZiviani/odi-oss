#!/bin/sh
# SPDX-FileCopyrightText: 2026 The odi-oss contributors
# SPDX-License-Identifier: GPL-2.0-or-later

# The register replay tables are text (tools/regtrace/tables/) and the
# blobs the image ships are built from it (replayblob.py build). This holds
# the two together:
#
# 1. Each table builds, and dumping the built blob and building that dump
#    gives the blob back byte for byte.
# 2. The committed text is what dump prints for its own blob: the same
#    header line and the same record lines, comments aside, so the text
#    carries no record dump would write another way.
# 3. A register record that src/diag/tools/regnames.txt names has a trailing
#    comment starting with that name, and no comment starts with the name
#    of another register.
# 4. build on synthetic text: comments and blank lines are ignored, the
#    header is checked, and an unknown field, a value too wide for its
#    field, a record the kernel loader refuses or more records than it
#    takes is refused, naming the line.
# 5. build-tables leaves a file alone when its bytes would not change.
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

python3 - "$T" <<'PY'
import os
import sys
import time

sys.path.insert(0, "tools/regtrace")
import replayblob as rb

T = sys.argv[1]
fails = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        fails.append(msg)


def refused(text, want):
    try:
        rb.build(text)
    except ValueError as e:
        check(want in str(e), "refused with %r, got %r" % (want, str(e)))
        return
    check(False, "accepted, want refused with %r" % want)


names = rb.load_regnames()
rb.build_tables(T)

for name in sorted(rb.TABLE_NAMES.values()):
    with open(os.path.join(rb.TABLES_DIR, name + ".txt")) as f:
        text = f.read()
    with open(os.path.join(T, name + ".bin"), "rb") as f:
        blob = f.read()

    # 1. build, dump, build again.
    check(rb.build(text) == blob, "%s: build-tables wrote what build gives" % name)
    check(rb.build(rb.dump(blob)) == blob, "%s: the dump of the blob builds it back" % name)

    # 2. The committed text against what dump prints.
    lines = text.splitlines()
    want = rb.dump(blob).splitlines()
    check(lines[0] == want[0], "%s: header line is %r" % (name, want[0]))
    bodies = [l.split("#", 1)[0].rstrip() for l in lines[1:]]
    bodies = [b for b in bodies if b]
    check(bodies == want[1:], "%s: record lines are exactly what dump prints" % name)

    # 3. Register names in the trailing comments.
    table, _crc, records = rb.unpack(blob)
    rec_lines = [l for l in lines[1:] if l.split("#", 1)[0].strip()]
    for rec, line in zip(records, rec_lines):
        comment = line.split("#", 1)[1].split() if "#" in line else []
        first = comment[0] if comment else None
        own = names.get(rb.record_offset(table, rec))
        if own:
            check(first == own, "%s: %r names its register %s" % (name, line, own))
        elif first in names.values():
            check(False, "%s: %r carries the name of another register" % (name, line))

# 4. Synthetic text.
HEAD = "# odi replay blob: table=sdkinit version=1"
ok = HEAD + "\n\n# a comment\nswitch REG off=0x00000010 val=0x00000001  # note\n" \
    "vlan CALL fill=vlan_members first=0x00000000 count=4096 words=-\n"
blob = rb.build(ok)
_t, crc, recs = rb.unpack(blob)
check(len(recs) == 2, "comments and blank lines are not records")
check(rb.build(rb.dump(blob)) == blob, "a header with records= and crc32= builds")
dumped = rb.dump(blob).splitlines()
bad_count = dumped[0].replace("records=2", "records=3")
refused("\n".join([bad_count] + dumped[1:]), "records=3, the text has 2")
bad_crc = dumped[0][:-8] + ("00000000" if not dumped[0].endswith("00000000") else "00000001")
refused("\n".join([bad_crc] + dumped[1:]), "crc32=")
refused("switch REG off=0x0 val=0x0\n", "line 1: not an odi replay blob header")
refused("# odi replay blob: table=nope version=1\n", "unknown table")
refused("# odi replay blob: table=sdkinit version=2\n", "version")
refused(HEAD + "\nnoverb REG off=0x00000010 val=0x00000001\n", "line 2: unknown verb")
refused(HEAD + "\nswitch REG off=0x00000010 val=0x100000000\n", "line 2: val=")
refused(HEAD + "\nswitch REG off=0x00000010 value=0x00000001\n", "line 2: expected val=")
refused(HEAD + "\nswitch REG off=0x00000010 val=0x00000001 extra\n", "line 2: unknown record")
refused(HEAD + "\nswitch TABLE table=23 idx=0x00000001 n=2 words=0x00000001\n",
        "line 2: n=2 but 1 words")
refused(HEAD + "\nvlan REG off=0x00000010 val=0x00000001\nswitch REG off=0x00000010 val=0x00000001\n",
        "line 3: sdkinit records not grouped by verb")
refused(HEAD + "\nvlan CALL fill=vlan_members first=0x00000000 count=4097 words=-\n",
        "line 2: call record rows")
refused("# odi replay blob: table=modload version=1\ncat=0 grp=0 TABLE table=20 idx=0x00000000 n=1 words=0x00000000\n",
        "line 2: modload table record category or reg_group out of range")
refused("# odi replay blob: table=modload version=1\ncat=0 grp=26 REG off=0x00000010 val=0x00000001\n",
        "line 2: modload register record category or reg_group out of range")
refused("# odi replay blob: table=gpon_init version=1\nREG sn=5 off=0x00700000 val=0x00000001\n",
        "line 2: gpon init record sn_word out of range")
refused("# odi replay blob: table=gpon_init version=1\nTABLE table=1 idx=0x00000000 n=0 words=-\n",
        "line 2: table record word count out of range")

rec = "switch REG off=0x00000010 val=0x00000001\n"
check(len(rb.unpack(rb.build(HEAD + "\n" + rec * rb.MAX_RECORDS))[2]) == rb.MAX_RECORDS,
      "a table of MAX_RECORDS records builds")
refused(HEAD + "\n" + rec * (rb.MAX_RECORDS + 1), "more than 65536 records")

# 5. build-tables rewrites nothing that would not change.
path = os.path.join(T, "sdkinit.bin")
os.utime(path, (1, 1))
rb.build_tables(T)
check(os.stat(path).st_mtime == 1, "build-tables left an unchanged blob alone")
with open(path, "r+b") as f:
    f.write(b"X")
rb.build_tables(T)
with open(path, "rb") as f:
    check(f.read(1) != b"X", "build-tables rewrote a blob that differed")
check(not [n for n in os.listdir(T) if n.endswith(".tmp")], "no temporary file is left")

if fails:
    for m in fails:
        print("FAIL " + m)
    sys.exit(1)
print("replay_tables_test: %d checks ok" % checks)
PY
