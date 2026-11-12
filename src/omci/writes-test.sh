#!/bin/sh
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later

# What a Create, Set or Delete does to the MIB and to MIB data sync, against
# G.988: a request the OLT repeats (same transaction id, same frame) is
# answered again with the same answer and changes nothing; a Create of an
# instance that exists is refused with result 7; a Set that names a read-only
# or set-by-create attribute fails with result 9, the failed attributes in the
# answer, and stores none of them.
#
# Run from src/omci, inside the toolchain image (make test-omci-writes):
#   docker run --rm -v "$PWD/../..":/src -w /src/src/omci "$(../../toolchain/image.sh diag)" \
#          sh writes-test.sh
set -u
Q=qemu-mips-static
fail=0
. ./testlib.sh

check() {
	if [ "$2" = "$3" ]; then
		echo "ok    $1"
	else
		echo "FAIL  $1"
		echo "--- expected"; printf '%s\n' "$3"
		echo "--- got";      printf '%s\n' "$2"
		fail=$((fail + 1))
	fi
}

# frame <tci> <type> <class> <inst> <contents hex>: one baseline frame, the
# contents padded with zeros to 32 bytes; trailer, zero CRC (not checked).
frame() {
	_c=$5
	while [ "${#_c}" -lt 64 ]; do _c="${_c}0"; done
	printf '%s%s0a%s%s%s0000002800000000' "$1" "$2" "$3" "$4" "$_c"
}

# answer <log> <tci>: the contents of the last answer to that tci (64 hex).
answer() {
	grep -- "^-> $2" "$1" | tail -1 | cut -c20-83
}

send() {
	inject_wait "$1" "$(frame "$2" "$3" "$4" "$5" "$6")"
}

# mds <log> <tci>: the MIB data sync the ONU reports (a Get of ONT data).
mds() {
	send "$1" "$2" 49 0002 0000 8000
	answer "$1" "$2" | cut -c7-8
}

mkdir -p /var/config /var/run /var/log
rm -f /var/config/odi.conf /var/config/lastgood.xml /var/config/lastgood_hs.xml \
      /var/config/omci-identity.on /var/run/omcid-mib.snap /var/run/omcid-resume-decision
printf 'state 5 (O5)\nonu_id 1\nsn 4f44495400000001\nalloc_ids 0\n' > /tmp/writes-gpon

log=/tmp/omcid-writes.log
: > "$log"
$Q respond/build/omcid -d -g /tmp/writes-gpon > "$log" 2>&1 &
pid=$!
omcid_up "$log"
send "$log" 0001 4f 0002 0000 ""                         # MIB reset
check "after a MIB reset the sync is 0" "$(mds "$log" 0002)" "00"

# ------------------------------------------------ Create, and its repeat
# GemPortCtp (268) instance 5: port id, T-CONT pointer, direction, upstream
# traffic manager, upstream descriptor, downstream queue. The UNI counter is
# read-only and is not in a Create.
GEM=01230000030000000000000000
send "$log" 0010 44 010c 0005 "$GEM"
check "a Create is answered ok" "$(answer "$log" 0010 | cut -c1-2)" "00"
check "and counts once" "$(mds "$log" 0011)" "01"
first=$(answer "$log" 0010)
send "$log" 0010 44 010c 0005 "$GEM"
check "the same frame again gets the same answer" "$(answer "$log" 0010)" "$first"
check "and is logged as a repeat" \
      "$(grep -c 'retransmission, answer repeated' "$log")" "1"
check "the repeat does not count again" "$(mds "$log" 0012)" "01"

# A different request for the instance that exists is a Create of an
# existing instance: result 7, no count.
send "$log" 0013 44 010c 0005 "$GEM"
check "a new Create of an existing instance is refused with result 7" \
      "$(answer "$log" 0013 | cut -c1-2)" "07"
check "and does not count" "$(mds "$log" 0014)" "01"

# ------------------------------------------- Set: read-only attributes
# Attributes 1 (port id, writable) and 6 (UNI counter, read-only), mask 8400.
send "$log" 0020 48 010c 0005 "84000456aa"
check "a Set naming a read-only attribute fails with result 9, the failed mask 0400" \
      "$(answer "$log" 0020 | cut -c1-10)" "0900000400"
send "$log" 0021 49 010c 0005 "8400"
check "the writable one was stored and the read-only one was not" \
      "$(answer "$log" 0021 | cut -c1-12)" "008400045600"
check "the Set that stored something counts" "$(mds "$log" 0022)" "02"
reply=$(answer "$log" 0020)
send "$log" 0020 48 010c 0005 "84000456aa"
check "a repeated Set gets the same answer" "$(answer "$log" 0020)" "$reply"
check "and does not count again" "$(mds "$log" 0023)" "02"
send "$log" 0024 48 010c 0005 "0400aa"
check "a Set of only a read-only attribute fails with result 9" \
      "$(answer "$log" 0024 | cut -c1-10)" "0900000400"
check "and, storing nothing, does not count" "$(mds "$log" 0025)" "02"
send "$log" 0026 48 010c 0005 "8000789a"
check "a Set of writable attributes alone is ok, with no failed mask" \
      "$(answer "$log" 0026 | cut -c1-2)" "00"
check "and counts" "$(mds "$log" 0027)" "03"

# Set-by-create only: CircuitPack (6) Type is written at Create and never by
# a Set; AdminState (attribute 6) next to it is writable.
send "$log" 0030 48 0006 0101 "84000501"
check "a Set of a set-by-create attribute fails, the writable one beside it does not" \
      "$(answer "$log" 0030 | cut -c1-10)" "0900008000"
send "$log" 0031 49 0006 0101 "0400"
check "AdminState was stored" "$(answer "$log" 0031 | cut -c1-8)" "00040001"

# ------------------------------------------------------ Delete and repeat
send "$log" 0040 46 010c 0005 ""
check "a Delete is answered ok" "$(answer "$log" 0040 | cut -c1-2)" "00"
before=$(mds "$log" 0041)
send "$log" 0040 46 010c 0005 ""
check "a repeated Delete is answered again" "$(answer "$log" 0040 | cut -c1-2)" "00"
check "and does not count again" "$(mds "$log" 0042)" "$before"

# A MIB reset starts the OLT's transaction ids over: the same Create as the
# first one must run, not be answered from the cache.
send "$log" 0050 4f 0002 0000 ""
send "$log" 0010 44 010c 0005 "$GEM"
check "after a MIB reset the same frame is a new request" "$(mds "$log" 0051)" "01"

# ------------------------------------------- credentials: log and dumps
# What an OLT provisions into an authentication security method (148), a
# large string (157) and an octet string (307) is the line owner's secret. It
# stays out of omcid.log, `omcli mib` masks it unless --secrets is given, and
# the diag bundle never asks for it.
hexof() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; }
PW=hunter2xyz;           PWX=$(hexof "$PW")
USR=alice-sip;           USRX=$(hexof "$USR")
URL=http://bob:URLpw99@acs; URLX=$(hexof "$URL")
OCT=octetsecret42;       OCTX=$(hexof "$OCT")

send "$log" 0060 44 0094 0001 ""                         # 148 Create
send "$log" 0061 48 0094 0001 "4000${USRX}"              # Username1
send "$log" 0062 48 0094 0001 "2000${PWX}"               # Password
send "$log" 0063 44 009d 0002 ""                         # 157 Create
send "$log" 0064 48 009d 0002 "4000${URLX}"              # Part1
send "$log" 0065 44 0133 0003 ""                         # 307 Create
send "$log" 0066 48 0133 0003 "4000${OCTX}"              # Part1
check "the Set of a password is answered ok" \
      "$(answer "$log" 0062 | cut -c1-2)" "00"

leaks() {	# <file> <value> <hex of it>: lines carrying it as text, hex or
		# spaced hex (the log prints raw contents a byte at a time)
	grep -c -i -e "$2" -e "$3" -e "$(printf '%s' "$3" | sed 's/../& /g;s/ $//')" "$1" || true
}
for v in "$PW $PWX" "$USR $USRX" "$URL $URLX" "$OCT $OCTX"; do
	set -- $v
	check "omcid.log never carries $1" "$(leaks "$log" "$1" "$2")" "0"
done
check "the contents of those Sets are logged as withheld" \
      "$(grep -c 'contents withheld (credentials)' "$log")" "7"

dump() {	# omcli mib arguments: one dump, to a file
	$Q cli/build/omcli "$@" > /tmp/writes-dump 2>&1
}
for c in 148 157 307; do
	dump mib "$c"
	check "omcli mib $c masks the bytes" \
	      "$([ "$(grep -c '\*\*' /tmp/writes-dump)" -gt 0 ] && echo masked)" masked
done
dump mib 148
check "the default dump of 148 hides the password and the username" \
      "$(leaks /tmp/writes-dump "$PW" "$PWX")$(leaks /tmp/writes-dump "$USR" "$USRX")" "00"
pwlen() { grep Password /tmp/writes-dump | tr -s ' ' | cut -d' ' -f3 | tr -d '\n' | wc -c; }
masked=$(pwlen)
dump mib 148 --secrets
check "a masked attribute keeps the width of the real one" "$masked" "$(pwlen)"
dump mib 148
dump mib 157
check "the default dump of 157 hides the URL" "$(leaks /tmp/writes-dump "$URL" "$URLX")" "0"
dump mib 307
check "the default dump of 307 hides the octets" "$(leaks /tmp/writes-dump "$OCT" "$OCTX")" "0"
dump mib 148 --secrets
check "--secrets shows the password" "$(leaks /tmp/writes-dump "$PW" "$PWX")" "1"
dump --secrets mib 148 1
check "--secrets works in front too, with an entity id" \
      "$(leaks /tmp/writes-dump "$USR" "$USRX")" "1"
dump mib 157 --secrets
check "--secrets shows the URL" "$(leaks /tmp/writes-dump "$URL" "$URLX")" "1"
dump mib 307 --secrets
check "--secrets shows the octets" "$(leaks /tmp/writes-dump "$OCT" "$OCTX")" "1"
dump mib 148
check "an attribute that is not a secret is not masked" \
      "$(grep -c 'ValidSchem  *00' /tmp/writes-dump)" "1"

# The vendor protocol (`omcicli mib get`, `mib getattr`) is a second way to
# the same rows.
dump --vendor mib get 148
check "the vendor-protocol dump of 148 masks it" \
      "$(leaks /tmp/writes-dump "$PW" "$PWX")" "0"
dump --vendor mib get 148 --secrets
check "and shows it with --secrets" "$(leaks /tmp/writes-dump "$PW" "$PWX")" "1"
dump mib getattr 148 1 3
check "mib getattr masks the password" "$(leaks /tmp/writes-dump "$PW" "$PWX")" "0"
dump mib getattr 148 1 3 --secrets
check "and shows it with --secrets" "$(leaks /tmp/writes-dump "$PW" "$PWX")" "1"
dump mib getattr 148 1 1
check "a getattr of a plain attribute is untouched" \
      "$(cat /tmp/writes-dump)" "class 148 entity 1 attr 1 = 00"

# The diag bundle runs `omcli mib <class>` for a fixed list and never passes
# --secrets; a secret class in that list would still be masked, but it does
# not belong there.
for c in 148 157 307 340 134; do
	check "diag-bundle.sh does not dump class $c" \
	      "$(sed -n 's/^OMCI_CLASSES="\(.*\)"/\1/p' ../../rootfs/skeleton/etc/scripts/diag-bundle.sh | tr ' ' '\n' | grep -cx "$c")" "0"
done
check "diag-bundle.sh never passes --secrets" \
      "$(grep -v '^[[:space:]]*#' ../../rootfs/skeleton/etc/scripts/diag-bundle.sh | grep -e '--secrets' | grep -vc -e '--secrets-list')" "0"

# ------------------------------------- wide rows: large string, auth method
# A row holds 96 bytes; LargeString (157) packs 376 and the authentication
# method (148) 101, so their later attributes need the side pool. attribute k
# of both is a 25-byte string (k = 1 of 157 is the number of parts), and a
# Get answers 25 bytes of one attribute: result, mask, value.
amask() { printf '%04x' $((1 << (16 - $1))); }
aval() {                                 # 25 bytes, all of the byte 0x10 + k
	_b=$(printf '%02x' $((0x10 + $1)))
	_s=""; _n=0
	while [ "$_n" -lt 25 ]; do _s="$_s$_b"; _n=$((_n + 1)); done
	printf '%s' "$_s"
}
wtci() { printf '%04x' $(($1)); }

send "$log" 0100 4f 0002 0000 ""         # MIB reset: the rows start empty
send "$log" 0101 44 009d 0001 ""
check "Create of a large string is answered ok" "$(answer "$log" 0101 | cut -c1-2)" "00"
send "$log" "$(wtci 0x102)" 48 009d 0001 "$(amask 1)0f"
check "Set of the number of parts (15) is ok" "$(answer "$log" 0102 | cut -c1-2)" "00"
k=2
while [ "$k" -le 16 ]; do
	send "$log" "$(wtci $((0x110 + k)))" 48 009d 0001 "$(amask $k)$(aval $k)"
	k=$((k + 1))
done
k=2; bad=0
while [ "$k" -le 16 ]; do
	send "$log" "$(wtci $((0x130 + k)))" 49 009d 0001 "$(amask $k)"
	[ "$(answer "$log" "$(wtci $((0x130 + k)))" | cut -c1-56)" = "00$(amask $k)$(aval $k)" ] ||
		{ bad=$k; echo "      large string attribute $k (part $((k - 1))) did not read back whole"; }
	k=$((k + 1))
done
check "all 15 parts of a large string read back whole" "$bad" "0"
send "$log" 0150 49 009d 0001 "$(amask 1)"
check "and so does the number of parts" "$(answer "$log" 0150 | cut -c1-8)" "0080000f"
check "the row is not marked truncated" \
      "$($Q cli/build/omcli mib get 157 2>&1 | grep -c 'row truncated')" "0"

# 148 with a username longer than 25 bytes: username 2 is the fifth attribute.
send "$log" 0160 44 0094 0002 ""
check "Create of an authentication method is ok" "$(answer "$log" 0160 | cut -c1-2)" "00"
for k in 2 3 4 5; do
	send "$log" "$(wtci $((0x170 + k)))" 48 0094 0002 "$(amask $k)$(aval $k)"
done
bad=0
for k in 2 3 4 5; do
	send "$log" "$(wtci $((0x180 + k)))" 49 0094 0002 "$(amask $k)"
	[ "$(answer "$log" "$(wtci $((0x180 + k)))" | cut -c1-56)" = "00$(amask $k)$(aval $k)" ] ||
		{ bad=$k; echo "      authentication method attribute $k did not read back"; }
done
check "username 1, password, realm and username 2 all read back" "$bad" "0"

# The dump (omcicli mib get) shows the last part, not a row cut at part 3:
# with --secrets, since a large string's parts are masked by default, and
# masked across the whole width without it.
check "the dump with --secrets shows part 15 of the large string" \
      "$($Q cli/build/omcli mib get 157 --secrets 2>&1 | grep -ci "$(aval 16)")" "1"
check "and the default dump masks part 15 too" \
      "$($Q cli/build/omcli mib get 157 2>&1 | grep -ci "$(aval 16)")" "0"

# MIB upload: the wide rows are the last two entities, 157 in 15 records (the
# number of parts and part 1 share the first; every later part is a record of
# its own, as no two fit 26 bytes) and 148 in four, the last of them
# username 2. An attribute is never split across records.
send "$log" 0190 4d 0002 0000 ""
n=$((0x$(answer "$log" 0190 | cut -c1-4)))
first=$((n - 19))
s=$first
while [ "$s" -lt "$n" ]; do
	send "$log" "$(wtci $((0x1a0 + s - first)))" 4e 0002 0000 "$(printf '%04x' "$s")"
	s=$((s + 1))
done
rec() { answer "$log" "$(wtci $((0x1a0 + $1)))"; }
check "upload: record 0 of the large string carries the parts count and part 1 (mask c000)" \
      "$(rec 0 | cut -c1-12)" "009d0001c000"
check "upload: the last record of the large string is part 15, whole" \
      "$(rec 14 | cut -c1-62)" "009d00010001$(aval 16)"
check "upload: the next entity is the authentication method" \
      "$(rec 15 | cut -c1-12)" "00940002c000"
check "upload: its last record is username 2, whole" \
      "$(rec 18 | cut -c1-62)" "009400020800$(aval 5)"

# The pool is bounded: past it a wide row behaves as before. 148 and the
# large string above hold two of its 32 slots; 34 more large strings
# exhaust it, the last four finding it full.
k=0
while [ "$k" -lt 34 ]; do
	send "$log" "$(wtci $((0x200 + k * 4)))" 44 009d "$(printf '%04x' $((0x10 + k)))" ""
	send "$log" "$(wtci $((0x201 + k * 4)))" 48 009d "$(printf '%04x' $((0x10 + k)))" "$(amask 2)$(aval 2)"
	send "$log" "$(wtci $((0x202 + k * 4)))" 48 009d "$(printf '%04x' $((0x10 + k)))" "$(amask 6)$(aval 6)"
	k=$((k + 1))
done
send "$log" 0300 49 009d 0010 "$(amask 6)"
check "an early large string beyond the 96 bytes still reads back" \
      "$(answer "$log" 0300 | cut -c1-56)" "00$(amask 6)$(aval 6)"
send "$log" 0301 49 009d 0031 "$(amask 6)"
check "one that found the pool full drops attribute 6, as before" \
      "$(answer "$log" 0301 | cut -c1-56)" "00$(amask 6)$(printf '%050d' 0)"
send "$log" 0302 49 009d 0031 "$(amask 2)"
check "and keeps the attributes inside 96 bytes" \
      "$(answer "$log" 0302 | cut -c1-56)" "00$(amask 2)$(aval 2)"
check "the pool running out is logged once" \
      "$(grep -c 'wide-row pool of 32 is full' "$log")" "1"

# ------------------------------------------------ Test (18)
# Only the ANI-G has a test here (the optical measurements, answered with a
# Test result after the response). Any other class the ONU holds is refused
# with result 2 and no result follows, so the OLT is not left waiting for a
# measurement nobody takes. 0x52 is Test with AR set.
send "$log" 0400 52 0107 8001 ""
check "a Test on the ANI-G is answered ok" "$(answer "$log" 0400 | cut -c1-2)" "00"
check "and its Test result follows" "$(grep -c '^-> test result' "$log")" "1"
for t in "0401 0006 0101 circuit-pack" "0402 0086 0000 IP-host" "0403 0100 0000 ONU-G" "0404 002f 0001 MAC-bridge-port"; do
	set -- $t
	send "$log" "$1" 52 "$2" "$3" ""
	check "a Test on the $4 is refused, result 2" \
	      "$(answer "$log" "$1" | cut -c1-2)" "02"
done
check "none of them sent a Test result" "$(grep -c '^-> test result' "$log")" "1"
send "$log" 0405 52 ff00 0000 ""
check "a class the ONU does not hold keeps its old answer, ok" \
      "$(answer "$log" 0405 | cut -c1-2)" "00"

# ------------------------------------------------ Dot1X port extension (290)
# An OLT that runs 802.1X on a UNI Sets class 290, instance 257 (the PPTP
# Ethernet UNI), attributes 1 and 2 (mask c000: enable 1, action register 2),
# and starts the exchange only if the Set is accepted. This run has no
# OMCI_UNKNOWN_ME_OK (the key is off above), so the class has to be modelled.
# omcid stores the values and does not act on them. The ONU holds no instance
# until the Set, so the MIB upload gains one entity only then.
send "$log" 0600 4f 0002 0000 ""
send "$log" 0601 4d 0002 0000 ""
before=$((0x$(answer "$log" 0601 | cut -c1-4)))
send "$log" 0602 49 0122 0101 "c000"
check "a Get of 290 before any Set answers zeros, not an unknown entity" \
      "$(answer "$log" 0602 | cut -c1-10)" "00c0000000"
send "$log" 0603 48 0122 0101 "c0000102"
check "a Set of 290 attributes 1 and 2 is answered ok without OMCI_UNKNOWN_ME_OK" \
      "$(answer "$log" 0603 | cut -c1-2)" "00"
send "$log" 0604 49 0122 0101 "c000"
check "and a Get returns the stored values" \
      "$(answer "$log" 0604 | cut -c1-10)" "00c0000102"
send "$log" 0605 48 0122 0101 "40000a"
send "$log" 0606 49 0122 0101 "c000"
check "a Set of one attribute keeps the other" \
      "$(answer "$log" 0606 | cut -c1-10)" "00c000010a"
check "the key is still off" "$(grep -c '^unknown entities: answered unknown entity' "$log")" "1"
send "$log" 0607 4d 0002 0000 ""
after=$((0x$(answer "$log" 0607 | cut -c1-4)))
check "after the Set the MIB upload lists one more entity" "$((after - before))" "1"
s=$before
found=""
while [ "$s" -lt "$after" ]; do
	send "$log" "$(wtci $((0x610 + s - before)))" 4e 0002 0000 "$(printf '%04x' "$s")"
	r=$(answer "$log" "$(wtci $((0x610 + s - before)))" | cut -c1-16)
	[ "$(printf '%s' "$r" | cut -c1-8)" = "01220101" ] && found=$r
	s=$((s + 1))
done
check "and it is 290 instance 257 with both attributes" "$found" "01220101c000010a"
send "$log" 0620 4f 0002 0000 ""
send "$log" 0621 4d 0002 0000 ""
check "a MIB reset takes it out of the upload again" \
      "$((0x$(answer "$log" 0621 | cut -c1-4)))" "$before"

# ------------------------------------------------ the MIB full
# The store holds 320 rows. Past them a Create is answered result 1,
# processing error, logged "no room in the MIB", and changes nothing; the
# rows already held keep working. 330 Creates of TCP/UDP config data (no
# driver side to them) in one batch after a MIB reset.
send "$log" 0500 4f 0002 0000 ""
i=1
while [ "$i" -le 330 ]; do
	frame "$(printf '%04x' $((0x1000 + i)))" 44 0088 "$(printf '%04x' "$i")" "13c411000000"
	echo
	i=$((i + 1))
done > /tmp/writes-full.txt
$Q cli/build/omcli --inject-file /tmp/writes-full.txt > /dev/null 2>&1
sync_send "$log"
check "320 Creates fill the MIB" "$(grep -c '^-> created (320 rows held)' "$log")" "1"
check "the ten past it are refused, no room in the MIB" \
      "$(grep -c '^-> no room in the MIB' "$log")" "10"
check "with result 1, processing error" "$(answer "$log" 114a | cut -c1-2)" "01"
send "$log" 0501 48 0088 0140 "100001"
check "a row already held still takes a Set" "$(answer "$log" 0501 | cut -c1-2)" "00"
send "$log" 0502 4f 0002 0000 ""

# The stock setters omcid does not implement: refused by the client, nothing
# sent, exit 1 (the request used to be dropped silently). mib reset is
# implemented and is not in this list. set logfile is accepted by omcid and
# does nothing, which reads the same; set resetlauth gets an empty reply that
# prints as if it had worked.
for c in "mib set 84 4354 FwdOp 0x00" "mib create 84 1 0" "mib delete 84 1" \
	 "set sn ABCD 01020304" "set devmode router" "set dmmode off" \
	 "set loid abc" "set log err err" "set logfile 1" "set resetlauth" \
	 "debug simavc 84 1 1"; do
	got=$($Q cli/build/omcli $c -f 2>&1); rc=$?
	check "omcli $c: refused, status 1" "$rc" "1"
	check "omcli $c: says omcid does not implement it" \
	      "$(printf '%s' "$got" | grep -c 'omcid does not implement .*nothing was sent')" "1"
done
got=$($Q cli/build/omcli --vendor --id 28 -f 2>&1); rc=$?
check "omcli --vendor --id 28 -f: refused too" "$rc:$(printf '%s' "$got" | grep -c 'does not implement')" "1:1"
got=$($Q cli/build/omcli mib reset -f 2>&1); rc=$?
check "an implemented setter (mib reset -f) is not refused" \
      "$rc:$(printf '%s' "$got" | grep -c 'does not implement')" "0:0"

omcid_stop "$pid"
[ "$fail" -eq 0 ] || keep_log "$log" writes-test.log
if [ "$fail" -eq 0 ]; then echo "writes-test.sh: all checks passed"; else echo "FAILED ($fail)"; fi
suite_exit
